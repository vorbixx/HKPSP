"""Convert room-local breakables and the original Tutorial door fixture."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import hashlib
import json
import math
import shutil
import struct
from assetripper_client import ROOT
from convert_terrain import observe, require
from breakable_effect_assets import source_effect_costs


def hit_triangles(paths):
    """Split polygon outlines into convex pieces, retaining concavities/holes.

    Between consecutive vertex/intersection X coordinates, each filled even-odd
    interval is a trapezoid. No triangulation edge can bridge an empty interval.
    Separate collider components call this independently (their union is solid).
    """
    edges = []
    for path in paths:
        require(len(path) >= 3 and all(math.isfinite(v) and abs(v) <= 100000 for p in path for v in p),
                'Invalid breakable polygon')
        edges.extend((a, b) for a, b in zip(path, path[1:] + path[:1]) if a != b)
    require(3 <= len(edges) <= 256, 'Breakable polygon edge budget exceeded')
    cuts = {p[0] for edge in edges for p in edge}
    cross = lambda a, b: a[0] * b[1] - a[1] * b[0]
    for i, (a, b) in enumerate(edges):
        u = (b[0] - a[0], b[1] - a[1])
        for c, d in edges[i + 1:]:
            v = (d[0] - c[0], d[1] - c[1])
            determinant = cross(u, v)
            if determinant == 0:
                continue
            offset = (c[0] - a[0], c[1] - a[1])
            t, s = cross(offset, v) / determinant, cross(offset, u) / determinant
            if 0 < t < 1 and 0 < s < 1:
                cuts.add(a[0] + t * u[0])
                require(len(cuts) <= 512, 'Breakable polygon intersection budget exceeded')
    cuts = sorted(cuts)
    triangles = []
    for left, right in zip(cuts, cuts[1:]):
        middle = (left + right) * .5
        # PolygonCollider2D closes each path implicitly. A slab can be only
        # one double ULP wide after projection/intersection, so its midpoint
        # may round onto a vertex. Select edges spanning the whole slab;
        # midpoint comparisons can drop just one edge and invent an open path.
        active = [(a, b) for a, b in edges
                  if min(a[0], b[0]) <= left and right <= max(a[0], b[0])]
        def y(edge, x):
            a, b = edge
            return a[1] + (x - a[0]) * (b[1] - a[1]) / (b[0] - a[0])
        active.sort(key=lambda edge: y(edge, middle))
        require(len(active) % 2 == 0, 'Unclosed breakable polygon')
        for low, high in zip(active[::2], active[1::2]):
            quad = [(left, y(low, left)), (right, y(low, right)),
                    (right, y(high, right)), (left, y(high, left))]
            for triangle in (quad[:3], [quad[0], quad[2], quad[3]]):
                triangle = [struct.unpack('<2f', struct.pack('<2f', *p)) for p in triangle]
                a, b, c = triangle
                if abs(cross((b[0] - a[0], b[1] - a[1]), (c[0] - a[0], c[1] - a[1]))) > 1e-9:
                    triangles.append(triangle)
                    require(len(triangles) <= 256, 'Breakable triangle budget exceeded')
    require(0 < len(triangles) <= 256, 'Breakable triangle budget exceeded or empty polygon')
    return triangles


def room_breakables(source, output, target):
    """Compile room-local Breakable/BreakableObject activation and hit geometry.

    Persistent secret walls remain owned by the secret-wall converter. References
    are scoped to this package, so equal Unity path IDs in two rooms are safe.
    """
    from convert_room_visuals import project
    from breakable_debris import compile_debris, launch
    from breakable_hierarchy import scene_objects
    from room_breakable_audio import compile_audio, write_audio
    maps, paths = [], {}
    for folder, name in (('terrain', 'terrain-map.json'), ('visuals', 'room-visuals-map.json')):
        directory = source / folder
        mapping = json.loads((directory / name).read_text())
        maps.append(mapping)
        for key, digest in mapping.get('source_sha256', {}).items():
            paths.setdefault(key, (directory / key, digest))
    cache = {}

    def read(key):
        if key not in cache:
            path, expected = paths[key]
            raw = path.read_bytes()
            require(hashlib.sha256(raw).hexdigest() == expected, 'Changed breakable source: ' + key)
            cache[key] = json.loads(raw)
        return cache[key]

    inventory = {}
    for mapping in maps:
        for row in [*mapping['inventory'], *scene_objects(mapping)]:
            inventory.setdefault(row['go'], []).append(row)
    records, missing, excluded = [], [], []
    for key, rows in inventory.items():
        parts = {}
        for row in rows:
            components = row.get('components', {})
            if isinstance(components, list):
                # Terrain inventories also carry every ancestor's components.
                # A debris child must never become a second copy of its owner.
                components = {kind: refs for group in components if row['go'] in group.get('GameObject', [])
                              for kind, refs in group.items()}
            parts.update(components)
        kind = next((k for k in ('Breakable', 'BreakableObject') if k in parts), None)
        if not kind:
            continue
        go = read(key)
        data = read(parts[kind][0])
        if not data.get('m_Enabled', True) or not all(read(k)['m_IsActive'] for k in rows[0]['owners']):
            continue
        # Secret walls are save-driven, multi-hit room access, owned separately.
        if any(k in parts for k in ('BreakableWall', 'BreakableWallGeo')):
            excluded.append(dict(owner=key, reason='persistent secret wall owner'))
            continue
        chain = [read(k) for k in rows[0]['chain']]
        origin = project((0, 0, 0), chain)
        data = data['m_Structure']
        if kind == 'Breakable' and not (data['inertForegroundThreshold'] <= origin[2] <= data['inertBackgroundThreshold']):
            continue
        # BreakableObject.Start disables its receiver outside this authored plane.
        if kind == 'BreakableObject' and abs(origin[2] - .004) > 1:
            continue
        prefix = key.rsplit('-', 1)[0]

        def refkey(ref):
            return f"{prefix}-{ref['m_PathID']}.json" if ref.get('m_FileID', 0) == 0 and ref.get('m_PathID') else None

        def resolve(source_key, ref):
            if not ref.get('m_PathID'):
                return None
            collection = (source_key or key).rsplit('-', 1)[0]
            tables = maps[1].get('collection_dependencies', {})
            if ref.get('m_FileID', 0):
                collection = tables.get(collection, {}).get(str(ref['m_FileID']))
                if not collection:
                    return None
            return f"{collection}-{ref['m_PathID']}.json"

        def owners(refs):
            roots = {refkey(r) for r in refs} - {None}
            return sorted({int(k.rsplit('-', 1)[1][:-5]) for k, rr in inventory.items()
                           if k in roots or any(roots.intersection(r.get('owners', [])) for r in rr)})

        triangles, circles, segments, vertices = [], [], [], []
        for collider_kind in ('BoxCollider2D', 'PolygonCollider2D', 'CircleCollider2D', 'EdgeCollider2D'):
            for collider in parts.get(collider_kind, []):
                shape = read(collider)
                if not shape.get('m_Enabled', True):
                    continue
                offset = shape['m_Offset']
                if collider_kind == 'EdgeCollider2D':
                    points = [project((p['m_X'] + offset['m_X'], p['m_Y'] + offset['m_Y'], 0), chain)[:2]
                              for p in shape['m_Points']]
                    require(all(math.isfinite(v) and abs(v) <= 100000 for p in points for v in p),
                            'Invalid breakable edge')
                    segments.extend([a, b] for a, b in zip(points, points[1:]) if a != b)
                    continue
                if collider_kind == 'CircleCollider2D':
                    center = project((offset['m_X'], offset['m_Y'], 0), chain)[:2]
                    basis = [project(p, chain)[:2] for p in ((1, 0, 0), (0, 1, 0))]
                    radius = shape['m_Radius'] * max(math.hypot(p[0]-origin[0], p[1]-origin[1]) for p in basis)
                    require(all(math.isfinite(v) for v in (*center, radius)) and radius > 0 and
                            max(abs(v) for v in center) + radius <= 100000, 'Invalid breakable circle')
                    circles.append([*center, radius])
                    continue
                if collider_kind == 'BoxCollider2D':
                    size = shape['m_Size']
                    outlines = [[(x * size['m_X'], y * size['m_Y'])
                                 for x, y in ((-.5, -.5), (.5, -.5), (.5, .5), (-.5, .5))]]
                else:
                    outlines = [[(p['m_X'], p['m_Y']) for p in path] for path in shape['m_Points']['m_Paths']]
                outlines = [[project((x + offset['m_X'], y + offset['m_Y'], 0), chain)[:2]
                             for x, y in path] for path in outlines]
                if not vertices and collider_kind == 'BoxCollider2D':
                    vertices = outlines[0]
                triangles.extend(hit_triangles(outlines))
        if not triangles and not circles and not segments:
            missing.append(dict(owner=key, feature='no enabled box/polygon/circle/edge hit geometry'))
            continue
        require(len(triangles) + len(circles) + len(segments) <= 256, 'Breakable owner geometry budget exceeded')
        owner = int(key.rsplit('-', 1)[1][:-5])
        whole = sorted({owner, *owners(data.get('wholeParts', []))})
        renderer = refkey(data.get('wholeRenderer', {}))
        if renderer and renderer in paths:
            # A disabled renderer leaves its children active.
            whole = sorted({*whole, read(renderer)['m_GameObject']['m_PathID']})
        remnants = owners(data.get('remnantParts', []))
        whole_children = set(whole) - {owner}
        terrain_ids = {s['id'] for s in json.loads((output / 'terrain/manifest.json').read_text())['shapes']}
        colliders = sorted({int(r['collider'].rsplit('-', 1)[1][:-5]) for rr in inventory.values() for r in rr
                            if 'collider' in r and (r['go'] == key or
                                any(int(k.rsplit('-', 1)[1][:-5]) in whole_children for k in r.get('owners', [])))} & terrain_ids)
        for field in ('wholeParts', 'remnantParts', 'debrisParts', 'flingDebris'):
            for ref in data.get(field, []) or []:
                if not ref.get('m_PathID'):
                    continue
                dependency = resolve(key, ref)
                if not dependency or dependency not in paths:
                    missing.append(dict(owner=key, feature=field, dependency=dependency, reason='missing export'))
                elif dependency not in inventory:
                    missing.append(dict(owner=key, feature=field, dependency=dependency,
                                        reason='missing scene hierarchy or prefab instance'))
        effect = data.get('effectOffset', {})
        rigid_debris, unresolved_debris = compile_debris(data, kind, inventory, read, resolve, project)
        flash_fields = (('objectNailEffectPrefab', 4), ('midpointNailEffectPrefab', 8)) if kind == 'BreakableObject' else (
            ('strikeEffectPrefab', 4), ('nailHitEffectPrefab', 8))
        effects = sum(bit for field, bit in (*flash_fields, ('dustHitRegularPrefab', 1), ('dustHitDownPrefab', 2))
                      if data.get(field, {}).get('m_PathID'))
        angle = data.get('angleOffset', 0)
        if kind == 'Breakable':
            # Breakable.Start uses this object's local X scale, not the world determinant.
            angle *= -1 if read(rows[0]['chain'][0])['m_LocalScale']['m_X'] < 0 else 1
        effect_z = effect.get('z', 0)
        require(all(math.isfinite(v) and abs(v) <= 100000 for v in (angle, effect_z)),
                'Invalid breakable effect pose')
        # BreakableObject selects from an AudioClip array; Breakable uses an
        # AudioEvent or clip table. Empty/null arrays must remain silent.
        cut_sounds = data.get('cutSound') or []
        if isinstance(cut_sounds, dict):
            cut_sounds = [cut_sounds]
        if (data.get('breakAudioClipTable', {}).get('m_PathID') or
                data.get('breakAudioEvent', {}).get('Clip', {}).get('m_PathID') or
                any(clip.get('m_PathID') for clip in cut_sounds)):
            effects |= 16
        source_effects = maps[1].get('breakable_prefabs', {}).get('effects', {})
        audio_authored = 'audio_cues' in source_effects
        audio = (compile_audio(data, kind, parts[kind][0], read, resolve, source_effects, paths, target)
                 if audio_authored else [])
        records.append(dict(owner=owner, name=go['m_Name'], kind=kind, layer=go['m_Layer'],
                            audio_authored=audio_authored, audio=audio,
                            vertices=vertices, triangles=triangles, circles=circles, segments=segments, whole=whole, remnants=remnants, colliders=colliders,
                            origin=origin[:3], effect=[effect.get('x', 0), effect.get('y', 0)],
                            angle=angle, effect_z=effect_z, effects=effects,
                            debris=bool(unresolved_debris), rigid_debris=rigid_debris,
                            impulse=kind == 'BreakableObject', launch=launch(data, kind)))
    require(len(records) < 128, 'Room breakables exceed the combat admission budget')
    require(sum(len(c['choices']) for r in records for c in r['audio']) <= 512,
            'Room breakable audio metadata exceeds 512 choices')
    triangle_count = sum(len(row['triangles']) for row in records)
    circle_count = sum(len(row['circles']) for row in records)
    segment_count = sum(len(row['segments']) for row in records)
    require(triangle_count + circle_count + segment_count <= 2048, 'Room breakable geometry exceeds 48 KiB budget')
    require(sum(len(r['rigid_debris']) for r in records) <= 512, 'Room debris exceeds 512-piece budget')
    require(sum(len(p['owners']) for r in records for p in r['rigid_debris']) <= 2048, 'Room debris renderer budget')
    binary = bytearray(struct.pack('<4sI', b'HKB8', len(records)))
    for row in records:
        binary += struct.pack('<12I23f', row['owner'], row['layer'], int(row['debris']),
                              len(row['whole']), len(row['remnants']), len(row['colliders']), row['effects'],
                              len(row['triangles']), len(row['circles']), len(row['rigid_debris']), int(row['impulse']), len(row['segments']),
                              *row['origin'], *row['effect'], row['angle'], *row['launch'], row['effect_z'])
        for triangle in row['triangles']:
            binary += struct.pack('<6f', *(v for p in triangle for v in p))
        for circle in row['circles']:
            binary += struct.pack('<3f', *circle)
        for segment in row['segments']:
            binary += struct.pack('<4f', *(v for p in segment for v in p))
        for field in ('whole', 'remnants', 'colliders'):
            require(len(row[field]) <= 512, 'Breakable activation hierarchy exceeds budget')
            binary += struct.pack('<' + 'I' * len(row[field]), *row[field])
        for piece in row['rigid_debris']:
            values = [*piece['physics'], *piece['hull'], *piece['finish']]
            require(all(math.isfinite(v) and abs(v) <= 1e6 for v in values), 'Invalid debris physics')
            binary += struct.pack('<2I35f', piece['flags'], len(piece['owners']), *values)
            binary += struct.pack('<' + 'I' * len(piece['owners']), *piece['owners'])
        binary += struct.pack('<I', row['audio_authored']) + write_audio(row['audio'])
    (target / 'breakables.bin').write_bytes(binary)
    (target / 'room-breakable-audio.json').write_text(json.dumps(dict(scene=maps[1]['scene'],
        owners=[dict(owner=row['owner'], cues=row['audio']) for row in records if row['audio_authored']])))
    report = dict(scene=maps[1]['scene'], records=records, missing_dependencies=missing, excluded=excluded,
                  source_effects=source_effect_costs(maps[1].get('breakable_prefabs', {}).get('effects', {}), read),
                  audio_peak_pcm_bytes=max((p.stat().st_size-44 for p in target.glob('room-sfx-*.wav')),default=0),
                  approximations=['Exported owner audio uses native PCM and weighted choices/pitch/gain in the six shared transient voices; bursts beyond capacity steal the oldest tail. Legacy exports retain the Tutorial audio family. Other effect limitations are in the room effect manifest.'],
                  bytes=len(binary), geometry_bytes=triangle_count * 24 + circle_count * 12 + segment_count * 16, extra_texture_bytes=0, max_actors=127)
    (output / 'breakables.json').write_text(json.dumps(report, indent=2) + '\n')
    return report

def convert():
    source=ROOT/"_private/research/terrain"
    _,terrain=observe(source,[108,4,144,20])
    mapping=json.loads((source/"terrain-map.json").read_text())
    read=lambda name:json.loads((source/name).read_text())
    row=next(r for r in mapping["inventory"] if read(r["go"])["m_Name"]=="Tute Door 5" and r["type"]=="BoxCollider2D")
    go=read(row["go"]);require(go["m_Layer"]==8,"Selected door layer changed")
    require("NonBouncer" in row["components"][0],"Missing NonBouncer")
    non_bouncer=read(row["components"][0]["NonBouncer"][0])["m_Structure"]
    require(non_bouncer["active"] is True,"Door NonBouncer inactive")
    data=read(row["components"][0]["Breakable"][0])["m_Structure"]
    require(not data["forwardBreakEvent"] and data["hitEventReciever"]["m_PathID"]==0,"Door event dependency unsupported")
    require(not data["containingParticles"] and not data["remnantParts"],"Unsupported content/remnant branch")
    state=read(row["components"][0]["PersistentBoolItem"][0])["m_Structure"]
    require(not state["semiPersistent"] and not state["persistentBoolData"]["activated"],"Changed default save state")
    whole={int(p["m_PathID"]) for p in data["wholeParts"]}
    trigger=int(row["collider"].split("-")[-1].split(".")[0])
    selected={s["id"]:s for s in terrain["shapes"]}
    require(trigger in selected and selected[trigger]["flags"]&1,"Missing door trigger in selected terrain")
    ids=[trigger]
    for item in mapping["inventory"]:
        identity=int(item["collider"].split("-")[-1].split(".")[0])
        owner_ids={int(name.split("-")[-1].split(".")[0]) for name in item["owners"]}
        if identity in selected and owner_ids & whole:
            ids.append(identity)
    ids=list(dict.fromkeys(ids))
    require(len(ids)==2 and selected[ids[1]]["name"]=="Hero_Solid","Selected activation topology changed")
    binary=struct.pack("<4s4I",b"HKD1",trigger,len(ids),go["m_Layer"],0)+struct.pack("<"+"I"*len(ids),*ids)
    output=ROOT/"_private/converted/door";output.mkdir(parents=True,exist_ok=True)
    (output/"door.bin").write_bytes(binary)
    shutil.copyfile(output/"door.bin",ROOT/"_private/converted/hero/romfs/door.bin")
    report=dict(build=22529139,door_name=go["m_Name"],trigger=trigger,disabled_shapes=ids,
                source_sha256=mapping["source_sha256"],binary_sha256=hashlib.sha256(binary).hexdigest(),
                scope="Nail receiver and static collision activation only; debris/VFX/audio/save storage not exported")
    (output/"manifest.json").write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps(dict(trigger=trigger,disabled_shapes=ids,bytes=len(binary))))

if __name__=="__main__":convert()
