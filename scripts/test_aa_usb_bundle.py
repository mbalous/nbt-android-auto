"""Check USB validation archives contain the local headers their source includes."""
from pathlib import Path
import re
import unittest

import aa_usb_bundle


class UsbBundleTests(unittest.TestCase):
    def test_local_includes_are_staged_for_both_archive_layouts(self):
        source = (aa_usb_bundle.SOURCE / "nbt_android_auto.c").read_bytes()
        for directory in ("diagnostics", "."):
            files = {}
            aa_usb_bundle.add_usb_headers(files, directory)
            for name in re.findall(rb'^\s*#\s*include\s*"([^"]+)"', source, re.M):
                path = str(Path(directory) / name.decode("ascii"))
                self.assertEqual(files[path], (aa_usb_bundle.SOURCE / name.decode("ascii")).read_bytes())


if __name__ == "__main__":
    unittest.main()
