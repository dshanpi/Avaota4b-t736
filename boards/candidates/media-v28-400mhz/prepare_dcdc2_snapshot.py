#!/usr/bin/env python3
"""Prepare an isolated DCDC2 read-only sysfs patch; never edit baseline files."""
from pathlib import Path
import difflib
import hashlib
import json

candidate = Path(__file__).resolve().parent
root = candidate.parents[2]
source = root / 'boards/t736-gpu-supply.c'
assert hashlib.sha256(source.read_bytes()).hexdigest() == (
    '697544d5d9a46d6eadf0896bc7a71f634f5ec5124e5b9c4b8b2070c4c37dafda'
), 'PMIC baseline changed; review before regenerating this candidate'
before = source.read_text()
insert = (candidate / 'dcdc2-readonly.inc').read_text()

def change(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)

after = change(before, '#include <linux/t736-pmic.h>',
               '#include <linux/t736-pmic.h>\n#include <linux/sysfs.h>')
after = change(after, 'struct t736_gpu_supply {\n',
               'struct t736_gpu_supply {\n\tstruct device *dev;\n')
after = change(after, 'EXPORT_SYMBOL_GPL(t736_pmic_read_cpu_supplies);\n',
               'EXPORT_SYMBOL_GPL(t736_pmic_read_cpu_supplies);\n\n' + insert)
after = change(after, '\tmutex_init(&p->lock);',
               '\tp->dev = dev;\n\tmutex_init(&p->lock);')
after = change(after, '\t\t.suppress_bind_attrs = true },',
               '\t\t.suppress_bind_attrs = true, .dev_groups = t736_pmic_groups },')
out = candidate / 't736-gpu-supply.c'
out.write_text(after, newline='\n')
patch = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
    fromfile='a/drivers/regulator/t736-gpu-supply.c',
    tofile='b/drivers/regulator/t736-gpu-supply.c'))
patch_path = candidate / 'dcdc2-readonly-snapshot.patch'
patch_path.write_text(patch, newline='\n')
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
record = {
    'schema': 1, 'status': 'ISOLATED_SOURCE_CANDIDATE_REVIEW_PENDING',
    'board_access': False, 'boot_time_read_added': False,
    'kernel_path': 'drivers/regulator/t736-gpu-supply.c',
    'baseline_source': str(source.relative_to(root)).replace('\\', '/'),
    'baseline_sha256': sha(source), 'candidate_sha256': sha(out),
    'patch_sha256': sha(patch_path),
    'attribute': 'dcdc2_snapshot', 'mode': '0444',
    'transaction': 'Explicit sysfs read only; existing owner lock then bus mutex; reads 0x0e, 0x10, 0x13, 0x1b, 0x1d',
    'guards': ['Correct bound owner', 'PMIC ID 3', 'DCDC2 enabled',
               'Non-reserved 7-bit selector', 'DCDC2/3 multiphase disabled'],
    'preserved': ['CPU snapshot struct and exported API', 'All PMIC write functions and callers',
                  'Bus initialization policy', 'Accepted v27 source and output'],
    'scope': 'PMIC programmed setpoint readback, not ADC or physical rail measurement'
}
(candidate / 'dcdc2-readonly-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record))
