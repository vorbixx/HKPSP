"""Export/verify the bounded dependency graph of the 22 static Tutorial decors.

Original data stays private. Verification replays the same traversal offline;
serialized particle settings do not establish runtime particle counts/physics.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import wave

from assetripper_client import Client, ROOT
from convert_decor import observe
from extract_crawler import Export, native_component_kind
from extract_plant_hit_dependencies import FIELDS, references

DEFAULT = ROOT / '_private/research/decor-effects'
ROOM = ROOT / '_private/research/room-visuals'
INPUT = ROOT / '_private/analysis-input'
MANIFEST = 'decor-effects.json'
# Files used by this export. Other scenes may also be present in analysis-input.
ASSET_INPUTS = (
    'globalgamemanagers', 'globalgamemanagers.assets', 'globalgamemanagers.assets.resS',
    'level0', 'level6',
    'resources.assets', 'resources.assets.resS', 'resources.resource',
    'sharedassets0.assets', 'sharedassets0.assets.resS', 'sharedassets0.resource',
    'sharedassets6.assets', 'sharedassets6.assets.resS', 'sharedassets6.resource',
)
# Ownership is traversed explicitly. Mixer routing and editor provenance do not
# supply effect visuals, clips or authored particle/rigidbody parameters.
SKIP = {'m_CorrespondingSourceObject', 'm_PrefabAsset', 'm_PrefabInstance',
        'm_GameObject', 'm_Father', 'm_OutputAudioMixerGroup'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def local(name):
    require(Path(name).name == name and '\\' not in name, 'Nonlocal dependency: ' + name)
    return name


def file_hash(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def analysis_inputs(root=INPUT):
    data = root / 'hollow_knight_Data'
    managed = data / 'Managed'
    return sorted([p for p in managed.iterdir() if p.is_file()] +
                  [data / name for name in ASSET_INPUTS])


def verify_analysis_inputs(hashes, root=INPUT):
    paths = analysis_inputs(root)
    require(all(path.is_file() for path in paths) and
            {p.relative_to(root).as_posix() for p in paths} <= hashes.keys(),
            'Incomplete input provenance')
    # Older exports recorded extra scenes present in the staging folder. Keep
    # checking every recorded hash while requiring all dependencies used here.
    for name, expected in hashes.items():
        # Normalize without following links: the input folder may be a
        # junction to the read-only game data; '..' escapes stay rejected.
        path = Path(os.path.abspath(root / name))
        require(path.is_relative_to(Path(os.path.abspath(root))), 'Nonlocal input')
        require(file_hash(path) == expected, 'Changed analysis input: ' + name)


class Store:
    def __init__(self, output, client=None, report=None):
        self.output, self.client = output, client
        self.export = Export(client, output) if client else None
        self.cache, self.tables, self.hashes, self.paths = {}, {}, {}, {}
        self.media = {} if client else report['media']
        if client:
            row = next(r for r in client.search('Crossroads Statue Horned')
                       if r['type'] == 'GameObject' and r['collection'] == 'level6')
            self.register(row['path']['C'])
        else:
            self.tables = report['dependency_tables']
            self.hashes = report['source_sha256']
            for key, expected in self.hashes.items():
                raw = (output / local(key)).read_bytes()
                require(sha(raw) == expected, 'Changed dependency: ' + key)
                if key.endswith('.json'):
                    self.cache[key] = json.loads(raw)
        self.used, self.used_media = set(), set()

    def register(self, path):
        name, deps = self.export.collection(path)
        self.paths[name] = path
        for dep, label in deps.values():
            self.paths[label] = dep
        return {str(i): label for i, (_, label) in deps.items()}

    def get(self, key):
        local(key)
        self.used.add(key)
        if key not in self.cache and self.client:
            require(len(self.cache) < 2048, 'Dependency graph exceeded export bound')
            collection, number = key.removesuffix('.json').rsplit('-', 1)
            path = self.paths[collection]
            self.tables[key] = self.register(path)
            self.cache[key], actual = self.export.fetch(path, int(number))
            require(actual == key, 'Collection name mismatch')
            self.hashes[key] = self.export.hashes[key]
        require(key in self.cache and key in self.tables, 'Missing dependency: ' + key)
        return self.cache[key]

    def resolve(self, source, ref):
        self.get(source)
        require(ref['m_PathID'] != 0, 'Required reference is null: ' + source)
        table = self.tables[source]
        require(str(ref['m_FileID']) in table, 'Missing dependency table entry: ' + source)
        key = f'{table[str(ref["m_FileID"])]}-{ref["m_PathID"]}.json'
        return key, self.get(key)

    def save(self, name, raw):
        (self.output / local(name)).write_bytes(raw)
        self.hashes[name] = sha(raw)

    def binary(self, key, kind):
        self.used_media.add(key)
        if self.client and key not in self.media:
            collection, number = key.removesuffix('.json').rsplit('-', 1)
            path = json.dumps(dict(C=self.paths[collection], D=int(number)))
            stem = key.removesuffix('.json')
            if kind == 'texture':
                name = stem + '.png'
                self.save(name, self.client.request('/Assets/Image', Path=path, Extension='png'))
                self.media[key] = dict(kind=kind, png=name)
            else:
                import soundfile as sf
                raw = self.client.request('/Assets/Audio', Path=path)
                self.save(stem + '.audio', raw)
                samples, rate = sf.read(io.BytesIO(raw), dtype='int16', always_2d=True)
                target = io.BytesIO()
                sf.write(target, samples, rate, format='WAV', subtype='PCM_16')
                self.save(stem + '.wav', target.getvalue())
                self.media[key] = dict(kind=kind, encoded=stem + '.audio', pcm=stem + '.wav',
                                       frames=len(samples), channels=samples.shape[1], rate=rate)
        require(key in self.media and self.media[key]['kind'] == kind, 'Missing media: ' + key)
        row = self.media[key]
        names = [row['png']] if kind == 'texture' else [row['encoded'], row['pcm']]
        for name in names:
            require(name in self.hashes, 'Untracked media: ' + name)
        if kind == 'texture':
            from PIL import Image
            with Image.open(self.output / row['png']) as image:
                image.load()
                data = self.get(key)
                require(image.size == (data['m_Width'], data['m_Height']), 'Texture dimensions changed')
        else:
            with wave.open(str(self.output / row['pcm']), 'rb') as audio:
                require((audio.getnframes(), audio.getnchannels(), audio.getframerate(),
                         audio.getsampwidth(), audio.getcomptype()) ==
                        (row['frames'], row['channels'], row['rate'], 2, 'NONE') and
                        row['frames'] > 0, 'Invalid PCM: ' + key)


def derive(store, observation):
    objects, visited, excluded, particles, audio_tables, script_types = {}, set(), [], {}, {}, {}

    def walk(key):
        data = store.get(key)
        if key in visited:
            return
        visited.add(key)
        if 'm_Components' in data:
            row = dict(name=data['m_Name'], active=data['m_IsActive'], components={}, children=[])
            objects[key] = row
            for item in data['m_Components']:
                ck, component = store.resolve(key, item['m_Component'])
                kind = native_component_kind(component)
                if isinstance(component.get('m_Script'), dict):
                    _, script = store.resolve(ck, component['m_Script'])
                    kind = script['m_ClassName']
                row['components'].setdefault(kind or 'Unclassified', []).append(ck)
                walk(ck)
                if kind == 'Transform':
                    for ref in component['m_Children']:
                        tk, child = store.resolve(ck, ref)
                        child_key, _ = store.resolve(tk, child['m_GameObject'])
                        row['children'].append(child_key)
                        walk(child_key)
            return
        if isinstance(data.get('m_GameObject'), dict) and data['m_GameObject']['m_PathID']:
            owner, _ = store.resolve(key, data['m_GameObject'])
            walk(owner)
        if 'm_TextureSettings' in data and 'm_Width' in data:
            store.binary(key, 'texture')
        if 'm_CompressionFormat' in data and 'm_Channels' in data:
            store.binary(key, 'audio')
        if 'm_InitialModule' in data:
            # Complete module dictionaries, including curves, are retained in JSON.
            particles[key] = [k for k in data if k.endswith('Module')]
        if isinstance(data.get('m_Script'), dict):
            _, script = store.resolve(key, data['m_Script'])
            script_types[key] = script['m_ClassName']
            if script['m_ClassName'] in ('AudioClipTable', 'RandomAudioClipTable'):
                audio_tables[key] = data['m_Structure']
        for location, ref in references(data):
            if location.split('/')[1] in SKIP:
                excluded.append(dict(source=key, field=location, reason='ownership/editor/mixer routing'))
                continue
            target, _ = store.resolve(key, ref)
            walk(target)

    records, anchors = [], {}
    for record in observation['records']:
        owner = record['whole']
        key = f'level6-{owner}.json'
        go = store.get(key)
        for name in [key] + [store.resolve(key, x['m_Component'])[0] for x in go['m_Components']]:
            require(store.hashes[name] == observation['source_sha256'][name], 'Changed live root: ' + name)
            anchors[name] = store.hashes[name]
        component = record['component']
        data = store.get(component)['m_Structure']
        # Root components are anchors, not traversal roots: whole/remnant artwork
        # and scene parents belong to the already accepted room export.
        debris, prefabs = [], {}
        for ref in data['debrisParts']:
            dk, _ = store.resolve(component, ref)
            debris.append(dk)
            walk(dk)
        for field in FIELDS:
            ref = data[field]
            if not ref['m_PathID']:
                prefabs[field] = None
                continue
            tk, transform = store.resolve(component, ref)
            pk, _ = store.resolve(tk, transform['m_GameObject'])
            prefabs[field] = dict(reference=tk, owner=pk)
            walk(pk)
        audio = {}
        for field, ref in [('event_clip', data['breakAudioEvent']['Clip']),
                           ('clip_table', data['breakAudioClipTable'])]:
            audio[field] = None
            if ref['m_PathID']:
                ak, _ = store.resolve(component, ref)
                audio[field] = ak
                walk(ak)
        records.append(dict(owner=owner, breakable=component, debris=debris, prefabs=prefabs,
                            audio=audio, audio_event=data['breakAudioEvent']))
    return dict(records=records, objects=objects, anchors=anchors, particles=particles,
                audio_tables=audio_tables, script_types=script_types, excluded=excluded)


def extract(client, output):
    require(output.resolve().is_relative_to((ROOT / '_private').resolve()), 'Export must remain private')
    require(not output.exists(), 'Use a fresh export directory')
    observation = observe(ROOM)
    require(len(observation['records']) == 22, 'Expected 22 static decors')
    # Hash only the explicitly loaded input folder, never the Steam installation.
    inputs = {p.relative_to(INPUT).as_posix(): file_hash(p) for p in analysis_inputs()}
    output.mkdir(parents=True)
    store = Store(output, client)
    derived = derive(store, observation)
    behavior_names = {'Breakable', 'AudioEvent', 'AudioClipTable'} | set(derived['script_types'].values())
    behavior_dir = ROOT / '_private/research/decompiled/all'
    behavior = {n + '.cs': file_hash(behavior_dir / (n + '.cs')) for n in sorted(behavior_names)
                if (behavior_dir / (n + '.cs')).exists()}
    for name in behavior:
        store.save(name, (behavior_dir / name).read_bytes())
    report = dict(schema=1, build=22529139, scene='Tutorial_01',
                  input_sha256=inputs, room_manifest_sha256=observation['manifest_sha256'],
                  derived=derived, dependency_tables=store.tables, media=store.media,
                  behavior_sha256=behavior, source_sha256=store.hashes,
                  limitation='Authored dependencies only; no runtime physics/counts or mixer reconstruction. '
                             'Whole/remnant artwork remains in the room export.')
    (output / MANIFEST).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return verify_saved(output)


def verify_saved(output):
    report = json.loads((output / MANIFEST).read_bytes())
    require((report['schema'], report['build'], report['scene']) == (1, 22529139, 'Tutorial_01'),
            'Changed schema/build/scene')
    observation = observe(ROOM)
    require(len(observation['records']) == 22 and
            report['room_manifest_sha256'] == observation['manifest_sha256'], 'Changed room manifest')
    verify_analysis_inputs(report['input_sha256'])
    store = Store(output, report=report)
    require(derive(store, observation) == report['derived'], 'Incomplete or changed effect graph')
    require(store.used_media == report['media'].keys(), 'Unaccounted media')
    require(set(store.tables) == store.used, 'Unaccounted dependency tables')
    media_files = {v for row in report['media'].values() for k, v in row.items()
                   if k in ('png', 'pcm', 'encoded')}
    require(set(report['source_sha256']) == store.used | media_files | report['behavior_sha256'].keys(),
            'Incomplete source inventory')
    for name, expected in report['behavior_sha256'].items():
        require(report['source_sha256'][name] == expected == file_hash(
            ROOT / '_private/research/decompiled/all' / local(name)), 'Changed behavior hash')
    behavior_names = {'Breakable', 'AudioEvent', 'AudioClipTable'} | set(report['derived']['script_types'].values())
    require(set(report['behavior_sha256']) == {n + '.cs' for n in behavior_names
            if (ROOT / '_private/research/decompiled/all' / (n + '.cs')).exists()},
            'Incomplete behavior provenance')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path, default=DEFAULT)
    parser.add_argument('--decoder-dir', type=Path, default=ROOT / '_private/tools/audio-python')
    parser.add_argument('--verify', action='store_true')
    args = parser.parse_args()
    sys.path.insert(0, str(args.decoder_dir.resolve()))
    report = verify_saved(args.output) if args.verify else extract(Client(args.port), args.output)
    print(json.dumps(dict(records=len(report['derived']['records']), objects=len(report['derived']['objects']),
                          particles=len(report['derived']['particles']), media=len(report['media']),
                          files=len(report['source_sha256']))))


if __name__ == '__main__':
    main()
