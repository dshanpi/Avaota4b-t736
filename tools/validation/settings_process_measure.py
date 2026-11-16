#!/usr/bin/env python3
"""Measure Settings launch or explicit same-process panel activation.

Uses the adjacent x11_window_wait.py and read-only /proc observations. Launch
refuses an existing Settings process. --reuse-pid explicitly activates a panel
in that process; no app is killed. No system settings, affinities, caches or
display modes are changed by this wrapper. Settings itself remembers its panel.
Mapped and CLI dispatch are NOT first-paint or completed-panel latency metrics.
"""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


ENV_KEYS = ("DISPLAY", "GDK_BACKEND", "GDK_DISABLE", "GSK_RENDERER",
            "LD_LIBRARY_PATH", "LIBGL_DRIVERS_PATH", "MESA_LOADER_DRIVER_OVERRIDE",
            "GTK_MEDIA", "GST_GL_API", "GST_GL_PLATFORM", "GST_GL_WINDOW",
            "XDG_CACHE_HOME")


def read(path):
    try:
        return Path(path).read_text()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None


def stat_record(path):
    raw = read(path)
    if raw is None:
        return None
    close = raw.rfind(")")
    fields = raw[close + 2:].split()
    return {"comm": raw[raw.index("(") + 1:close], "state": fields[0],
            "minor_faults": int(fields[7]), "major_faults": int(fields[9]),
            "user_ticks": int(fields[11]), "system_ticks": int(fields[12]),
            "threads": int(fields[17]), "start_ticks": int(fields[19]),
            "last_cpu": int(fields[36]), "blkio_delay_ticks": int(fields[39])}


def existing_settings():
    found = []
    for path in Path("/proc").iterdir():
        if not path.name.isdigit():
            continue
        try:
            if path.stat().st_uid == os.getuid() and Path(os.readlink(path / "exe")).name == "gnome-control-center":
                found.append(int(path.name))
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
    return sorted(found)


def validate_target(pid, expected_start=None):
    directory = Path("/proc") / str(pid)
    if directory.stat().st_uid != os.getuid() or Path(os.readlink(directory / "exe")).name != "gnome-control-center":
        raise RuntimeError("Target UID/executable is not the selected desktop Settings")
    value = stat_record(directory / "stat")
    if value is None or (expected_start is not None and value["start_ticks"] != expected_start):
        raise RuntimeError("Selected Settings process identity changed")
    return value["start_ticks"]


def snapshot(pid, origin):
    directory = Path("/proc") / str(pid)
    process = stat_record(directory / "stat")
    if process is None:
        return {"elapsed_seconds": time.monotonic() - origin, "exited": True}
    io = {}
    for line in (read(directory / "io") or "").splitlines():
        key, value = line.split(":", 1)
        io[key] = int(value)
    tasks = {}
    try:
        for task in (directory / "task").iterdir():
            value = stat_record(task / "stat")
            if value is not None:
                tasks[task.name] = value
    except (FileNotFoundError, ProcessLookupError):
        pass
    wchan = (read(directory / "wchan") or "").strip()
    ending = stat_record(directory / "stat")
    if ending is None or ending["start_ticks"] != process["start_ticks"]:
        return {"elapsed_seconds": time.monotonic() - origin,
                "identity_invalid": True, "reason": "Target exited or PID was reused during /proc snapshot"}
    return {"elapsed_seconds": time.monotonic() - origin, "process": process,
            "io": io, "main_thread_wchan": wchan,
            "tasks": tasks}


def libraries(pid):
    return sorted({line.split()[-1] for line in (read(f"/proc/{pid}/maps") or "").splitlines()
                   if "/" in line and any(token in line for token in
                   ("libgtk-4", "libadwaita", "libEGL", "libGL.", "libGLES", "libpvr",
                    "libgst", "libavcodec", "libcairo", "swrast", "libLLVM"))})


def target_environment(pid):
    try:
        entries = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
        values = dict(entry.split(b"=", 1) for entry in entries if b"=" in entry)
        return {key: values[key.encode()].decode(errors="replace")
                for key in ENV_KEYS if key.encode() in values}
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None


def summarize(samples, pid, baseline):
    valid = [s for s in samples if "process" in s]
    if not valid:
        return {"error": "No target process snapshot"}
    hz = os.sysconf("SC_CLK_TCK")
    final = valid[-1]
    initial = baseline or {"process": {}, "io": {}}
    names = ("user_ticks", "system_ticks", "minor_faults", "major_faults", "blkio_delay_ticks")
    delta = {key: final["process"][key] - initial["process"].get(key, 0) for key in names}
    io_delta = {key: value - initial["io"].get(key, 0) for key, value in final["io"].items()}
    # Last-seen counters give a lower bound when threads start/exit between polls.
    seen = {}
    for item in valid:
        for tid, value in item["tasks"].items():
            seen[(tid, value["start_ticks"])] = value
    top = []
    for (tid, start_ticks), value in seen.items():
        before = (baseline or {}).get("tasks", {}).get(tid, {})
        if before.get("start_ticks") != start_ticks:
            before = {}
        ticks = value["user_ticks"] + value["system_ticks"] - before.get("user_ticks", 0) - before.get("system_ticks", 0)
        top.append({"tid": int(tid), "comm": value["comm"], "cpu_seconds_lower_bound": ticks / hz,
                    "last_sampled_cpu": value["last_cpu"]})
    main_samples = [s["tasks"][str(pid)] for s in valid if str(pid) in s["tasks"]]
    return {"clock_ticks_per_second": hz, "process_counter_delta": delta,
            "process_cpu_seconds": (delta["user_ticks"] + delta["system_ticks"]) / hz,
            "proc_io_delta": io_delta,
            "main_thread_last_cpu_sample_counts": dict(Counter(str(s["last_cpu"]) for s in main_samples)),
            "main_thread_state_samples": dict(Counter(s["state"] for s in main_samples)),
            "largest_thread_cpu_lower_bounds": sorted(top, key=lambda row: -row["cpu_seconds_lower_bound"])[:16],
            "note": "last_cpu is a polling sample, not measured CPU residency; exited-thread accounting can be incomplete; CPU counters are tick-quantized and can miss short activity; process CPU excludes MMC kernel workers"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--panel", choices=("multitasking", "mouse", "background"), required=True)
    parser.add_argument("--output", type=Path, required=True, help="New JSON file; adjacent .app.log/.mapped.json must not exist")
    parser.add_argument("--reuse-pid", type=int, help="Explicit same-process panel activation; no map latency claimed")
    parser.add_argument("--media", choices=("inherit", "none"), default="inherit")
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--interval", type=float, default=0.5)
    args = parser.parse_args()
    if not 5 <= args.seconds <= 45 or not 0.25 <= args.interval <= 2:
        parser.error("seconds must be 5..45 and interval 0.25..2")
    if args.reuse_pid is not None and args.reuse_pid <= 0:
        parser.error("--reuse-pid must be positive")
    if os.getuid() == 0 or not all(os.environ.get(key) for key in ("DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "LIBGL_DRIVERS_PATH")):
        parser.error("Run as the desktop user with its full X11/D-Bus/GPU environment, including LIBGL_DRIVERS_PATH")
    if args.reuse_pid and args.media != "inherit":
        parser.error("A forwarded panel request cannot change the existing process media environment")
    helper = Path(__file__).with_name("x11_window_wait.py")
    if not helper.is_file():
        parser.error("Adjacent x11_window_wait.py is required")
    existing = existing_settings()
    if (not args.reuse_pid and existing) or (args.reuse_pid and existing != [args.reuse_pid]):
        parser.error(f"Existing Settings PIDs={existing}; launch requires none, --reuse-pid requires exactly the selected PID")
    app_path = args.output.with_suffix(args.output.suffix + ".app.log")
    mapped_path = args.output.with_suffix(args.output.suffix + ".mapped.json")
    for path in (args.output, app_path, mapped_path):
        if path.exists():
            parser.error(f"Refusing existing result: {path}")
    env = os.environ.copy()
    if args.media == "none":
        env["GTK_MEDIA"] = "none"
    result = {"schema": "t736-settings-process-v1", "status": "RUNNING",
              "kernel": os.uname().release, "boot_id": (read("/proc/sys/kernel/random/boot_id") or "").strip(),
              "mode": "panel_activation" if args.reuse_pid else "new_process_launch",
              "panel": args.panel, "environment": {key: env.get(key) for key in ENV_KEYS},
              "script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "mapped_helper_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
              "first_paint_verified": False, "panel_complete_verified": False,
              "physical_display_verified": False, "app_auto_terminated": False,
              "poll_interval_seconds": args.interval, "window_seconds": args.seconds,
              "samples": [], "app_log": str(app_path), "mapped_log": str(mapped_path)}
    observer = None
    with args.output.open("x", encoding="utf-8") as output, app_path.open("x", encoding="utf-8") as app_log, mapped_path.open("x", encoding="utf-8") as mapped_log:
        wrapper_cpu = time.process_time()
        try:
            selected_start = validate_target(args.reuse_pid) if args.reuse_pid else None
            baseline = snapshot(args.reuse_pid, time.monotonic()) if args.reuse_pid else None
            if baseline and "process" not in baseline:
                raise RuntimeError("Selected Settings process exited before activation")
            if baseline:
                validate_target(args.reuse_pid, selected_start)
                if baseline["process"]["start_ticks"] != selected_start:
                    raise RuntimeError("Target changed during baseline collection")
            origin = time.monotonic()
            request = subprocess.Popen(["gnome-control-center", args.panel], env=env,
                                       stdout=app_log, stderr=subprocess.STDOUT)
            pid = args.reuse_pid or request.pid
            result.update(pid=pid, request_pid=request.pid, start_monotonic=origin)
            if not args.reuse_pid:
                observer = subprocess.Popen([sys.executable, str(helper), "--pid", str(pid),
                    "--timeout", str(min(30, args.seconds)), "--interval", "0.05",
                    "--start-monotonic", str(origin)], env=env, stdout=mapped_log, stderr=app_log)
            else:
                result["mapped"] = {"status": "not_measured_for_reused_window",
                                     "reason": "Existing-window mapping cannot measure panel completion"}
            identity = validate_target(pid, selected_start)
            deadline = origin + args.seconds
            while True:
                item = snapshot(pid, origin)
                result["samples"].append(item)
                if "process" not in item:
                    raise RuntimeError("Target Settings exited during observation; possible GApplication forwarding")
                if identity is None:
                    identity = item["process"]["start_ticks"]
                if item["process"]["start_ticks"] != identity:
                    raise RuntimeError("Target PID identity changed")
                if args.reuse_pid and "dispatch" not in result and request.poll() is not None:
                    result["dispatch"] = {"exit_code": request.returncode,
                                          "observed_exit_seconds": time.monotonic() - origin,
                                          "observation_resolution_seconds": args.interval,
                                          "metric": "CLI request return, not first paint or completed panel"}
                if time.monotonic() >= deadline:
                    break
                time.sleep(min(args.interval, max(0, deadline - time.monotonic())))
            validate_target(pid, identity)
            result["libraries"] = libraries(pid)
            result["target_environment"] = target_environment(pid)
            validate_target(pid, identity)
            result["request_running_at_end"] = request.poll() is None
            result["summary"] = summarize(result["samples"], pid, baseline)
            if observer:
                try:
                    observer.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    observer.terminate()  # Only our read-only observer, never Settings.
                    observer.wait(timeout=2)
                mapped_log.flush()
                text = mapped_path.read_text().strip()
                result["mapped"] = json.loads(text) if text else {"status": "observer_timeout"}
                result["status"] = "COMPLETE" if result["mapped"].get("status") == "mapped" else "UNUSABLE"
            else:
                result["status"] = "COMPLETE" if result.get("dispatch", {}).get("exit_code") == 0 else "UNUSABLE"
        except (Exception, KeyboardInterrupt) as error:
            result["status"] = "ERROR"
            result["error"] = repr(error)
        finally:
            if observer and observer.poll() is None:
                observer.terminate()
                observer.wait(timeout=2)
            result["observer_process_cpu_seconds"] = time.process_time() - wrapper_cpu
            result["observer_cpu_scope"] = "Wrapper only; excludes the separate X11 observer process"
            output.write(json.dumps(result, indent=2) + "\n")
            output.flush()
    print(json.dumps({key: result.get(key) for key in ("status", "mode", "panel", "pid", "mapped", "dispatch", "summary", "error")}))
    return 0 if result["status"] == "COMPLETE" else 1


if __name__ == "__main__":
    sys.exit(main())
