#!/usr/bin/env python3
"""Build and validate the source-only FEL SRAM probe; never access hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cross", default="arm-linux-gnueabi-")
    parser.add_argument("--host-cc", default="cc")
    args = parser.parse_args()
    mainline = Path(__file__).resolve().parents[1]
    source = mainline / "spl/early"
    out = mainline / "build/sram-probe"
    out.mkdir(parents=True, exist_ok=True)
    commands = []

    def run(argv):
        commands.append([str(x) for x in argv])
        proc = subprocess.run(commands[-1], check=False, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode:
            print(proc.stdout, end="")
            proc.check_returncode()
        return proc.stdout

    tests = out / "test_twi"
    run([args.host_cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         str(source / "test_twi.c"), str(source / "twi.c"), "-o", str(tests)])
    test_output = run([str(tests)])
    (out / "host-tests.txt").write_text(test_output)
    print(test_output, end="")
    timer_tests = out / "test_timer"
    run([args.host_cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         str(source / "test_timer.c"), str(source / "timer_init.c"), "-o", str(timer_tests)])
    timer_output = run([str(timer_tests)])
    (out / "timer-tests.txt").write_text(timer_output)
    print(timer_output, end="")
    flags = ["-mcpu=cortex-a55", "-marm", "-mfloat-abi=soft", "-Os", "-g",
             "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-pic", "-fstack-usage",
             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror"]
    objects = []
    for name in ["entry.S", "platform.c", "timer_init.c", "twi.c", "probe.c"]:
        obj = out / (Path(name).stem + ".o")
        run([args.cross + "gcc", *flags, "-c", str(source / name), "-o", str(obj)])
        objects.append(str(obj))
    elf = out / "t736-sram-probe.elf"
    raw = out / "t736-sram-probe.bin"
    run([args.cross + "gcc", *flags, "-nostdlib", "-no-pie", "-Wl,--gc-sections",
         "-Wl,--build-id=none", "-Wl,-T," + str(source / "probe.ld"),
         "-Wl,-Map," + str(out / "t736-sram-probe.map"), *objects, "-o", str(elf)])
    undefined = run([args.cross + "nm", "-u", str(elf)])
    if undefined.strip():
        raise RuntimeError("Undefined symbols: " + undefined)
    header = run([args.cross + "readelf", "-h", str(elf)])
    segments = run([args.cross + "readelf", "-lW", str(elf)])
    if not re.search(r"Entry point address:\s+0x48c00", header):
        raise RuntimeError("Unexpected entry point")
    loads = re.findall(r"LOAD\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)", segments)
    if not loads:
        raise RuntimeError("No loadable segments")
    for vaddr, paddr, filesz, memsz in loads:
        va, pa, fs, ms = map(lambda x: int(x, 16), (vaddr, paddr, filesz, memsz))
        if not (va == pa and 0x48c00 <= va and va + ms <= 0x50000 and fs <= ms):
            raise RuntimeError("Segment outside reserved SRAM probe window")
    run([args.cross + "objcopy", "-O", "binary", str(elf), str(raw)])
    if raw.stat().st_size > 0x4e000 - 0x48c00:
        raise RuntimeError("Raw image overlaps fixed report")
    (out / "readelf.txt").write_text(header + segments)
    (out / "disassembly.txt").write_text(run([args.cross + "objdump", "-d", str(elf)]))
    stack_entries = []
    for usage in sorted(out.glob("*.su")):
        for line in usage.read_text().splitlines():
            name, size, allocation = line.rsplit("\t", 2)
            if allocation != "static":
                raise RuntimeError("Unbounded stack use: " + line)
            stack_entries.append((name, int(size)))
    # These source files contain no recursion; summing every frame is deliberately
    # more conservative than a single call-chain calculation.
    stack_sum = sum(size for _, size in stack_entries)
    if not stack_entries or stack_sum >= 4096:
        raise RuntimeError("Stack budget exceeded")
    record = {
        "kind": "source-only-sram-probe-not-spl", "hardware_validated": False,
        "entry": "0x00048c00", "reserved_window": ["0x00048c00", "0x00050000"],
        "report": "0x0004e000", "stack": ["0x0004f000", "0x00050000"],
        "pmic_writes": 0, "ddr_initialized": False,
        "compiler": run([args.cross + "gcc", "--version"]).splitlines()[0],
        "commands": commands,
        "input_sha256": {str(p.relative_to(mainline)): hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(source.iterdir()) if p.is_file()},
        "output_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [elf, raw]},
        "binary_size": raw.stat().st_size,
        "stack_frame_sum_bytes": stack_sum,
        "builder_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    }
    (out / "build-record.json").write_text(json.dumps(record, indent=2) + "\n")
    print(f"Built {raw}: {raw.stat().st_size} bytes; entry 0x48c00; source-only probe, not full SPL")


if __name__ == "__main__":
    main()
