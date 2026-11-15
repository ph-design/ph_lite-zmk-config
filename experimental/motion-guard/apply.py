"""Apply preview.9 to an isolated, pinned EVO + Studio-over-2.4G source tree."""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("zmk_root", type=Path)
parser.add_argument("messages_root", type=Path,
                    help="zmk-studio-messages module used by the build")
parser.add_argument("--check", action="store_true", help="validate without modifying either tree")
args = parser.parse_args()
assets = Path(__file__).resolve().parent
root, messages = args.zmk_root.resolve(), args.messages_root.resolve()

expected = {
    root / "app/src/studio/motion_subsystem.c": "9648fa9975d1e5f9607f1c42105bf544cff4096a89e6269a5eb019d8dd408ab9",
    messages / "proto/zmk/motion.proto": "cecf5f2ea47478c05f85c978d89776542d8a003fa7af1febd3a535500c31b1b2",
}
for path, digest in expected.items():
    if not path.is_file() or hashlib.sha256(path.read_text().encode()).hexdigest() != digest:
        raise SystemExit(f"Unsupported baseline: {path}. Use the revisions in README.md.")

copies = {
    "lis2dh_decode.h": root / "app/module/drivers/sensor/lis2dh/lis2dh_decode.h",
    "tap.c": root / "app/src/tap.c",
    "no_imu.c": root / "app/src/no_imu.c",
    "power_subsystem.c": root / "app/src/studio/power_subsystem.c",
    "tap.h": root / "app/include/zmk/tap.h",
    "power.c": root / "app/src/power.c",
    "power_policy.h": root / "app/src/power_policy.h",
    "power.h": root / "app/include/zmk/power.h",
    "power_activity.c": root / "app/src/activity.c",
    "guard.h": root / "app/src/guard.h",
    "motion_guard.c": root / "app/src/motion_guard.c",
    "motion_guard.h": root / "app/include/zmk/motion_guard.h",
    "motion-telemetry.h": root / "app/include/zmk/motion-telemetry.h",
    "motion_subsystem.c": root / "app/src/studio/motion_subsystem.c",
    "motion.proto": messages / "proto/zmk/motion.proto",
}
for name, path in copies.items():
    if name not in ("motion_subsystem.c", "motion.proto", "power_activity.c") and path.exists():
        raise SystemExit(f"Overlay already exists: {path}. Start from a clean isolated tree.")

patch = assets / "integration.patch"
subprocess.run(["git", "-C", str(root), "apply", "--check", str(patch)], check=True)
subprocess.run(["git", "-C", str(root), "apply", "--check", str(assets / "power-lights.patch")], check=True)
subprocess.run(["git", "-C", str(root), "apply", "--check", str(assets / "tap-driver.patch")], check=True)
subprocess.run(["git", "-C", str(root), "apply", "--check", str(assets / "driver-units.patch")], check=True)
if args.check:
    print("PASS: pinned motion sources, protocol and integration patch")
else:
    subprocess.run(["git", "-C", str(root), "apply", str(patch)], check=True)
    subprocess.run(["git", "-C", str(root), "apply", str(assets / "power-lights.patch")], check=True)
    subprocess.run(["git", "-C", str(root), "apply", str(assets / "tap-driver.patch")], check=True)
    subprocess.run(["git", "-C", str(root), "apply", str(assets / "driver-units.patch")], check=True)
    cmake = root / "app/CMakeLists.txt"
    cmake.write_bytes(cmake.read_text().replace("src/motion_guard.c", "src/motion_guard.c src/tap.c").replace(
        "target_sources_ifdef(CONFIG_ZMK_MOTION", "target_sources(app PRIVATE src/power.c)\nif(NOT CONFIG_ZMK_MOTION)\n  target_sources(app PRIVATE src/no_imu.c)\nendif()\ntarget_sources_ifdef(CONFIG_ZMK_MOTION", 1).encode())
    studio_cmake = root / "app/src/studio/CMakeLists.txt"
    studio_cmake.write_bytes((studio_cmake.read_text() + "\nif(NOT CONFIG_ZMK_MOTION)\n  target_sources(app PRIVATE power_subsystem.c)\nendif()\n").encode())
    for name, target in copies.items():
        target.parent.mkdir(parents=True, exist_ok=True)
        # Normalize source newlines so Windows checkouts work with Linux toolchains.
        target.write_bytes((assets / name).read_text().encode())
    subprocess.run([sys.executable, str(assets / "apply_tap.py"), str(root)], check=True)
    print("Applied EVO Motion Guard preview.9; reconfigure/rebuild protobuf and firmware.")
