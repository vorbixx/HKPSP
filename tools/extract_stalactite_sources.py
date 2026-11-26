"""Export the bounded Tutorial_01 stalactite effect and audio dependency graph."""
import argparse
import hashlib
import io
import json
import sys
from pathlib import Path

from assetripper_client import Client, ROOT
from extract_crawler import Export, native_component_kind
from extract_decor_effects import Store, SKIP
from extract_plant_hit_dependencies import references

ROOTS = (352, 768, 878, 1316, 2569, 2751, 2950, 2993)
DEFAULT = ROOT / '_private/research/stalactite-sources'
OWNERSHIP = {'m_CorrespondingSourceObject', 'm_PrefabAsset', 'm_PrefabInstance',
             'm_GameObject', 'm_Father', 'm_OutputAudioMixerGroup', 'm_Children',
             'm_Component'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def extract(client, output):
    output = output.absolute()
    require(output.is_relative_to((ROOT / '_private').absolute()), 'Export must remain private')
    require(not output.exists(), 'Use a fresh stalactite source directory')
    matches = {row['path']['D']: row for row in client.search('Stalactite Hazard')
               if row['type'] == 'GameObject' and row['collection'] == 'level6'}
    require(set(ROOTS) <= matches.keys(), 'AssetRipper is missing a Tutorial_01 stalactite root')
    output.mkdir(parents=True)

    # Initialize the existing decor Store without its unrelated Statue seed.
    store = Store.__new__(Store)
    store.output, store.client = output, client
    store.export = Export(client, output)
    store.cache, store.tables, store.hashes, store.paths = {}, {}, {}, {}
    store.media = {}
    store.used, store.used_media = set(), set()
    objects, visited, particles, script_types = {}, set(), {}, {}

    def note_binary(key, data):
        if 'm_TextureSettings' in data and 'm_Width' in data:
            store.binary(key, 'texture')
        elif 'm_CompressionFormat' in data and 'm_Channels' in data:
            collection, number = key.removesuffix('.json').rsplit('-', 1)
            path = json.dumps(dict(C=store.paths[collection], D=int(number)))
            stem = key.removesuffix('.json')
            name = stem + '.audio'
            if key not in store.media:
                store.save(name, client.request('/Assets/Audio', Path=path))
                store.media[key] = dict(kind='audio', encoded=name)
        if 'm_InitialModule' in data:
            particles[key] = [field for field in data if field.endswith('Module')]

    def walk(key):
        if key in visited:
            return
        require(len(visited) < 2048, 'Stalactite dependency graph exceeded export bound')
        visited.add(key)
        data = store.get(key)
        if 'm_Components' in data:
            row = dict(name=data['m_Name'], active=data['m_IsActive'], components={}, children=[])
            objects[key] = row
            for item in data['m_Components']:
                component_key, component = store.resolve(key, item['m_Component'])
                kind = native_component_kind(component)
                if isinstance(component.get('m_Script'), dict):
                    script_key, script = store.resolve(component_key, component['m_Script'])
                    kind = script['m_ClassName']
                    script_types[component_key] = kind
                    store.get(script_key)
                row['components'].setdefault(kind or 'Unclassified', []).append(component_key)
                walk(component_key)
                if kind == 'Transform':
                    for ref in component['m_Children']:
                        transform_key, transform = store.resolve(component_key, ref)
                        child_key, _ = store.resolve(transform_key, transform['m_GameObject'])
                        row['children'].append(child_key)
                        walk(child_key)
            return

        note_binary(key, data)
        if isinstance(data.get('m_GameObject'), dict) and data['m_GameObject']['m_PathID']:
            owner_key, _ = store.resolve(key, data['m_GameObject'])
            walk(owner_key)
        for location, ref in references(data):
            field = location.split('/')[1] if '/' in location else location
            if field in SKIP or field in OWNERSHIP:
                continue
            target_key, target = store.resolve(key, ref)
            if 'm_Components' in target:
                walk(target_key)
            else:
                walk(target_key)

    roots = []
    for owner in ROOTS:
        row = matches[owner]
        store.register(row['path']['C'])
        key = f"{row['collection']}-{owner}.json"
        walk(key)
        roots.append(dict(owner=owner, key=key, name=row['name']))

    audio = {}
    for key, meta in store.media.items():
        if meta['kind'] != 'audio':
            continue
        clip = store.get(key)
        rate, channels, duration = clip['m_Frequency'], clip['m_Channels'], clip['m_Length']
        frames = round(rate * duration)
        audio[key] = dict(name=clip.get('m_Name', ''), encoded=meta['encoded'],
                          native_bytes=clip['m_Resource']['m_Size'], frames_estimate=frames,
                          sample_values_estimate=frames*channels, channels=channels,
                          sample_rate=rate, duration_seconds=duration,
                          compression_format=clip['m_CompressionFormat'])

    particle_profiles = {}
    for key in particles:
        p = store.get(key); initial = p['m_InitialModule']; emission = p['m_EmissionModule']
        rate = emission['m_RateOverTime']
        life = initial['m_StartLifetime']
        particle_profiles[key] = dict(duration=p['m_LengthInSec'], looping=p['m_Looping'],
            play_on_awake=p['m_PlayOnAwake'], max_particles=initial['m_MaxNumParticles'],
            rate_mode=rate['m_MinMaxState'], rate=rate['m_Scalar'], bursts=emission['m_Bursts'],
            lifetime_mode=life['m_MinMaxState'], lifetime=life['m_Scalar'],
            modules=particles[key])

    report = dict(schema=1, build=22529139, scene='Tutorial_01', roots=roots,
        dependency_tables=store.tables, source_sha256=store.hashes, media=store.media,
        objects=objects, particle_systems=particle_profiles, audio_clips=audio,
        script_types=script_types, limits=['Only the eight stalactite roots and their serialized dependencies were traversed.'])
    (output / 'stalactite-sources.json').write_text(json.dumps(report, indent=2) + '\n')
    return dict(objects=len(objects), particles=len(particle_profiles), audio=len(audio),
                files=len(store.hashes), roots=len(roots))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=58291)
    parser.add_argument('--output', type=Path, default=DEFAULT)
    parser.add_argument('--decoder-dir', type=Path, default=ROOT / '_private/tools/audio-python')
    args = parser.parse_args()
    sys.path.insert(0, str(args.decoder_dir.resolve()))
    print(json.dumps(extract(Client(args.port), args.output)))


if __name__ == '__main__':
    main()
