"""Converter text goes to the text pack, never into a tracked header.

A converter that used to embed original dialogue in a generated header now
writes `text-fragment.json` beside its converted output and puts "@table/key"
in the header; tools/psp/pack_text.py packs every fragment into `text.bin`.
"""
import json
from pathlib import Path

NAME = 'text-fragment.json'


def write(directory, table, texts):
    """Write {table/key: text}; return {key: "@table/key"} for the header."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    data = {f'{table}/{key}': text for key, text in texts.items()}
    (directory / NAME).write_text(json.dumps(data, indent=1, sort_keys=True, ensure_ascii=False) + '\n',
                                  encoding='utf-8', newline='\n')
    return {key: f'@{table}/{key}' for key in texts}
