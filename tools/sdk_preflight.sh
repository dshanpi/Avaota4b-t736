#!/bin/sh
# Read-only host checks. Does not invoke build scripts or install dependencies.
set -u
sdk_path=${1:-/home/ubuntu/t736-aiot/tina}
printf 'SDK=%s\n' "$sdk_path"
uname -a
if [ ! -d "$sdk_path" ]; then printf 'SDK directory unavailable\n' >&2; exit 1; fi
df -h "$sdk_path"
df -i "$sdk_path"
for tool in git make gcc aarch64-linux-gnu-gcc dtc python3 bison flex; do
    if command -v "$tool" >/dev/null 2>&1; then command -v "$tool"; else printf 'MISSING: %s\n' "$tool"; fi
done
for rel in out/t736/demo_aiot-bak/debian/.sunxi.dts out/t736/demo_aiot-bak/debian/sunxi.dtb out/t736/demo_aiot-bak/pack_out/boot_package.fex out/t736/demo_aiot-bak/pack_out/fes1.fex; do
    target="$sdk_path/$rel"
    if [ -f "$target" ]; then ls -l "$target"; sha256sum "$target"; else printf 'MISSING: %s\n' "$target"; fi
done
printf 'Preflight finished; missing tools/files are not a successful build.\n'
