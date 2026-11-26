"""Retain source particle/audio dependencies without importing gameplay graphs."""
import hashlib
import io
import json
from pathlib import Path
import sys
import wave
from urllib.error import HTTPError

from extract_crawler import write_asset
from extract_plant_hit_dependencies import references


SKIP = {'m_CorrespondingSourceObject', 'm_PrefabAsset', 'm_PrefabInstance',
        'm_GameObject', 'm_Father', 'm_Children', 'm_OutputAudioMixerGroup'}
EFFECT_COMPONENTS = ('ParticleSystem', 'ParticleSystemRenderer', 'AudioSource',
                     'AudioClipTable', 'RandomAudioClipTable', 'tk2dSprite',
                     'tk2dSpriteAnimator', 'PlayAudioAndRecycle', 'SpatterOrange')


def decode_audio(raw):
    """Use native PCM directly, or the established private libsndfile decoder."""
    try:
        with wave.open(io.BytesIO(raw), 'rb') as source:
            if source.getsampwidth() == 2 and source.getcomptype() == 'NONE':
                pcm = io.BytesIO()
                with wave.open(pcm, 'wb') as target:
                    target.setparams(source.getparams())
                    target.writeframes(source.readframes(source.getnframes()))
                return pcm.getvalue(), source.getnframes(), source.getnchannels(), source.getframerate()
    except wave.Error:
        pass
    # Shared tool dependency used by extract_decor_effects.py as well.
    decoder = Path(__file__).resolve().parents[1] / '_private/tools/audio-python'
    if str(decoder) not in sys.path:
        sys.path.insert(0, str(decoder))
    import soundfile as sf
    samples, rate = sf.read(io.BytesIO(raw), dtype='int16', always_2d=True)
    pcm = io.BytesIO()
    sf.write(pcm, samples, rate, format='WAV', subtype='PCM_16')
    return pcm.getvalue(), len(samples), samples.shape[1], rate


def export_effect_assets(export, dependencies, limit=4096):
    """Export media once per canonical source ID and retain owner cue references.

    Hierarchies are traversed separately. Stop at GameObjects/Transforms so a
    reference cannot pull an unrelated controller or room into the effect bank.
    """
    visited, media, particles, cues, excluded = set(), {}, {}, {}, []

    def save(name, raw):
        write_asset(export.output / name, raw)
        export.hashes[name] = hashlib.sha256(raw).hexdigest()

    def binary(key, collection, kind):
        if key in media:
            return
        path = json.dumps(dict(C=collection, D=int(key.removesuffix('.json').rsplit('-', 1)[1])))
        stem = key.removesuffix('.json')
        if kind == 'texture':
            name = stem + '.png'
            if name not in export.hashes:
                save(name, export.client.request('/Assets/Image', Path=path, Extension='png'))
            media[key] = dict(kind=kind, png=name)
        else:
            try:
                raw = export.client.request('/Assets/Audio', Path=path)
            except HTTPError as error:
                raise ValueError('Breakable audio unavailable (check copied stream data): ' + key) from error
            pcm, frames, channels, rate = decode_audio(raw)
            save(stem + '.audio', raw)
            save(stem + '.wav', pcm)
            media[key] = dict(kind=kind, encoded=stem + '.audio', pcm=stem + '.wav',
                              frames=frames, channels=channels, rate=rate)

    def walk(data, key, collection):
        pending = [(data, key, collection)]
        while pending:
            data, key, collection = pending.pop()
            if key in visited:
                continue
            if len(visited) >= limit:
                raise ValueError('Breakable effect dependency budget exceeded at ' + key)
            visited.add(key)
            if 'm_Components' in data or 'm_LocalPosition' in data:
                excluded.append(dict(source=key, reason='hierarchy handled separately'))
                continue
            if 'm_Width' in data and 'm_TextureSettings' in data:
                binary(key, collection, 'texture')
            if 'm_CompressionFormat' in data and 'm_Channels' in data:
                binary(key, collection, 'audio')
            if 'm_InitialModule' in data:
                particles[key] = [name for name in data if name.endswith('Module')]
            children = []
            for location, ref in references(data):
                if any(part in SKIP for part in location.split('/')):
                    continue
                children.append(export.resolve(collection, ref))
            pending.extend(reversed(children))

    for row in dependencies['objects'].values():
        for kind in EFFECT_COMPONENTS:
            for key in row['components'].get(kind, []):
                walk(export.cache[key], key, row['collection'])
    for source in dependencies.get('sources', []):
        key, collection = source['component'], source['collection']
        data = export.cache[key]['m_Structure']
        cue = {}
        for field in ('breakAudioEvent', 'breakAudioClipTable', 'cutSound'):
            entries = []
            for location, ref in references({field: data.get(field)}):
                target, target_key, target_collection = export.resolve(collection, ref)
                walk(target, target_key, target_collection)
                entries.append(dict(path=location, reference=target_key))
            if entries:
                cue[field] = entries
        if cue:
            # Volume/pitch ranges stay in the hashed source component JSON.
            cues[key] = cue
    return dict(dependencies=sorted(visited), media=media, particles=particles,
                audio_cues=cues, excluded=excluded)


def source_effect_costs(effects, read):
    """Source admission inputs; these bytes/particles are not runtime allocations."""
    systems = []
    for key in sorted(effects.get('particles', {})):
        data = read(key)
        systems.append(dict(source=key,
            max_particles=data['m_InitialModule']['m_MaxNumParticles'],
            duration=data['m_LengthInSec'], looping=data['m_Looping'],
            bursts=bool(data['m_EmissionModule']['m_Bursts'])))
    texture_bytes, pcm_bytes = 0, 0
    for key, media in effects.get('media', {}).items():
        if media['kind'] == 'texture':
            texture = read(key)
            texture_bytes += texture['m_Width'] * texture['m_Height'] * 2
        elif media['kind'] == 'audio':
            pcm_bytes += media['frames'] * media['channels'] * 2
    return dict(particle_systems=systems,
                authored_particle_cap_sum=sum(row['max_particles'] for row in systems),
                untrimmed_rgba4_texture_bytes=texture_bytes, decoded_pcm_bytes=pcm_bytes,
                runtime_texture_bytes=0, runtime_draws_added=0,
                status='Source inventory only; per-owner concurrency and atlas admission remain required.')
