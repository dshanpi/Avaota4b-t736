#!/usr/bin/env python3
"""Record scoped support searches against the locked upstream Git commits.

Search absence is evidence about the listed paths and identifiers, not a proof
that generic drivers cannot be adapted. This tool never applies vendor patches.
"""
import argparse
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path

from prepare_sources import validate_lock
from workspace_policy import outside_tina

ROOT = Path(__file__).resolve().parents[1]
SOC = r"a733|sun60i|(^|[^a-z0-9])t736([^a-z0-9]|$)"
QUERIES = {
    "u-boot": [
        ("soc_boot_support", SOC, ["arch/arm/mach-sunxi", "arch/arm/include/asm/arch-sunxi", "configs"]),
        ("soc_clock_pinctrl_mmc", SOC, ["drivers/clk/sunxi", "drivers/pinctrl/sunxi", "drivers/mmc"]),
        ("soc_dts", SOC, ["arch/arm/dts", "dts/upstream/src/arm64/allwinner"]),
        ("packed_pmic_model", "axp8191", ["drivers/power", "include"]),
    ],
    "linux": [
        ("soc_dts_and_board_binding", SOC, ["arch/arm64/boot/dts/allwinner", "Documentation/devicetree/bindings/arm/sunxi.yaml"]),
        ("soc_clocks", SOC, ["drivers/clk/sunxi-ng", "include/dt-bindings/clock", "include/dt-bindings/reset"]),
        ("soc_pinctrl", SOC, ["drivers/pinctrl/sunxi"]),
        ("soc_mmc_and_packed_compatibles", SOC + "|sunxi-mmc-v4p6x|sunxi-mmc-v5p6x", ["drivers/mmc/host/sunxi-mmc.c", "Documentation/devicetree/bindings/mmc"]),
        ("packed_pmic_model", "axp8191", ["drivers/mfd", "drivers/regulator", "include/linux/mfd", "Documentation/devicetree/bindings/mfd"]),
    ],
    "armbian-build": [
        ("board_and_family_declarations", SOC, ["config/boards", "config/sources/families"]),
    ],
}


def run(repo, *args, allowed=(0,)):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode not in allowed:
        raise ValueError(result.stderr.strip() or "git operation failed")
    return result.stdout.strip()


def audit(sources, lock):
    result = {"recorded_utc": datetime.now(timezone.utc).isoformat(), "scope": "identifier_search_in_explicit_paths_at_locked_commits", "components": []}
    for component in validate_lock(lock):
        name, commit = component["name"], component["commit"]
        repo = sources / name
        if run(repo, "remote", "get-url", "origin") != component["url"]:
            raise ValueError("unexpected origin: " + name)
        if run(repo, "rev-parse", "HEAD") != commit:
            raise ValueError("unexpected HEAD: " + name)
        dirty = run(repo, "status", "--porcelain", "--untracked-files=all")
        # Queries below explicitly name the immutable commit, never the worktree.
        # Keep dirty/materialization failures visible without substituting them
        # for the object-database contents being audited.
        row = {"name": name, "origin": component["url"], "commit": commit, "checkout_clean": not bool(dirty), "worktree_status": dirty.splitlines(), "queries": []}
        for identifier, pattern, paths in QUERIES[name]:
            hits = run(repo, "grep", "-n", "-i", "-E", pattern, commit, "--", *paths, allowed=(0, 1))
            row["queries"].append({"id": identifier, "pattern": pattern, "paths": paths, "matches": hits.splitlines() if hits else []})
        result["components"].append(row)
    result["limitations"] = [
        "No hardware execution or compiler test was performed by this audit.",
        "An absent match means only that the selected identifiers are absent from the selected paths.",
        "The packed DTB reports AXP8191; the physical PMIC and live configuration still need confirmation.",
        "No Armbian board configuration or vendor patch series has been selected or executed.",
    ]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, default=ROOT / "sources")
    parser.add_argument("--output", type=Path, default=ROOT / "evidence/upstream-support-audit.json")
    args = parser.parse_args()
    try:
        data = audit(args.sources, json.loads((ROOT / "config/sources.lock.json").read_text(encoding="utf-8")))
        output = outside_tina(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"output": str(output), "results": [{"component": r["name"], "queries": [{"id": q["id"], "match_count": len(q["matches"])} for q in r["queries"]]} for r in data["components"]]}, indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"audit_upstream: {error}\n")


if __name__ == "__main__":
    main()
