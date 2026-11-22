"""Stage the local headers used by the standalone Android Auto USB entry point."""
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[1] / "src/android_auto"


def add_usb_headers(files, directory):
    pending = ["nbt_android_auto.c"]
    seen = set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        data = (SOURCE / name).read_bytes()
        if name != "nbt_android_auto.c":
            files[str(Path(directory) / name)] = data
        for header in re.findall(rb'^\s*#\s*include\s*"([^"]+)"', data, re.M):
            pending.append(header.decode("ascii"))
