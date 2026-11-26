"""Export shared owners into one lossless HPT2/PCL1 page namespace.

Tables retain their sprite IDs and opaque tails. The output manifest maps each
owner to a table beside the common pages; runtime staging must use this mapping
instead of copying the pages once per owner. This is a separate candidate only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import repack_pages as pack


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def route_profile(trace, manifest, layout, owners):
    """Bind the pool to the same sources and observed visits as the route plan."""
    import route_mixed_page_report as route
    trace, manifest, layout = Path(trace), Path(manifest), Path(layout)
    reference = json.loads(layout.read_text())
    if reference['manifest_sha256'] != sha(manifest) or any(
            sha(trace/name) != digest for name, digest in reference['trace_sha256'].items()):
        raise ValueError("Route layout source SHA256 mismatch")
    tables, aliases, rooms, assets = route.load_banks(trace, manifest)
    rows, _, frames, _ = route.usage(trace, tables, aliases, rooms, assets)
    if len(owners) < 2 or len(set(owners)) != len(owners):
        raise ValueError("Select at least two distinct shared owners")
    references = {b['bank']: b for b in reference['banks']}
    banks, lookup, old_pages = [], {}, {}
    offset = 0
    used = set().union(*frames)
    for name in owners:
        if name not in tables or tables[name]['scene'] is not None:
            raise ValueError("Pool owner must be a known shared bank")
        bank, ref = tables[name], references[name]
        if bank['source_version'] != 1:
            raise ValueError("Shared pool requires v1 tables")
        groups = ['used' if (name, i) in used else f'unused:{name}'
                  for i in range(len(bank['sprites']))]
        if [g == 'used' for g in ref['profile']['groups']] != [g == 'used' for g in groups]:
            raise ValueError("Route used-sprite profile mismatch")
        banks.append(dict(name=name, table=str(bank['table']), groups=groups,
                          table_sha256=ref['profile']['table_sha256'],
                          page_sha256=ref['profile']['page_sha256']))
        for i, sprite in enumerate(ref['sprite_records']):
            lookup[name, i] = offset+i
            old_pages[name, i] = (name, sprite[0])
        offset += len(bank['sprites'])
    footprints, measurements = [], []
    for room in reference['rooms']:
        scene = room['scene']
        ids = {item for row, frame in zip(rows, frames) if row['scene'] == scene
               for item in frame if item[0] in owners}
        footprints.append(sorted(lookup[item] for item in ids))
        measurements.append(dict(scene=scene, frames=room['frames'],
            before=room['after'], replaced_pages=len({old_pages[item] for item in ids})))
    return dict(banks=banks, footprints=footprints, rooms=measurements,
                margin_pixels=reference['margin_pixels'], layout_sha256=sha(layout),
                trace_sha256=reference['trace_sha256'], manifest_sha256=sha(manifest))


def layout_profile(profile, groups):
    """Bind a measured pool to source bytes independently of checkout paths."""
    return dict(algorithm='shared-measured-clut8-v1', groups=groups,
                banks=[{k: entry[k] for k in ('name', 'table_sha256', 'page_sha256', 'groups')}
                       for entry in profile['banks']], footprints=profile.get('footprints'))


def make_plan(profile):
    """Validate every source hash before a multi-owner relocation."""
    Image = pack.room.import_pil()
    pages, sprites, groups, sources = [], [], [], []
    names = set()
    if len(profile['banks']) < 2:
        raise ValueError("Select at least two distinct shared owners")
    for entry in profile['banks']:
        name, table = entry['name'], Path(entry['table'])
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("Duplicate/invalid shared owner")
        names.add(name)
        raw = table.read_bytes()
        if sha(table) != entry['table_sha256']:
            raise ValueError(f"Shared table SHA256 mismatch: {name}. If the owner kept its sprites and "
                             "groups, rebase the recipe: python -I tools/psp/rebase_shared_layout.py --write")
        count, records, end = pack.read_bank(raw)
        if raw[:4] != b'PSA1':
            raise ValueError("Shared pool requires animation owners")
        if [sha(table.parent/f'page-{i}.hpt') for i in range(count)] != entry['page_sha256']:
            raise ValueError("Shared page SHA256 mismatch")
        source_groups = entry['groups']
        if len(source_groups) != len(records) or any(
                g not in ('used', f'unused:{name}') for g in source_groups):
            raise ValueError("Shared used/unused groups must cover every sprite")
        start = len(sprites)
        sources.append(dict(name=name, table=table, raw=raw, end=end,
                            start=start, count=len(records), before_pages=count))
        sprites.extend((s[0]+len(pages), *s[1:]) for s in records)
        groups.extend(source_groups)
        pages.extend(pack.read_pages(Image, table.parent, count))
    if 'layout' in profile:
        measured = layout_profile(profile, groups)
        measured['layout'] = profile['layout']
        plan = pack.restore_mixed_layout(pages, sprites, measured)
    else:
        plan = pack.pack_mixed(pages, sprites, groups, profile.get('footprints'))
    if len(plan['indices']) > 128 or not 1 <= len(plan['palettes']) <= 128:
        raise ValueError("Shared bank exceeds runtime page/CLUT limits")
    return plan, pages, sprites, groups, sources


def export(profile, output):
    """Atomically write and re-read the common pages and every owner table."""
    output = Path(output)
    if output.exists():
        raise ValueError("Use a new directory for the shared candidate")
    plan, pages, sprites, groups, sources = make_plan(profile)
    width, height = plan['size']
    if width != pack.room.PAGE or height != pack.room.PAGE:
        raise ValueError("Serialized shared pages require the runtime 512px view")
    report = dict(format='HPT2/PCL1/SCL1', owners=len(sources), sprites=len(sprites),
        before_pages=sum(s['before_pages'] for s in sources),
        after_pages=len(plan['indices']), used_pages=plan['groups'].count('used'),
        unused_pages=len(plan['indices'])-plan['groups'].count('used'),
        palettes=len(plan['palettes']), algorithm=plan['algorithm'],
        profile=profile, banks=[], rooms=[], page_groups=plan['groups'],
        index_sha256=[hashlib.sha256(p).hexdigest() for p in plan['indices']],
        clut_sha256=[hashlib.sha256(p).hexdigest() for p in plan['palettes']],
        limitations=['Observed route visits only; no full-route or combat acceptance',
                     'Candidate tables share one page namespace; shipping staging pending',
                     'No runtime heap, hardware cost or route raster parity acceptance'])
    for footprint, measurement in zip(profile.get('footprints', []), profile.get('rooms', [])):
        shared_pages = len({plan['sprites'][i][0] for i in footprint})
        before = measurement['before']
        pinned = before['observed_pinned_pages']-measurement['replaced_pages']+shared_pages
        window = before['max_static_grown_window_pages']
        total = pinned+window
        report['rooms'].append(dict(scene=measurement['scene'], frames=measurement['frames'],
            before=before, replaced_pages=measurement['replaced_pages'],
            pooled_pages=shared_pages, after=dict(observed_pinned_pages=pinned,
                max_static_grown_window_pages=window, pinned_plus_static_window=total,
                fits_21_slots=total <= 21, pool_bytes=total*265216)))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='shared-repack-', dir=output.parent) as temporary:
        staging = Path(temporary)/'bank'
        staging.mkdir()
        for i, indices in enumerate(plan['indices']):
            header = struct.pack('<4sHH4BI', b'HPT2', width, height, 5, 3, 1, 0, len(plan['palettes']))
            (staging/f'page-{i}.hpt').write_bytes(header+pack.room.swizzle(indices, width, height))
        pcl = struct.pack('<4sII', b'PCL1', 1, len(plan['palettes']))+b''.join(plan['palettes'])
        (staging/'palettes.pcl').write_bytes(pcl)
        actual_records, actual_cluts = [], []
        for number, source in enumerate(sources):
            start, count = source['start'], source['count']
            records = plan['sprites'][start:start+count]
            cluts = plan['cluts'][start:start+count]
            data = bytearray(source['raw'][:28])
            struct.pack_into('<I', data, 12, len(plan['indices']))
            data += b''.join(struct.pack('<5I4f', *s) for s in records)
            data += source['raw'][source['end']:]
            data += struct.pack('<4sI', b'SCL1', count)+struct.pack(f'<{count}I', *cluts)
            filename = f'bank-{number}.bin'
            (staging/filename).write_bytes(data)
            serialized = (staging/filename).read_bytes()
            _, records, end = pack.read_bank(serialized, with_cluts=True)
            if serialized[end:-8-4*count] != source['raw'][source['end']:]:
                raise ValueError("Shared animation tail changed")
            actual_records.extend(records)
            actual_cluts.extend(struct.unpack(f'<{count}I', serialized[-4*count:]))
            sidecars = {}
            for sidecar in sorted(source['table'].parent.iterdir()):
                if (not sidecar.is_file() or sidecar == source['table'] or
                        sidecar.name in ('manifest.json', 'repack.json', 'palettes.pcl') or
                        re.fullmatch(r'page-\d+\.(hpt|png)', sidecar.name)):
                    continue
                destination = staging/'sidecars'/f'bank-{number}'/sidecar.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(sidecar, destination)
                if sha(sidecar) != sha(destination):
                    raise ValueError("Shared sidecar changed")
                sidecars[destination.relative_to(staging).as_posix()] = sha(destination)
            report['banks'].append(dict(bank=source['name'], table=filename, sprites=count,
                sprite_records=records, sprite_cluts=cluts, sidecars_sha256=sidecars))
        serialized_palettes = (staging/'palettes.pcl').read_bytes()[12:]
        actual_indices = []
        for i in range(len(plan['indices'])):
            data = (staging/f'page-{i}.hpt').read_bytes()
            if data[:16] != struct.pack('<4sHH4BI', b'HPT2', width, height, 5, 3, 1, 0, len(plan['palettes'])):
                raise ValueError("Shared page header changed")
            actual_indices.append(pack.room.unswizzle(data[16:], width, height))
        actual = dict(plan, sprites=actual_records, cluts=actual_cluts, indices=actual_indices,
            palettes=[serialized_palettes[i:i+1024] for i in range(0, len(serialized_palettes), 1024)])
        pack.verify_mixed(pages, sprites, groups, actual)
        report['package_bytes'] = sum(p.stat().st_size for p in staging.rglob('*') if p.is_file())
        report['verification'] = 'Every serialized sprite: exact padded indices, CLUT, geometry and tail'
        (staging/'repack.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        staging.rename(output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--layout', type=Path, required=True)
    parser.add_argument('--bank', action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    profile = route_profile(args.trace, args.manifest, args.layout, args.bank)
    result = export(profile, args.output)
    print(f"{result['owners']} owners / {result['sprites']} sprites verified; "
          f"{result['used_pages']} shared used pages + {result['unused_pages']} unused pages; "
          f"{result['package_bytes']} package bytes excluding audit JSON")


if __name__ == '__main__':
    main()
