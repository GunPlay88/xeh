# DRI client adapters

`libxeh-dri` is optional and runs only in an external process. It never
changes XEH's socket protocol or links libDRM/XCB into the Xorg module.
Build with `-Ddri=enabled` and the `xcb-dri2` and `xcb-dri3` development
packages. Include `<xeh_dri.h>` and link using
`pkg-config --cflags --libs libxeh-dri`.

`xeh_dri_probe(xcb, &support)` asks the X server for DRI2 and DRI3 versions.
It may block on XCB replies. DRI2 is deliberately **discovery-only** here:
its legacy driver buffer names and device setup are not a generic path for
software-rendered pixels. Applications should use `xeh_import_pixels()` or
`xeh_import_shm()` for portable software rendering, even if DRI2 exists.

`xeh_dri3_import_dmabuf_pixmap()` validates one-plane XRGB8888 metadata,
duplicates the caller's dma-buf FD, and sends a checked DRI3
`PixmapFromBuffer` request. The caller retains its FD and owns the returned
X11 pixmap; free it with `xcb_free_pixmap()`. This is an X11 pixmap XID, **not**
an XEH object handle. A server may advertise DRI3 yet reject a particular
FD or layout; check the returned status and optional X11 error code. The
adapter does not allocate dma-bufs, select DRM devices, present frames, or
make arbitrary sealed memfds compatible with DRI3. DRI3 import uses XCB and
may block the external process while waiting for the checked result; it does
not block the Xorg event loop through XEH.

For a language binding, the portable path is the stable `libxeh` C ABI:
prepare packed pixels, call `xeh_import_pixels()`, wait for its callback,
bind the resulting handle to the requesting X11 client, then let that client
send `BLIT_BUFFER`. Only an application with a genuine dma-buf and an XCB
connection should choose the DRI3 adapter. These DRI adapters have offline
validation tests and compile checks, but no live-driver test in this VM.

The [XCB DRI2](https://xcb.freedesktop.org/manual/group__XCB__DRI2__API.html)
and [XCB DRI3](https://xcb.freedesktop.org/manual/group__XCB__DRI3__API.html)
API references describe the underlying requests. The inspected Xorg source
routes DRI3 `PixmapFromBuffer` to its screen driver hook.
