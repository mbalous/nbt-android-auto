"""Build the local NbtAndroidAuto plugin package (XML, manifest, text/image zips). Never installs anything.
Usage: python make_aa_entry_resources.py <output-dir>   (libNbtAndroidAuto.so is built separately in the QNX VM)
Text ids 2..9 are the status lines the plugin switches between (aa_entry_plugin.cpp kTxt*)."""
from pathlib import Path
import shutil
import sys
import zipfile
import xml.etree.ElementTree as ET

source = Path(__file__).resolve().parent
output = Path(sys.argv[1]).resolve()
output.mkdir(parents=True, exist_ok=True)
for name in ('NbtAndroidAuto.xml', 'NbtAndroidAuto.manifest'):
    ET.parse(source / name)
    shutil.copyfile(source / name, output / name)
# keep every line short: the page shows about 25 characters per line (car 2026-11-08)
ENGLISH = {
    1: 'Android Auto',
    2: 'Connect phone via USB',
    3: 'No phone connected',
    4: 'Starting Android Auto ...',
    5: 'Android Auto connected',
    6: 'Reconnect the phone',
    7: 'No Android Auto device',
    8: 'Service not running',
    9: 'Connect only the phone',
    10: 'Picture mode',
    11: 'Split screen',
    12: 'Full width',
    13: 'Tip: 16:9 + split screen',
    14: 'Widescreen: a bit softer',
    15: 'To switch: unplug phone',
    16: 'Split screen (active)',
    17: 'Full width (active)',
    18: 'Picture mode saved',
    19: 'Restart Android Auto',
    20: 'Android Auto restarting',
    21: 'Restart system',
    22: 'Restart the head unit now?',
    23: 'Restart now',
    24: 'Restarting ...',
    25: 'Not yet: 30 s after boot',
    26: 'Restart already running',
    27: 'Open the page again',
    28: 'Gateway not reachable',
    29: 'Reset refused by the unit',
    30: 'No answer from gateway',
    31: 'Start at boot: on',
    32: 'Start at boot: off',
    33: 'Setting saved',
    34: 'Start at boot',
}
languages = ['de', 'en-UK', 'en-US', 'es-US', 'es-ES', 'fr-FR', 'fr-CA', 'it', 'pt', 'ru']
with zipfile.ZipFile(output / 'NbtAndroidAuto_ALL_Texts.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for language in languages:
        body = '100000=' + language + '\n' + ''.join('%d=%s\n' % (k, v) for k, v in sorted(ENGLISH.items()))
        archive.writestr(language + '.txt', body)
with zipfile.ZipFile(output / 'NbtAndroidAuto_ALL_Images.zip', 'w'):
    pass
print('Local package resources created:', output)
