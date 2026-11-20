"""Host-side tests for audio route recovery and asynchronous teardown.

Set NBT_AAD_REFERENCE to a clean checkout of the pinned NBT-AAD revision.
"""
import os
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/android_auto"
REFERENCE = Path(os.environ.get("NBT_AAD_REFERENCE", ROOT / ".local/reference-nbt-aad")).resolve()
REVISION = "41975f269b065dbdbc77af9f568387cd22b9ed27"


def run(args, **kwargs):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{args!r} failed:\n{result.stdout.decode(errors='replace')}")
    return result


class AudioSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not REFERENCE.exists() and "NBT_AAD_REFERENCE" not in os.environ:
            raise unittest.SkipTest("pinned NBT-AAD checkout not present; set NBT_AAD_REFERENCE")
        revision = run(["git", "-C", str(REFERENCE), "rev-parse", "HEAD"]).stdout.decode().strip()
        if revision != REVISION or run(["git", "-C", str(REFERENCE), "status", "--porcelain"]).stdout.strip():
            raise RuntimeError("NBT-AAD reference must be clean and at the pinned revision")
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.runtime = cls.root / "runtime"
        cls.runtime.mkdir()
        define = '-DNBT_AUDIO_RUNTIME_DIR="' + str(cls.runtime) + '"'
        cls.tool = cls.root / "audioconn"
        run(["c++", "-std=c++11", "-Wall", "-Wextra", define,
             "-I" + str(SOURCE / "knob/tests/fake"), "-I" + str(SOURCE / "knob/ref"),
             str(SOURCE / "knob/nbt_audioconn.cpp"), "-pthread", "-o", str(cls.tool)])
        cls.cleanup = cls.root / "cleanup"
        run(["cc", "-std=gnu99", "-Wall", "-Wextra", str(SOURCE / "test_audio_cleanup.c"),
             "-o", str(cls.cleanup)])

        audio = cls.root / "milestone-7/src/audio"
        audio.mkdir(parents=True)
        for name in ("audio_route.cpp", "audio_route.hpp", "browser_audio.hpp"):
            shutil.copyfile(REFERENCE / "milestone-7/src/audio" / name, audio / name)
        run(["git", "apply", str(ROOT / "patches/nbt-aad-audio-route-lifetime.patch")], cwd=cls.root)
        cls.lifetime = cls.root / "lifetime"
        run(["c++", "-std=c++11", "-Wall", "-Wextra", "-I" + str(audio.parent),
             str(audio / "audio_route.cpp"), str(SOURCE / "test_audio_route_lifetime.cpp"),
             "-pthread", "-o", str(cls.lifetime)])

        shutil.copyfile(SOURCE / "audio_a2i/asound_device.hpp", audio / "asound_device.hpp")
        fake = cls.root / "fake_a2i.c"
        fake.write_text("""
int A2I_Client_Open(void **h, const void *cfg) { (void)h; (void)cfg; return 1; }
int A2I_Client_Close(void **h) { (void)h; return 0; }
int A2I_Client_GetNotification(void *h) { (void)h; return 0; }
int A2I_Client_GetBuffer(void *h, void **b, unsigned long *n) { (void)h; (void)b; (void)n; return 0; }
int A2I_Client_Out_Write(void *h, const void *b, unsigned long n, unsigned x) {
    (void)h; (void)b; (void)n; (void)x; return 0;
}
""")
        cls.library = cls.root / "fake_a2i.so"
        run(["cc", "-shared", "-fPIC", str(fake), "-o", str(cls.library)])
        probe = cls.root / "probe.cpp"
        probe.write_text('#include "audio/asound_device.hpp"\n'
                         'int main() { nbt::AsoundDevice d; return !d.load() || d.open(); }\n')
        cls.probe = cls.root / "probe"
        run(["c++", "-std=c++11", define, "-I" + str(audio.parent),
             str(SOURCE / "audio_a2i/asound_device.cpp"), str(probe), "-ldl", "-o", str(cls.probe)])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        for item in self.runtime.iterdir():
            item.unlink()

    def tool_call(self, op, conn="0xb8", reply=None):
        env = os.environ.copy()
        env.pop("TEST_REPLY", None)
        env.pop("TEST_START_FAIL", None)
        if reply:
            env["TEST_REPLY"] = reply
        return subprocess.run([str(self.tool), op, conn], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_unknown_build_is_recorded_and_removed_only_after_grant(self):
        self.assertEqual(self.tool_call("-b", "0xb9", "timeout").returncode, 3)
        marker = self.runtime / "audio-route.built"
        self.assertEqual(marker.read_text().strip(), "0xb9")
        self.assertEqual(self.tool_call("-r", "0xb9", "error").returncode, 1)
        self.assertTrue(marker.exists())
        self.assertEqual(self.tool_call("-r", "0xb9").returncode, 0)
        self.assertFalse(marker.exists())

    def test_pending_connection_cannot_be_overwritten(self):
        marker = self.runtime / "audio-route.built"
        marker.write_text("0xb9\n")
        self.assertEqual(self.tool_call("-b", "0xb8").returncode, 4)
        self.assertEqual(marker.read_text().strip(), "0xb9")
        self.assertEqual(self.tool_call("-r", "0xb8").returncode, 0)
        self.assertTrue(marker.exists())

    def test_build_and_cleanup_are_serialized(self):
        env = os.environ.copy()
        env["TEST_REPLY"] = "timeout"
        env.pop("TEST_START_FAIL", None)
        proc = subprocess.Popen([str(self.tool), "-b", "0xb8"], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            marker = self.runtime / "audio-route.built"
            deadline = time.monotonic() + 2
            while not marker.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(marker.exists())
            self.assertEqual(self.tool_call("-r").returncode, 0)
            self.assertEqual(proc.wait(timeout=5), 3)
            self.assertFalse(marker.exists())
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=5)

    def test_granted_route_does_not_clear_output_failure(self):
        marker = self.runtime / "audio-output.failed"
        marker.write_text("endpoint unavailable\n")
        self.assertEqual(self.tool_call("-b").returncode, 0)
        self.assertTrue(marker.exists())
        self.assertEqual(self.tool_call("-r").returncode, 0)

    def test_failed_cleanup_keeps_recorded_id_and_falls_back(self):
        marker = self.runtime / "audio-route.built"
        marker.write_text("0xb9\n")
        (self.runtime / "aa-audio").write_text("0\n")
        executable = self.runtime / "audioconn"
        executable.write_text("#!/bin/sh\nprintf '%s\\n' \"$*\" > args\nexit 1\n")
        executable.chmod(0o700)
        output = run([str(self.cleanup)], cwd=self.runtime).stdout.decode()
        self.assertIn("test mode=bt", output)
        self.assertEqual((self.runtime / "args").read_text().strip(), "-r 0xb9")
        self.assertTrue(marker.exists())

    def test_output_open_failure_records_fallback(self):
        env = os.environ.copy()
        env["NBT_A2I_LIB"] = str(self.library)
        run([str(self.probe)], env=env)
        self.assertTrue((self.runtime / "audio-output.failed").exists())
        (self.runtime / "aa-audio").write_text("0\n")
        output = run([str(self.cleanup)], cwd=self.runtime).stdout.decode()
        self.assertIn("test mode=bt", output)

    def test_audio_modes_select_source_and_prompt_route(self):
        executable = self.runtime / "audioconn"
        executable.write_text("#!/bin/sh\nexit 0\n")
        executable.chmod(0o700)
        output = run([str(self.cleanup)], cwd=self.runtime).stdout.decode()
        self.assertIn("test media=input_stereo_1 source=f6 prompts=b8", output)
        (self.runtime / "prompts-off").touch()
        output = run([str(self.cleanup)], cwd=self.runtime).stdout.decode()
        self.assertIn("test media=input_stereo_1 source=f6 prompts=0", output)
        (self.runtime / "media-jack").touch()
        output = run([str(self.cleanup)], cwd=self.runtime).stdout.decode()
        self.assertIn("test media=(unset) source=bb prompts=0", output)
        (self.runtime / "media-jack").unlink()
        (self.runtime / "aa-audio").write_text("0xb8\n")
        output = run([str(self.cleanup)], cwd=self.runtime).stdout.decode()
        self.assertIn("test mode=browser", output)
        self.assertIn("test media=(unset) source=bb prompts=0", output)

    def test_wait_idle_drains_route_callback(self):
        executable = self.root / "slow-route"
        executable.write_text('#!/bin/sh\ntouch "$TEST_ROUTE_STARTED"\nsleep 0.4\n')
        executable.chmod(0o700)
        run([str(self.lifetime), str(executable), str(self.root / "route-started")])

    def test_session_and_release_build_use_same_audio_sources(self):
        import build_all
        spec = importlib.util.spec_from_file_location("session_test", ROOT / "scripts/Test-AndroidAutoSession.py")
        session = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(session)
        class Archive:
            def __init__(self):
                self.files = {}
            def add(self, name, data):
                self.files[name] = data
        archive = Archive()
        build_all.add_nbt_aad(archive, REFERENCE)
        files = {}
        for folder in ("src", "include"):
            for path in (REFERENCE / "milestone-7" / folder).rglob("*"):
                if path.is_file():
                    name = "reference/" + path.relative_to(REFERENCE / "milestone-7").as_posix()
                    files[name] = path.read_bytes().replace(b"\r\n", b"\n")
        name = "reference/src/aa_session.cpp"
        files[name] = session.harden_session(files[name].decode()).encode()
        session.prepare_audio_sources(files)
        for name in ("asound_device.cpp", "asound_device.hpp", "browser_audio.hpp",
                     "browser_audio.cpp", "audio_route.hpp", "audio_route.cpp"):
            key = "reference/src/audio/" + name
            self.assertEqual(files[key], archive.files[key], key)


if __name__ == "__main__":
    unittest.main()
