"""Keep generated sources and build products outside the existing Tina SDK."""
from pathlib import Path

FORBIDDEN_PARTS = {"tina", "tinasdk", "tina-sdk", "tina_sdk"}


def outside_tina(path):
    path = Path(path).resolve()
    if any(part.casefold() in FORBIDDEN_PARTS for part in path.parts):
        raise ValueError("writing inside a Tina SDK is prohibited: " + str(path))
    for parent in (path, *path.parents):
        if (parent / "device/config/chips").is_dir() and (parent / "build/envsetup.sh").is_file():
            raise ValueError("destination is inside an existing Allwinner SDK: " + str(parent))
    return path
