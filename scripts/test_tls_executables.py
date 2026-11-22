"""Build and run the first-party C++ TLS tools against the pinned Mbed TLS."""
import importlib.util
import os
from pathlib import Path
import ssl
import subprocess
import tarfile
import tempfile
import unittest

from build_all import Archive, MBEDTLS_VERSION, ROOT, add_mbedtls, add_nbt_aad


class TlsExecutableTests(unittest.TestCase):
    def test_tls_and_certificate_tools(self):
        source = ROOT / "src/android_auto"
        fixture_spec = importlib.util.spec_from_file_location("qnx_tls", ROOT / "scripts/Test-QnxTls.py")
        qnx_tls = importlib.util.module_from_spec(fixture_spec)
        fixture_spec.loader.exec_module(qnx_tls)

        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            archive = Archive(work / "sources.tar")
            try:
                add_mbedtls(archive)
            finally:
                archive.close()
            with tarfile.open(work / "sources.tar") as tar:
                tar.extractall(work)
            mbedtls = work / f"mbedtls-{MBEDTLS_VERSION}"
            subprocess.run(["make", "-j2", "lib"], cwd=mbedtls, check=True, timeout=180)

            fixtures = qnx_tls.fixtures()
            for name, data in fixtures.items():
                (work / name).write_bytes(data)
            (work / "client.der").write_bytes(ssl.PEM_cert_to_DER_cert(fixtures["lab-client.pem"].decode("ascii")))

            cxx = [os.environ.get("CXX", "c++"), "-std=gnu++0x", "-Wall", "-Wextra",
                   "-I", str(mbedtls / "include")]
            libs = [str(mbedtls / "library" / f"libmbed{name}.a") for name in ("tls", "x509", "crypto")]
            tools = [
                ("test_tls_memory", ["-DNBT_TLS_BACKEND_TEST", str(source / "aa_cryptor_mbedtls.cpp")], []),
                ("test_legacy_time", [], []),
                ("test_certificate_compat", [], ["client.der", "lab-ca.pem"]),
            ]
            for name, flags, args in tools:
                executable = work / name
                subprocess.run([*cxx, *flags, str(source / f"{name}.cpp"), *libs, "-o", str(executable)],
                               check=True, timeout=60)
                subprocess.run([executable, *args], cwd=work, check=True, timeout=75)

    def test_patched_session_protocol(self):
        reference = Path(os.environ.get("NBT_AAD_REFERENCE", ROOT / ".local/reference-nbt-aad")).resolve()
        if not reference.exists() and "NBT_AAD_REFERENCE" not in os.environ:
            self.skipTest("pinned NBT-AAD checkout not present; set NBT_AAD_REFERENCE")
        fixture_spec = importlib.util.spec_from_file_location("qnx_tls", ROOT / "scripts/Test-QnxTls.py")
        qnx_tls = importlib.util.module_from_spec(fixture_spec)
        fixture_spec.loader.exec_module(qnx_tls)

        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            archive = Archive(work / "sources.tar")
            try:
                add_mbedtls(archive)
                add_nbt_aad(archive, reference)
            finally:
                archive.close()
            with tarfile.open(work / "sources.tar") as tar:
                tar.extractall(work)
            mbedtls = work / f"mbedtls-{MBEDTLS_VERSION}"
            subprocess.run(["make", "-j2", "lib"], cwd=mbedtls, check=True, timeout=180)
            for name, data in qnx_tls.fixtures().items():
                (work / name).write_bytes(data)
            reference_src = work / "reference/src"
            sources = ["aa_session.cpp", "aa_cryptor.cpp", "audio/audio_acks.cpp",
                       "sdr_builder.cpp", "crash_handler.cpp"]
            libs = [str(mbedtls / "library" / f"libmbed{name}.a") for name in ("tls", "x509", "crypto")]
            executable = work / "test_session_protocol"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=gnu++0x", "-Wall", "-Wextra", "-pthread",
                            "-I", str(mbedtls / "include"), "-I", str(reference_src),
                            "-I", str(work / "reference/include"),
                            str(ROOT / "src/android_auto/test_session_protocol.cpp"),
                            *[str(reference_src / name) for name in sources], *libs, "-o", str(executable)],
                           check=True, timeout=120)
            subprocess.run([executable], cwd=work, check=True, timeout=100)
