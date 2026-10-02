# Protocol fuzzing

`protocol_fuzz.c` exports `LLVMFuzzerTestOneInput` and calls the standalone
protocol decoders and SHM metadata validator. It does not start Xorg, open a
socket, map memory, or trust decoded lengths as allocations.

The regular test suite builds `fuzz-smoke-test`, which runs a valid SHM frame,
all its truncations, and 4096 deterministic mutations. For longer coverage,
use Clang with libFuzzer:

```sh
CC=clang meson setup build-fuzz -Dfuzz=true -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-fuzz
mkdir -p /tmp/xeh-fuzz-corpus
printf '5845483100010000040000020000000100000007000000200000000200000002000000080000000100000000000000000000000000000010' | xxd -r -p > /tmp/xeh-fuzz-corpus/shm-import
build-fuzz/xeh-protocol-fuzz /tmp/xeh-fuzz-corpus
```

Meson rejects `-Dfuzz=true` when the compiler does not support
`-fsanitize=fuzzer`. The corpus is intentionally outside the source tree;
preserve useful crash inputs and add regression tests for them.
