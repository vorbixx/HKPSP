"""Host equivalent of the shared top-screen UI transforms."""
import re
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
LAYOUT = {key: float(value) for key, value in re.findall(
    r'(\w+)\s*=\s*(-?[\d.]+)f',
    (ROOT / 'include/top_screen_ui_layout.hpp').read_text())}


def on_top(layer, group):
    scale = LAYOUT[group + 'Scale']
    result = Image.new('RGBA', (400, 240))
    result.alpha_composite(layer.resize(
        (round(layer.width * scale), round(layer.height * scale)),
        Image.Resampling.NEAREST),
        (round(LAYOUT[group + 'X']), round(LAYOUT[group + 'Y'])))
    return result
