"""Export only Tutorial_01 reminder/Focus presentation and its source evidence."""
import argparse
import base64
import hashlib
import json
import re
import subprocess
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export
from inspect_fsm import decode_fsm


def decrypt_language(encoded):
    # Mirror the archived game's StringEncrypt.DecryptData; .NET supplies AES
    # without an additional Python dependency. The Steam install is never read.
    code = (ROOT / '_private/research/decompiled/all/StringEncrypt.cs').read_text()
    key = re.search(r'GetBytes\("([^"]+)"\)', code)[1]
    script = '''$d=[Console]::In.ReadToEnd()|ConvertFrom-Json
$a=[System.Security.Cryptography.Aes]::Create()
$a.Mode='ECB';$a.Padding='PKCS7';$a.Key=[Text.Encoding]::UTF8.GetBytes($d.key)
$b=[Convert]::FromBase64String($d.encoded)
$p=$a.CreateDecryptor().TransformFinalBlock($b,0,$b.Length)
[Console]::Out.Write([Convert]::ToBase64String($p))'''
    # 2026-11-25: the console writes text in the OEM code page, which broke the
    # Cyrillic RU sheets; the plaintext bytes travel as Base64 and decode here.
    output = subprocess.run(['pwsh','-NoProfile','-Command',script],input=json.dumps(dict(key=key,encoded=encoded)),
                            capture_output=True,text=True,encoding='utf-8',check=True).stdout
    return base64.b64decode(output).decode('utf-8')


def extract(client, output):
    output = output.absolute()
    if not output.is_relative_to(ROOT / '_private'):
        raise ValueError('Original resources must remain private')
    output.mkdir(parents=True, exist_ok=True)
    e = Export(client, output)
    rows = client.search('Control Reminder')
    rc = next(r['path']['C'] for r in rows if r['collection'] == 'resources.assets')
    lc = next(r['path']['C'] for r in client.search('Interact Reminder') if r['collection'] == 'level6')
    _, key = e.fetch(rc, 20600)
    if (output / key).read_bytes() != (ROOT / '_private/research/assets/c1-20600.json').read_bytes():
        raise ValueError('Source build differs from 22529139')
    objects, fonts, sprites, fsms = {}, {}, {}, {}

    def visit(c, owner, children=False):
        prefix, _ = e.collection(c)
        identity = f'{prefix}:{owner}'
        if identity in objects:
            return
        parts = e.components(c, owner)
        objects[identity] = parts
        for filename in parts.get('Transform', []) + parts.get('RectTransform', []):
            transform = e.cache[filename]
            parent = transform['m_Father']
            # Preserve complete parent geometry for source trigger/world placement.
            if parent['m_PathID']:
                data, _, pc = e.resolve(c, parent)
                visit(pc, data['m_GameObject']['m_PathID'])
            if children:
                for ref in transform['m_Children']:
                    data, _, cc = e.resolve(c, ref)
                    visit(cc, data['m_GameObject']['m_PathID'], True)
        for filename in parts.get('PlayMakerFSM', []):
            fsms[filename] = decode_fsm(e.cache[filename]['m_Structure']['fsm'])
        for filename in parts.get('TextMeshPro', []):
            text = e.cache[filename]['m_Structure']
            font, fk, fc = e.resolve(c, text['m_fontAsset'])
            fonts[fk] = font['m_Structure']
            _, ak, ac = e.resolve(fc, font['m_Structure']['atlas'])
            png = ak.replace('.json', '.png')
            if png not in e.hashes:
                raw = client.request('/Assets/Image', Path=json.dumps({'C': ac, 'D': font['m_Structure']['atlas']['m_PathID']}), Extension='png')
                (output / png).write_bytes(raw)
                e.hashes[png] = hashlib.sha256(raw).hexdigest()
            for field in ('m_sourceFontFile', 'sourceFontFile'):
                ref = font['m_Structure'].get(field, {})
                if ref.get('m_PathID'):
                    e.resolve(fc, ref)
        for filename in parts.get('SpriteRenderer', []):
            ref = e.cache[filename]['m_Sprite']
            if not ref['m_PathID']:
                continue
            sprite, sk, sc = e.resolve(c, ref)
            texture_ref = sprite['m_RD']['m_Texture']
            _, tk, tc = e.resolve(sc, texture_ref)
            png = tk.replace('.json', '.png')
            if png not in e.hashes:
                raw = client.request('/Assets/Image', Path=json.dumps({'C': tc, 'D': texture_ref['m_PathID']}), Extension='png')
                (output / png).write_bytes(raw)
                e.hashes[png] = hashlib.sha256(raw).hexdigest()
            sprites[sk] = png

    visit(rc, 4400, True)
    visit(lc, 520, True)
    for owner in (1309, 1631, 2910, 1673, 1692, 709):
        visit(lc, owner)
    # Tablet actions own the Focus ColorFader events; damage/Soul own the reminder.
    for c, ids in ((lc, (12118,)), (rc, (21305, 21294, 21207))):
        for ident in ids:
            data, key = e.fetch(c, ident)
            fsms[key] = decode_fsm(data['m_Structure']['fsm'])
    language, _ = e.fetch(rc, 1064)
    xml = decrypt_language(language['m_Script']).encode('utf-8')
    (output / 'EN_Prompts-decrypted.xml').write_bytes(xml)
    e.hashes['EN_Prompts-decrypted.xml'] = hashlib.sha256(xml).hexdigest()
    mapping = dict(build_id=22529139, objects=objects, fonts=fonts, sprites=sprites,
                   source_sha256=e.hashes)
    (output / 'prompt-map.json').write_text(json.dumps(mapping, indent=2), encoding='utf-8')
    (output / 'fsm-decoded.json').write_text(json.dumps(fsms, indent=2), encoding='utf-8')
    print(f'Prompts: {len(objects)} owners, {len(fonts)} fonts, {len(e.hashes)} hashed dependencies')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path, default=ROOT / '_private/research/tutorial-reminders')
    args = parser.parse_args()
    extract(Client(args.port), args.output)
