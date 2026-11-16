#!/usr/bin/env python3
"""Inspect fresh visible Firefox instances via loopback-only Marionette.

Run as the desktop user. Existing Firefox processes/profiles are never changed.
No sandbox or graphics-blocklist override is applied. Each test owns and closes
its separate process group, profile and loopback listener.
System access is explicitly enabled only for these diagnostic instances so
current Firefox permits the read-only about:support page through Marionette.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import tempfile
import time


class Marionette:
    def __init__(self, connection, timeout):
        self.connection = connection
        self.connection.settimeout(timeout)
        self.sequence = 0
        self.hello = self.receive()
        if self.hello.get("marionetteProtocol") != 3:
            raise RuntimeError(f"Unsupported protocol: {self.hello}")

    def exact(self, count):
        result = b""
        while len(result) < count:
            block = self.connection.recv(count - len(result))
            if not block:
                raise EOFError("Marionette connection closed")
            result += block
        return result

    def receive(self):
        prefix = b""
        while True:
            byte = self.exact(1)
            if byte == b":":
                break
            prefix += byte
            if len(prefix) > 10 or not prefix.isdigit():
                raise ValueError("Bad Marionette frame length")
        length = int(prefix)
        if length > 2 * 1024 * 1024:
            raise ValueError("Marionette response exceeds bound")
        return json.loads(self.exact(length))

    def command(self, name, parameters=None):
        self.sequence += 1
        body = json.dumps([0, self.sequence, name, parameters or {}]).encode()
        self.connection.sendall(str(len(body)).encode() + b":" + body)
        reply = self.receive()
        if not isinstance(reply, list) or reply[:2] != [1, self.sequence]:
            raise RuntimeError(f"Unexpected Marionette reply: {reply}")
        if reply[2]:
            raise RuntimeError(f"{name}: {reply[2]}")
        return reply[3]


EXTRACT = """
const ids = ['graphics-features-tbody', 'graphics-gpu-1-tbody',
 'graphics-gpu-2-tbody', 'graphics-diagnostics-tbody',
 'graphics-decisions-tbody', 'graphics-crashguards-tbody',
 'graphics-workarounds-tbody', 'graphics-failures-tbody', 'media-info-tbody'];
const sections = {};
for (const id of ids) {
 const element = document.getElementById(id);
 sections[id] = element ? element.innerText : null;
}
return {url: location.href, title: document.title, sections,
 codecTables: Array.from(document.querySelectorAll('[id]'))
  .filter(e => /codec.*support|support.*codec/i.test(e.id))
  .map(e => ({id: e.id, text: e.innerText || e.textContent})).slice(0, 12)};
"""


def process_snapshot(group, reference_pid):
    """Capture only the probe's group and reference, tolerating /proc races."""
    snapshot = {"processes": [], "mmc_diskstats": []}
    for directory in Path("/proc").iterdir():
        if not directory.name.isdigit():
            continue
        try:
            if directory.stat().st_uid != os.getuid():
                continue
            fields = (directory / "stat").read_text().rsplit(")", 1)[1].split()
            pid = int(directory.name)
            if int(fields[2]) != group and pid != reference_pid:
                continue
            entry = {"pid": pid, "state": fields[0], "ppid": int(fields[1]),
                "pgrp": int(fields[2]), "minor_faults": int(fields[7]),
                "major_faults": int(fields[9]), "user_ticks": int(fields[11]),
                "system_ticks": int(fields[12])}
            for name in ("comm", "wchan", "io"):
                try:
                    entry[name] = (directory / name).read_text().strip()
                except OSError as exc:
                    entry[name + "_error"] = str(exc)
            snapshot["processes"].append(entry)
        except (OSError, ValueError, IndexError):
            continue
    try:
        snapshot["mmc_diskstats"] = [line.strip()
            for line in Path("/proc/diskstats").read_text().splitlines()
            if line.split()[2].startswith("mmcblk")]
    except OSError as exc:
        snapshot["diskstats_error"] = str(exc)
    snapshot["processes"].sort(key=lambda item: item["pid"])
    return snapshot


def drm_device_snapshot():
    def target(path):
        return str(path.resolve()) if path.exists() else None

    devices = []
    for path in sorted(Path("/dev/dri").glob("*")):
        try:
            info = path.stat()
            if not stat.S_ISCHR(info.st_mode):
                continue
            sysfs = Path("/sys/dev/char") / f"{os.major(info.st_rdev)}:{os.minor(info.st_rdev)}"
            devices.append({"path": str(path), "major": os.major(info.st_rdev),
                "minor": os.minor(info.st_rdev),
                "user_can_read_write": os.access(path, os.R_OK | os.W_OK),
                "device": target(sysfs / "device"),
                "driver": target(sysfs / "device/driver"),
                "module": target(sysfs / "device/driver/module")})
        except OSError:
            continue
    return devices


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-pid", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=["baseline", "egl-gles", "egl-gles-drm", "both", "all"], default="both")
    parser.add_argument("--startup-timeout", type=float, default=120,
        help="seconds to allow cold disk-backed Firefox startup (default: 120)")
    parser.add_argument("--protocol-timeout", type=float, default=60,
        help="seconds for Marionette greeting/each command (default: 60)")
    parser.add_argument("--drm-device", type=Path,
        help="verified render node for the separate egl-gles-drm candidate")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("run as the desktop user, not root")
    if not 10 <= args.startup_timeout <= 600:
        parser.error("startup timeout must be between 10 and 600 seconds")
    if not 5 <= args.protocol_timeout <= 180:
        parser.error("protocol timeout must be between 5 and 180 seconds")
    if args.mode in {"egl-gles-drm", "all"}:
        if args.drm_device is None:
            parser.error("egl-gles-drm/all requires --drm-device")
        args.drm_device = args.drm_device.resolve()
        if (args.drm_device.parent != Path("/dev/dri")
                or not args.drm_device.name.startswith("renderD")
                or not stat.S_ISCHR(args.drm_device.stat().st_mode)
                or not os.access(args.drm_device, os.R_OK | os.W_OK)):
            parser.error("DRM device must be an accessible /dev/dri/renderD* character node")
        module = Path("/sys/class/drm") / args.drm_device.name / "device/driver/module"
        if not module.exists() or module.resolve().name != "pvrsrvkm":
            parser.error("selected render node is not owned by the validated pvrsrvkm module")
    elif args.drm_device is not None:
        parser.error("--drm-device applies only to egl-gles-drm/all; other modes use automatic selection")
    if args.output.exists():
        parser.error("output exists; preserve previous evidence")
    reference = Path("/proc") / str(args.reference_pid)
    if reference.stat().st_uid != os.getuid():
        parser.error("reference Firefox must belong to this desktop user")
    command = reference.joinpath("cmdline").read_bytes().split(b"\0")
    if b"-contentproc" in command or b"firefox" not in command[0]:
        parser.error("reference PID must be the existing parent Firefox process")
    binary = str(reference.joinpath("exe").resolve())
    previous_environment = dict(part.decode(errors="replace").split("=", 1)
        for part in reference.joinpath("environ").read_bytes().split(b"\0") if b"=" in part)
    keys = {"DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS",
            "XDG_SESSION_TYPE", "LD_LIBRARY_PATH", "LIBGL_DRIVERS_PATH", "GDK_BACKEND",
            "GDK_DISABLE", "GSK_RENDERER", "LIBGL_ALWAYS_SOFTWARE", "GALLIUM_DRIVER",
            "MESA_LOADER_DRIVER_OVERRIDE", "__EGL_VENDOR_LIBRARY_FILENAMES"}
    environment = os.environ.copy()
    for key in keys:
        if key in previous_environment:
            environment[key] = previous_environment[key]
        elif key not in {"DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"}:
            environment.pop(key, None)
    for key in list(environment):
        if key.startswith("MOZ_"):
            environment.pop(key)
    if not environment.get("DISPLAY") or not environment.get("XAUTHORITY"):
        parser.error("reference must supply DISPLAY and XAUTHORITY")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    artifact_directory = args.output.with_suffix(".files")
    artifact_directory.mkdir(exist_ok=False)
    record = {"schema": 4, "reference_pid": args.reference_pid,
              "startup_timeout_seconds": args.startup_timeout,
              "protocol_timeout_seconds": args.protocol_timeout,
              "drm_devices": drm_device_snapshot(),
              "reference_profile_modified": False, "binary": binary,
              "environment": {key: environment[key] for key in sorted(keys) if key in environment},
              "sandbox_disabled": False, "blocklist_overridden": False,
              "diagnostic_remote_system_access": True, "tests": []}
    modes = ["baseline", "egl-gles"] if args.mode in {"both", "all"} else [args.mode]
    if args.mode == "all":
        modes.append("egl-gles-drm")
    for mode in modes:
        result = {"mode": mode, "complete": False, "startup_samples": []}
        process = None
        connection = None
        started = None
        profile = tempfile.TemporaryDirectory(prefix="t736-firefox-" + mode + "-")
        log_path = artifact_directory / (mode + ".log")
        log = log_path.open("w", encoding="utf-8")
        try:
            # Select a free loopback port. Firefox itself uses loopbackOnly=true
            # in this installed release's Marionette server.sys.mjs.
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            preferences = {"marionette.port": port, "browser.shell.checkDefaultBrowser": False,
                "browser.startup.homepage": "about:blank", "browser.startup.page": 0,
                "browser.startup.firstrunSkipsHomepage": True,
                "browser.aboutwelcome.enabled": False}
            run_environment = environment.copy()
            if mode in {"egl-gles", "egl-gles-drm"}:
                run_environment["MOZ_X11_EGL"] = "1"
                preferences["gfx.egl.prefer-gles.enabled"] = True
            if mode == "egl-gles-drm":
                run_environment["MOZ_DRM_DEVICE"] = str(args.drm_device)
            Path(profile.name, "user.js").write_text("\n".join(
                f"user_pref({json.dumps(key)}, {json.dumps(value)});" for key, value in preferences.items())
                + "\n", encoding="utf-8")
            result["preferences"] = preferences
            result["MOZ_X11_EGL"] = run_environment.get("MOZ_X11_EGL")
            result["MOZ_DRM_DEVICE"] = run_environment.get("MOZ_DRM_DEVICE")
            result["phase"] = "launch"
            process = subprocess.Popen([binary, "--no-remote", "--profile", profile.name,
                "--marionette", "--remote-allow-system-access", "about:blank"], env=run_environment,
                stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
            result["pid"] = process.pid
            started = time.monotonic()
            deadline = started + args.startup_timeout
            next_sample = started
            result["phase"] = "wait_for_tcp_listener"
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"Diagnostic Firefox exited {process.returncode}")
                if time.monotonic() >= next_sample:
                    sample = process_snapshot(process.pid, args.reference_pid)
                    sample["elapsed_seconds"] = round(time.monotonic() - started, 3)
                    result["startup_samples"].append(sample)
                    print(json.dumps({"mode": mode, "startup_sample": sample}), flush=True)
                    next_sample = time.monotonic() + 5
                try:
                    connection = socket.create_connection(("127.0.0.1", port), timeout=1)
                    break
                except OSError:
                    time.sleep(0.25)
            if connection is None:
                result["startup_timeout_snapshot"] = process_snapshot(process.pid, args.reference_pid)
                raise TimeoutError(f"Diagnostic Firefox did not start Marionette within {args.startup_timeout:g}s; this is not a renderer result")
            result["marionette_tcp_connected_seconds"] = round(time.monotonic() - started, 3)
            result["phase"] = "wait_for_protocol_greeting"
            client = Marionette(connection, args.protocol_timeout)
            result["marionette_hello"] = client.hello
            result["marionette_hello_received_seconds"] = round(time.monotonic() - started, 3)
            result["phase"] = "new_session"
            # Direct Marionette takes the negotiated, flat capabilities map;
            # the HTTP WebDriver capabilities/alwaysMatch wrapper is geckodriver's.
            result["session"] = client.command("WebDriver:NewSession", {
                "acceptInsecureCerts": False})
            client.command("WebDriver:SetTimeouts", {"pageLoad": 20000, "script": 15000})
            result["phase"] = "navigate_about_support"
            client.command("WebDriver:Navigate", {"url": "about:support"})
            result["phase"] = "extract_about_support"
            deadline = time.monotonic() + 15
            while True:
                response = client.command("WebDriver:ExecuteScript", {"script": EXTRACT, "args": [],
                    "newSandbox": True, "sandbox": "default"})
                support = response.get("value", response) if isinstance(response, dict) else response
                if not isinstance(support, dict):
                    raise RuntimeError(f"Unexpected DOM extraction: {support}")
                result["support"] = support
                section = support.get("sections", {}).get("graphics-features-tbody") or ""
                if "WebRender" in section or time.monotonic() >= deadline:
                    break
                time.sleep(0.5)
            result["loaded_graphics_libraries"] = sorted({line.split()[-1]
                for line in Path(f"/proc/{process.pid}/maps").read_text().splitlines()
                if any(word in line for word in ("libEGL", "libGLES", "libGL.so", "libGLX", "pvr", "swrast", "gallium"))})
            result["complete"] = True
            result["phase"] = "complete"
            try:
                client.command("Marionette:Quit", {"flags": ["eAttemptQuit"]})
            except (EOFError, OSError):
                pass
        except Exception as exc:
            result["error"] = f"{type(exc).__name__}: {exc}"
            if process is not None:
                result["failure_snapshot"] = process_snapshot(process.pid, args.reference_pid)
            if started is not None:
                result["failure_elapsed_seconds"] = round(time.monotonic() - started, 3)
        finally:
            if connection:
                connection.close()
            if process:
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=3)
                result["exit_code"] = process.returncode
            log.close()
            profile.cleanup()
            result["profile_removed"] = True
            result["log"] = str(log_path)
            record["tests"].append(result)
            args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
            print(json.dumps(result), flush=True)
    return 0 if all(item["complete"] for item in record["tests"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
