"""Build and run hardware-independent C++ tests on a Linux host."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def png_chunk(kind, data):
    payload = kind + data
    return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="enable AddressSanitizer and UBSan")
    args = parser.parse_args()

    flags = ["-std=gnu++0x", "-O1", "-g", "-Wall", "-Wextra"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]

    with tempfile.TemporaryDirectory(prefix="nbt-host-tests-") as directory:
        work = Path(directory)
        # A synthetic Annex-B stream exercises framing, not video decoding.
        h264 = work / "fixture.h264"
        h264.write_bytes(b"\x00\x00\x00\x01\x67\x42\x00\x1e"
                         b"\x00\x00\x01\x68\xce\x06"
                         b"\x00\x00\x01\x65\x88\x84")
        png = work / "fixture.png"
        png.write_bytes(
            b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(b"\x00\x00\x00\x00"))
            + png_chunk(b"IEND", b"")
        )
        tests = [
            ("src/android_auto/tests/test_usb_wait.cpp", [], ["-pthread"]),
            ("src/android_auto/tests/test_usb_zlp_policy.cpp", [], []),
            ("src/android_auto/tests/test_usb_transfer_lifecycle.cpp", [], ["-pthread"]),
            ("src/android_auto/tests/test_usb_capture.c", [],
             ["-pthread", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
              "-DEOK=0", "-idirafter", str(ROOT / "toolchain/sysroot/usr/include")]),
            ("src/android_auto/tests/test_aoa_reenumeration.cpp", [], []),
            ("src/android_auto/tests/test_native_switch.c", [], []),
            ("src/android_auto/tests/test_usb_transport_diag.cpp", [], []),
            ("src/android_auto/test_h264_annexb.cpp", [str(h264), str(work / "normalized.h264")], []),
            ("experiments/png-pipeline/test_frame_packet.cpp", [str(png)], []),
            ("src/android_auto/tests/test_video_capture_output.cpp", [],
             ["-pthread", "-I", str(ROOT / "src/android_auto/tests/fake")]),
            ("src/android_auto/tests/test_fifo_http.cpp", [], ["-pthread"]),
            ("src/android_auto/tests/test_video_supervisor.c", [], ["-pthread"]),
            ("src/idrive/test_exit_handoff.cpp", [str(work)],
             ["-pthread", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"]),
            ("src/android_auto/knob/tests/test_prompt_route.cpp", [str(work)],
             ["-pthread", "-ffunction-sections", "-Wl,--gc-sections",
              "-I", str(ROOT / "src/android_auto/knob/ref")]),
        ]
        for source, arguments, extra_flags in tests:
            executable = work / Path(source).stem
            compiler = os.environ.get("CC", "cc") if source.endswith(".c") else os.environ.get("CXX", "g++")
            source_flags = (["-std=gnu99" if flag == "-std=gnu++0x" else flag for flag in flags]
                            if source.endswith(".c") else flags)
            subprocess.run([compiler, *source_flags, *extra_flags, str(ROOT / source),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable), *arguments], check=True)


if __name__ == "__main__":
    main()
