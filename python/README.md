# Python runtime

`python/xeh` uses `ctypes` to call `libxeh`; it does not reimplement the wire
protocol or load Python into Xorg. It needs Python 3 and an installed libxeh,
or `XEH_LIBXEH_PATH=/path/to/libxeh.so` during development. The runtime is
single-threaded and uses `selectors` to wait on `xeh_get_fd()`.

Build with `meson setup build -Dpython=true` and `meson compile -C build`.
For a source-tree run:

```sh
PYTHONPATH=python XEH_LIBXEH_PATH="$PWD/build/libxeh.so" \
  python3 python/examples/hello_extension.py "$XEH_SOCKET_PATH"
```

Subclass `xeh.Extension`, give it a name and version, and decorate methods
with `@xeh.request(number)`. A handler receives a copied `Request` and can
return bytes, a UTF-8 string, or a JSON-serializable value (encoded as compact
UTF-8 JSON). `None` defers the reply; call `connection.reply(request, value)`
or `connection.send_error(request, code)` later. `Request.string()` decodes
UTF-8, allowing up to three trailing NUL alignment bytes. `connection.event()`
queues an asynchronous event. The host currently denies events by policy.

`Connection.fd`, `Connection.dispatch()`, and `Connection.step()` support
integration with other event loops. `xeh.run()` handles registration and a
blocking selector loop until interrupted. Request exceptions yield a protocol
error and are logged. Object creation is not yet exposed by `libxeh`.

For software buffers, request `xeh.CAP_SHM` in the extension and enable SHM
in the host. `Connection.import_pixels(data, width, height, callback)` copies
raw bytes to a sealed memfd and queues import. The callback receives
`(status, handle)`. Once it succeeds, `bind_buffer(handle, request.client_id,
callback)` grants one X11 client access; `release_buffer(handle)` releases
the host buffer. Import and bind callbacks run during `dispatch()` and must
not disconnect the connection. The Python wrapper uses only `libxeh`, not
libX11, DRI2, or DRI3.
