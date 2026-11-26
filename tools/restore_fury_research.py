"""Restore missing Fury chest/presentation snapshots from the original Tutorial.

Run under validation-lock.ps1 -Exclusive with this task's verified original
assets loaded in AssetRipper. Reference maps contain no invented game values.
"""
import argparse
import base64
import hashlib
import json

from assetripper_client import Client, ROOT
from extract_crawler import Export, write_asset
from inspect_fsm import decode_fsm
from restore_managed_research import ORIGINAL_SHA256, verify_collection


class Snapshot:
    def __init__(self, client, group, output=None):
        self.client = client
        self.stage = output or ROOT / '_private/converted/research-fury-stage' / group
        self.output = output
        self.stage.mkdir(parents=True, exist_ok=True)
        self.export = Export(client, self.stage)
        self.mapping = dict(build=22529139, original_sha256=ORIGINAL_SHA256,
                            visual_assets={}, trees={}, fsms={})

    def save(self, name, value):
        raw = (json.dumps(value, indent=2) + '\n').encode()
        write_asset(self.stage / name, raw)
        self.export.hashes[name] = hashlib.sha256(raw).hexdigest()

    def texture(self, collection, ident):
        _, name = self.export.fetch(collection, ident)
        row = self.mapping['visual_assets'].setdefault(name, dict(references={}))
        png = name.removesuffix('.json') + '.png'
        if png not in self.export.hashes:
            raw = self.client.request('/Assets/Image', Path=json.dumps({'C': collection, 'D': ident}), Extension='png')
            if not raw.startswith(b'\x89PNG\r\n\x1a\n'):
                raise ValueError('Expected original texture PNG')
            write_asset(self.stage / png, raw)
            self.export.hashes[png] = hashlib.sha256(raw).hexdigest()
        row['png'] = png
        return name

    def font(self, collection):
        font, _ = self.export.fetch(collection, 1405)
        if font['m_Name'] != 'Perpetua':
            raise ValueError('Expected original Perpetua face')
        raw = base64.b64decode(font['m_FontData'], validate=True)
        write_asset(self.stage / 'Perpetua.ttf', raw)
        self.export.hashes['Perpetua.ttf'] = hashlib.sha256(raw).hexdigest()

    def visual(self, collection, ident):
        data, name = self.export.fetch(collection, ident)
        if name in self.mapping['visual_assets']:
            return name
        row = self.mapping['visual_assets'][name] = dict(references={})
        structure = data.get('m_Structure', {})

        def ref(label, value, texture=False):
            if not value['m_PathID']:
                return
            _, target, owner = self.export.resolve(collection, value)
            row['references'][label] = (self.texture(owner, value['m_PathID']) if texture
                                        else self.visual(owner, value['m_PathID']))
            return target, owner

        if 'm_RD' in data:
            ref('m_RD/m_Texture', data['m_RD']['m_Texture'], texture=True)
        if 'm_Sprite' in data:
            ref('m_Sprite', data['m_Sprite'])
        for index, value in enumerate(data.get('m_Materials', [])):
            ref(f'm_Materials/{index}', value)
        envs = data.get('m_SavedProperties', {}).get('m_TexEnvs', {})
        if '_MainTex' in envs:
            ref('m_SavedProperties/m_TexEnvs/_MainTex/m_Texture', envs['_MainTex']['m_Texture'], texture=True)
        if 'collection' in structure:
            ref('collection', structure['collection'])
        for index, value in enumerate(structure.get('textures', [])):
            ref(f'textures/{index}', value, texture=True)
        if 'library' in structure:
            resolved = ref('library', structure['library'])
            if resolved:
                library_name, owner = resolved
                library = self.export.cache[library_name]['m_Structure']
                library_refs = self.mapping['visual_assets'][library_name]['references']
                for clip in library['clips']:
                    for frame in clip['frames']:
                        value = frame['spriteCollection']
                        _, _, sprite_owner = self.export.resolve(owner, value)
                        target = self.visual(sprite_owner, value['m_PathID'])
                        label = f"clip/{clip['name']}/{frame['spriteId']}"
                        row['references'][label] = library_refs[label] = target
        return name

    def walk(self, collection, ident, path):
        parts = self.export.components(collection, ident)
        go = self.export.cache[parts['GameObject'][0]]
        transform_key = parts['Transform'][0]
        components = [transform_key] + [name for kind, names in parts.items()
                                       if kind not in ('GameObject', 'Transform') for name in names]
        self.mapping['trees'][path] = dict(name=go['m_Name'], components=components)
        for name in components:
            self.visual(collection, int(name.rsplit('-', 1)[1].removesuffix('.json')))
        for name in parts.get('PlayMakerFSM', []):
            decoded = name.removesuffix('.json') + '-fsm.json'
            self.save(decoded, decode_fsm(self.export.cache[name]['m_Structure']['fsm']))
            self.mapping['fsms'][name] = decoded
        for value in self.export.cache[transform_key]['m_Children']:
            child, _, owner = self.export.resolve(collection, value)
            child_id = child['m_GameObject']['m_PathID']
            self.walk(owner, child_id, path + '/' + str(child_id))

    def publish(self, group, manifest):
        self.mapping['source_sha256'] = dict(self.export.hashes)
        self.save(manifest, self.mapping)
        if self.output is not None:
            print(group + ': exported ' + str(len(self.export.hashes)) + ' original/derived files', flush=True)
            return
        output = ROOT / '_private/research' / group
        for name in self.export.hashes:
            target = output / name
            if target.exists() and target.read_bytes() != (self.stage / name).read_bytes():
                raise ValueError('Refusing different existing source: ' + str(target))
        for name in self.export.hashes:
            write_asset(output / name, (self.stage / name).read_bytes())
        print(group + ': restored ' + str(len(self.export.hashes)) + ' original/derived files', flush=True)


def restore(port, group, *, client=None, assembly=None, output=None):
    assembly = assembly or ROOT / '_private/converted/research-input/hollow_knight_Data/Managed/Assembly-CSharp.dll'
    if hashlib.sha256(assembly.read_bytes()).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Original assembly SHA-256 mismatch')
    client = client or Client(port)
    resource = next(r['path']['C'] for r in client.search('Hollow Shade')
                    if r['collection'] == 'resources.assets' and r['path']['D'] == 4541)
    verify_collection(client, resource, 'resources.assets')
    snapshot = Snapshot(client, group, output)
    if group == 'fury-chest':
        scene = next(r['path']['C'] for r in client.search('Shiny Item (1)')
                     if r['collection'] == 'level6' and r['path']['D'] == 2549)
        ui = next(r['path']['C'] for r in client.search('Charm Tutorial Msg')
                  if r['collection'] == 'sharedassets6.assets' and r['path']['D'] == 491)
        verify_collection(client, scene, 'level6')
        verify_collection(client, ui, 'sharedassets6.assets')
        snapshot.walk(scene, 1759, 'Chest')
        snapshot.walk(ui, 491, 'CharmTutorial')
        snapshot.mapping['fury_icon'] = dict(sprite=snapshot.visual(resource, 1711))
        snapshot.font(resource)
        snapshot.publish(group, 'fury-chest-map.json')
    elif group == 'fury-font':
        snapshot.font(resource)
        snapshot.publish('fury-chest', 'restored-font.json')
    else:
        for ident in (9607, 14616, 21845, 24232):
            snapshot.visual(resource, ident)
        for ident in (23926,):
            data, name = snapshot.export.fetch(resource, ident)
            snapshot.save(name.removesuffix('.json') + '-fsm.json', decode_fsm(data['m_Structure']['fsm']))
        clip, _ = snapshot.export.fetch(resource, 1130)
        curves = [curve for curve in clip['m_PPtrCurves']
                  if curve['m_Attribute'] == 'm_Sprite' and not curve['m_Path']]
        if len(curves) != 1:
            raise ValueError('Expected original pickup burst sprite curve')
        frames = []
        for key in curves[0]['m_Curve']:
            ref = key['m_Value']
            _, _, owner = snapshot.export.resolve(resource, ref)
            frames.append(dict(time=key['m_Time'], sprite=snapshot.visual(owner, ref['m_PathID'])))
        snapshot.mapping['groups'] = dict(pickup_burst=dict(frames=frames))
        snapshot.publish(group, 'fury-presentation-map.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58301)
    parser.add_argument('--only', choices=('fury-chest', 'fury-font', 'fury-presentation-source-20261022'), required=True)
    args = parser.parse_args()
    restore(args.port, args.only)
