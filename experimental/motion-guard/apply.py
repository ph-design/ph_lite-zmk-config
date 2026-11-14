"""Apply preview.5 to an isolated, pinned EVO + Studio-over-2.4G source tree."""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess

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
    "guard.h": root / "app/src/guard.h",
    "motion_guard.c": root / "app/src/motion_guard.c",
    "motion_guard.h": root / "app/include/zmk/motion_guard.h",
    "motion-telemetry.h": root / "app/include/zmk/motion-telemetry.h",
    "motion_subsystem.c": root / "app/src/studio/motion_subsystem.c",
    "motion.proto": messages / "proto/zmk/motion.proto",
}
for name, path in copies.items():
    if name not in ("motion_subsystem.c", "motion.proto") and path.exists():
        raise SystemExit(f"Overlay already exists: {path}. Start from a clean isolated tree.")

patch = assets / "integration.patch"
subprocess.run(["git", "-C", str(root), "apply", "--check", str(patch)], check=True)
if args.check:
    print("PASS: pinned motion sources, protocol and integration patch")
else:
    subprocess.run(["git", "-C", str(root), "apply", str(patch)], check=True)
    for name, target in copies.items():
        target.parent.mkdir(parents=True, exist_ok=True)
        # Normalize source newlines so Windows checkouts work with Linux toolchains.
        target.write_bytes((assets / name).read_text().encode())
    print("Applied EVO Motion Guard preview.5; reconfigure/rebuild protobuf and firmware.")
