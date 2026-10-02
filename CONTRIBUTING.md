# Contributing

XEH crosses an untrusted IPC boundary inside Xorg. Keep changes narrowly
scoped, maintain the server/protocol/libxeh separation, and never put Xorg
pointers or native C structure layouts on the wire. New protocol fields need
explicit endian-safe codecs, length checks, ownership rules, and tests.

Before proposing a change, run:

```sh
meson setup build -Dtests=true -Dpython=true
meson compile -C build
meson test -C build --print-errorlogs
```

For changes to decoders, fd passing, or lifetime management, also test with
`-Db_sanitize=address,undefined` and review the
[fuzzing guide](fuzz/README.md). Xorg module builds require an SDK matching
the intended server version. A module build is not evidence of runtime
compatibility; report clearly whether a live Xorg test was performed.

Do not commit `build*/`, Python caches, downloaded Xorg source, package
archives, or fuzzer crash artifacts. The project uses the [MIT license](LICENSE).
