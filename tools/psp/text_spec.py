"""Rebuild the port's text from the original language sheets.

The repository keeps no original text. `text_spec.json` maps each key the
code uses ("@Sheet/ENTRY", optionally "#n" for another form of one entry) to
its sheet, entry and the edits that turn the original entry into the port's
string: page selection, line breaks and ASCII punctuation. An edit is
[start, end, replacement] over the original entry text; replacements carry
only punctuation and whitespace, never words (`check_edits`).
"""
import difflib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET

SPEC = Path(__file__).resolve().with_name('text_spec.json')
# Edits may add punctuation the port spells in ASCII, never original words.
MAX_LETTERS = 3


def sheet_entries(path):
    """Entry name -> text exactly as the game reads it (entities decoded)."""
    return {entry.get('name'): ''.join(entry.itertext()) for entry in ET.parse(path).getroot().iter('entry')}


def derive(original, literal):
    """Edits that turn `original` into `literal`."""
    edits = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, original, literal, autojunk=False).get_opcodes():
        if tag != 'equal':
            edits.append([i1, i2, literal[j1:j2]])
    return edits


def apply(original, edits):
    out, position = [], 0
    for start, end, replacement in edits:
        if start < position or end < start or end > len(original):
            raise ValueError(f'Edit {start}:{end} out of order or range')
        out += [original[position:start], replacement]
        position = end
    out.append(original[position:])
    return ''.join(out)


def check_edits(key, edits):
    letters = sum(len(re.findall(r'[A-Za-z]', replacement)) for _, _, replacement in edits)
    if letters > MAX_LETTERS:
        raise ValueError(f'{key}: edits add {letters} letters; the port text must come from the original')


def load_spec(path=SPEC):
    return json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}


def build(spec, sheets):
    """Key (without '@') -> text, from `sheets` (sheet name -> entries)."""
    texts = {}
    for key, item in spec.items():
        entries = sheets.get(item['sheet'])
        if entries is None:
            raise ValueError(f'{key}: original sheet {item["sheet"]} not exported')
        if item['entry'] not in entries:
            raise ValueError(f'{key}: {item["sheet"]} has no entry {item["entry"]}')
        check_edits(key, item.get('edits', []))
        original = entries[item['entry']]
        # Titles the port shows in capitals keep that as a flag, not as text.
        texts[key] = apply(original.upper() if item.get('upper') else original, item.get('edits', []))
    return texts
