"""Source material decoding and stable per-owner samples for grass sway."""
import hashlib
import math


MATERIAL_NAME = 'SwayingGrass_Crossroads'
GREENPATH_MATERIAL_NAME = 'SwayingGrass_Greenpath'
SUPPORTED_MATERIAL_NAMES = frozenset((MATERIAL_NAME, GREENPATH_MATERIAL_NAME))
_REQUIRED = ('_SwaySpeed', '_SwayAmount', '_WorldOffset', '_HeightOffset', '_ClampZ', '_PushAmount')


def is_grass_material(material):
    return material.get('m_Name') in SUPPORTED_MATERIAL_NAMES


def constants(material):
    if not is_grass_material(material):
        raise ValueError('Not the source swaying grass material')
    floats = material.get('m_SavedProperties', {}).get('m_Floats', {})
    if any(name not in floats for name in _REQUIRED):
        raise ValueError('Grass material is missing shader constants')
    values = {name: float(floats[name]) for name in _REQUIRED}
    if not all(math.isfinite(value) for value in values.values()):
        raise ValueError('Grass material has a non-finite shader constant')
    return values


def owner_offsets(owner):
    """Stable approximation of Unity Start RNG; same owner always shares a sample."""
    digest = hashlib.sha256(f'hk3ds-grass-sway-v1:{int(owner)}'.encode('ascii')).digest()
    epsilon_u = int.from_bytes(digest[0:4], 'little') / 0xffffffff
    jitter_u = int.from_bytes(digest[4:8], 'little') / 0xffffffff
    return -0.01 + epsilon_u * 0.02, -0.0001 + jitter_u * 0.0002


def parameters(material, owner, translation, basis_y_sum):
    """Build runtime HKR3 sway fields from ObjectToWorld source values."""
    values = constants(material)
    epsilon, jitter = owner_offsets(owner)
    tz = float(translation[2]) + jitter
    clamp_z = values['_ClampZ']
    depth_gain = 1 + abs(max(-clamp_z, min(clamp_z, tz)))
    return dict(
        speed=values['_SwaySpeed'],
        amplitude=1.25 * values['_SwayAmount'] * depth_gain,
        phase=(float(translation[0]) + tz) * values['_WorldOffset'],
        push=(values['_PushAmount'] + epsilon) * depth_gain,
        height=values['_HeightOffset'] * float(basis_y_sum),
        z_jitter=jitter,
        epsilon=epsilon,
    )
