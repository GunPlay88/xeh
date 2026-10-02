# Xorg adapter (Milestone 6)

`xeh_xorg.c` is a loadable Xorg extension module for the Xorg 21.1 extension
ABI. It uses `LoadExtensionList`/`AddExtension`, `SetNotifyFd`,
`ClientStateCallback`, and `TimerSet` from the inspected Xorg 21.1.12 source.
No XEH thread is created. The server owns all registry, handle, and pending
request state; peers receive only protocol values, never Xorg pointers.

Build with an installed Xorg SDK (`xserver-xorg-dev` on Ubuntu) and pixman:

```sh
meson setup build-xorg -Dxorg=true
meson compile -C build-xorg
meson test -C build-xorg --print-errorlogs
```

If the SDK headers are not installed globally, pass
`-Dxorg_sdk_include=/path/to/usr/include/xorg`. The Xorg module is
`build-xorg/server/xorg/libxeh.so`; the separate client library is
`build-xorg/libxeh.so`.
Build against the SDK for the running server package; `xorg-server.h` must
precede other Xorg/X11 headers because it sets server-side X11 type widths.

Set `XEH_SOCKET_PATH` to an absolute pathname before starting Xorg. The
module is inactive when this variable is absent. Add
`build-xorg/server/xorg` to
Xorg's `ModulePath` and `Load "xeh"` in the Xorg module section. The socket
is mode 0600 and Linux `SO_PEERCRED` restricts peers to the Xorg process's
effective UID. By default all requested capabilities are granted as zero.
With `-Dshm=true` and `XEH_ENABLE_SHM=1` set in the Xorg process environment,
the host may grant only `XEH_CAP_SHM` to same-UID peers that request it. This
is intentionally restrictive: a root-run Xorg cannot host a normal user's
extension under this policy. Use a rootless server for this prototype.

Each connection must send `HELLO`, then may register one extension. It may
send `PING`, `REPLY`, `ERROR`, and `EVENT`, though event authorization is
currently denied. On disconnect, pending requests fail, owned objects are
destroyed, and registry entries are removed. A periodic Xorg timer expires
pending requests after 30 seconds.

The X11 extension is named `XEH`. Its minor opcodes and request layouts are
in `include/xehx11.h`. `QUERY_VERSION` and `LOOKUP` reply synchronously.
`FORWARD` pauses only its originating X11 client until a reply, error,
disconnect, or timeout, preserving that client's reply order. The reply
payload is opaque; `data00` is an `xeh_protocol_error` status. X11 multibyte
request fields follow the X11 client's byte order. IPC fields use the XEH
protocol's big-endian encoding.

The optional `xorg-smoke-test` target (built when XCB development headers
are available) runs against an already-started Xorg display. Set `DISPLAY`
and `XEH_SOCKET_PATH`, then run `build-xorg/xorg-smoke-test`. It tests HELLO,
registration, X11 lookup, forwarding, reply, peer disconnect, outstanding
request failure, and registry cleanup. A headless dummy-driver Xorg passed
this test during Milestone 6.

SHM imports carry one sealed memfd via `SCM_RIGHTS` and 32-byte control
metadata. The host verifies seals, exact file size, dimensions, stride,
format, ownership, and a 256 MiB aggregate quota. Imports create generation-
aware buffer handles; release or disconnect destroys them. The host maps
buffers read-only. No graphics operation consumes them yet.

Current limits: no X11 event emission, general object creation commands,
dma-buf import, or authentication for peers with a different UID. The X11
request format currently supports ordinary request lengths, not the
BIG-REQUESTS extended-length form.
