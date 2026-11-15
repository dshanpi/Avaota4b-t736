"""Apply exported Ethernet patches to untouched upstream files and compare."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
assert root == Path('/home/ubuntu/t736-mainline')
upstream = root / 'sources/snapshots/linux/linux-6.18.63'
work = root / 'sources/work/linux'
patches = [root / 'userpatches/kernel/t736-mainline-6.18/0001-t736-minimal-board.patch']
patches += sorted((root / 'patches/linux').glob('000[2-9]-*.patch'))
assert len(patches) == 9
target = Path(tempfile.mkdtemp(prefix='ethernet-patch-check-', dir=root / 'build'))
paths = set()
for patch in patches:
    for name in re.findall(r'^--- a/([^\t\n]+)', patch.read_text(), re.M):
        paths.add(name)
        dest = target / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if (upstream / name).is_file():
            shutil.copy2(upstream / name, dest)
for patch in patches:
    subprocess.run(['patch', '--forward', '-p1', '-d', str(target)],
                   input=patch.read_bytes(), check=True)
for name in ['drivers/net/ethernet/stmicro/stmmac/dwmac-t736.c',
             'arch/arm64/boot/dts/allwinner/t736-mainline-bringup.dts',
             'drivers/pinctrl/sunxi/pinctrl-sunxi.c',
             'drivers/pinctrl/sunxi/pinctrl-sunxi.h',
             'drivers/pinctrl/sunxi/pinctrl-sunxi-dt.c']:
    assert (target / name).read_bytes() == (work / name).read_bytes(), name
print('NINE_FRESH_OFFICIAL_PATCHES_MATCH_WORK_TREE', target)
