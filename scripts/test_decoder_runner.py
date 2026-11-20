"""Verify the C++ decoder launcher without the stock QNX player."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src/android_auto"


class DecoderRunnerTests(unittest.TestCase):
    def test_player_arguments_and_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            runner = work / "decode_runner"
            player = work / "fake-player"
            compiler = os.environ.get("CXX", "c++")
            for name in ("nbt_decode_runner", "test_decoder_deadline"):
                executable = runner if name == "nbt_decode_runner" else work / name
                subprocess.run([compiler, "-std=gnu++0x", "-Wall", "-Wextra", "-I", str(SOURCE / "tests/fake"),
                                str(SOURCE / (name + ".cpp")), "-o", str(executable)], check=True)
            player.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$NBT_TEST_ARGS\"\nexit 42\n", encoding="ascii")
            player.chmod(0o700)

            url = "http://127.0.0.1:18765/live.h264"
            args_file = work / "args"
            result = subprocess.run([runner, "5", player, url, "screen"], capture_output=True,
                                    text=True, timeout=10, env={**os.environ, "NBT_TEST_ARGS": str(args_file)},
                                    cwd=work)
            self.assertEqual(result.returncode, 42, result.stderr)
            arguments = args_file.read_text().splitlines()
            self.assertIn("NmeDeviceScreen", arguments)
            self.assertIn("0,0,1000,625", arguments)
            self.assertNotIn("-a0", arguments)
            self.assertEqual(arguments[-1], url)
