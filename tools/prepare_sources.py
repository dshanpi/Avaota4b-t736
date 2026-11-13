#!/usr/bin/env python3
"""Prepare pinned upstream checkouts; never build, patch, or flash a device."""
import argparse
import json
import re
import subprocess
from pathlib import Path
from workspace_policy import outside_tina

ROOT = Path(__file__).resolve().parents[1]
ALLOW = {
    "u-boot": "https://github.com/u-boot/u-boot.git",
    "linux": "https://github.com/gregkh/linux.git",
    "armbian-build": "https://github.com/armbian/build.git",
}


def validate_lock(data):
    if data.get("schema_version") != 1:
        raise ValueError("unsupported lock schema")
    rows = data.get("components")
    if not isinstance(rows, list) or len(rows) != len(ALLOW):
        raise ValueError("expected exactly the three approved upstream components")
    seen = set()
    for row in rows:
        name = row.get("name")
        if name not in ALLOW or name in seen or row.get("url") != ALLOW[name]:
            raise ValueError("unapproved or duplicated source: " + str(name))
        seen.add(name)
        if not re.fullmatch(r"[0-9a-f]{40}", row.get("commit", "")):
            raise ValueError("a full immutable commit is required")
        ref = row.get("ref", "")
        if not (ref == row["commit"] or re.fullmatch(r"refs/tags/v[0-9]+(?:\.[0-9]+)+", ref)):
            raise ValueError("only a numeric release tag or the exact commit is allowed")
    return rows


def destination_for(parent, name):
    if name not in ALLOW:
        raise ValueError("unknown source directory")
    parent = outside_tina(parent)
    target = parent / name
    if target.is_symlink() or target.resolve().parent != parent:
        raise ValueError("source path escapes its destination")
    return target


def git(*args, cwd=None, capture=False):
    # No shell interpolation, recursive submodules, or remote helper protocols.
    result = subprocess.run(
        ["git", "-c", "protocol.file.allow=never", "-c", "protocol.ext.allow=never", *args],
        cwd=cwd, check=True, text=True, encoding="utf-8", capture_output=capture,
    )
    return result.stdout.strip() if capture else None


def prepare(row, parent):
    target = destination_for(parent, row["name"])
    resumed = False
    if target.exists():
        if not (target / ".git").is_dir():
            raise ValueError(f"existing destination is not a standalone checkout: {target}")
        if git("remote", "get-url", "origin", cwd=target, capture=True) != row["url"]:
            raise ValueError(f"existing checkout origin differs: {target}")
        if git("status", "--porcelain", "--untracked-files=all", cwd=target, capture=True):
            raise ValueError(f"existing checkout has local changes: {target}")
        try:
            head = git("rev-parse", "--verify", "HEAD", cwd=target, capture=True)
        except subprocess.CalledProcessError:
            # Resume an interrupted initial fetch only in an empty worktree.
            # Ignored files must also prevent an unexpected checkout overwrite.
            if any(p.name != ".git" for p in target.iterdir()):
                raise ValueError(f"unborn repository has existing files: {target}")
            resumed = True
        else:
            if head != row["commit"]:
                raise ValueError(f"existing checkout differs from pinned HEAD: {target}")
            return {"component": row["name"], "state": "already_matches", "commit": head}
    else:
        target.mkdir(parents=True)
        # A failed fetch leaves its directory for inspection. No reset/clean/delete.
        git("init", str(target))
        git("remote", "add", "origin", row["url"], cwd=target)
    git("fetch", "--depth=1", "--no-recurse-submodules", "origin", row["ref"], cwd=target)
    actual = git("rev-parse", "FETCH_HEAD^{commit}", cwd=target, capture=True)
    if actual != row["commit"]:
        raise ValueError(f"ref changed: expected {row['commit']}, got {actual}")
    git("-c", "core.autocrlf=false", "checkout", "--detach", row["commit"], cwd=target)
    if git("rev-parse", "HEAD", cwd=target, capture=True) != row["commit"]:
        raise ValueError("checkout did not produce pinned HEAD")
    if git("status", "--porcelain", "--untracked-files=all", cwd=target, capture=True):
        raise ValueError(f"checkout is not clean: {target}; use a case-sensitive Linux filesystem. The fetched Git objects are retained; no reset or cleanup was performed.")
    return {"component": row["name"], "state": "checked_out", "commit": actual, "resumed_initial_fetch": resumed}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true")
    group.add_argument("--fetch", action="store_true")
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--component", choices=sorted(ALLOW), help="Prepare only one locked component")
    parser.add_argument("--http1", action="store_true", help="Use HTTP/1.1 for this process; keep TLS verification enabled")
    parser.add_argument("--lock", type=Path, default=ROOT / "config/sources.lock.json")
    args = parser.parse_args()
    try:
        rows = validate_lock(json.loads(args.lock.read_text(encoding="utf-8-sig")))
        if args.component:
            rows = [r for r in rows if r["name"] == args.component]
        if args.http1:
            # Per-process Git setting; does not change user/global configuration.
            import os
            count = int(os.environ.get("GIT_CONFIG_COUNT", "0"))
            os.environ[f"GIT_CONFIG_KEY_{count}"] = "http.version"
            os.environ[f"GIT_CONFIG_VALUE_{count}"] = "HTTP/1.1"
            os.environ["GIT_CONFIG_COUNT"] = str(count + 1)
        if args.fetch and args.destination is None:
            parser.error("--fetch requires --destination")
        if args.check:
            print(json.dumps({"state": "lock_valid", "downloaded": False, "sources": rows}, indent=2))
        else:
            for row in rows:
                print(json.dumps(prepare(row, args.destination)), flush=True)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"prepare_sources: {error}\n")


if __name__ == "__main__":
    main()
