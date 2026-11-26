"""Run one offline converter, recording its checkout file accesses for graph guards."""
import json
import importlib.machinery
import os
from pathlib import Path
import runpy
import sys
import converted_files
from regen_memory import process_peak_bytes


def main():
    root = Path(sys.argv[1]).absolute()
    report = Path(sys.argv[2])
    script, *arguments = sys.argv[3:]
    reads, writes = set(), set()
    active = True
    sys.dont_write_bytecode = True
    # Worktrees share snapshots/tools via junctions. Attribute physical accesses
    # back to the checkout even if a compiler resolves one of those junctions.
    aliases = []
    private = root / '_private'
    if private.is_dir():
        for child in private.iterdir():
            resolved = child.resolve()
            if resolved != child:
                aliases.append((resolved, child))

    class SourceLoader(importlib.machinery.SourceFileLoader):
        def get_code(self, fullname):
            # Content hashes may change while size/mtime stay unchanged. Never let
            # Python's timestamp-based .pyc cache execute the old compiler bytes.
            return self.source_to_code(self.get_data(self.path), self.path)

    class SourceFinder:
        def find_spec(self, fullname, path=None, target=None):
            spec = importlib.machinery.PathFinder.find_spec(fullname, path, target)
            if (spec and spec.origin and spec.origin.endswith('.py') and Path(spec.origin).is_relative_to(root)
                    and not bundled(Path(spec.origin))):
                spec.loader = SourceLoader(fullname, spec.origin)
                return spec

    # The player bundle keeps its interpreter inside the checkout (runtime/python).
    # Its standard library and packages belong to the interpreter, not to the
    # converter inputs (found on Linux, where the library is plain .py files).
    interpreter = [Path(os.path.abspath(prefix)) for prefix in {sys.prefix, sys.base_prefix}]

    def bundled(path):
        return any(path.is_relative_to(prefix) for prefix in interpreter if prefix != root)

    sys.meta_path.insert(0, SourceFinder())

    def audit(event, args):
        if not active or converted_files.is_internal_access():
            return
        if event == 'socket.connect':
            raise RuntimeError('Asset regeneration must use saved exports, without network access')
        if event != 'open' or not isinstance(args[0], (str, bytes, os.PathLike)):
            return
        raw_path = os.fsdecode(args[0])
        if os.path.normcase(raw_path) == os.path.normcase(os.devnull):
            return
        # Normalize .. lexically, retaining checkout junction attribution.
        path = Path(os.path.abspath(raw_path))
        for resolved, lexical in aliases:
            if path.is_relative_to(resolved):
                path = lexical / path.relative_to(resolved)
                break
        if not path.is_relative_to(root) or path.suffix == '.pyc' or bundled(path):
            return
        relative = path.relative_to(root).as_posix()
        mode, flags = args[1:3]
        if flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_APPEND):
            writes.add(relative)
            if relative.startswith('_private/research/'):
                raise RuntimeError('Asset regeneration cannot modify saved research: ' + relative)
        elif path.is_file():
            # Optional package probes for absent files are not converter inputs.
            reads.add(relative)

    sys.addaudithook(audit)
    sys.argv = [script, *arguments]
    sys.path.insert(0, str((root / script).parent))
    try:
        runpy.run_path(str(root / script), run_name='__main__')
    finally:
        active = False
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(dict(reads=sorted(reads), writes=sorted(writes),
                                          peakMemoryBytes=process_peak_bytes()), indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
