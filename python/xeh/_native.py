"""Private ctypes bridge to libxeh. Wire parsing remains in the C library."""

import ctypes as c
import ctypes.util
import os


class NativeRequest(c.Structure):
    _fields_ = [
        ("sequence", c.c_uint32),
        ("extension_id", c.c_uint32),
        ("request_number", c.c_uint16),
        ("client_id", c.c_uint32),
        ("target_object", c.c_uint32),
        ("payload", c.POINTER(c.c_uint8)),
        ("payload_length", c.c_size_t),
    ]


RequestCallback = c.CFUNCTYPE(None, c.c_void_p, c.POINTER(NativeRequest), c.c_void_p)
RegistrationCallback = c.CFUNCTYPE(None, c.c_void_p, c.c_int, c.c_void_p)
ErrorCallback = c.CFUNCTYPE(
    None, c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint16, c.c_uint32, c.c_void_p
)


class ExtensionInfo(c.Structure):
    _fields_ = [
        ("name", c.c_char_p),
        ("major_version", c.c_uint16),
        ("minor_version", c.c_uint16),
        ("request_count", c.c_uint16),
        ("event_count", c.c_uint16),
        ("error_count", c.c_uint16),
        ("requested_capabilities", c.c_uint64),
        ("on_request", RequestCallback),
        ("on_registered", RegistrationCallback),
        ("userdata", c.c_void_p),
    ]


def load():
    path = os.environ.get("XEH_LIBXEH_PATH") or ctypes.util.find_library("xeh")
    if not path:
        raise OSError("libxeh not found; set XEH_LIBXEH_PATH")
    lib = c.CDLL(path, use_errno=True)
    signatures = {
        "xeh_connect_with_timeout": (c.c_void_p, [c.c_char_p, c.c_int]),
        "xeh_disconnect": (None, [c.c_void_p]),
        "xeh_register_extension": (c.c_int, [c.c_void_p, c.POINTER(ExtensionInfo), c.POINTER(c.c_void_p)]),
        "xeh_unregister_extension": (c.c_int, [c.c_void_p]),
        "xeh_extension_is_ready": (c.c_bool, [c.c_void_p]),
        "xeh_extension_id": (c.c_uint32, [c.c_void_p]),
        "xeh_extension_capabilities": (c.c_uint64, [c.c_void_p]),
        "xeh_get_fd": (c.c_int, [c.c_void_p]),
        "xeh_wants_write": (c.c_bool, [c.c_void_p]),
        "xeh_dispatch": (c.c_int, [c.c_void_p]),
        "xeh_set_error_handler": (None, [c.c_void_p, ErrorCallback, c.c_void_p]),
        "xeh_send_reply": (c.c_int, [c.c_void_p, c.POINTER(NativeRequest), c.c_void_p, c.c_size_t]),
        "xeh_send_error": (c.c_int, [c.c_void_p, c.POINTER(NativeRequest), c.c_uint16, c.c_uint16, c.c_uint32]),
        "xeh_send_event": (c.c_int, [c.c_void_p, c.c_uint16, c.c_uint32, c.c_uint32, c.c_void_p, c.c_size_t]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(lib, name)
        function.restype = result
        function.argtypes = arguments
    return lib
