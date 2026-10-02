# XEH-HELLO (C)

Build with `meson setup build && meson compile -C build`. Run the resulting
`build/xeh-hello /absolute/path/to/xeh.sock`, or set `XEH_SOCKET_PATH` and
omit the argument. This example does not start Xorg. A host must already be
listening on the socket with the same effective UID.

The extension registers as `XEH-HELLO` version 1.0 with one request and one
event. Request 1 accepts 1-96 printable ASCII name bytes (trailing X11
alignment NULs are allowed) and replies with `Hello, NAME!`. It also sends
an event targeted to the originating client. The current Xorg host rejects
all external events; the error callback reports that permission denial.

The main loop uses `poll()`, `xeh_get_fd()`, `xeh_wants_write()`, and
`xeh_dispatch()`; it creates no threads. SIGINT/SIGTERM trigger unregister,
a bounded wait for the acknowledgement, then disconnect. Object creation
is not demonstrated because the host has not implemented those operations.
