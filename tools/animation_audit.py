"""Read-only route actor animation audit; no game, extraction service or PIL.

CSV cells say 'missing' / 'data not exported' rather than treating absent banks
or source components as equality. Findings are data, not permission to substitute
clips. Run facing export first to extend the old component snapshots.
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from animation_library import parse_events
from export_actor_facing import ROOT, read_json, source_root, structure


def constants(path):
    """Read converter tables without importing converters or their dependencies."""
    values = {}

    def evaluate(node):
        if isinstance(node, ast.Name) and node.id in values:
            return values[node.id]
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == 'dict' and not node.args:
            return {k.arg: evaluate(k.value) for k in node.keywords}
        if isinstance(node, (ast.Tuple, ast.List)):
            return [evaluate(x) for x in node.elts]
        if isinstance(node, ast.Dict):
            return {evaluate(k): evaluate(v) for k, v in zip(node.keys, node.values)}
        return ast.literal_eval(node)

    if not path.exists():
        return values
    for node in ast.parse(path.read_text(encoding='utf-8-sig')).body:
        if isinstance(node, ast.Assign):
            try:
                value = evaluate(node.value)
            except (ValueError, TypeError, KeyError):
                continue
            for target in node.targets:
                if isinstance(target, ast.Name):
                    values[target.id] = value
    return values


def find_bank(root, folder, name):
    converted = Path(root) / '_private/converted'
    candidates = [converted/folder/name, converted/folder/'romfs'/name,
                  converted/'hero/romfs'/name]
    return next((p for p in candidates if p.is_file()), candidates[0])


def inventory(root, facing_root=None):
    """Inventory the implemented first-ending route's enemy, boss and NPC banks.

    Effect-only prefixes (Needle, gates, floors, smoke) remain in bank validation,
    but are not misreported as independently facing actors.
    """
    root = Path(root)
    research, converted = root/'_private/research', root/'_private/converted'
    if not research.is_dir():
        raise ValueError(f'{research}: source research root is missing')
    facing_root = facing_root or ROOT/'_private/converted-psp/actor-facing'
    result, clip_sources = [], {}

    def read_source(path):
        data = read_json(path)
        libraries = [structure(data).get('clips', [])] if isinstance(data, dict) else []
        if isinstance(data, dict):
            libraries += list(data.get('source_clips', {}).values())
        digest = None
        for clips in libraries:
            for clip in clips:
                if isinstance(clip, dict):
                    if digest is None:
                        digest = hashlib.sha256(Path(path).read_bytes()).hexdigest()
                    clip_sources[id(clip)] = (str(path), digest)
        return data

    def add(family, prefix, clips, bank, module, aliases=None, components=None):
        result.append(dict(id=f'{family}/{prefix}', family=family, prefix=prefix,
                           clips=[c for c in clips if c.get('name')], aliases=aliases or {},
                           bank=str(find_bank(root, family, bank)), module=module,
                           components=components or [],
                           source_files=sorted(set(clip_sources[id(c)] for c in clips if id(c) in clip_sources))))

    for stem in ('tiktik', 'gruzzer', 'aspid-hatchling'):
        directory = research/'crossroads-enemies-a'
        f = directory/f'{stem}-map.json'
        if f.exists():
            m = read_source(f)
            lib = m['library']
            if isinstance(lib, dict):
                lib = lib.get('file', lib.get('name'))
            clips = structure(read_source(directory/lib))['clips']
            add('crossroads-enemies-a', stem, clips, f'{stem}-anim.bin', 'src/enemy.cpp',
                components=[dict(directory=str(directory), parts=m['actor'])])
    # The original Buzzer is also the route's Vengefly implementation.
    for family, stem, directory, map_name, module in [
            ('crawler', 'crawler', 'enemy', 'crawler-map.json', 'src/enemy.cpp'),
            ('buzzer', 'vengefly', 'buzzer', 'buzzer-map.json', 'src/room_entities.cpp')]:
        directory = research/directory
        f = directory/map_name
        if f.exists():
            m = read_source(f)
            parts = m.get('actor', m.get('parts', {}))
            component_directory = directory
            if family == 'buzzer':
                # This converter's authored FSMs live in the Tutorial inventory,
                # not the later Crossroads scene snapshots.
                tutorial = research/'tutorial'
                tutorial_map = tutorial/'inventory.json'
                if tutorial_map.exists():
                    parts = {}
                    for row in read_source(tutorial_map)['rows']:
                        if row['type'] != 'BoxCollider2D':
                            continue
                        if not read_source(tutorial/row['go'])['m_Name'].startswith('Buzzer'):
                            continue
                        for kind, names in row['parts'].items():
                            parts.setdefault(kind, []).extend(names)
                    component_directory = tutorial
            lib = m['library']
            if isinstance(lib, dict):
                lib = lib.get('file', lib.get('name'))
            add(family, stem, structure(read_source(directory/lib))['clips'],
                f'{family}-anim.bin', module,
                components=[dict(directory=str(component_directory), parts=parts)])
            if family == 'buzzer':
                add('crossroads-enemies-a', 'vengefly', result[-1]['clips'], 'vengefly-anim.bin', 'src/enemy.cpp',
                    components=result[-1]['components'])
    directory = research/'crossroads-enemies-b'
    f = directory/'enemies-map.json'
    if f.exists():
        mapping = read_source(f)
        for group, prefix in zip(('wandering_husk', 'husk_hornhead', 'leaping_husk', 'goam'),
                                 ('wandering', 'hornhead', 'leaping', 'goam')):
            actor = mapping[group]
            aliases = dict(idle='Idle', walk='Walk', turn='Turn', windup='Attack Anticipate',
                           attack='Attack Lunge', land='Attack Cooldown', death='Death Air', death_land='Death Land', air='Fall')
            if prefix == 'leaping':
                aliases.update(windup='Attack', attack='Attack', air='Attack', land='Land')
            if prefix == 'goam':
                aliases = dict(idle='Down', rise='Burst', exposed='Idle', retract='Retract')
            add('enemy-b', prefix, structure(read_source(directory/actor['libraries'][0]))['clips'],
                'enemy-b-anim.bin', 'src/enemy_b.cpp', aliases,
                [dict(directory=str(directory), parts=actor['parts'])])
    tables = constants(ROOT/'tools/convert_enemy_c.py')
    for prefix, (key, aliases) in {**tables.get('ACTORS', {}), **tables.get('BATTLE_ACTORS', {})}.items():
        directory = (ROOT/'_private/converted/battle-enemies-source' if prefix in tables.get('BATTLE_ACTORS', {})
                     else research/'crossroads-enemies-c')
        f = directory/'enemies-map.json'
        if f.exists():
            actor = read_source(f)['actors'][key]
            components = [dict(directory=str(directory), parts=actor['components'])]
            # Extend battle snapshots with all components on their original GO.
            transform = actor['components'].get('Transform', [])
            if transform:
                t = structure(read_source(directory/transform[0]))
                owner = t['m_GameObject']['m_PathID']
                collection = transform[0].rsplit('-', 1)[0]
                components = [dict(directory=str(directory), collection=collection, identity=owner)]
            add('enemy-c', prefix, structure(read_source(directory/actor['animation_libraries'][0]))['clips'],
                'enemy-c-anim.bin', 'src/enemy_c.cpp', aliases, components)
    families = [
        ('greenpath-enemies', 'greenpath-art.bin', 'src/greenpath_enemy.cpp', 'convert_greenpath_enemies.py',
         ('Mosscreep','Maskfly','MossKnight','Squit','Aluba','Durandoo','VolatileMosskin','MossCharger','Hornet')),
        ('crossroads-bosses', 'boss-art.bin', 'src/crossroads_boss.cpp', 'convert_crossroads_bosses.py', ('Gruz','Knight','Head','Gruzzer')),
        ('dung-defender', 'dung-art.bin', 'src/crossroads_boss.cpp', 'convert_dung_defender.py', ('Dung',)),
        ('broken-vessel', 'broken-vessel-art.bin', 'src/broken_vessel.cpp', 'convert_broken_vessel_art.py', ('BrokenVessel','Balloon','Scuttler')),
        ('mantis-lords', 'mantis-art.bin', 'src/mantis_lords.cpp', 'convert_mantis_lords.py', ('Lord',)),
        ('watcher-knights', 'watcher-art.bin', 'src/watcher_knights.cpp', 'convert_watcher_art.py', ('Watcher',)),
        ('uumuu-body', 'uumuu-body-art.bin', 'src/uumuu_body.cpp', 'convert_uumuu_body_art.py', ('Uumuu',)),
        ('uumuu-quirrel', 'uumuu-quirrel-art.bin', 'src/uumuu_quirrel.cpp', 'convert_uumuu_quirrel_art.py', ('Quirrel',)),
    ]
    for family, bank, module, converter, prefixes in families:
        f = converted/family/'manifest.json'
        if not f.exists():
            f = research/family/'manifest.json'
        if not f.exists():
            # Missing families cannot quietly disappear from coverage.
            for prefix in prefixes:
                add(family, prefix, [], bank, module)
            continue
        manifest = read_source(f)
        tables = constants(ROOT/'tools'/converter)
        aliases = tables.get('ALIASES', {})
        if family == 'greenpath-enemies':
            aliases = {p: (file, lib, a) for _, p, _, file, lib, a in tables['ACTORS']}
        rows_path = research/family/'source-components.json'
        rows = read_source(rows_path) if rows_path.exists() else []
        for prefix in prefixes:
            alias = aliases.get(prefix, [None, None, {}])[-1]
            if not isinstance(alias, dict):
                alias = {}
            components = []
            prefab = next((r[0] for r in tables.get('ACTORS', []) if isinstance(r, list) and len(r) > 1 and r[1] == prefix), None)
            selected = [r for r in rows if r.get('native_type') == 'Transform' and not r.get('local_position')
                        and (prefab is None or r['actor'] == prefab)]
            # Match boss library actors to their original GO names, not all
            # transforms in a scene (which would mix independent conventions).
            names = {'Gruz': {'Giant Fly'}, 'Knight': {'False Knight New'}, 'Gruzzer': {'Fly'}}
            if prefix in names:
                selected = [r for r in selected if r['actor'] in names[prefix]]
            if selected:
                row = selected[0]
                components = [dict(collection=row['scene'], identity=row['data']['m_GameObject']['m_PathID'])]
            add(family, prefix, manifest.get('source_clips', {}).get(prefix, []), bank, module, alias, components)
    for family, folder, map_name, bank, module in [
            ('cornifer-original', 'cornifer', 'cornifer-map.json', 'cornifer-anim.bin', 'src/cartographer.cpp'),
            ('stag', 'stag-stations', 'stag-map.json', 'stag-anim.bin', 'src/resting.cpp')]:
        directory = research/folder
        f = directory/map_name
        if f.exists():
            m = read_source(f)
            parts = m.get('parts', {})
            add(family, family, m['clips'], bank, module,
                components=[dict(directory=str(directory), parts=parts)])
    # Separate route banks must not disappear simply because they use a
    # specialized converter instead of convert_crossroads_bosses.
    for family, folder, bank, module, prefixes in [
            ('elder-baldur', 'converted/elder-baldur', 'elder-baldur-art.bin', 'src/elder_baldur.cpp', ('', 'Roller')),
            ('false-knight', 'converted/false-knight', 'false-knight-art.bin', 'src/crossroads_boss.cpp', ('Knight', 'Head')),
            ('monomon-archive-art', 'minimum-bosses/monomon-archive-art', 'monomon-archive-art.bin', 'src/monomon_room.cpp', ('Monomon',)),
            ('monomon-guardian-art', 'minimum-bosses/monomon-guardian-art', 'monomon-guardian-art.bin', 'src/monomon_room.cpp', ('Monomon',)),
    ]:
        path = root/'_private'/folder/'manifest.json'
        manifest = read_source(path) if path.exists() else {}
        for prefix in prefixes:
            clips = manifest.get('source_clips', {}).get(prefix, [])
            add(family, prefix, clips, bank, module,
                aliases={c['name']: c['name'] for c in clips})
    for quest, folder, family, bank in [
            ('sly', 'art', 'world-quest-art', 'world-quest-art.bin'),
            ('bretta', 'bretta-art', 'bretta-quest-art', 'bretta-quest-art.bin')]:
        directory = root/'_private/minimum-world-quests'/folder
        path = directory/'world-quest-art-source.json'
        mapping = read_source(path) if path.exists() else {}
        library = directory/mapping.get('library', '__missing__')
        clips = structure(read_source(library))['clips'] if library.exists() else []
        add(family, quest, clips, bank, 'src/world_quests.cpp', mapping.get('clip_sources'),
            [dict(directory=str(directory), parts={})])
    directory = research/'hollow-shade'
    path = directory/'shade-map.json'
    mapping = read_source(path) if path.exists() else {}
    library = directory/mapping.get('libraries', {}).get('shade', '__missing__')
    add('shade', 'shade', structure(read_source(library))['clips'] if library.exists() else [],
        'shade-anim.bin', 'src/shade.cpp')
    directory = research/'conditional-events'
    path = directory/'event-map.json'
    mapping = read_source(path) if path.exists() else {}
    for prefix in ('mushroom', 'grimmkin'):
        library = directory/mapping.get('libraries', {}).get(prefix, {}).get('file', '__missing__')
        add('conditional-events', prefix, structure(read_source(library))['clips'] if library.exists() else [],
            'events-anim.bin', 'src/conditional_events.cpp',
            components=[dict(directory=str(directory), parts=mapping.get('actors', {}).get(prefix, {}))])
        result[-1]['bank_prefix'] = prefix+' '
    # These converters originally retained only selected clip names. Optional
    # read-only extraction in export_actor_facing extends them to full libraries.
    for family, prefix, module in [('sly-shop', 'sly-shop-actor', 'src/main.cpp'),
                                  ('route-tolls', 'city-toll-actor', 'src/tutorial.cpp'),
                                  ('route-tolls', 'mines-toll-actor', 'src/tutorial.cpp'),
                                  ('greenpath-tolls', 'greenpath-toll-actor', 'src/tutorial.cpp')]:
        snapshot = facing_root/'source-libraries'/(prefix+'.json')
        mapping = read_source(snapshot) if snapshot.exists() else {}
        add(family, prefix, mapping.get('clips', []), prefix+'-art.bin', module,
            {'Sly Idle': 'Idle'} if family == 'sly-shop' else {})
    # Source identities are the authored GameObject names/collections, not
    # conventions inferred from a similar actor's pixels.
    source_owners = {
        'crossroads-bosses/Gruz': ('level40', ['Giant Fly']),
        'crossroads-bosses/Knight': ('level48', ['False Knight New']),
        'crossroads-bosses/Gruzzer': ('level40', ['Fly']),
        'dung-defender/Dung': ('level355', ['Dung Defender']),
        'mantis-lords/Lord': ('level180', ['Mantis Lord']),
        'broken-vessel/BrokenVessel': ('level343', ['Infected Knight']),
        'watcher-knights/Watcher': ('level115', ['Black Knight 1']),
        'uumuu-body/Uumuu': ('level229', ['Mega Jellyfish']),
        'uumuu-quirrel/Quirrel': ('level228', ['Quirrel']),
    }
    for actor in result:
        if actor['id'] in source_owners:
            collection, names = source_owners[actor['id']]
            actor['components'] = [dict(collection=collection, names=names)]
        if actor['id'] == 'enemy-c/baldur':
            actor['components'] = [dict(collection='level76', identity=261)]
        snapshots = {
            'monomon-archive-art/Monomon': ('uumuu/level228-source.json', 1607),
            'monomon-guardian-art/Monomon': ('uumuu/level402-source.json', 84),
        }
        if actor['id'] in snapshots:
            name, identity = snapshots[actor['id']]
            actor['components'] = [dict(snapshot=str(root/'_private/minimum-bosses'/name), identity=identity)]
        if actor['family'] in ('uumuu-body', 'uumuu-quirrel'):
            actor['modules'] = [str(p.relative_to(ROOT)).replace('\\', '/') for p in sorted((ROOT/'src').glob('uumuu*.cpp'))]
        else:
            actor['modules'] = [actor['module']]
    return result


def bank_clips(path):
    """Validate HKA1/PSA1 structure before comparing their 68-byte clip tables."""
    data = Path(path).read_bytes()
    if len(data) < 28:
        raise ValueError(f'{path}: truncated animation header')
    if data[:4] == b'HKA1':
        pages, sprites, clips, frames = struct.unpack_from('<4I', data, 4)
        offset = 20
    elif data[:4] == b'PSA1':
        version, size, pages, sprites, clips, frames = struct.unpack_from('<6I', data, 4)
        if version != 1 or size not in (128, 256, 512):
            raise ValueError(f'{path}: unsupported PSA1 header')
        offset = 28
    else:
        raise ValueError(f'{path}: not an animation bank')
    if not (0 < pages <= 32 and 0 < sprites <= 65536 and 0 < clips <= 8192 and 0 < frames <= 1048576):
        raise ValueError(f'{path}: invalid bank counts')
    offset += sprites * 36
    end = offset + clips * 68 + frames * 4
    if len(data) < end:
        raise ValueError(f'{path}: incorrect bank length')
    events = parse_events(data[end:], frames)
    result = {}
    frame_table = struct.unpack_from(f'<{frames}I', data, offset + clips * 68)
    if any(i >= sprites for i in frame_table):
        raise ValueError(f'{path}: invalid sprite index')
    for i in range(clips):
        name, fps, wrap, loop, first, count = struct.unpack_from('<48sf4I', data, offset + i * 68)
        if (b'\0' not in name or not count or first + count > frames or
                not math.isfinite(fps) or fps <= 0 or wrap > 6 or (wrap == 1 and loop >= count)):
            raise ValueError(f'{path}: invalid clip record')
        name = name.split(b'\0', 1)[0].decode('utf-8')
        if name in result:
            raise ValueError(f'{path}: duplicate clip {name}')
        result[name] = dict(fps=fps, wrap=wrap, loopStart=loop, count=count,
                            frames=frame_table[first:first+count],
                            events=[(at-first,info,integer,value) for at,info,integer,value in events
                                    if first <= at < first+count])
    return result


def cpp_calls(path):
    """Collect play/enter calls, preserving source lines and dynamic arguments.

    Comments and quoted strings are tokenized, so commas/parentheses inside clip
    names cannot corrupt argument boundaries. Conditional literals are retained.
    """
    if not path.exists():
        return []
    source = path.read_text(encoding='utf-8-sig')
    tokens = list(re.finditer(r'''//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[A-Za-z_]\w*|[^\s]''', source))
    tokens = [t for t in tokens if not t.group().startswith(('//', '/*'))]
    result = []
    for i, token in enumerate(tokens[:-1]):
        if token.group() not in ('play', 'enter') or tokens[i+1].group() != '(':
            continue
        # Out-of-class definitions and simple declarations are not call sites.
        if i and (tokens[i-1].group() == ':' or tokens[i-1].group() in ('void', 'bool')):
            continue
        depth, args, current = 0, [], []
        for t in tokens[i+2:]:
            value = t.group()
            if value == ')' and depth == 0:
                args.append(current)
                break
            if value == ',' and depth == 0:
                args.append(current); current = []
            else:
                current.append(t)
                if value in ('(', '{', '['): depth += 1
                elif value in (')', '}', ']'): depth -= 1
        literals = [json.loads(t.group()) for a in args for t in a if t.group().startswith('"')]
        result.append(dict(call=token.group(), line=source.count('\n', 0, token.start()) + 1,
                           literals=literals,
                           expression=','.join(''.join(t.group() for t in a) for a in args)))
    return result


def compare(source, bank):
    if bank is None:
        return 'missing'
    expected = (float(source['fps']), source['wrapMode'], source.get('loopStart', 0), len(source['frames']))
    actual = (bank['fps'], bank['wrap'], bank['loopStart'], bank['count'])
    # Source JSON floats are compared after the same float32 encoding as HKA1.
    expected = (struct.unpack('<f', struct.pack('<f', expected[0]))[0], *expected[1:])
    return 'equal' if expected == actual else 'different'


def compare_events(source, bank):
    if bank is None: return 'missing'
    expected=[(i,f.get('eventInfo',''),f.get('eventInt',0),
               struct.unpack('<f',struct.pack('<f',f.get('eventFloat',0)))[0])
              for i,f in enumerate(source['frames']) if f.get('triggerEvent')]
    return 'equal' if expected==bank.get('events',[]) else 'different'


def audit(actors, facing, psp_root, output):
    output.mkdir(parents=True, exist_ok=True)
    cache, rows, runtime = {}, [], []
    for actor in actors:
        path = Path(actor['bank'])
        if path not in cache:
            cache[path] = bank_clips(path) if path.exists() else {}
        bank = cache[path]
        psp_names = (path.name, path.with_suffix('.psp.bin').name)
        psp_paths = sorted({p for name in psp_names for p in psp_root.rglob(name)
                            if 'host' not in p.relative_to(psp_root).parts})
        psp_banks = [(p, bank_clips(p)) for p in psp_paths]
        prefix = actor['prefix']
        f = facing.get(actor['id'], {})
        calls = [(module, call) for module in actor.get('modules', [actor['module']])
                 for call in cpp_calls(ROOT/module)]
        actor_names = set(bank)
        qualified = any(n.startswith(prefix + '/') for n in bank)
        name_prefix = actor.get('bank_prefix', prefix+'/' if qualified else '')
        resolved = set()
        for module, call in calls:
            for name in call['literals'] or [None]:
                # A literal containing a slash already names its own family.
                if name is not None and '/' in name and not name.startswith(prefix + '/'):
                    continue
                full = name_prefix + name if name is not None and name_prefix and not name.startswith(name_prefix) and '/' not in name else name
                exists = full in actor_names
                resolved.add(full)
                runtime.append(dict(actor=actor['id'], module=module, line=call['line'],
                                    call=call['call'], literal=name or '', resolved=full or '',
                                    expression=call['expression'],
                                    verdict='dynamic argument' if name is None else 'bank candidate' if exists else 'unresolved candidate',
                                    applicability='module literal; family branch requires review'))
        for clip in actor['clips'] or [dict(name='', fps=0, wrapMode=-1, loopStart=0, frames=[])]:
            original = clip['name']
            aliases = sorted(a for a, target in actor['aliases'].items() if target == original)
            names = [original, *aliases]
            names = [name_prefix + n for n in names]
            exported = [n for n in names if n in bank]
            original_name = name_prefix+original
            refs = f.get('referenced_clips', [])
            row = dict(actor=actor['id'], original_clip=original, aliases=';'.join(aliases),
                       source_files=json.dumps(actor.get('source_files', [])),
                       exported='yes' if exported else 'no', bank=str(path),
                       original_exported='yes' if original_name in bank else 'no',
                       original_psp_exported='yes' if psp_banks and all(original_name in b for _,b in psp_banks) else 'no',
                       source_fps=clip['fps'], source_wrap=clip['wrapMode'],
                       source_loop_start=clip.get('loopStart', 0), source_frames=len(clip['frames']),
                       bank_names=';'.join(exported),
                       bank_clip_metadata=json.dumps({n: {k: bank[n][k] for k in ('fps', 'wrap', 'loopStart', 'count')} for n in exported}),
                       bank_metadata=';'.join(f'{n}:{compare(clip, bank[n])}' for n in exported) or 'missing',
                       event_metadata=compare_events(clip,bank.get(original_name)),
                       psp_event_metadata=';'.join(compare_events(clip,b.get(original_name)) for _,b in psp_banks) or 'missing',
                       psp_banks=len(psp_banks),
                       psp_clip_metadata=json.dumps({str(p): {n: {k: b[n][k] for k in ('fps', 'wrap', 'loopStart', 'count')}
                                                              for n in exported if n in b} for p,b in psp_banks}),
                       psp_metadata=';'.join(f'{p}:{n}:{compare(clip, b.get(n))}'
                                             for p,b in psp_banks for n in exported) or 'missing',
                       wrap_4_5='yes' if clip['wrapMode'] in (4,5) else 'no',
                       trigger_frames=sum(bool(frame.get('triggerEvent')) for frame in clip['frames']),
                       trigger_events=json.dumps([dict(frame=i, **{k:v for k,v in frame.items() if k.startswith('event')})
                                                  for i,frame in enumerate(clip['frames']) if frame.get('triggerEvent')]),
                       source_reference='yes' if original in refs else 'data not exported',
                       runtime_played='literal candidate' if any(n in resolved for n in names) else 'no direct literal; dynamic calls need review',
                       renamed_aliases=';'.join(a for a in aliases if a != original),
                       substitution_candidates=';'.join(a for a in aliases if a != original and a in {c['name'] for c in actor['clips']}
                                                        and name_prefix+a in bank
                                                        and (original_name not in bank or bank[name_prefix+a]['frames']==bank[original_name]['frames'])),
                       substituted_aliases=';'.join(a for a in aliases if a != original and a in refs and a in {c['name'] for c in actor['clips']}
                                                     and name_prefix+a in bank
                                                     and (original_name not in bank or bank[name_prefix+a]['frames']==bank[original_name]['frames'])),
                       facing_source=f.get('verdict', 'no source data'),
                       source_right_sign=f.get('right_sign'), instance_scale=f.get('instance_scale'),
                       runtime_right_sign='not verified',
                       facing_evidence=';'.join(e['file'] + ':' + e['pointer'] for e in f.get('evidence', [])),
                       source_data='ok' if original else 'missing clip library')
            rows.append(row)
    for name, records in [('audit.csv', rows), ('runtime-calls.csv', runtime)]:
        with (output/name).open('w', newline='', encoding='utf-8') as stream:
            if records:
                writer = csv.DictWriter(stream, fieldnames=list(records[0]))
                writer.writeheader(); writer.writerows(records)
    summary = dict(actors=len(actors), rows=len(rows), source_clips=sum(bool(r['original_clip']) for r in rows),
                   not_exported=sum(r['exported']=='no' for r in rows),
                   trigger_frames=sum(r['trigger_frames'] for r in rows),
                   original_not_exported=sum(r['original_exported']=='no' for r in rows),
                   original_psp_not_exported=sum(r['original_psp_exported']=='no' for r in rows),
                   event_differences=sum(r['event_metadata']!='equal' for r in rows),
                   psp_event_differences=sum(any(v!='equal' for v in r['psp_event_metadata'].split(';')) for r in rows),
                   wrap_4_5=sum(r['wrap_4_5']=='yes' for r in rows),
                   bank_metadata_differences=sum(':different' in r['bank_metadata'] for r in rows),
                   psp_metadata_differences=sum(':different' in r['psp_metadata'] for r in rows),
                   rows_without_psp=sum(r['psp_banks']==0 for r in rows),
                   substituted_rows=sum(bool(r['substituted_aliases']) for r in rows),
                   substitution_candidate_rows=sum(bool(r['substitution_candidates']) for r in rows),
                   missing_facing=sum(facing.get(a['id'],{}).get('verdict')!='ok' for a in actors),
                   missing_libraries=[a['id'] for a in actors if not a['clips']],
                   missing_modules=sorted({m for a in actors for m in a.get('modules', [a['module']]) if not (ROOT/m).exists()}),
                   runtime_calls=len(runtime), unresolved_candidates=sum(r['verdict']=='unresolved candidate' for r in runtime),
                   dynamic_candidates=sum(r['verdict']=='dynamic argument' for r in runtime),
                   unresolved_by_actor=dict(Counter(r['actor'] for r in runtime if r['verdict']=='unresolved candidate')),
                   dynamic_by_actor=dict(Counter(r['actor'] for r in runtime if r['verdict']=='dynamic argument')))
    (output/'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=source_root())
    parser.add_argument('--psp-root', type=Path, default=ROOT/'_private/converted-psp')
    parser.add_argument('--facing', type=Path, default=ROOT/'_private/converted-psp/actor-facing/config.json')
    parser.add_argument('--library-root', type=Path, help='Read regenerated host banks from this private export')
    parser.add_argument('--output', type=Path, default=ROOT/'build/animation-audit')
    args = parser.parse_args()
    actors = inventory(args.source_root, args.facing.parent)
    if args.library_root:
        for actor in actors:
            filename=Path(actor['bank']).name
            matches=list((args.library_root/'host'/actor['family']).rglob(filename))
            if len(matches)!=1: raise ValueError(f'{filename}: expected exactly one complete host bank')
            actor['bank']=str(matches[0])
    if not actors:
        raise ValueError('No source route inventory; refusing an empty success')
    facing = read_json(args.facing)['actors'] if args.facing.exists() else {}
    summary = audit(actors, facing, args.psp_root, args.output)
    print(json.dumps(summary, separators=(',', ':')))


if __name__ == '__main__':
    main()
