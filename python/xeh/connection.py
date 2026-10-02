"""Single-threaded, selector-driven Python runtime over libxeh."""

import ctypes
import json
import logging
import os
import selectors
import time

from ._native import (ErrorCallback, ExtensionInfo, NativeShmInfo,
                      RegistrationCallback, RequestCallback, ShmResultCallback, load)
from .request import Request


LOGGER = logging.getLogger(__name__)
CAP_SHM = 1 << 3
BUFFER_XRGB8888 = 1
BUFFER_ARGB8888 = 2
BUFFER_RGB565 = 3


class XEHError(RuntimeError):
    pass


def _check(status, operation):
    if status < 0:
        raise XEHError(f"{operation} failed with libxeh status {status}")


def _payload(value):
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("utf-8")
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


class Connection:
    def __init__(self, socket_path=None, timeout_ms=5000):
        self._lib = load()
        path = os.fsencode(socket_path) if socket_path is not None else None
        self._handle = self._lib.xeh_connect_with_timeout(path, timeout_ms)
        if not self._handle:
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), socket_path)
        self._extension = None
        self._handlers = {}
        self._registration_status = None
        self._callback_failure = None
        self._buffer_callbacks = {}
        self._callbacks = (
            RequestCallback(self._on_request),
            RegistrationCallback(self._on_registered),
            ErrorCallback(self._on_error),
        )
        self._lib.xeh_set_error_handler(self._handle, self._callbacks[2], None)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    @property
    def fd(self):
        return self._lib.xeh_get_fd(self._handle) if self._handle else -1

    @property
    def extension_id(self):
        return self._lib.xeh_extension_id(self._extension) if self._extension else 0

    @property
    def capabilities(self):
        return self._lib.xeh_extension_capabilities(self._extension) if self._extension else 0

    def register(self, extension):
        if self._extension is not None:
            raise XEHError("one extension per connection")
        handlers = extension._handlers()
        name = extension.name
        version = extension.version
        if not isinstance(name, str) or not isinstance(version, tuple) or len(version) != 2:
            raise ValueError("extension needs a name and (major, minor) version")
        if any(not isinstance(value, int) or not 0 <= value <= 65535 for value in version):
            raise ValueError("invalid extension version")
        if any(not isinstance(value, int) or not 0 <= value <= 65535 for value in
               (extension.event_count, extension.error_count)):
            raise ValueError("invalid event or error count")
        if not isinstance(extension.requested_capabilities, int) or not 0 <= extension.requested_capabilities <= 0xffffffffffffffff:
            raise ValueError("invalid capabilities")
        info = ExtensionInfo(name.encode("ascii"), *version, max(handlers, default=0),
                             extension.event_count, extension.error_count,
                             extension.requested_capabilities,
                             self._callbacks[0], self._callbacks[1], None)
        pointer = ctypes.c_void_p()
        _check(self._lib.xeh_register_extension(self._handle, ctypes.byref(info),
                                                 ctypes.byref(pointer)), "register")
        self._extension = pointer
        self._handlers = handlers
        extension.connection = self

    def _on_registered(self, _extension, status, _userdata):
        self._registration_status = status

    def _on_error(self, _connection, sequence, object_id, code, detail, _userdata):
        LOGGER.warning("host error: sequence=%d object=%d code=%d detail=%d",
                       sequence, object_id, code, detail)

    def _on_request(self, _connection, native_request, _userdata):
        try:
            self._handle_request(native_request)
        except BaseException as error:
            self._callback_failure = error

    def _handle_request(self, native_request):
        request = Request.from_native(native_request)
        handler = self._handlers.get(request.number)
        if handler is None:
            self.send_error(request, 6)
            return
        try:
            result = handler(request)
            if result is not None:
                self.reply(request, result)
        except (ValueError, UnicodeError) as error:
            LOGGER.warning("invalid request %d: %s", request.number, error)
            self.send_error(request, 1)
        except Exception:
            LOGGER.exception("request %d failed", request.number)
            self.send_error(request, 10)

    def reply(self, request, value):
        payload = _payload(value)
        native = request._native()
        _check(self._lib.xeh_send_reply(self._handle, ctypes.byref(native),
                                        payload, len(payload)), "reply")

    def send_error(self, request, code, detail=0):
        native = request._native()
        _check(self._lib.xeh_send_error(self._handle, ctypes.byref(native),
                                        code, 0, detail), "send error")

    def event(self, number, target_client, value, target_object=0):
        payload = _payload(value)
        _check(self._lib.xeh_send_event(self._extension, number, target_client,
                                        target_object, payload, len(payload)), "event")

    def _buffer_result(self, operation, callback):
        if not callable(callback):
            raise TypeError("callback must be callable")
        if operation in self._buffer_callbacks:
            raise XEHError(f"{operation} already pending")

        def complete(_extension, status, handle, _userdata):
            keepalive = self._buffer_callbacks.pop(operation, None)
            try:
                callback(status, handle)
            except BaseException as error:
                self._callback_failure = error
            finally:
                del keepalive

        native = ShmResultCallback(complete)
        self._buffer_callbacks[operation] = native
        return native

    def import_pixels(self, pixels, width, height, callback,
                      stride=None, pixel_format=BUFFER_XRGB8888):
        if not self._extension or not self._lib.xeh_extension_is_ready(self._extension):
            raise XEHError("extension is not registered")
        if (self.capabilities & CAP_SHM) == 0:
            raise XEHError("SHM capability was not granted")
        if (not isinstance(width, int) or not 0 < width <= 16384 or
                not isinstance(height, int) or not 0 < height <= 16384):
            raise ValueError("invalid buffer dimensions")
        if pixel_format not in (BUFFER_XRGB8888, BUFFER_ARGB8888, BUFFER_RGB565):
            raise ValueError("unsupported pixel format")
        bytes_per_pixel = 2 if pixel_format == BUFFER_RGB565 else 4
        if stride is None:
            stride = width * bytes_per_pixel
        if (not isinstance(stride, int) or
                not width * bytes_per_pixel <= stride <= 0xffffffff or
                stride % bytes_per_pixel):
            raise ValueError("invalid buffer stride")
        try:
            view = memoryview(pixels)
        except TypeError as error:
            raise TypeError("pixels must be bytes-like") from error
        if not 0 < view.nbytes <= 536870912 or stride * height > view.nbytes:
            raise ValueError("invalid buffer size")
        data = view.tobytes()
        info = NativeShmInfo(width, height, stride, pixel_format, 0, len(data))
        native = self._buffer_result("import", callback)
        try:
            _check(self._lib.xeh_import_pixels(self._extension, data, len(data),
                                                ctypes.byref(info), native, None),
                   "import pixels")
        except BaseException:
            self._buffer_callbacks.pop("import", None)
            raise

    def bind_buffer(self, handle, client_id, callback):
        if not self._extension or not self._lib.xeh_extension_is_ready(self._extension):
            raise XEHError("extension is not registered")
        if (not isinstance(handle, int) or not 0 < handle <= 0xffffffff or
                not isinstance(client_id, int) or not 0 < client_id <= 0xffffffff):
            raise ValueError("invalid buffer handle or client id")
        native = self._buffer_result("bind", callback)
        try:
            _check(self._lib.xeh_bind_shm_client(self._extension, handle,
                                                  client_id, native, None),
                   "bind buffer")
        except BaseException:
            self._buffer_callbacks.pop("bind", None)
            raise

    def release_buffer(self, handle):
        if not self._extension or not self._lib.xeh_extension_is_ready(self._extension):
            raise XEHError("extension is not registered")
        if not isinstance(handle, int) or not 0 < handle <= 0xffffffff:
            raise ValueError("invalid buffer handle")
        _check(self._lib.xeh_release_shm(self._extension, handle), "release buffer")

    def dispatch(self):
        if not self._handle:
            raise XEHError("connection is closed")
        status = self._lib.xeh_dispatch(self._handle)
        if self._callback_failure is not None:
            error = self._callback_failure
            self._callback_failure = None
            raise XEHError("request callback failed") from error
        _check(status, "dispatch")

    def step(self, timeout=None):
        if not self._handle:
            raise XEHError("connection is closed")
        with selectors.DefaultSelector() as selector:
            events = selectors.EVENT_READ
            if self._lib.xeh_wants_write(self._handle):
                events |= selectors.EVENT_WRITE
            selector.register(self.fd, events)
            if selector.select(timeout):
                self.dispatch()

    def wait_registered(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while self._registration_status is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("extension registration timed out")
            self.step(remaining)
        _check(self._registration_status, "registration")
        if not self._lib.xeh_extension_is_ready(self._extension):
            raise XEHError("extension registration failed")

    def serve_forever(self):
        while True:
            self.step()

    def close(self):
        if not self._handle:
            return
        if self._extension and self._lib.xeh_extension_is_ready(self._extension):
            if self._lib.xeh_unregister_extension(self._extension) >= 0:
                deadline = time.monotonic() + 1.0
                while self.extension_id and time.monotonic() < deadline:
                    try:
                        self.step(min(0.1, deadline - time.monotonic()))
                    except XEHError:
                        break
        self._lib.xeh_disconnect(self._handle)
        self._handle = None
        self._buffer_callbacks.clear()


def run(extension, socket_path=None):
    with Connection(socket_path) as connection:
        connection.register(extension)
        connection.wait_registered()
        try:
            connection.serve_forever()
        except KeyboardInterrupt:
            pass
