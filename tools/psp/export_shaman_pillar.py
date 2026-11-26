"""Export the Crossroads_06 Shaman pillar feedback sources from the game files.

Writes the inputs `tools/shaman_pillar_feedback.py` reads: the six particle
emitters (level42), their transform chains, the three billboard materials and
textures (resources.assets), the three PCM clips and `source-report.json`.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from shaman_pillar_feedback import EMITTERS

SCENE = 'level42'
MATERIALS = (146, 162, 170)
TEXTURES = (248, 371, 438)
CLIPS = (('hit', 'switch_gate_switch', 'sharedassets7.assets', 106),
         ('shake', 'switch_gate_gate', 'sharedassets39.assets', 17),
         ('finish', 'platform_dirt_fall_1', 'sharedassets6.assets', 170))


def dump(value):
    # Raw byte fields (texture data) are recorded by length only.
    return json.dumps(value, indent=2, default=lambda raw: {'byteLength': len(raw)}).encode('utf-8')


def export(source, output):
    import UnityPy

    output.mkdir(parents=True, exist_ok=True)
    files = {}

    def objects(name):
        if name not in files:
            files[name] = next(iter(UnityPy.load(str(source / name)).files.values())).objects
        return files[name]

    dependencies = []

    def save(collection, ident, prefix):
        obj = objects(collection)[ident]
        raw = dump(obj.read_typetree())
        (output / f'{prefix}-{ident}.json').write_bytes(raw)
        dependencies.append(dict(file=collection, id=ident, type=obj.type.name,
                                 sha256=hashlib.sha256(raw).hexdigest()))
        return obj

    scene = objects(SCENE)
    children = []
    for owner, particle, renderer in EMITTERS:
        game_object = save(SCENE, owner, SCENE).read_typetree()
        components = []
        for entry in game_object['m_Component']:
            ident = entry['component']['m_PathID']
            kind = scene[ident].type.name
            components.append(dict(id=ident, type=kind))
            if kind == 'Transform':
                transform = save(SCENE, ident, SCENE).read_typetree()
                while transform['m_Father']['m_PathID']:
                    transform = save(SCENE, transform['m_Father']['m_PathID'], SCENE).read_typetree()
        if not {particle, renderer} <= {row['id'] for row in components}:
            raise ValueError(f'Changed Shaman pillar emitter {owner}')
        save(SCENE, particle, SCENE)
        save(SCENE, renderer, SCENE)
        children.append(dict(game_object=owner, name=game_object['m_Name'], components=components))
    for ident in MATERIALS:
        save('resources.assets', ident, 'resources')
    for ident in TEXTURES:
        texture = save('resources.assets', ident, 'resources')
        texture.read().image.save(output / f'resources-{ident}.png')
    clips = []
    for role, name, collection, ident in CLIPS:
        clip = objects(collection)[ident].read()
        if clip.m_Name != name:
            raise ValueError(f'Changed Shaman pillar clip {role}: {clip.m_Name}')
        samples = clip.samples
        if len(samples) != 1:
            raise ValueError(f'Unexpected sample count for {name}')
        raw = next(iter(samples.values()))
        (output / f'{role}.wav').write_bytes(raw)
        clips.append(dict(role=role, name=name, collection=collection, path_id=ident,
                          sha256=hashlib.sha256(raw).hexdigest()))
    report = dict(scene='Crossroads_06', children=children, dependencies=dependencies, clips=clips)
    (output / 'source-report.json').write_bytes(dump(report))
    print(f'Shaman pillar: {len(children)} emitters, {len(dependencies)} objects, {len(clips)} clips')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--source', type=Path, default=ROOT / '_private/original/Hollow Knight/hollow_knight_Data')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/minimum-obstacles/shaman-pillar-feedback')
    args = parser.parse_args()
    export(args.source, args.output)


if __name__ == '__main__':
    main()
