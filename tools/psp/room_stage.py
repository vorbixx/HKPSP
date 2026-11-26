"""Complete a startup path trace with the selected route package dependencies.

Shared loaders open scene sidecars and simulation animation banks directly under
their roots, bypassing Platform::path. Keep source-backed indices/metadata;
presentation uses the separately converted rooms/ namespace. No 3DS texels ship.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import sys
import textwrap
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
from room_packages import DEFAULT_STAGE, explicit_rooms, scene_directory, selected_rooms
import transcode_room as room

SCENE_ENEMY_RENDER = (
    ("tiktik", "crossroads-enemies-a/tiktik-anim"),
    ("gruzzer", "crossroads-enemies-a/gruzzer-anim"),
    ("vengefly", "crossroads-enemies-a/vengefly-anim"),
    ("aspid-hatchling", "crossroads-enemies-a/aspid-hatchling-anim"),
    ("enemy-b", "enemy-b/enemy-b-anim"),
    ("enemy-c", "enemy-c/enemy-c-scene"),
    ("zombie-guard", "zombie-guard/zombie-guard-anim"),
    ("spitter", "spitter/spitter-anim"),
    ("fungus-flyer", "fungus-flyer/fungus-flyer-anim"),
    ("jellyfish", "jellyfish/jellyfish-anim"),
    ("fungoon-baby", "fungoon-baby/fungoon-baby-anim"),
    ("fung-crawler", "fung-crawler/fung-crawler-anim"),
    ("greenpath", "greenpath-enemies/greenpath-scene"),
    ("elder-baldur", "elder-baldur/elder-baldur-art"),
    ("broken-vessel", "broken-vessel/broken-vessel-art"),
    ("watcher", "watcher-knights/watcher-art"),
    ("mantis", "mantis-lords/mantis-art"),
    ("uumuu-body", "uumuu-body/uumuu-body-art"),
    ("uumuu-quirrel", "uumuu-quirrel/uumuu-quirrel-art"),
    ("uumuu-gate", "uumuu-gate/uumuu-gate-art"),
    ("uumuu-effects", "uumuu-effects/uumuu-effects-art"),
    ("monomon-archive", "monomon-archive-art/monomon-archive-art"),
    ("monomon-guardian", "monomon-guardian-art/monomon-guardian-art"),
)
SCENE_ENEMY_ART = {"greenpath": "greenpath-art.bin", "elder-baldur": "elder-baldur-art.bin",
                   "broken-vessel": "broken-vessel-art.bin", "watcher": "watcher-art.bin", "mantis": "mantis-art.bin",
                   **{f'uumuu-{part}': f'uumuu-{part}-art.bin' for part in ('body', 'quirrel', 'gate', 'effects')},
                   'monomon-archive': 'monomon-archive-art.bin', 'monomon-guardian': 'monomon-guardian-art.bin'}
NAME = re.compile(r"(?:(?:rooms|scenes)/[A-Za-z0-9_-]+/)?[A-Za-z0-9_-]+\.[A-Za-z0-9]+"
                  r"|rooms/[A-Za-z0-9_-]+/static-room-visual\.bin\.switches"
                  r"|route-tolls/[A-Za-z0-9_-]+\.bin")
ROUTE_TOLL_SCENES = {"Crossroads_49b", "Fungus1_31", "Mines_33"}
# Toll machine PSA1 art by RTS4 kind (route-tolls/<scene>.bin, main.cpp:1867-1876): 1 city lift,
# 2 mines gate, 3 greenpath bench. Each library is two pages: page-0.hpt and page-1.hpt.
ROUTE_TOLL_ART = {1: "anim-libraries/route-tolls/city-toll-actor-art",
                  2: "anim-libraries/route-tolls/mines-toll-actor-art",
                  3: "anim-libraries/greenpath-tolls/greenpath-toll-actor-art"}
ENEMY_A = {"Climber": "tiktik", "Fly": "gruzzer", "Buzzer": "vengefly",
           "Hatcher Baby": "aspid-hatchling"}
ENEMY_B = {"Zombie Runner", "Zombie Hornhead", "Zombie Runner Sp", "Zombie Hornhead Sp", "Zombie Leaper", "Worm"}
ENEMY_C = {"Zombie Barger", "Roller", "Hatcher", "Mawlek Body", "Mushroom Brawler", "Ruins Sentry",
           "Ruins Sentry 1", "Ruins SentryB", "Ruins Sentry Fat", "Ruins Sentry FatB", "Ruins Sentry Fat B",
           "Ruins Flying Sentry", "Ruins Flying SentryB"}
INSTANCE_SUFFIX = re.compile(r" (?:[0-9]+|\([0-9]+\))$")
ABYSS_ACID_PIPE_OWNERS = (1340, 1348, 1349, 1352)
SCENE_BANKS = {
    "Ruins2_03": ("watcher-art.bin",),
    "Fungus2_15": ("mantis-art.bin",),
    "Abyss_19": ("broken-vessel-art.bin",),
    "Fungus3_archive_02": ("monomon-archive-art.bin", "uumuu-body-art.bin", "uumuu-quirrel-art.bin",
                           "uumuu-gate-art.bin", "uumuu-effects-art.bin"),
    "Dream_Guardian_Monomon": ("monomon-guardian-art.bin",),
}


def scene_enemy_banks(table: Path) -> set[str]:
    """SceneBank placement records, including spawns that may activate after entry."""
    data = table.read_bytes()
    if len(data) < 24 or data[:4] not in (b"HKS1", b"HKS2", b"HKS3"):
        raise ValueError(f"Invalid scene table: {table}")
    gates, = struct.unpack_from("<I", data, 4)
    offset = 24 + gates * 124
    if gates > 32 or offset > len(data) or (data[:4] != b"HKS1" and offset + 4 > len(data)):
        raise ValueError(f"Invalid scene gates: {table}")
    banks = set(SCENE_BANKS.get(table.parent.name, ()))
    if data[:4] == b"HKS1":
        return banks
    count, = struct.unpack_from("<I", data, offset)
    offset += 4
    if count > 128 or offset + count * 88 > len(data):
        raise ValueError(f"Invalid scene enemy table: {table}")
    for index in range(count):
        pos = offset + index * 88
        owner, raw = struct.unpack_from("<I64s", data, pos)
        name, end, _ = raw.partition(b"\0")
        if not name or not end:
            raise ValueError(f"Invalid enemy prefab {index}: {table}")
        name = name.decode("utf-8")
        candidates = {name, INSTANCE_SUFFIX.sub("", name)}
        for base in candidates:
            if base in ENEMY_A:
                stem = ENEMY_A[base]
                banks.update((stem + ".bin", stem + "-anim.bin"))
            if base in ENEMY_B:
                banks.update(("enemy-b.bin", "enemy-b-anim.bin"))
            if base in ENEMY_C:
                banks.add("enemy-c-anim.bin")
            if base == "Blocker" and (table.parent.name, owner) in {
                ("Crossroads_11_alt", 729), ("Crossroads_ShamanTemple", 2371)
            }:
                banks.add("elder-baldur-art.bin")
            if base == "Zombie Guard" and table.parent.name == "Crossroads_21" and owner == 1207:
                banks.add("zombie-guard-anim.bin")
            if base == "Spitter" and table.parent.name in ("Crossroads_03", "Crossroads_08", "Crossroads_19"):
                banks.add("spitter-anim.bin")
            if base == "Fungus Flyer":
                banks.add("fungus-flyer-anim.bin")
            if base in ("Jellyfish", "Jellyfish Baby", "Jellyfish Baby Inert"):
                banks.add("jellyfish-anim.bin")
            if base == "Fungoon Baby":
                banks.add("fungoon-baby-anim.bin")
            if base == "Fung Crawler":
                banks.add("fung-crawler-anim.bin")
    return banks


def read_terrain_bank(path: Path):
    data = path.read_bytes()
    if len(data) >= 60:
        magic, shapes, vertices, version, reserved = struct.unpack_from('<4s4I', data)
        end = 60 + shapes * 28
        if (magic == b'HKT1' and version == 1 and reserved == 0
                and 1 <= shapes <= 256 and 1 <= vertices <= 8192 and len(data) == end + vertices * 8):
            return data[12:60], data[60:end], data[end:]
    raise ValueError(f'Invalid terrain bank: {path}')


def false_knight_terrain(directory: Path) -> Path:
    """Select the room bank that includes the authored, boss-controlled arena floor."""
    boss = (directory / 'boss-room.bin').read_bytes()

    def read_ids(offset, limit):
        count, = struct.unpack_from('<I', boss, offset)
        if count > limit:
            raise ValueError(f'Invalid boss room ID count: {directory}')
        return struct.unpack_from(f'<{count}I', boss, offset + 4), offset + 4 * (1 + count)

    try:
        magic, gate_count = struct.unpack_from('<4s32xI', boss)
        if magic not in (b'HKB1', b'HKB2') or not 1 <= gate_count <= 2:
            raise ValueError(f'Invalid boss room header: {directory}')
        offset = 40
        for _ in range(gate_count):
            _, offset = read_ids(offset + 20, 128)
        floor, offset = read_ids(offset, 64)
        _, offset = read_ids(offset, 256)
        if magic == b'HKB2':
            _, offset = read_ids(offset, 256)
    except struct.error as error:
        raise ValueError(f'Truncated boss room bank: {directory}') from error
    if not floor or offset != len(boss):
        raise ValueError(f'Invalid arena floor metadata: {directory}')
    floor_ids = set(floor)

    primary = directory / 'tutorial-room.bin'
    header, shapes, vertices = read_terrain_bank(primary)
    ids = {row[0] for row in struct.iter_unpack('<7I', shapes)}
    if floor_ids <= ids:
        return primary

    # A terrain-only refresh can overwrite the primary bank with the base scene.
    # The full scene export appends the additive floor to those exact contours.
    full = directory / 'tutorial.bin'
    if full.is_file():
        full_header, full_shapes, full_vertices = read_terrain_bank(full)
        full_ids = {row[0] for row in struct.iter_unpack('<7I', full_shapes)}
        if (floor_ids <= full_ids and full_header == header
                and full_shapes.startswith(shapes) and full_vertices.startswith(vertices)):
            return full
    raise ValueError(f'No matching terrain with the arena floor: {directory}')


LEVER_REPAIR_ROOMS = {'Fungus2_14', 'Ruins1_05', 'Ruins1_05b', 'Ruins1_05c'}
LEVER_GATE_FLAG = 2048


def lever_terrain(directory: Path, terrain_stage: Path) -> dict[str, Path]:
    """Recover the matched terrain and lever sidecar from the complete export."""
    primary = directory / 'tutorial-room.bin'
    sidecar = directory / 'terrain-levers.bin'
    header, shapes, vertices = read_terrain_bank(primary)
    gates = [row[0] for row in struct.iter_unpack('<7I', shapes) if row[2] & LEVER_GATE_FLAG]
    if gates:
        validate_lever_bank(sidecar, gates)
        return {primary.name: primary, sidecar.name: sidecar}

    full = directory / 'tutorial.bin'
    full_header, full_shapes, full_vertices = read_terrain_bank(full)
    gates = [row[0] for row in struct.iter_unpack('<7I', full_shapes) if row[2] & LEVER_GATE_FLAG]
    plain_shapes = bytearray(full_shapes)
    for offset in range(8, len(full_shapes), 28):
        flags, = struct.unpack_from('<I', full_shapes, offset)
        struct.pack_into('<I', plain_shapes, offset, flags & ~LEVER_GATE_FLAG)
    recovered = terrain_stage / sidecar.name
    if (not gates or (header, shapes, vertices) != (full_header, plain_shapes, full_vertices)
            or sidecar.read_bytes() != struct.pack('<4sII', b'HKL1', 1, 0)
            or (terrain_stage / primary.name).read_bytes() != full.read_bytes()):
        raise ValueError(f'No matching terrain with lever gates: {directory}')
    validate_lever_bank(recovered, gates)
    return {primary.name: full, sidecar.name: recovered}


def validate_lever_bank(path: Path, gates: list[int]) -> None:
    data = path.read_bytes()
    try:
        magic, version, count = struct.unpack_from('<4sII', data)
        if magic != b'HKL1' or version not in (1, 2) or not 1 <= count <= 8 or count != len(gates):
            raise ValueError(f'Invalid lever bank: {path}')
        offset = 12
        linked = []
        for _ in range(count):
            collider, _, _, points = struct.unpack_from('<4I', data, offset + 8)
            if not 3 <= points <= 16:
                raise ValueError(f'Invalid lever polygon count: {path}')
            linked.append(collider)
            offset += 60 + (20 if version == 2 else 0) + 8 * points
        if sorted(gates) != sorted(linked) or offset != len(data):
            raise ValueError(f'Lever bank does not match the terrain: {path}')
    except struct.error as error:
        raise ValueError(f'Truncated lever bank: {path}') from error


def stage_abyss_activation(source: Path, destination: Path) -> Path:
    """Keep the level327 pipe_acid children hidden until the Waterways drain."""
    data = source.read_bytes()
    owners = ABYSS_ACID_PIPE_OWNERS
    if len(data) < 12:
        raise ValueError(f"Truncated Abyss activation bank: {source}")
    magic, node_count, target_count = struct.unpack_from("<4sII", data)
    if (magic != b"HKA1" or len(data) != 12 * (1 + node_count + target_count)
            or node_count >= 2048 or target_count + len(owners) > 8192):
        raise ValueError(f"Invalid Abyss activation bank: {source}")
    boundary = 12 * (1 + node_count)
    existing_targets = data[boundary:]
    field = 2166136261
    for byte in b"waterwaysAcidDrained":
        field = ((field ^ byte) * 16777619) & 0xffffffff
    nodes = list(struct.iter_unpack("<III", data[12:boundary]))
    pipe = {owner: node for kind, owner, node in struct.iter_unpack("<III", existing_targets)
            if kind == 0 and owner in owners}
    if pipe:
        # 2026-11-25: Steam exports carry the pipe FSM, so the scene converter already
        # emits this rule; accept it only when every pipe owner waits for the drain.
        if set(pipe) == set(owners) and all(node < node_count and nodes[node] == (1, field, 1)
                                            for node in pipe.values()):
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            return destination
        raise ValueError(f"Abyss pipe already has activation records: {source}")
    bank = (struct.pack("<4sII", b"HKA1", node_count + 1, target_count + len(owners))
            + data[12:boundary] + struct.pack("<III", 1, field, 1) + existing_targets
            + b"".join(struct.pack("<III", 0, owner, node_count) for owner in owners))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(bank)
    return destination


def route_toll_library(table: Path) -> str:
    """Return the ROUTE_TOLL_ART library for an RTS4 definition (kind at byte 4)."""
    header = table.read_bytes()[:8]
    if len(header) != 8 or header[:4] != b"RTS4":
        raise ValueError(f"Invalid route toll definition: {table}")
    kind = struct.unpack("<I", header[4:8])[0]
    if kind not in ROUTE_TOLL_ART:
        raise ValueError(f"Unknown route toll kind {kind}: {table}")
    return ROUTE_TOLL_ART[kind]


DOOR_PROMPT_MARKERS = {
    'Crossroads_01': (b'Ascend', (52.61, 10.69), (52.61, 14.63)),
    'Crossroads_06': (b'Enter', (28.41, 29.66), (28.41, 33.60)),
}


def stage_authored_door_marker(scene: str, source: Path, destination: Path) -> Path:
    """Refresh the measured Door Control marker in the frozen HK3DS bank."""
    label, origin, marker = DOOR_PROMPT_MARKERS[scene]
    bank = bytearray(source.read_bytes())
    size = {b'HKD1': 52, b'HKD2': 72, b'HKD3': 76}.get(bytes(bank[:4]))
    if not size or len(bank) < 8 or len(bank) != 8 + size * struct.unpack_from('<I', bank, 4)[0]:
        raise ValueError(f'Invalid door bank: {source}')
    matches = [offset for offset in range(8, len(bank), size)
               if bank[offset + 4:offset + 12] == struct.pack('<2f', *origin)
               and bank[offset + 20:offset + 52].split(b'\0', 1)[0] == label]
    if len(matches) != 1:
        raise ValueError(f'Expected one authored {label!r} door in {source}')
    struct.pack_into('<2f', bank, matches[0] + 12, *marker)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(bank)
    return destination


def stage_dream_dialogue(source: Path, target: Path) -> Path:
    research = source / '_private/research/conditional-events'
    def entries(filename):
        return {entry.attrib['name']: entry.text or ''
                for entry in ET.parse(research / filename).iter('entry')}
    dreamers = entries('EN_Dreamers.xml')
    witch = entries('EN_Dream Witch.xml')
    texts = [['<br>'.join(dreamers[f'DREAMERS_INSPECT_RG{index}'] for index in range(1, 6))]]
    texts += [[dreamers[f'DREAMERS_EC2_{suffix}']] for suffix in
              ('1A', '1A_2', '1B', '1C', '1D', '1E', '1F', '1G', '2')]
    texts += [witch[key].split('<page>') for key in ('WITCH_DREAM1', 'WITCH_DREAM_FALL')]
    punctuation = {0x2018: 39, 0x2019: 39, 0x201c: 34, 0x201d: 34,
                   0x2013: 45, 0x2014: 45, 0x2026: '...'}
    pages = []
    for group, source_pages in enumerate(texts):
        for source_page in source_pages:
            lines = []
            for paragraph in source_page.replace('<page>', '<br>').split('<br>'):
                lines.extend(textwrap.wrap(paragraph.strip().translate(punctuation), width=52,
                                           break_long_words=False) or [''])
            for offset in range(0, len(lines), 4):
                rows = [line.encode('ascii') for line in lines[offset:offset + 4]]
                if any(len(row) >= 96 for row in rows):
                    raise ValueError('Dream dialogue exceeds its bounded row')
                pages.append(struct.pack('<I96s96s96s96s', group, *(rows + [b''] * (4 - len(rows)))))
    if not 12 <= len(pages) <= 32:
        raise ValueError('Dream dialogue exceeds its bounded bank')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(struct.pack('<4sI', b'HKDN', len(pages)) + b''.join(pages))
    return target


def complete_trace(trace: Path, source: Path, assets: Path, stage: Path, output: Path, legs=None, rooms=()):
    files = {}
    aliases = {}

    def add(name, path):
        if not NAME.fullmatch(name):
            raise ValueError(f"Invalid logical asset name: {name}")
        path = path.resolve(strict=True)
        if not path.is_file():
            raise ValueError(f"Not an asset file: {path}")
        key = name.casefold()
        if key in aliases and aliases[key] != name:
            raise ValueError(f"Case alias: {name}")
        if name in files and files[name] != path:
            # A replay against --data records the copied memory-stick layout.
            # Accept it only at the same logical suffix and with identical bytes.
            previous = files[name]
            suffix = tuple(name.split("/"))
            copied = tuple(previous.parts[-len(suffix):]) == suffix
            if not copied or previous.stat().st_size != path.stat().st_size:
                raise ValueError(f"Conflicting logical asset: {name}")
            with previous.open("rb") as a, path.open("rb") as b:
                if hashlib.file_digest(a, "sha256").digest() != hashlib.file_digest(b, "sha256").digest():
                    raise ValueError(f"Conflicting logical asset: {name}")
        aliases[key] = name
        files[name] = path

    for row in trace.read_text(encoding="utf-8-sig").splitlines():
        fields = row.split("\t")
        if len(fields) != 2:
            raise ValueError(f"Invalid trace row: {row}")
        add(fields[0], Path(fields[1]))
    if not files:
        raise ValueError("Empty startup asset trace")
    for name in ("menu-fill.hpt", "title-background.hpt", "title-logo.hpt", "title-mote.hpt"):
        add(name, assets / "menus" / name)
    root = source / "_private/converted/hero/romfs"
    scenes, _, _ = selected_rooms(source, stage, legs, assets)
    scenes = list(dict.fromkeys([*scenes, *explicit_rooms(source, rooms, assets)]))
    if scenes:
        for name in ('map-page-0.hpt', 'map-regions.hpt'):
            add(name, assets / 'map' / name)
    if any(scene in scenes for scene in ('RestingGrounds_04', 'Dream_Nailcollection')):
        add('dream-dialogue.bin', stage_dream_dialogue(source, output.parent / 'dream-dialogue.bin'))
    if any(scene in scenes for scene in ('Town', 'Crossroads_01', 'Fungus1_01')):
        title = assets / 'hud/region-title.bin'
        add(title.name, title)
        for i in range(int.from_bytes(title.read_bytes()[12:16], 'little')):
            name = f'region-title-page-{i}.hpt'
            add(name, title.parent / name)
    enemy_banks = set()
    greenpath_present = False
    for scene, prefix in (('Town', 'town-lift'), ('Crossroads_06', 'shaman-pillar'), ('Fungus2_04', 'mantis-gate'), ('Crossroads_03', 'world-switch'), ('Ruins1_05b', 'waterways-art'),
                          ('Crossroads_49b', 'city-lift-art'), ('Crossroads_49', 'city-lift-art'), ('Fungus2_21', 'city-crest-fx')):
        if scene not in scenes:
            continue
        art = assets / prefix
        table = art / f'{prefix}.psp.bin'
        header = table.read_bytes()[:16]
        if len(header) != 16 or header[:4] != b'PSA1':
            raise ValueError(f'Invalid PSP {prefix} table')
        version, size, pages = struct.unpack_from('<3I', header, 4)
        if version != 1 or size != 512 or not 0 < pages <= 32:
            raise ValueError(f'Invalid PSP {prefix} pages')
        add(f'rooms/{scene}/{prefix}-visual.bin', table)
        for page in range(pages):
            add(f'rooms/{scene}/{prefix}-page-{page}.hpt', art / f'page-{page}.hpt')
    for scene in scenes:
        package = assets / scene
        if scene == 'Dream_Guardian_Monomon':
            film = source / '_private/converted/monomon-presentation/mask-shatter.bin'
            with film.open('rb') as stream:
                if (stream.read(20) != struct.pack('<4s4I', b'HKM1', 200, 112, 348, 30)
                        or film.stat().st_size != 20 + 200 * 112 * 348 * 2):
                    raise ValueError('Invalid Monomon film')
            add(f'rooms/{scene}/mask-shatter.bin', film)
        if scene in ('Crossroads_49b', 'Crossroads_49'):
            # City lift sidecars (main.cpp:1917-1918 bindSavedCityToll): read with plain fopen, so the host trace
            # never lists them. The lift table is the PSA1 art; these are the rooms and machine data.
            leaves = ('city-lift.bin', 'city-transport.bin', 'city-lift-art-room.bin')
            if scene == 'Crossroads_49b':
                leaves += ('city-toll.bin',)
            for leaf in leaves:
                add(f'scenes/{scene}/{leaf}', root / 'scenes' / scene / leaf)
        if scene in ('Crossroads_33', 'Fungus1_06', 'Fungus2_18', 'Abyss_04', 'Fungus2_25'):
            cornifer_package = assets / 'Crossroads_33'
            cornifer = cornifer_package / 'cornifer-visual.bin'
            with cornifer.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 1):
                    raise ValueError('Invalid PSP Cornifer table')
            for leaf in ('cornifer-visual.bin', 'cornifer-page-0.hpt'):
                add(f'rooms/Crossroads_33/{leaf}', cornifer_package / leaf)
        if scene == 'Room_ruinhouse':
            # Sly world quest: PSA1 art (anim-libraries) and the source quest definition
            # (main.cpp:1865-1875, src/world_quests.cpp loadScene). Rooms namespace per scene.
            library = assets / 'anim-libraries/world-quest-art/world-quest-art'
            art = library / 'world-quest-art.psp.bin'
            with art.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 1):
                    raise ValueError('Invalid PSP world quest table')
            add(f'rooms/{scene}/world-quest-visual.bin', art)
            add(f'rooms/{scene}/world-quest-page-0.hpt', library / 'page-0.hpt')
            add('world-quests.bin', root / 'world-quests.bin')
        if scene == 'Fungus2_23':
            # Bretta world quest: PSA1 art (anim-libraries/bretta-quest-art) under the bretta-quest prefix that
            # main.cpp:1870 uses, and the source quest definition (world_quests.cpp loadScene).
            library = assets / 'anim-libraries/bretta-quest-art/bretta-quest-art'
            art = library / 'bretta-quest-art.psp.bin'
            with art.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 1):
                    raise ValueError('Invalid PSP Bretta quest table')
            add(f'rooms/{scene}/bretta-quest-visual.bin', art)
            add(f'rooms/{scene}/bretta-quest-page-0.hpt', library / 'page-0.hpt')
            add('bretta-quest.bin', root / 'bretta-quest.bin')
        if scene == 'Town':
            for npc in ('elderbug', 'tiso'):
                library = assets / npc
                art = library / f'{npc}-visual.bin'
                with art.open('rb') as stream:
                    if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 1):
                        raise ValueError(f'Invalid PSP {npc} table')
                add(f'rooms/{scene}/{npc}-visual.bin', art)
                add(f'rooms/{scene}/{npc}-page-0.hpt', library / 'page-0.hpt')
                add(f'{npc}.bin', library / f'{npc}.bin')
        if scene in ('Fungus2_01', 'Fungus2_14', 'Ruins1_02', 'Fungus3_47'):
            library = assets / 'quirrel' / scene
            art = library / 'quirrel-visual.bin'
            with art.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 1):
                    raise ValueError('Invalid PSP Quirrel table')
            for name in ('quirrel.bin', 'quirrel-visual.bin', 'quirrel-page-0.hpt'):
                add(f'rooms/{scene}/{name}', library / ('page-0.hpt' if name.endswith('.hpt') else name))
        if scene in ('RestingGrounds_07', 'Ruins1_05b'):
            prefix = 'seer' if scene == 'RestingGrounds_07' else 'lemm'
            library = assets / prefix
            for suffix in ('.bin', '-visual.bin', '-page-0.hpt'):
                add(f'rooms/{scene}/{prefix}{suffix}', library / ('page-0.hpt' if suffix.endswith('.hpt') else prefix + suffix))
        if scene == 'Crossroads_10':
            # False Knight boss art: PSA1 table of 11 pages (anim-libraries), the
            # 3DS romfs false-knight-art.bin/page-N bank (main.cpp:1926-1934) in PSP form.
            library = assets / 'anim-libraries/false-knight/false-knight-art'
            art = library / 'false-knight-art.psp.bin'
            with art.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 11):
                    raise ValueError('Invalid PSP False Knight table')
            add(f'rooms/{scene}/false-knight-visual.bin', art)
            # FKF4 effect configs: the same 3DS romfs file the shared CrossroadsBoss::loadEffects reads.
            add(f'rooms/{scene}/false-knight-fx.bin', root / 'false-knight-fx.bin')
            for page in range(11):
                add(f'rooms/{scene}/false-knight-page-{page}.hpt', library / f'page-{page}.hpt')
        if scene == 'Crossroads_04':
            # Gruz Mother boss art: PSA1 table of 10 pages (anim-libraries), the 3DS romfs
            # boss-art.bin/boss-page-N bank (main.cpp:1926-1934, generic non-knight path) in PSP form.
            library = assets / 'anim-libraries/crossroads-bosses/boss-art'
            art = library / 'boss-art.psp.bin'
            with art.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 10):
                    raise ValueError('Invalid PSP Gruz Mother table')
            add(f'rooms/{scene}/boss-visual.bin', art)
            for page in range(10):
                add(f'rooms/{scene}/boss-page-{page}.hpt', library / f'page-{page}.hpt')
        if scene == 'Waterways_05':
            # Dung Defender boss art: PSA1 table of 3 pages (anim-libraries), the 3DS romfs
            # dung-art.bin/dung-page-N bank (main.cpp:1926-1931, dung path) in PSP form.
            library = assets / 'anim-libraries/dung-defender/dung-art'
            art = library / 'dung-art.psp.bin'
            with art.open('rb') as stream:
                if stream.read(16) != struct.pack('<4s3I', b'PSA1', 1, 512, 3):
                    raise ValueError('Invalid PSP Dung Defender table')
            add(f'rooms/{scene}/dung-visual.bin', art)
            for page in range(3):
                add(f'rooms/{scene}/dung-page-{page}.hpt', library / f'page-{page}.hpt')
        if scene in ('Crossroads_47', 'Room_Town_Stag_Station'):
            # Stag bank: anim-libraries/stag/stag-anim, PSA1 table of 5 pages (main.cpp:1829 in PSP form). Both stag
            # rooms bind it; the Town station draws its stand and bell from it.
            if scene == 'Crossroads_47':
                # Stag station art: PSA1 table of 2 pages (transcode_stag_station.py). The 3DS stag-station-art.bin
                # and -room.bin stay in scenes/ (bindSavedStagStation, main.cpp:1907), staged by the loop below.
                station = assets / 'stag-station-art'
                art = station / 'stag-station-art.psp.bin'
                header = art.read_bytes()[:16]
                if len(header) != 16 or header[:4] != b'PSA1' or struct.unpack_from('<3I', header, 4) != (1, 512, 2):
                    raise ValueError('Invalid PSP stag station table')
                add(f'rooms/{scene}/stag-station-art-visual.bin', art)
                for page in range(2):
                    add(f'rooms/{scene}/stag-station-art-page-{page}.hpt', station / f'page-{page}.hpt')
            library = assets / 'anim-libraries/stag/stag-anim'
            art = library / 'stag-anim.psp.bin'
            header = art.read_bytes()[:16]
            if len(header) != 16 or header[:4] != b'PSA1' or struct.unpack_from('<3I', header, 4) != (1, 512, 5):
                raise ValueError('Invalid PSP stag bank table')
            add(f'rooms/{scene}/stag-visual.bin', art)
            for page in range(5):
                add(f'rooms/{scene}/stag-page-{page}.hpt', library / f'page-{page}.hpt')
        table = package / "room-visual.psp.bin"
        with table.open("rb") as stream:
            header = stream.read(16)
        if len(header) != 16 or header[:4] != b"PSR1":
            raise ValueError(f"Invalid PSP room table: {table}")
        version, size, pages = struct.unpack("<3I", header[4:])
        if version not in (1, 2) or size != 512 or not 0 < pages <= 64:
            raise ValueError(f"Invalid PSP room header: {table}")
        prefix = f"rooms/{scene}/"
        add(prefix + "static-room-visual.bin", table)
        switches = table.with_suffix(".bin.switches")
        if switches.is_file():
            add(prefix + "static-room-visual.bin.switches", switches)
        for page in range(pages):
            add(prefix + f"static-room-page-{page}.hpt", package / f"page-{page}.hpt")
        for sidecar in sorted(package.glob("static-*.bin")):
            add(prefix + sidecar.name, sidecar)
        if scene == "Tutorial_01":
            continue  # Tutorial simulation metadata already belongs to the startup trace.
        directory = scene_directory(root / "scenes", scene, assets)
        required_metadata = ("scene.bin", "tutorial-room.bin", "scene-lighting.bin")
        terrain = {}
        if scene == "Crossroads_10":
            terrain["tutorial-room.bin"] = false_knight_terrain(directory)
        if scene in LEVER_REPAIR_ROOMS:
            terrain = lever_terrain(directory, root.parent.parent / "scene-terrain" / scene / "romfs")
            required_metadata += ("terrain-levers.bin",)
        for required in required_metadata:
            name = f"scenes/{scene}/{required}"
            if required in terrain:
                # A replay trace may refer to terrain staged before validation.
                files.pop(name, None)
            add(name, terrain.get(required, directory / required))
        if scene == "Fungus2_21":
            for suffix in ("controller", "room", "audio", "fx-art", "fx", "slam", "rocks"):
                name = f"city-crest-{suffix}.bin"
                add(f"scenes/{scene}/{name}", directory / name)
        if scene in ROUTE_TOLL_SCENES:
            toll = f"route-tolls/{scene}.bin"
            add(toll, root / toll)
            library = assets / route_toll_library(root / toll)
            table = library / f"{library.name}.psp.bin"
            with table.open("rb") as stream:
                if stream.read(16) != struct.pack("<4s3I", b"PSA1", 1, 512, 2):
                    raise ValueError("Invalid PSP route toll table")
            add(f"rooms/{scene}/toll-visual.bin", table)
            add(f"rooms/{scene}/toll-page-0.hpt", library / "page-0.hpt")
            add(f"rooms/{scene}/toll-page-1.hpt", library / "page-1.hpt")
        if scene == "Room_mapper":
            # Iselda's shop: the ornament page only (her one item has no icon).
            icons = assets / 'sly-shop'
            add('rooms/Room_mapper/sly-icons-visual.bin', icons / 'sly-shop-icons.psp.bin')
            add('rooms/Room_mapper/sly-icons-page-0.hpt', icons / 'page-0.hpt')
        if scene == "Room_shop":
            add("sly-shop.bin", root / "sly-shop.bin")
            actor = assets / 'anim-libraries/sly-shop/sly-shop-actor-art'
            icons = assets / 'sly-shop'
            add('rooms/Room_shop/sly-actor-visual.bin', actor / 'sly-shop-actor-art.psp.bin')
            add('rooms/Room_shop/sly-actor-page-0.hpt', actor / 'page-0.hpt')
            add('rooms/Room_shop/sly-icons-visual.bin', icons / 'sly-shop-icons.psp.bin')
            add('rooms/Room_shop/sly-icons-page-0.hpt', icons / 'page-0.hpt')
        enemy_banks.update(scene_enemy_banks(directory / "scene.bin"))
        greenpath = directory / "greenpath-enemies.bin"
        if greenpath.is_file():
            with greenpath.open('rb') as stream:
                header = stream.read(8)
            if len(header) != 8 or header[:4] not in (b'HKG1', b'HKG2'):
                raise ValueError(f'Invalid Greenpath metadata: {greenpath}')
            greenpath_present |= struct.unpack_from('<I', header, 4)[0] != 0
        ambient = directory / "scene-ambient.bin"
        if ambient.is_file():
            bank = ambient.read_bytes()
            if len(bank) != 8 or not (bank[:4] == b"HAP1" and int.from_bytes(bank[4:], "little") in (0, 1)
                                     or bank[:4] == b"HAP2" and int.from_bytes(bank[4:], "little") in (0, 5, 9, 10, 25)):
                raise ValueError(f"Invalid scene ambient bank: {ambient}")
            zone = int.from_bytes(bank[4:], "little")
            if zone in (9, 10, 25):
                name = f"deepnest-ambient-{zone}.bin"
                add(name, root / name)
        # SceneBank, terrain, breakables, collectibles and room mechanisms use
        # optional sidecars. Ship present files so omission cannot change rules.
        # The current resident table also supplies original sprite bounds to
        # TutorialSession::bindSceneBreakables; its .t3x pages are never needed.
        for metadata in sorted(directory.glob("*.bin")):
            if metadata.name in required_metadata:
                continue
            if scene == "Abyss_01" and metadata.name == "activation.bin":
                continue
            if not metadata.name.startswith("room-resident-old-"):
                name = f"scenes/{scene}/{metadata.name}"
                if metadata.name == 'doors.bin' and scene in DOOR_PROMPT_MARKERS:
                    metadata = stage_authored_door_marker(scene, metadata, output.parent / name)
                add(name, metadata)
        if scene == "Abyss_01":
            name = "scenes/Abyss_01/activation.bin"
            add(name, stage_abyss_activation(directory / "activation.bin", output.parent / name))
        if scene == room.archive_particles.SCENE:
            bank_name = room.archive_particles.BANK
            add(f"scenes/{scene}/{bank_name}", assets / scene / bank_name)
    for name in sorted(enemy_banks):
        generated = assets / "scene-enemies" / name
        if name == "zombie-guard-anim.bin":
            generated = assets / "anim-libraries/host/zombie-guard/zombie-guard-anim" / name
        if name == "spitter-anim.bin":
            generated = assets / "anim-libraries/host/spitter/spitter-anim" / name
        if name == "fungus-flyer-anim.bin":
            generated = assets / "anim-libraries/host/fungus-flyer/fungus-flyer-anim" / name
        if name == "jellyfish-anim.bin":
            generated = assets / "anim-libraries/host/jellyfish/jellyfish-anim" / name
        if name == "fungoon-baby-anim.bin":
            generated = assets / "anim-libraries/host/fungoon-baby/fungoon-baby-anim" / name
        if name == "fung-crawler-anim.bin":
            generated = assets / "anim-libraries/host/fung-crawler/fung-crawler-anim" / name
        add(name, root / name if name.endswith("-art.bin") else generated)
    for stem, folder in SCENE_ENEMY_RENDER:
        name = SCENE_ENEMY_ART.get(stem, f'{stem}-anim.bin')
        if not (greenpath_present if stem == 'greenpath' else name in enemy_banks):
            continue
        library = assets / 'anim-libraries' / folder
        table = library / name.replace('.bin', '.psp.bin')
        add(f'rooms/scene-enemy/{name}', table)
        header = table.read_bytes()[:16]
        if len(header) != 16 or header[:4] != b'PSA1':
            raise ValueError(f'Invalid PSP scene enemy table: {table}')
        pages = struct.unpack_from('<I', header, 12)[0]
        if not 0 < pages <= 8:
            raise ValueError(f'Invalid PSP scene enemy page count {pages}: {table}')
        for page in range(pages):
            add(f'rooms/scene-enemy/{stem}-page-{page}.hpt', library / f'page-{page}.hpt')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("".join(f"{name}\t{files[name]}\n" for name in sorted(files)), encoding="utf-8")
    return {"rooms": len(scenes), "files": len(files),
            "bytes": sum(path.stat().st_size for path in files.values())}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=room.source_root())
    parser.add_argument("--assets", type=Path, default=room.REPO / "_private/converted-psp")
    parser.add_argument("--stage", type=Path, default=DEFAULT_STAGE)
    parser.add_argument("--legs", nargs="+", type=int)
    parser.add_argument("--rooms", nargs="+", default=(), help="Additional converted rooms to stage")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    print(complete_trace(args.trace, args.source, args.assets, args.stage, args.out, args.legs, args.rooms))
