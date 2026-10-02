# libxeh (Milestone 7)

`libxeh` is the native client library for external XEH extension processes.
It links the shared protocol codec and Unix socket transport but exposes only
the versioned `xeh_*` API in `include/xeh.h`. It does not link to Xorg or
start threads.

Build with `meson setup build && meson compile -C build`. The build creates
`build/libxeh.so`; `meson install -C build` installs the library, headers,
and `libxeh.pc`. Include `<xeh.h>` with `pkg-config --cflags --libs libxeh`.

`xeh_connect()` establishes a nonblocking Unix connection and completes the
HELLO exchange within five seconds. Use `xeh_connect_with_timeout()` to
choose another positive timeout. A NULL path uses `XEH_SOCKET_PATH`.
Registration is asynchronous: `xeh_register_extension()` queues it, and
`xeh_dispatch()` delivers the registration callback and later requests.
The request payload is borrowed only during its callback; copy it to retain
it. A copied `xeh_request`'s scalar fields can be used for a deferred reply.

For an event loop, monitor `xeh_get_fd()` for `POLLIN`, plus `POLLOUT` while
`xeh_wants_write()` is true. Call `xeh_dispatch()` when ready and again if it
returns `XEH_MORE_WORK`. Reply and event functions queue and opportunistically
flush without blocking. Do not call `xeh_disconnect()` or recursively call
`xeh_dispatch()` inside a callback.

The Xorg host accepts one registration per connection. Its event authorizer
still denies events. With `-Dshm=true` (the default) and an explicit
`XEH_ENABLE_SHM=1` in the Xorg process environment, same-UID peers may request
`XEH_CAP_SHM`. Otherwise all capabilities remain denied.

For SHM, create a sealed memfd with `xeh_memfd_from_bytes()`, then call
`xeh_import_shm()` with an `xeh_shm_info`. The call queues one fd and the
32-byte control message; close your fd after it succeeds. Dispatch until its
callback receives the server-owned handle, then eventually call
`xeh_release_shm()`. Only one import and one release may be outstanding per
connection. Supported formats are XRGB8888, ARGB8888, and RGB565; imports are
read-only and require `F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW`. Framebuffer
bytes travel in the memfd, never in a protocol payload. `-Dshm=false` leaves
the API present but returns `XEH_ERR_STATE` or `ENOSYS` for SHM functions.
`xeh_import_pixels()` combines memfd creation and import for a caller-owned
raw pixel array. It is part of core `libxeh` and is suitable for C FFI from
Rust, Zig, Python, or other languages; no Xlib or DRI dependency is needed.

To make an imported XRGB8888 buffer usable by one X11 client, call
`xeh_bind_shm_client(extension, handle, request->client_id, callback, data)`
and wait for its callback before returning the handle to that client. A later
X11 `BLIT_BUFFER` request can copy it to a drawable the client may write.
Binding to another client fails; disconnect of the bound client destroys the
buffer. See [graphics details](../server/graphics/README.md).

`examples/hello-c` is a standalone C extension using this API and `poll()`.

## XImage adapter

`libxeh-ximage` is an optional, separate library for extensions that already
use libX11. Build it with `-Dximage=enabled -Dshm=true`, include
`<xeh_ximage.h>`, and link with `pkg-config --cflags --libs libxeh-ximage`.
The core `libxeh` and Xorg module do not link libX11.

`xeh_ximage_to_memfd(image, &info)` normalizes a ZPixmap `XImage` into a
sealed, little-endian XRGB8888 memfd; the caller closes the fd after queuing
`xeh_import_shm()`. `xeh_import_ximage(extension, image, callback, data)`
performs both steps and closes its fd, while the import result still arrives
asynchronously through the callback. The image can be destroyed after either
function returns because conversion is synchronous. The caller must provide
a valid `XImage` backing allocation for its declared dimensions and stride.

The adapter supports standard RGB masks on 24/32-bit ZPixmap images and
RGB565 masks on 16-bit ZPixmap images, in either XImage byte order. It rejects
palette, planar, unusual-mask, oversized, and malformed images. Conversion
is a CPU copy in the extension process; image bytes still travel to Xorg
through a sealed memfd, not the control socket.

## DRI adapters

Optional `libxeh-dri` uses XCB on the external process's X11 connection.
It probes DRI2 and DRI3 versions and provides checked DRI3 import of a
caller-owned dma-buf FD as an X11 pixmap. DRI2 is discovery-only because its
legacy buffer names are not a portable software-rendering transport. Build
with `-Ddri=enabled` and link with
`pkg-config --cflags --libs libxeh-dri`. See [DRI details](DRI.md).
