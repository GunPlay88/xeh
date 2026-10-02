# XEH: X Extension Host

XEH is a prototype broker for running X server extensions in separate
processes. A small Xorg module retains privileged X server operations while
external C, Python, or other runtimes communicate over a Unix socket. Python
is never embedded into Xorg, and XEH adds no Xorg worker thread.

```text
X11 client --X11 protocol--> Xorg + XEH module
                                      |
                              nonblocking AF_UNIX socket
                                      |
                              libxeh / Python runtime
                                      |
                              extension process
```

The socket is the **control plane**. Large graphics data belongs in a sealed
memfd passed with `SCM_RIGHTS`, not in a protocol message. The protocol uses
explicit big-endian encoding, a 24-byte header, version negotiation, bounded
frames, and opaque handles; no Xorg pointer crosses IPC.

## Status and boundaries

- HELLO, registration, request/reply routing, errors, pending-request timeout,
  disconnect cleanup, and generation-aware object ownership are implemented.
- `libxeh` and the Python runtime support external, single-threaded extensions.
- Optional `libxeh-ximage` converts supported client-side libX11 `XImage`
  objects to sealed memfds for the same SHM import path. Xorg never receives
  an `XImage *` or links libX11 for this adapter.
- `xeh_import_pixels()` and Python `Connection.import_pixels()` accept raw
  bytes for language-neutral software rendering. Optional `libxeh-dri`
  probes DRI2/DRI3 and can import a real dma-buf through DRI3 on a capable
  server; DRI2 is discovery-only.
- Linux sealed-memfd import/release creates read-only, server-owned buffer
  handles. An extension can bind a buffer to one X11 client, which can copy
  a bounded XRGB8888 rectangle to an authorized drawable via `BLIT_BUFFER`.
- Event messages are parsed and validated, but the Xorg adapter currently
  denies event emission. General object-creation commands and dma-buf import
  through XEH IPC are not implemented.
- This is a research prototype, **not a security-reviewed Xorg deployment**.
  Current Xorg builds compile, but live server behavior has not been verified
  in this virtual environment.

The graphics path currently supports only little-endian XRGB8888 on a
depth-24, 32-bpp screen with standard RGB masks. The X11 client must have
write access to the drawable and use access to a matching GC. A blit stages
at most 4 MiB per request, so it is a simple CPU path, not zero-copy or
GPU presentation. See [graphics details](server/graphics/README.md) and
the [DRI adapter notes](libxeh/DRI.md).

The Xorg socket is mode 0600 and peers must have the server's effective UID,
checked with `SO_PEERCRED`. Connecting does not grant capabilities. The host
grants none by default; `XEH_CAP_SHM` additionally requires a build with SHM
enabled and `XEH_ENABLE_SHM=1` in the Xorg process environment. Imported
buffers require write, grow, and shrink seals, exact file-size agreement,
validated geometry, and an aggregate quota. On extension disconnect, the host
fails pending requests and destroys its resources before removing registration.

## Build and test

Requirements: Meson, Ninja, a C11 compiler, and Linux for the socket and
memfd paths. Python 3 is needed only for `-Dpython=true`. Xorg headers and
pixman are needed only for `-Dxorg=true`. libX11 development files are needed
for `-Dximage=enabled`; the adapter is skipped when unavailable in auto mode.
`-Ddri=enabled` requires XCB DRI2 and DRI3 development packages; this
adapter is also skipped when unavailable in auto mode.

```sh
meson setup build -Dtests=true -Dpython=true
meson compile -C build
meson test -C build --print-errorlogs
```

For a module build, use `meson setup build-xorg -Dxorg=true` and, when SDK
headers are not installed globally, pass
`-Dxorg_sdk_include=/path/to/usr/include/xorg`. Build against an SDK matching
the target server package before deployment. Do not treat a successful module
build as a live-server test. `-Dshm=false` disables SHM while keeping the
public API stable. ASan/UBSan can be enabled with
`-Db_sanitize=address,undefined`.

## Run an extension

An Xorg instance must be configured to load the module and set an absolute
`XEH_SOCKET_PATH`; see [Xorg adapter notes](server/xorg/README.md). In an
environment with that socket available:

```sh
build/xeh-hello "$XEH_SOCKET_PATH"
PYTHONPATH=python XEH_LIBXEH_PATH="$PWD/build/libxeh.so" \
  python3 python/examples/hello_extension.py "$XEH_SOCKET_PATH"
```

The C example demonstrates registration, request handling, reply, event
attempt, and graceful unregister. The Python example uses `@xeh.request(1)`
and returns JSON-encoded responses. See [libxeh](libxeh/README.md),
[Python runtime](python/README.md), and the [protocol definitions](include/xehproto.h)
for their contracts. X11 client request layouts are in
[xehx11.h](include/xehx11.h). The [fuzzing guide](fuzz/README.md) covers the
standalone protocol fuzz target.

## Source layout

- `include/`: IPC and X11-facing protocol definitions.
- `server/`: protocol codec, nonblocking IPC, registry, dispatch, objects,
  SHM and graphics validation, and the Xorg adapter.
- `libxeh/`: installable native client library.
- `python/`: `ctypes` binding and external Python runtime.
- `examples/`, `tests/`, `fuzz/`: extension samples and verification.
- `external/`: local Xorg source reference metadata. The downloaded upstream
  tree and archive are intentionally not part of this repository.

Licensed under [MIT](LICENSE).
