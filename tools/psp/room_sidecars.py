"""Sprite swaps from the 3DS room sidecar tables, for PSP density factors.

Animation frames that are never the sprite of a room draw are drawn by swapping them into an
existing draw. Each reader returns {sprite: set(draw indices)} so the transcoder can size the
frame with that draw's transform. Layouts follow the 3DS readers:
  plants   HKP1/2/3  src/plant.cpp PlantBank::load (clip keys -> plants with that clip -> owner draws)
  dynamics HKD1      src/room_dynamics.cpp (4 spike keys -> 32 spike owners -> owner draws)
  ambient  HKA1      src/room_ambient.cpp RoomAmbient::bind (22 glow-bug visuals: 6 frames -> draw)
  grass    HKG1      src/grass.cpp GrassBank::load (idle/push frames -> drawOwner draws; the stub
                     sprite has its own 16-float draw transform, returned as a pseudo draw)
Particle emitters (ambient HKP2 section) are not read: their sprites stay at source size.
"""
from __future__ import annotations

import struct
from collections import defaultdict


class Reader:
    def __init__(self, data: bytes, name: str):
        self.data, self.pos, self.name = data, 0, name

    def u32(self, n=1):
        values = struct.unpack_from(f"<{n}I", self.data, self.pos)
        self.pos += 4 * n
        return values

    def f32(self, n=1):
        values = struct.unpack_from(f"<{n}f", self.data, self.pos)
        self.pos += 4 * n
        return values

    def magic(self):
        value = self.data[self.pos:self.pos + 4]
        self.pos += 4
        return value

    def fail(self, why):
        raise ValueError(f"{self.name}: {why}")


def owner_draws(draws):
    result = defaultdict(set)
    for index, (_sprite, owner, _values) in enumerate(draws):
        result[owner].add(index)
    return result


def plants(data: bytes, sprite_count: int, draw_count: int, by_owner) -> dict:
    r = Reader(data, "plants")
    magic = r.magic()
    if magic not in (b"HKP1", b"HKP2", b"HKP3"):
        r.fail(f"magic {magic!r}")
    count, clip_count, sprites, draws, passive = r.u32(5)
    if sprites != sprite_count or draws != draw_count:
        r.fail(f"indexes {sprites} sprites/{draws} draws, room has {sprite_count}/{draw_count}")
    clips = []
    for _ in range(clip_count):
        _duration, = r.f32()
        n, = r.u32()
        keys = []
        for _ in range(n):
            r.f32()
            keys.append(r.u32()[0])
        clips.append(keys)
    result = defaultdict(set)
    rows = []
    for _ in range(count):
        owner, _authored, clip = r.u32(3)
        if magic != b"HKP1":
            r.u32(2)
            r.f32(13)
        rows.append((owner, clip))
    for _ in range(passive if magic == b"HKP3" else 0):
        owner, _authored, clip, _random = r.u32(4)
        rows.append((owner, clip))
    if r.pos != len(data):
        r.fail(f"{len(data) - r.pos} trailing bytes")
    for owner, clip in rows:
        for sprite in clips[clip]:
            result[sprite] |= by_owner.get(owner, set())
    return result


def dynamics(data: bytes, sprite_count: int, draw_count: int, by_owner) -> dict:
    r = Reader(data, "dynamics")
    if r.magic() != b"HKD1":
        r.fail("magic")
    sprites, draws = r.u32(2)
    r.f32()
    if sprites != sprite_count or draws != draw_count:
        r.fail("index counts differ from the room table")
    keys = []
    for _ in range(4):
        r.f32()
        keys.append(r.u32()[0])
    owners = [r.u32(2)[0] for _ in range(32)]
    result = defaultdict(set)
    for owner in owners:
        for sprite in keys:
            result[sprite] |= by_owner.get(owner, set())
    return result  # rocks and the CRB1 crumble extension draw room sprites directly


def ambient(data: bytes, sprite_count: int, draw_count: int) -> dict:
    r = Reader(data, "ambient")
    if r.magic() != b"HKA1":
        r.fail("magic")
    tablets, bugs, visuals, sprites, draws = r.u32(5)
    if sprites != sprite_count or draws != draw_count:
        r.fail("index counts differ from the room table")
    for _ in range(tablets):
        r.u32(3)
        r.f32(11)
    for _ in range(bugs):
        r.u32()
        r.f32(7)
    result = defaultdict(set)
    for _ in range(visuals):
        ids = r.u32(10)
        r.f32()
        if ids[2] >= draw_count:
            r.fail("visual draw out of range")
        for sprite in ids[4:10]:
            if sprite < sprite_count:
                result[sprite].add(ids[2])
    return result


GRASS_RECORD = 8 * 4 + (8 + 16 + 5 + 3) * 4


def grass(data: bytes, sprite_count: int, draw_count: int, by_owner, transforms: dict) -> dict:
    r = Reader(data, "grass")
    if r.magic() != b"HKG1":
        r.fail("magic")
    count, sprites, draws = r.u32(3)
    if sprites != sprite_count or draws != draw_count or len(data) != 16 + GRASS_RECORD * count:
        r.fail("counts or size differ from the room table")
    result = defaultdict(set)
    for _ in range(count):
        ids = r.u32(8)
        r.f32(8)
        stub = r.f32(16)
        r.f32(8)
        if max(ids[2:8]) >= sprite_count:
            r.fail("sprite out of range")
        for sprite in ids[2:7]:  # idle + 4 push frames swap into the bush draws
            result[sprite] |= by_owner.get(ids[1], set())
        transforms.setdefault(ids[7], []).append(stub)
    return result


def sprite_swaps(tables: dict, sprite_count: int, draws):
    """tables: {'plants'|'dynamics'|'ambient'|'grass': bytes}.

    Returns ({sprite: set(draw indices)}, {sprite: [16-float draw values]})."""
    by_owner = owner_draws(draws)
    merged, transforms = defaultdict(set), {}
    readers = {"plants": lambda d: plants(d, sprite_count, len(draws), by_owner),
               "dynamics": lambda d: dynamics(d, sprite_count, len(draws), by_owner),
               "ambient": lambda d: ambient(d, sprite_count, len(draws)),
               "grass": lambda d: grass(d, sprite_count, len(draws), by_owner, transforms)}
    for name, data in tables.items():
        for sprite, indices in readers[name](data).items():
            merged[sprite] |= indices
    return merged, transforms


def baked_sidecars(tables, table, mapping, sprite_count, classify):
    """PSR1 duplicates change sidecar count contracts; remap per-draw ambient frames.

    Tutorial_01's plants/dynamics/grass are all in front. Reject a future shared
    animated use that needs clip/owner duplication instead of silently rendering sharp.
    Source tables are never modified.
    """
    offsets = {"plants": 12, "dynamics": 4, "ambient": 16, "grass": 8}
    result = {}
    for name, data in tables.items():
        swaps, _ = sprite_swaps({name: data}, len(table.sprites), table.draws)
        if name != "ambient":
            for sprite, indices in swaps.items():
                if mapping.get(sprite, sprite) != sprite and any(
                        classify(table.sprites[sprite], table.draws[i][2], table.split_depth) == "behind"
                        for i in indices):
                    raise ValueError(f"{name}: shared behind-plane animation needs clip remapping")
        converted = bytearray(data)
        struct.pack_into("<I", converted, offsets[name], sprite_count)
        if name == "ambient":
            tablets, bugs, visuals = struct.unpack_from("<3I", data, 4)
            base = 24 + 56 * tablets + 32 * bugs
            for i in range(visuals):
                row = base + 44 * i
                draw = struct.unpack_from("<I", data, row + 8)[0]
                for frame in range(4, 10):
                    pos = row + 4 * frame
                    sprite = struct.unpack_from("<I", data, pos)[0]
                    if sprite in mapping and classify(table.sprites[sprite], table.draws[draw][2],
                                                       table.split_depth) == "behind":
                        struct.pack_into("<I", converted, pos, mapping[sprite])
        result[name] = bytes(converted)
    return result
