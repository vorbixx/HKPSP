"""Decompile the original managed assembly for the offline converters."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from restore_managed_research import ORIGINAL_SHA256
from extract_crawler import write_asset


def crlf(data):
    # ILSpy writes the platform's line ending. The converters pin source hashes
    # taken from the Windows (CRLF) export, so both platforms store CRLF.
    return data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')


def expected(signature, types):
    value = dict(build=22529139, assembly_sha256=signature, line_endings='crlf')
    return value if types is None else dict(value, types=types)


def prepare(ilspy, assembly, output):
    output = output.absolute()
    if output.resolve() != output:
        raise ValueError(f'Managed export requires a direct path: {output}')
    with assembly.open('rb') as stream:
        signature = hashlib.file_digest(stream, 'sha256').hexdigest()
    if signature != ORIGINAL_SHA256:
        raise ValueError('Managed export requires the reviewed Steam build 22529139')
    manifest = output / 'steam-managed.json'
    if manifest.is_symlink():
        raise ValueError(f'Managed export requires a regular manifest: {manifest}')
    if manifest.is_file() and {k: v for k, v in json.loads(manifest.read_text()).items() if k != 'types'} == expected(signature, None):
        print('Managed Steam export already prepared')
        return
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.decompile-', dir=output) as temporary:
        pending = Path(temporary) / 'all'
        subprocess.run([str(ilspy), '--disable-updatecheck', '--project', '--outputdir', str(pending),
                        '--referencepath', str(assembly.parent), str(assembly)], check=True)
        sources = sorted(pending.rglob('*.cs'))
        if not all((pending / name).is_file() for name in ('HeroController.cs', 'StalactiteControl.cs',
                           'HutongGames.PlayMaker.Actions/ChaseObject.cs', 'StringEncrypt.cs')):
            raise ValueError('ILSpy output is missing required type files')
        for source in sources:
            destination = output / 'all' / source.relative_to(pending)
            if destination.resolve() != destination or (destination.exists() and not destination.is_file()):
                raise ValueError(f'Managed export requires a regular source destination: {destination}')
            data = crlf(source.read_bytes())
            if not destination.exists() or destination.read_bytes() != data:
                write_asset(destination, data)
        value = expected(signature, len(sources))
        write_asset(manifest, (json.dumps(value, indent=2) + '\n').encode('utf-8'))
    print(f'Prepared {len(sources)} original managed source files')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ilspy', type=Path, required=True)
    parser.add_argument('--assembly', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.ilspy.resolve(strict=True), args.assembly.resolve(strict=True), args.output)
