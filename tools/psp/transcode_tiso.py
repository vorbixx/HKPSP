"""Convert the original Town Tiso idle and conversations."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from transcode_elderbug import convert, room


if __name__ == '__main__':
    convert(room.source_root(), room.REPO/'_private/converted-psp/tiso', 'tiso')
