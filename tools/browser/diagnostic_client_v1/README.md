# Frozen constructor diagnostic client v1

This is the exact source snapshot used to build the standalone diagnostic
`libt736_cedar_client_diag.so` on 2026-11-17. It uses wire protocol 2 and keeps
all v2 account/path/peer, metadata/seal, readonly+DONTFORK and cleanup guards.
It is **diagnostic only**, not a default launcher or a namespace fix. No client
v3 implementation or completed v3 test suite was produced before closeout.

The bounded logs contain stage, return code, errno and selected numeric UID/PID
metadata. They do not dump environment values, full argv, media pixels or
mapping addresses. Extra diagnostic calls can affect sandbox behavior/timing;
do not use this artifact for performance measurements.

The original build remains in `../../../build/cedar-shm-client-diag-v1-20261117/`.
Its library is 24,448 bytes, SHA256
`ec2de17d17a229f631eb45b9d827467479d801f4452256e0e0773dd635b0b5ee`.
Real RDD logs identify the existing v2 rejection: account UID 999 versus
namespace-projected directory owner 65534, with UID map `1000 1000 1`.
The frozen v2 transport sources/package were not changed.

`provenance.json` records exact source hashes, original inputs and validation.
The two test source snapshots adjust include paths for this self-contained
archive and normalize line endings; test logic is unchanged. Native ASan/UBSan/LSan helper
58-case and actual-client fork/exec suites passed. Independent diff review found
no weakened guard. The tests do not establish namespace-adapted authentication.

## Reproduce without deployment

Run from this directory on the recorded build host. Use a new output directory;
never replace a completed build or installed library. The output binary may
have different bytes with another compiler/path, so the original hash above
identifies the delivered artifact, not a generic reproducibility guarantee.

```sh
sdk=/home/ubuntu/t736-aiot/tina/out/t736/demo_aiot/buildroot/buildroot/host
out=/home/ubuntu/t736-mainline/build/cedar-client-diagnostic-reproduction
mkdir "$out"
"$sdk/bin/aarch64-none-linux-gnu-gcc" -std=c11 -Wall -Wextra -Werror -pthread \
  -O2 -fPIC -shared cedar_client.c \
  -Wl,-z,relro,-z,now,-soname,libt736_cedar_client.so \
  -o "$out/libt736_cedar_client_diag.so"

for test in client_fork seals; do
  gcc -std=c11 -Wall -Wextra -Werror -pthread -O1 -g \
    -DT736_SETUP_TIMEOUT_MS=200 -fsanitize=address,undefined \
    -fno-omit-frame-pointer "test_cedar_shm_$test.c" -o "$out/test-$test"
  env -u LD_PRELOAD -u LD_LIBRARY_PATH -u T736_CEDAR_ENABLE \
    -u T736_CEDAR_TEST_CLIENT ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$out/test-$test"
done
```

No installer, service change or default-launcher selection is included. The
real board constructor log stays in the private build evidence directory.
