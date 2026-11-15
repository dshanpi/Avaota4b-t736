#!/bin/sh
# Compile-only candidate: does not flash, initialize DDR, or claim board support.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
[ "$root" = /home/ubuntu/t736-mainline ]
commit=7d322909068fc4bb4ee35e9e43c78ed68d98619b
archive="$root/sources/audit/tf-a-inferno-$commit.tar.gz"
expected=fd1e430b4e6998a802577bf7d58f4b5721cb97ed03b408901767c244474c884a
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
work="$root/build/tfa-independent-candidate"
mkdir -p "$work"
src="$work/arm-trusted-firmware-$commit"
if [ ! -e "$src" ]; then
    tar -xf "$archive" -C "$work" --no-same-owner
fi
cd "$src"
patchfile="$root/patches/tf-a/0001-t736-bl31-dram-layout.patch"
if patch -p1 --dry-run --forward --fuzz=0 < "$patchfile" >/dev/null 2>&1; then
    patch -p1 --forward --fuzz=0 < "$patchfile"
else
    patch -p1 --dry-run --reverse --fuzz=0 < "$patchfile"
fi
make -j8 CROSS_COMPILE=aarch64-linux-gnu- PLAT=sun60i_a733 \
    DEBUG=1 LOG_LEVEL=40 PRELOADED_BL33_BASE=0x52000000 \
    BUILD_STRING=t736-independent-candidate-7d322909 bl31
bin="$src/build/sun60i_a733/debug/bl31.bin"
elf="$src/build/sun60i_a733/debug/bl31/bl31.elf"
test -s "$bin"
aarch64-linux-gnu-readelf -h -l "$elf"
aarch64-linux-gnu-nm -n "$elf" > "$work/bl31.symbols"
if grep -E 'sunxi_scpi|opteed|sp_min' "$work/bl31.symbols"; then
    printf 'Unexpected SCP/OP-TEE dependency in candidate\n' >&2
    exit 1
fi
mkdir -p "$root/output/independent-candidates"
cp "$bin" "$root/output/independent-candidates/t736-bl31.bin"
cp "$elf" "$root/output/independent-candidates/t736-bl31.elf"
sha256sum "$bin" "$elf"
printf 'TF_A_CANDIDATE_COMPILED_NOT_HARDWARE_VALIDATED\n'
