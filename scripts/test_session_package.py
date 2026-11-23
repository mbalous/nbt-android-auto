"""Check the session executable's path from build output to installation and launch."""
import importlib.util
import json
import contextlib
from pathlib import Path
import re
import socket
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

import build_all

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sys.path.insert(0, str(ROOT / "installer"))
import nbt_aa_installer as installer
payload = load_module("make_payload", "installer/make_payload.py")


class SessionPackageTests(unittest.TestCase):
    def test_exit_focus_events_target_native_main_menu(self):
        root = ET.parse(ROOT / "src/idrive/NbtAndroidAuto.xml").getroot()
        model = root.find(".//raIntModel[@id='96']")
        self.assertIsNotNone(model)
        self.assertEqual((model.get("modelType"), model.get("value")), ("NativeStateID", "1"))
        for event_id in range(720, 730):
            events = root.findall(".//focusEvent[@id='%d']" % event_id)
            self.assertEqual(len(events), 1)
            self.assertEqual(events[0].get("targetModel"), "96")
            self.assertEqual(sum(node.get("id") == str(event_id) for node in root.iter()
                                 if node.tag != "property"), 1)

    def test_build_package_and_runtime_use_same_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            built, pem_dir, out = root / "build", root / "pems", root / "payload"
            for source, _, _, _ in payload.built_files(built, pem_dir):
                if source.is_relative_to(root):
                    source.parent.mkdir(parents=True, exist_ok=True)
                    source.write_bytes(b"fixture")

            (built / "programs/libAppFramework.A.so").write_bytes(b"link-only-stub")
            build_all.write_provenance(built, "qnx-sdp", "real", "test-build")
            with mock.patch.object(payload, "OUT", out), mock.patch(
                    "sys.argv", ["make_payload.py", "test", "--built", str(built), "--pem-dir", str(pem_dir)]):
                payload.main()

            manifest = json.loads((out / "manifest.json").read_text())
            self.assertFalse(any(path.name == "libAppFramework.A.so" for path in out.rglob("*")))
            self.assertEqual(manifest["build_provenance"]["hmi_link"], "real")
            session = next(item for item in manifest["files"] if item["source"] == "rt/nbt_android_auto")
            executable = Path(session["source"]).name
            self.assertEqual(session["target"], installer.RT_DIR + "/" + executable)
            self.assertEqual((out / session["source"]).read_bytes(), b"fixture")
            self.assertEqual(build_all.PROGRAMS[executable], "rt")

            build_script = (ROOT / "scripts/build-qnx.sh").read_text()
            self.assertRegex(build_script, r"-o \$OUT/" + re.escape(executable) + r"\b")
            boot = (out / "rt/boot.sh").read_text()
            copy_list = re.search(r"for f in (.+); do", boot).group(1).split()
            self.assertIn(executable, copy_list)
            service = (ROOT / "src/android_auto/nbt_native_service.c").read_text()
            for option in ("--switch-aa", "--capture-video"):
                self.assertIn('{"./' + executable + '", "' + option + '"', service)

            class Nbt:
                def __init__(self):
                    self.files = {installer.RT_DIR + "/previous-session": b"existing"}
                    self.commands = []

                def put(self, data, target, mode):
                    self.files[target] = data

                def run(self, command, **kwargs):
                    self.commands.append(command)
                    return 0, b"", b""

            nbt = Nbt()
            with mock.patch.object(installer, "payload_dir", return_value=out), mock.patch.object(
                    installer, "preflight"):
                installer.install(nbt, lambda _: None)
            self.assertEqual(nbt.files[session["target"]], b"fixture")
            self.assertEqual(nbt.files[installer.RT_DIR + "/previous-session"], b"existing")
            restart_cmd = "'%s/restart.sh'" % installer.RT_DIR
            self.assertNotIn(restart_cmd, nbt.commands)

            nbt_restart = Nbt()
            with mock.patch.object(installer, "payload_dir", return_value=out), mock.patch.object(
                    installer, "preflight"):
                installer.install(nbt_restart, lambda _: None, restart=True)
            self.assertIn(restart_cmd, nbt_restart.commands)

    def test_latest_build_selects_newest_completed_qnx_build(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "qnx-build-20261120T100000" / "programs").mkdir(parents=True)
            (root / "qnx-build-20261120T110000" / "programs").mkdir(parents=True)
            (root / "qnx-build-20261120T120000").mkdir(parents=True)  # incomplete (no programs/)
            self.assertEqual(payload.latest_build(root), root / "qnx-build-20261120T110000")
            with self.assertRaises(SystemExit):
                payload.latest_build(root / "empty")

    def package_fixture(self, root, toolchain="qnx-sdp", mode="stubs"):
        built, pem_dir, out = root / "build", root / "pems", root / "payload"
        for source, _, _, _ in payload.built_files(built, pem_dir):
            if source.is_relative_to(root):
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(b"fixture")
        build_all.write_provenance(built, toolchain, mode, "test-build")
        with mock.patch.object(payload, "OUT", out), mock.patch(
                "sys.argv", ["make_payload.py", "test", "--built", str(built), "--pem-dir", str(pem_dir)]):
            payload.main()
        return built, pem_dir, out

    def test_packaging_rejects_missing_invalid_or_changed_metadata_before_removing_output(self):
        for bad in ("missing", "unknown", "changed"):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                built, pem_dir, out = self.package_fixture(root)
                marker = out / "keep"
                marker.write_text("existing output")
                metadata = built / "build-provenance.json"
                if bad == "missing":
                    metadata.unlink()
                elif bad == "unknown":
                    record = json.loads(metadata.read_text())
                    record["schema"] = 2
                    metadata.write_text(json.dumps(record))
                else:
                    (built / "programs" / "libNbtAndroidAuto.so").write_bytes(b"changed")
                with mock.patch.object(payload, "OUT", out), mock.patch(
                        "sys.argv", ["make_payload.py", "test", "--built", str(built), "--pem-dir", str(pem_dir)]):
                    with self.assertRaisesRegex(SystemExit, "Cannot package build"):
                        payload.main()
                self.assertEqual(marker.read_text(), "existing output")

    def test_stub_install_requires_opt_in_before_connect_or_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            _, _, out = self.package_fixture(Path(directory))
            nbt = mock.Mock()
            with mock.patch.object(installer, "payload_dir", return_value=out), \
                    mock.patch.object(installer, "Nbt") as connect:
                with self.assertRaisesRegex(installer.InstallError, "requires --allow-qnx-hmi-stubs"):
                    installer.run_cli(SimpleNamespace(cli="install", allow_qnx_hmi_stubs=False))
                connect.assert_not_called()
                with self.assertRaises(installer.InstallError):
                    installer.install(nbt, lambda _: None, root="/test", force=True, restart=True)
                nbt.run.assert_not_called()
                nbt.put.assert_not_called()
                manifest = installer.load_manifest()
                confirm = mock.Mock(return_value=False)
                self.assertFalse(installer.confirm_gui_install(manifest, confirm))
                confirm.assert_called_once()
                self.assertTrue(installer.confirm_gui_install(manifest, lambda _: True))
                with mock.patch.object(installer, "preflight"):
                    nbt.run.return_value = (0, b"", b"")
                    installer.install(nbt, lambda _: None, root="/test", allow_qnx_hmi_stubs=True)
                self.assertTrue(nbt.put.called)
                args = SimpleNamespace(cli="install", allow_qnx_hmi_stubs=True, key="key", host="unused", port=22,
                                       user="test", known_hosts=None, root="/test", force=False, restart=False)
                with mock.patch.object(installer, "paramiko", object()), mock.patch.object(installer, "install") as install:
                    installer.run_cli(args)
                    self.assertTrue(install.call_args.kwargs["allow_qnx_hmi_stubs"])
            self.assertFalse(any(path.name in ("libAppFramework.A.so", "libCoreFramework.A.so")
                                 for path in out.rglob("*")))

    def test_manifest_provenance_validation_and_other_install_modes(self):
        for toolchain, mode in (("qnx-sdp", "real"), ("open-experimental", "stubs"),
                                ("open-experimental", "real")):
            with self.subTest(toolchain=toolchain), tempfile.TemporaryDirectory() as directory:
                _, _, out = self.package_fixture(Path(directory), toolchain, mode)
                with mock.patch.object(installer, "payload_dir", return_value=out):
                    manifest = installer.load_manifest()
                    installer.check_install_allowed(manifest)
                    confirm = mock.Mock()
                    self.assertTrue(installer.confirm_gui_install(manifest, confirm))
                    confirm.assert_not_called()
                    path = out / "manifest.json"
                    record = json.loads(path.read_text())
                    del record["build_provenance"]
                    path.write_text(json.dumps(record))
                    installer.check_install_allowed(installer.load_manifest())

        for bad in ("null", "unknown", "mismatch", "missing-mode"):
            with self.subTest(bad=bad), tempfile.TemporaryDirectory() as directory:
                _, _, out = self.package_fixture(Path(directory))
                path = out / "manifest.json"
                record = json.loads(path.read_text())
                if bad == "null":
                    record["build_provenance"] = None
                elif bad == "unknown":
                    record["build_provenance"]["toolchain"] = "unknown"
                elif bad == "mismatch":
                    record["build_provenance"]["plugin_sha256"] = "0" * 64
                else:
                    record["build_provenance"]["hmi_link"] = "missing"
                path.write_text(json.dumps(record))
                with mock.patch.object(installer, "payload_dir", return_value=out), \
                        self.assertRaises(installer.InstallError):
                    installer.load_manifest()


@unittest.skipIf(installer.paramiko is None, "Paramiko is required for SSH tests")
class InstallerHostKeyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server_key = installer.paramiko.ECDSAKey.generate()
        cls.other_key = installer.paramiko.ECDSAKey.generate()

    @contextlib.contextmanager
    def server(self):
        paramiko = installer.paramiko
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(10)
        stop = threading.Event()

        class Server(paramiko.ServerInterface):
            def __init__(self):
                self.auth_attempts = 0

            def check_auth_password(self, username, password):
                self.auth_attempts += 1
                return paramiko.AUTH_SUCCESSFUL

        server = Server()

        def serve():
            transport = None
            try:
                connection, _ = listener.accept()
                transport = paramiko.Transport(connection)
                transport.add_server_key(self.server_key)
                transport.start_server(server=server)
                stop.wait(10)
            except (OSError, EOFError, paramiko.SSHException):
                pass
            finally:
                if transport:
                    transport.close()
                listener.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            yield listener.getsockname()[1], server
        finally:
            stop.set()
            thread.join(12)
            self.assertFalse(thread.is_alive())

    def known_hosts(self, directory, port, key):
        path = Path(directory) / "known_hosts"
        path.write_text("[127.0.0.1]:%d %s %s\n" % (port, key.get_name(), key.get_base64()))
        return path

    def test_pinned_host_connects(self):
        with tempfile.TemporaryDirectory() as directory, self.server() as (port, server):
            path = self.known_hosts(directory, port, self.server_key)
            nbt = installer.Nbt("127.0.0.1", port, "test", password="test", log=lambda _: None, known_hosts=path)
            nbt.close()
            self.assertEqual(server.auth_attempts, 1)

    def test_unknown_and_changed_host_keys_fail_before_authentication(self):
        for changed in (False, True):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as directory, self.server() as (port, server):
                path = Path(directory) / "known_hosts"
                if changed:
                    self.known_hosts(directory, port, self.other_key)
                else:
                    path.write_text("")
                with self.assertRaises(installer.InstallError):
                    installer.Nbt("127.0.0.1", port, "test", password="test", log=lambda _: None,
                                  known_hosts=path)
                self.assertEqual(server.auth_attempts, 0)

    def test_missing_known_hosts_fails_without_connecting(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
                installer.paramiko.SSHClient, "connect") as connect:
            missing = Path(directory) / "missing"
            with self.assertRaisesRegex(installer.InstallError, "known-hosts"):
                installer.Nbt("127.0.0.1", 22, "test", known_hosts=missing)
            connect.assert_not_called()

    def test_default_still_accepts_unknown_key(self):
        with self.server() as (port, server):
            nbt = installer.Nbt("127.0.0.1", port, "test", password="test", log=lambda _: None)
            nbt.close()
            self.assertEqual(server.auth_attempts, 1)


if __name__ == "__main__":
    unittest.main()
