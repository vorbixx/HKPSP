"""Hash-anchored Watcher candle scale animation; existing sprites and materials."""
import hashlib
import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RESEARCH = ROOT/'_private/research/route-watcher-animation'


def prepare(source, mapping, read):
    if mapping.get('scene') != 'Ruins2_Watcher_Room':
        return None
    if not (RESEARCH/'manifest.json').is_file():
        # Approximation: retain exported static scales when the original pulse clip is unavailable.
        return None
    manifest = json.loads((RESEARCH/'manifest.json').read_bytes())
    assets = {}
    for name, expected in manifest['source_sha256'].items():
        raw = (RESEARCH/name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError('Changed candle animation source: '+name)
        assets[name] = json.loads(raw)
    clip = assets[manifest['clips'][0]]
    curves = clip['m_ScaleCurves']
    if len(manifest['clips']) != 1 or len(curves) != 1 or curves[0]['m_Path']:
        raise ValueError('Unsupported candle animation binding')
    if any(clip.get(k) for k in ('m_PositionCurves', 'm_RotationCurves', 'm_EulerCurves', 'm_FloatCurves', 'm_PPtrCurves')):
        raise ValueError('Candle animation must change only scale')
    states = assets[manifest['controller']]['m_Controller']['m_StateMachineArray']
    state = states[0]['m_Data']['m_StateConstantArray'][0]['m_Data']
    if len(states) != 1 or not state['m_Loop'] or state['m_Speed'] != 1 or state['m_CycleOffset'] != 0:
        raise ValueError('Unsupported candle controller timing')
    raw_keys = curves[0]['m_Curve']['m_Curve']
    base = raw_keys[0]['m_Value']
    keys = []
    for key in raw_keys:
        if key['m_WeightedMode'] != 0:
            raise ValueError('Weighted candle curve needs a different sampler')
        values = [key[k]['m_X']/base['m_X'] for k in ('m_Value', 'm_InSlope', 'm_OutSlope')]
        for field, value in zip(('m_Value', 'm_InSlope', 'm_OutSlope'), values):
            if any(not math.isclose(key[field][axis]/base[axis], value, abs_tol=2e-6) for axis in ('m_Y', 'm_Z')):
                raise ValueError('Candle pulse must have proportional scale axes')
        keys.append((key['m_Time'], *values))
    if not 2 <= len(keys) <= 256 or keys[0][0] != 0 or any(b[0] <= a[0] for a,b in zip(keys, keys[1:])):
        raise ValueError('Invalid candle scale keys')
    anchor = assets[manifest['animator']]['m_Controller']
    owners = set()
    for item in mapping['inventory']:
        if not read(item['go'])['m_Name'].startswith('candle glow') or not item['active']:
            continue
        prefix = item['go'].rsplit('-', 1)[0]
        animators = []
        for ref in read(item['go'])['m_Components']:
            key = f"{prefix}-{ref['m_Component']['m_PathID']}.json"
            data = read(key)
            if 'm_Controller' in data and data.get('m_Enabled'):
                if key not in mapping['source_sha256']:
                    raise ValueError('Unverified candle Animator: '+key)
                animators.append(data)
        if len(animators) != 1 or animators[0]['m_Controller'] != anchor:
            raise ValueError('Candle Animator does not match the exported controller')
        owners.add(int(item['go'].rsplit('-',1)[1].split('.')[0]))
    if len(owners) != 7:
        raise ValueError('Expected seven verified Watcher candle glows')
    return dict(keys=keys, base=base, owners=owners)


def animate_chain(profile, owner, chain):
    if profile is None or owner not in profile['owners']:
        return chain
    # Animator replaces local scale, including the serialized narrower X value.
    return [dict(chain[0], m_LocalScale=dict(profile['base'])), *chain[1:]]


def compile_bank(profile, draws):
    if profile is None:
        return None
    owners = sorted({d['owner'] for d in draws if d['owner'] in profile['owners']})
    if set(owners) != profile['owners']:
        raise ValueError('Missing animated candle draw')
    for draw in draws:
        if draw['owner'] in owners and (draw.get('grass') or draw['values'][13] != 0 or draw['values'][14] != 0):
            raise ValueError('Candle scale animation supports flat sprites only')
    return (struct.pack('<4s2I', b'HKS1', len(profile['keys']), len(owners))+
            b''.join(struct.pack('<4f', *key) for key in profile['keys'])+
            struct.pack('<'+str(len(owners))+'I', *owners))
