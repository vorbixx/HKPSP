"""Resume completed scene conversions after an interrupted multi-room rule."""
import hashlib
import json
from pathlib import Path
from converted_files import unlink_converted


def _remove_tree(path, boundary):
    """Remove one owned output tree, detaching readonly shared files before chmod/unlink."""
    is_junction = getattr(path, 'is_junction', None)
    if path.is_symlink() or (is_junction and is_junction()):
        unlink_converted(path)
        return
    root = boundary.resolve()
    resolved = path.resolve()
    if path != boundary and root not in resolved.parents:
        unlink_converted(path)
        return
    if not path.is_dir():
        unlink_converted(path)
        return
    for child in path.iterdir():
        _remove_tree(child, boundary)
    try:
        path.rmdir()
    except PermissionError:
        path.chmod(path.stat().st_mode | 0o200)
        path.rmdir()


def snapshot(paths, excluded=()):
    """Hash content, including additions/removals; size/mtime alone are unsafe."""
    excluded = set(excluded)
    result = {}
    for base in paths:
        base = Path(base)
        if not base.exists():
            return None
        files = [base] if base.is_file() else sorted(base.rglob('*'))
        count = 0
        for path in files:
            if not path.is_file() or '__pycache__' in path.parts or path in excluded:
                continue
            with path.open('rb') as stream:
                result[str(path)] = hashlib.file_digest(stream, 'sha256').hexdigest()
            count += 1
        if not count:
            return None
    return result


def convert_cached(record, inputs, outputs, signature, action, force=False, preserved=()):
    """Rebuild owned output directories cleanly; checkpoint only complete results."""
    record = Path(record).resolve()
    inputs = [Path(path).resolve() for path in inputs]
    outputs = [Path(path).resolve() for path in outputs]
    preserved = {Path(path).resolve() for path in preserved}
    if any(path.parent not in outputs or path.is_dir() or path == record for path in preserved):
        raise ValueError('Preserved scene output must be a separate direct file')
    # Check absolute targets before recursive removal. Outputs must never own inputs.
    for output in outputs:
        if output == output.parent or any(
                source == output or output in source.parents or source in output.parents
                for source in inputs):
            raise ValueError('Scene output overlaps inputs or a filesystem root')
    temporary = record.with_suffix(record.suffix + '.tmp')
    excluded = (record, temporary, *preserved)
    source = snapshot(inputs)
    if source is None:
        raise ValueError('Missing scene conversion inputs')
    try:
        saved = json.loads(record.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        saved = {}
    if (not force and saved.get('version') == 1 and
            saved.get('signature') == signature and saved.get('inputs') == source and
            saved.get('outputs') is not None and
            saved['outputs'] == snapshot(outputs, excluded)):
        return False
    # A failed conversion must never retain a successful checkpoint.
    if record.exists():
        unlink_converted(record)
    for output in outputs:
        if output.exists():
            for child in output.iterdir():
                if child in preserved:
                    continue
                if child.is_dir():
                    _remove_tree(child, output)
                else:
                    unlink_converted(child)
        else:
            output.mkdir(parents=True)
    action()
    generated = snapshot(outputs, excluded)
    if generated is None:
        raise ValueError('Scene conversion produced incomplete outputs')
    record.parent.mkdir(parents=True, exist_ok=True)
    temporary.write_text(json.dumps(dict(version=1, signature=signature,
                                        inputs=source, outputs=generated)), encoding='utf-8')
    temporary.replace(record)
    return True
