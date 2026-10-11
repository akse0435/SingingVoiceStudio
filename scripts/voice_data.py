"""voice_data.py -- gather the voice data a release carries, for a machine that
has none of it (the release workflow, .github/workflows/release.yml).

    python scripts/voice_data.py OUT

writes, laid out as the build scripts look for them in this tree:

    OUT/assets/...                 VocalWriter 2.0's files, from the VocalWriter
                                   Studio repository (github.com/masonasons/vocalwriter)
    OUT/voices/microsoft/*.spd     Microsoft Sam, Mike and Mary, and their .sdf
                                   files, from the newest release of this
                                   program that carries them in its Windows zip

DECtalk is built from source and the SSI-263 needs nothing, so neither is
here. Needs git, and the GitHub CLI (gh) signed in or given GH_TOKEN.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

VOCALWRITER = 'https://github.com/masonasons/vocalwriter.git'
VW_FILES = [
    'assets/VocalWriter.app/Contents/Resources/VocalWriter.rsrc',
    'assets/GMSpeech.rsrc',
    'assets/GMBank.rsrc',
    'assets/EnglishLex',
    'assets/License Agreement.rtf',
]
MS_FILES = ['Sam.spd', 'Sam.sdf', 'Mike.spd', 'Mike.sdf', 'Mary.spd', 'Mary.sdf']
# the Windows zip, not the disk image or anything else on a release
WINDOWS_ZIP = re.compile(r'^SingingVoiceStudio-[0-9][^/]*\.zip$')


def run(*args, **kw):
    return subprocess.run(args, check=True, capture_output=True, text=True, **kw).stdout


def vocalwriter(out, work):
    repo = os.path.join(work, 'vocalwriter')
    run('git', 'clone', '-q', '--depth', '1', VOCALWRITER, repo)
    for f in VW_FILES:
        dest = os.path.join(out, *f.split('/'))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copyfile(os.path.join(repo, *f.split('/')), dest)
        print('VocalWriter: %s (%d bytes)' % (f, os.path.getsize(dest)))


def microsoft(out, work):
    releases = json.loads(run('gh', 'release', 'list', '--limit', '50', '--exclude-drafts',
                              '--json', 'tagName,publishedAt'))
    releases.sort(key=lambda r: r['publishedAt'], reverse=True)
    for r in releases:
        tag = r['tagName']
        assets = json.loads(run('gh', 'release', 'view', tag, '--json', 'assets'))['assets']
        names = [a['name'] for a in assets if WINDOWS_ZIP.match(a['name'])]
        if not names:
            continue
        where = os.path.join(work, tag)
        run('gh', 'release', 'download', tag, '--pattern', names[0], '--dir', where)
        got = {}
        with zipfile.ZipFile(os.path.join(where, names[0])) as z:
            for info in z.infolist():
                # written on Windows, the names may have backslashes in them
                path = info.filename.replace('\\', '/')
                parts = path.split('/')
                if len(parts) >= 3 and parts[-3:-1] == ['voices', 'microsoft']:
                    for want in MS_FILES:
                        if parts[-1].lower() == want.lower():
                            got[want] = z.read(info)
        if len(got) != len(MS_FILES):
            print('%s: its zip has only %s of the Microsoft voices' % (tag, sorted(got)))
            continue
        dest = os.path.join(out, 'voices', 'microsoft')
        os.makedirs(dest, exist_ok=True)
        for name, data in got.items():
            with open(os.path.join(dest, name), 'wb') as f:
                f.write(data)
            print('Microsoft: %s (%d bytes, from %s %s)' % (name, len(data), tag, names[0]))
        return
    sys.exit('no release has a Windows zip with Microsoft Sam, Mike and Mary in it')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix='svs-data-')
    try:
        vocalwriter(out, work)
        microsoft(out, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
