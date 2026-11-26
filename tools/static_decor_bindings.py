"""Retarget unchanged actor artwork to a bank with appended static sprites."""
import json
import struct


def protected_owners(directory):
    result = set()

    def visit(value):
        if isinstance(value, dict):
            if isinstance(value.get('owner'), int):
                result.add(value['owner'])
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    for name in ('plant-artwork.json', 'room-dynamics.json'):
        path = directory / name
        if path.exists():
            visit(json.loads(path.read_bytes()))
    return result


def retarget_bindings(directory, output, original, baked, metadata):
    _, _, sprites, draws, _ = struct.unpack_from('<4s4I', original)
    _, _, next_sprites, next_draws, _ = struct.unpack_from('<4s4I', baked)
    if draws != next_draws or original[20:20+sprites*36] != baked[20:20+sprites*36]:
        raise ValueError('static bake changed original sprite/index bindings')
    protected = protected_owners(metadata)
    if not protected:
        raise ValueError('missing runtime binding metadata')
    for index in range(draws):
        a, b = 20+sprites*36+index*72, 20+next_sprites*36+index*72
        owner = struct.unpack_from('<I', original, a+4)[0]
        if owner in protected and original[a:a+72] != baked[b:b+72]:
            raise ValueError(f'static bake modified runtime owner {owner}')
    for name, offset, magic in (('plants.bin', 12, (b'HKP1', b'HKP2', b'HKP3')),
                                ('room-dynamics.bin', 4, (b'HKD1', b'HKD2')),
                                ('room-ambient.bin', 16, (b'HKA1',)),
                                ('grass.bin', 8, (b'HKG1',))):
        data = bytearray((directory / name).read_bytes())
        if data[:4] not in magic or struct.unpack_from('<2I', data, offset) != (sprites, draws):
            raise ValueError(f'incompatible runtime binding: {name}')
        struct.pack_into('<2I', data, offset, next_sprites, draws)
        (output / ('static-'+name)).write_bytes(data)
