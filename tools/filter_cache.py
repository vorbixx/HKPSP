"""Verified content cache for expensive room-art filtering."""
import hashlib
from functools import lru_cache
import os
from pathlib import Path
import tempfile

import PIL
from PIL import Image

from converted_files import internal_access
from regen_cache import OutputCache, graph_lock

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = 'filtered.rgba'
MIN_PIXELS = 512 * 512


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


@lru_cache(maxsize=1)
def tool_identity():
    return dict(pillow=PIL.__version__, core=sha256(Path(Image.core.__file__)),
                code=sha256(Path(__file__)), filter_code=sha256(ROOT / 'tools/room_filtering.py'))


def filtered(image, radius, calculate, *, directory=None):
    if (os.environ.get('HK3DS_REGEN_CACHE') == '0'
            or image.width * image.height < MIN_PIXELS):
        return calculate(image, radius)
    associated = image.convert('RGBa')
    pixels = associated.tobytes()
    calculating = False
    result = None
    try:
        with internal_access():
            identity = dict(kind='room-prefilter', schema=1, size=list(image.size),
                            pixels=hashlib.sha256(pixels).hexdigest(), radius=radius.hex(),
                            **tool_identity())
            work = ROOT / 'build/regen/filter-cache-work'
            work.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=work) as temporary:
                output = Path(temporary) / OUTPUT
                cache = OutputCache(ROOT, graph_lock, sha256, directory=directory,
                                    files={OUTPUT: output})
                def valid(record):
                    return (isinstance(record, dict) and isinstance(record.get('outputs'), dict)
                            and set(record['outputs']) == {OUTPUT})
                if cache.restore(identity, valid) is not None:
                    raw = output.read_bytes()
                    if len(raw) == image.width * image.height * 4:
                        return Image.frombytes('RGBA', image.size, raw)
                calculating = True
                result = calculate(image, radius)
                calculating = False
                output.write_bytes(result.tobytes())
                cache.publish(identity, {'outputs': {OUTPUT: sha256(output)}})
                return result
    except (OSError, ValueError, TypeError, TimeoutError):
        if calculating:
            raise
        return result if result is not None else calculate(image, radius)
