"""Collect the car-tested build outputs into installer/payload (+ manifest.json with SHA-256 per file).
Usage: python installer/make_payload.py [version] --built build/qnx-build-<time> [--pem-dir <folder>]
The payload contains binaries and the head-unit TLS identity; it is not committed (see .gitignore) but bundled into the
release builds. Rebuild libNbtAndroidAuto.so in the QNX VM after any change to src/idrive/aa_entry_plugin.cpp first.
--built takes the programs and plugin files from a scripts/build_all.py run instead of the developer's build folders,
plus nbt_reset ("Restart system" on the Android Auto page);
the PEM files (public NBT-AAD head-unit identity) then come from --pem-dir (default: rt/ of an existing payload,
e.g. unpacked from a release ZIP).
"""
import argparse
import hashlib
import json
import shutil
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / 'installer' / 'payload'
RT = '/var/opt/hmi/data/rw/NbtAndroidAutoRT'
PLUGIN = '/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto'
# (payload name, target path on the car, mode)
FILES = [
    ('rt/native_service', RT + '/native_service', '700'),
    ('rt/nbt_android_auto', RT + '/nbt_android_auto', '700'),
    ('rt/nbt_knob', RT + '/nbt_knob', '700'),
    ('rt/nbt_hwvideo', RT + '/nbt_hwvideo', '700'),
    ('rt/decode_runner', RT + '/decode_runner', '700'),
    ('rt/nbt_cluster', RT + '/nbt_cluster', '700'),
    ('rt/audioconn', RT + '/audioconn', '700'),
    ('rt/accessory_window', RT + '/accessory_window', '700'),
    ('rt/fifo_http', RT + '/fifo_http', '700'),
    ('rt/cert.pem', RT + '/cert.pem', '600'),
    ('rt/key.pem', RT + '/key.pem', '600'),
    ('rt/ca.pem', RT + '/ca.pem', '600'),
    ('rt/boot.sh', RT + '/boot.sh', '700'),
    ('rt/restart.sh', RT + '/restart.sh', '700'),
    ('rt/incident.sh', RT + '/incident.sh', '700'),
    ('plugin/libNbtAndroidAuto.so', PLUGIN + '/libNbtAndroidAuto.so', '755'),
    ('plugin/NbtAndroidAuto.xml', PLUGIN + '/NbtAndroidAuto.xml', '644'),
    ('plugin/NbtAndroidAuto_ALL_Texts.zip', PLUGIN + '/NbtAndroidAuto_ALL_Texts.zip', '644'),
    ('plugin/NbtAndroidAuto_ALL_Images.zip', PLUGIN + '/NbtAndroidAuto_ALL_Images.zip', '644'),
    ('plugin/NbtAndroidAuto.manifest', PLUGIN + '/NbtAndroidAuto.manifest', '644'),
]


def built_files(built, pem_dir):
    """Resolve payload sources from a scripts/build_all.py output folder."""
    programs, resources = built / 'programs', built / 'plugin-resources'
    result = []
    for name, target, mode in FILES:
        leaf = Path(name).name
        if leaf.endswith('.pem'):
            source = pem_dir / leaf
        elif name.startswith('plugin/') and leaf != 'libNbtAndroidAuto.so':
            source = resources / leaf
        elif leaf in ('boot.sh', 'restart.sh', 'incident.sh'):
            source = REPO / 'scripts' / ('nbt-aa-' + leaf)
        else:
            source = programs / leaf
        result.append((source, name, target, mode))
    # "Restart system" on the Android Auto page (src/idrive/reboot); the car-specific "gateway" file next to it is left as
    # it is on the car
    result.append((programs / 'nbt_reset', 'rt/nbt_reset', RT + '/nbt_reset', '700'))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('version', nargs='?', default=time.strftime('%Y.%m.%d'))
    parser.add_argument('--built', type=Path, required=True, help='output folder of scripts/build_all.py')
    parser.add_argument('--pem-dir', type=Path, default=OUT / 'rt', help='folder with cert.pem, key.pem, ca.pem')
    args = parser.parse_args()
    version = args.version
    files = built_files(args.built.resolve(), args.pem_dir.resolve())
    pems = {}
    for source, name, _, _ in files:   # read the PEMs before OUT is replaced (they may live in it)
        if name.endswith('.pem'):
            pems[name] = source.read_bytes()
    if OUT.exists():
        shutil.rmtree(OUT)
    entries = []
    for source, name, target, mode in files:
        data = pems[name] if name in pems else source.read_bytes()
        if name.endswith('.sh'):
            data = data.replace(b'\r\n', b'\n')     # the car's shell does not accept CR
        destination = OUT / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        entries.append({'source': name, 'target': target, 'mode': mode, 'size': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()})
        print('%-42s %8d' % (name, len(data)))
    (OUT / 'manifest.json').write_text(json.dumps({'version': version, 'files': entries}, indent=2), encoding='utf-8')
    print('payload', version, '->', OUT)


if __name__ == '__main__':
    main()
