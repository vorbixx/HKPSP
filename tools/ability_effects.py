"""Read-only dependency evidence for Shiny and Crystal Heart presentation.

Called by ability_source.extract under the same private-output ownership guard.
Serialized fields remain source data; this exporter assigns no particle semantics.
"""
import hashlib
import io
import json
from pathlib import Path
import wave


def references(value):
    if isinstance(value, dict):
        if set(value) == {'m_FileID', 'm_PathID'}:
            if value['m_PathID']:
                yield value
        else:
            for child in value.values():
                yield from references(child)
    elif isinstance(value, list):
        for child in value:
            yield from references(child)


def extract(load, resolve, parts, pickups, events, output):
    if not output.resolve().is_relative_to((Path(__file__).resolve().parents[1]/'_private/converted-psp').resolve()):
        raise ValueError('Effect exports must stay in this checkout private PSP tree')
    output = output/'effects'
    output.mkdir(parents=True, exist_ok=True)
    records, roots, seen = [], {}, set()
    # Only authored assets referenced by these subtrees/actions are followed.
    # Scene objects outside a root remain explicit references, not copied rooms.
    dependencies = {'Sprite', 'Texture2D', 'Material', 'Shader', 'Mesh', 'AnimatorController',
                    'AnimationClip', 'AudioClip', 'MonoScript', 'PhysicsMaterial2D'}

    def identity(obj):
        return [obj.assets_file.name, obj.path_id]

    def record(obj):
        key = tuple(identity(obj))
        if key in seen:
            return
        seen.add(key)
        data = obj.read_typetree()
        row = {'ref': list(key), 'type': obj.type.name, 'data': data, 'references': []}
        records.append(row)
        basename = obj.assets_file.name.replace('.', '-')+'-'+str(obj.path_id)
        if obj.type.name in ('Sprite', 'Texture2D'):
            filename = basename+'.png'
            obj.read().image.convert('RGBA').save(output/filename)
            row['image'] = filename
        elif obj.type.name == 'AudioClip':
            # Preserve compressed source bytes before choosing a decoder. Some
            # references in unused FSM branches have no embedded audio resource.
            audio = obj.read()
            if audio.m_AudioData:
                raw = bytes(audio.m_AudioData)
            elif audio.m_Resource and audio.m_Resource.m_Size:
                resource = audio.m_Resource
                # Texture decoding can leave UnityPy's shared resource reader
                # buffered at another offset. Read exactly this source file span.
                source = Path(obj.assets_file.environment.path)/Path(resource.m_Source).name
                with source.open('rb') as stream:
                    stream.seek(resource.m_Offset)
                    raw = stream.read(resource.m_Size)
                if len(raw) != resource.m_Size:
                    raise ValueError(f'Truncated original AudioClip resource: {key}')
            else:
                raw = b''
            filename = basename+'.audio'
            (output/filename).write_bytes(raw)
            row['audio'] = filename
            row['audio_bytes'] = len(raw)
            row['audio_header_hex'] = raw[:8].hex()
            row['audio_sha256'] = hashlib.sha256(raw).hexdigest()
            if raw:
                if raw[:4] != b'FSB5':
                    raise ValueError(f'Unexpected build22529139 AudioClip container: {key}')
                import fmod_toolkit
                samples = fmod_toolkit.raw_to_wav(raw, audio.m_Name,
                                                audio.m_Channels or 2, audio.m_Frequency or 44100, True)
                if len(samples) != 1:
                    raise ValueError(f'Expected one original AudioClip stream: {key}')
                decoded = next(iter(samples.values()))
                with wave.open(io.BytesIO(decoded)) as pcm:
                    if (pcm.getnchannels() != audio.m_Channels or
                            pcm.getframerate() != audio.m_Frequency or
                            pcm.getnframes() != round(audio.m_Length*audio.m_Frequency)):
                        raise ValueError(f'Decoded AudioClip differs from source metadata: {key}')
                filename = basename+'.wav'
                (output/filename).write_bytes(decoded)
                row['wav'] = filename
        elif obj.type.name == 'AnimationClip':
            from convert_monomon_mask import streamed_channels
            row['streamed_channels'] = streamed_channels(data)
        for ref in references(data):
            dependency = resolve(obj.assets_file, ref)
            row['references'].append({'pointer': ref, 'ref': identity(dependency),
                                      'type': dependency.type.name})
            if dependency.type.name in dependencies:
                record(dependency)

    def tree(go):
        record(go)
        components = parts(go)
        for component in components:
            record(component)
        transform = next(c for c in components if c.type.name in ('Transform', 'RectTransform'))
        for child in transform.read_typetree()['m_Children']:
            child_transform = resolve(transform.assets_file, child)
            tree(resolve(child_transform.assets_file, child_transform.read_typetree()['m_GameObject']))

    flash = load('resources.assets').objects[7318]
    roots['shiny_flash'] = identity(flash)
    tree(flash)
    player = load('resources.assets').objects[4126]
    roots['audio_player'] = identity(player)
    tree(player)
    for name, pickup in pickups.items():
        scene, ident = pickup['ref']
        fsm = load(scene).objects[ident]
        # Audio and pooled-prefab parameters retain their original file context.
        record(fsm)
        go = resolve(fsm.assets_file, fsm.read_typetree()['m_GameObject'])
        for component in parts(go):
            if component.type.name == 'AudioSource':
                record(component)
        if name == 'heart':
            transform = next(c for c in parts(go) if c.type.name == 'Transform')
            parent = resolve(transform.assets_file, transform.read_typetree()['m_Father'])
            for child in parent.read_typetree()['m_Children']:
                tr = resolve(parent.assets_file, child)
                sibling = resolve(tr.assets_file, tr.read_typetree()['m_GameObject'])
                title = sibling.read_typetree()['m_Name']
                if title in ('White Flash', 'White Wave'):
                    roots[title] = identity(sibling)
                    tree(sibling)
    for event in events:
        scene, ident = event['ref']
        fsm = load(scene).objects[ident]
        go = resolve(fsm.assets_file, fsm.read_typetree()['m_GameObject'])
        roots[f'{event["name"]}:{ident}'] = identity(go)
        tree(go)
    result = {'roots': roots, 'objects': records}
    blobs = []
    def save_blob(value):
        if not isinstance(value, bytes):
            raise TypeError(f'Unsupported source value: {type(value).__name__}')
        filename = f'data-{len(blobs)}.bin'
        (output/filename).write_bytes(value)
        blobs.append(filename)
        return {'bytes_file': filename, 'bytes': len(value),
                'sha256': hashlib.sha256(value).hexdigest()}
    (output/'dependencies.json').write_text(json.dumps(result, indent=1, default=save_blob), encoding='utf-8')
    return {'file': 'effects/dependencies.json', 'objects': len(records), 'roots': roots}
