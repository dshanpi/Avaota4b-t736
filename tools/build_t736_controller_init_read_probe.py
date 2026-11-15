#!/usr/bin/env python3
"""Build the fixed SRAM controller initialization-CSR read-only diagnostic; no USB, serial or board access."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/controller-init-read-probe"
SOURCES = ["spl/early/entry.S", "spl/controller_init_read_probe/probe.c"]
HEADERS = ["spl/controller_init_read_probe/probe.ld", "spl/controller_init_read_probe/test_probe.c",
           "tools/build_t736_controller_init_read_probe.py"]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    commands = []
    def run(args):
        args = list(map(str, args)); commands.append(args)
        p = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if p.returncode:
            print(p.stdout); p.check_returncode()
        return p.stdout
    native = OUT / "test_probe"
    run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         ROOT/"spl/controller_init_read_probe/test_probe.c", "-o", native])
    native_report = OUT / "native-success-report.bin"
    native_output = run([native, native_report]); print(native_output, end="")
    flags = ["-mcpu=cortex-a55", "-marm", "-mfloat-abi=soft", "-mno-unaligned-access", "-O2", "-g", "-ffreestanding",
             "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-fstack-usage",
             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffunction-sections",
             "-fdata-sections", "-Wall", "-Wextra", "-Werror"]
    objects = []
    for name in SOURCES:
        obj = OUT / (Path(name).stem + ".o")
        run(["arm-linux-gnueabi-gcc", *flags, "-c", ROOT/name, "-o", obj]); objects.append(obj)
    elf, raw = OUT/"t736-controller-init-read-probe.elf", OUT/"t736-controller-init-read-probe.bin"
    run(["arm-linux-gnueabi-gcc", *flags, "-nostdlib", "-no-pie", "-Wl,--gc-sections",
         "-Wl,--build-id=none", "-Wl,-T,"+str(ROOT/"spl/controller_init_read_probe/probe.ld"),
         "-Wl,-Map,"+str(OUT/"t736-controller-init-read-probe.map"), *objects, "-o", elf])
    undefined = run(["arm-linux-gnueabi-nm", "-u", elf])
    if undefined.strip(): raise RuntimeError("Undefined symbols: " + undefined)
    header = run(["arm-linux-gnueabi-readelf", "-h", elf])
    attributes = run(["arm-linux-gnueabi-readelf", "-A", elf])
    if "Tag_CPU_unaligned_access: v6" in attributes:
        raise RuntimeError("ELF permits unaligned accesses")
    segments = run(["arm-linux-gnueabi-readelf", "-lW", elf])
    if not re.search(r"Entry point address:\s+0x48c00", header): raise RuntimeError("Wrong entry")
    loads = re.findall(r"LOAD\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", segments)
    if not loads: raise RuntimeError("No LOAD segment")
    for fields in loads:
        va, pa, fs, ms = [int(x, 16) for x in fields]
        if not (va == pa and 0x48c00 <= va and va+ms <= 0x50000 and fs <= ms):
            raise RuntimeError("Segment outside accepted SRAM window")
    run(["arm-linux-gnueabi-objcopy", "-O", "binary", elf, raw])
    if raw.stat().st_size > 0x5400: raise RuntimeError("Raw overlaps report")
    stack = 0
    for p in OUT.glob("*.su"):
        for line in p.read_text().splitlines():
            name, size, allocation = line.rsplit("\t", 2)
            if allocation != "static": raise RuntimeError("Nonstatic stack: " + line)
            stack += int(size)
    if not 0 < stack < 4096: raise RuntimeError("Stack bound exceeded")
    (OUT/"readelf.txt").write_text(header+segments+attributes)
    disassembly = run(["arm-linux-gnueabi-objdump", "-d", elf])
    (OUT/"disassembly.txt").write_text(disassembly)
    for line in disassembly.splitlines():
        match = re.search(r"\b(?:ldr|str)(?:eq|ne|gt|lt|ge|le|cc|cs|hi|ls)?\s+\w+,\s*\[\w+, #(-?\d+)\]", line)
        if match and int(match.group(1)) % 4:
            raise RuntimeError("Review unaligned word-offset instruction: " + line)
    record = {"kind":"source-only-controller-init-read-observation-not-DDR-init", "hardware_tested":False,
              "entry":"0x48c00", "report":"0x4e000", "report_bytes":1968,
              "window":["0x48c00","0x50000"], "binary_size":raw.stat().st_size,
              "stack_frame_sum_bytes":stack, "undefined_symbols":[], "mmio_writes_except_uart":False, "fixed_read_events":120, "observed_controller_words":40, "native_guard_drift_cases":80, "no_unaligned_access":True, "native_asan_ubsan_pass":True, "native_test_output":native_output, "commands":commands,
              "input_sha256":{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in SOURCES+HEADERS},
              "output_sha256":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [elf,raw,native_report]}}
    (OUT/"build-record.json").write_text(json.dumps(record,indent=2)+"\n")
    print(json.dumps({"bytes":record["binary_size"],"stack_sum":stack,"sha256":record["output_sha256"]},indent=2))


if __name__ == "__main__": main()
