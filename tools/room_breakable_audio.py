"""Compile hashed, owner-local break sounds without choosing a cosmetic variant."""
import hashlib
import math
import shutil
import struct
import wave


MAX_PCM_BYTES = 1024 * 1024


def compile_audio(data, kind, component, read, resolve, effects, paths, target):
    media = effects.get('media', {})
    cues = []

    def reference(owner, ref):
        ref = ref or {}
        key = resolve(owner, ref)
        if ref.get('m_PathID') and not key:
            raise ValueError('Unresolved breakable audio reference: ' + owner)
        return key

    def cue(options, gain, low, high):
        values = (gain, low, high)
        if not all(math.isfinite(v) for v in values) or not (0 <= gain <= 4 and 0 < low <= high <= 4):
            raise ValueError('Invalid breakable audio gain/pitch: ' + component)
        choices = []
        for key, weight in options:
            if not math.isfinite(weight) or weight < 0:
                raise ValueError('Invalid breakable audio weight: ' + component)
            if not weight:
                continue
            filename = ''
            if key:
                entry = media.get(key, {})
                if entry.get('kind') != 'audio':
                    raise ValueError('Missing exported breakable audio: ' + key)
                source, expected = paths[entry['pcm']]
                if hashlib.sha256(source.read_bytes()).hexdigest() != expected:
                    raise ValueError('Changed breakable PCM: ' + key)
                with wave.open(str(source), 'rb') as wav:
                    size = wav.getnframes() * wav.getnchannels() * 2
                    if (wav.getsampwidth() != 2 or wav.getcomptype() != 'NONE' or
                            wav.getnchannels() not in (1, 2) or not 8000 <= wav.getframerate() <= 96000 or
                            not 0 < size <= MAX_PCM_BYTES):
                        raise ValueError('Breakable PCM exceeds native format/budget: ' + key)
                filename = 'room-sfx-' + key.removesuffix('.json') + '.wav'
                if len(filename.encode('ascii')) >= 80 or '/' in filename or '\\' in filename:
                    raise ValueError('Invalid breakable PCM identity: ' + key)
                target.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target / filename)
            choices.append(dict(file=filename, weight=weight))
        if len(choices) > 32:
            raise ValueError('Breakable sound choice budget exceeded')
        if options and (not choices or sum(c['weight'] for c in choices) > 1e9):
            raise ValueError('Invalid breakable audio total weight: ' + component)
        if choices:
            cues.append(dict(gain=gain, pitch=[low, high], choices=choices))

    if kind == 'BreakableObject':
        refs = data.get('cutSound') or []
        if isinstance(refs, dict):
            refs = [refs]
        if refs:
            cue([(reference(component, ref), 1) for ref in refs], 1,
                data.get('pitchMin', 1), data.get('pitchMax', 1))
    else:
        event = data.get('breakAudioEvent') or {}
        clip = reference(component, event.get('Clip'))
        if clip and math.isfinite(event['PitchMin']) and event['PitchMin'] <= 0:
            # Hive breakables author Random.Range(-10, 10): Unity plays negative
            # pitches in reverse, which the native mixer cannot; the cue stays silent.
            clip = None
        if clip:
            # AudioEvent.SpawnAndPlayOneShot assigns Volume to AudioSource.volume,
            # which Unity clamps to 0..1; authored rooms use 100 for full volume.
            volume = event['Volume']
            cue([(clip, 1)], min(volume, 1) if math.isfinite(volume) else volume,
                event['PitchMin'], event['PitchMax'])
        table = reference(component, data.get('breakAudioClipTable'))
        if table:
            authored = read(table)['m_Structure']
            cue([(reference(table, option.get('Clip')), option['Weight'])
                 for option in authored.get('options', [])], 1,
                authored['pitchMin'], authored['pitchMax'])
    return cues


def write_audio(cues):
    binary = bytearray(struct.pack('<I', len(cues)))
    for cue in cues:
        binary += struct.pack('<I3f', len(cue['choices']), cue['gain'], *cue['pitch'])
        for choice in cue['choices']:
            binary += struct.pack('<80sf', choice['file'].encode('ascii'), choice['weight'])
    return binary
