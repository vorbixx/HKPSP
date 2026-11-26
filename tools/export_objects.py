"""Coverage and lifecycle checks shared by room exporters; no source assets here."""


def require_coverage(expected, actual, label):
    """Compare identities, not counts: filtering inactive objects must fail loudly."""
    expected, actual = list(expected), list(actual)
    for name, values in (('source', expected), ('output', actual)):
        if any(not isinstance(value, str) or not value for value in values):
            raise ValueError(f'{label}: missing {name} object ID')
        if len(set(values)) != len(values):
            raise ValueError(f'{label}: duplicate {name} object ID')
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    if missing or extra:
        raise ValueError(f'{label}: missing objects {missing}; unexpected objects {extra}')


def tutorial_groups(rows):
    """Discover gameplay candidates by components, before activity/name filtering.

    The numerical converters still validate the researched component behaviour.
    A new component layout cannot silently bypass the inventory by changing a name.
    Buzzer/stalactite records belong to the separate room-entities exporter.
    """
    groups = {k: [] for k in ('crawlers', 'doors', 'gates', 'hazards',
                             'camera_locks', 'hazard_respawns')}
    for row in rows:
        parts = row['parts']
        if 'HealthManager' in parts and 'LineOfSightDetector' not in parts:
            groups['crawlers'].append(row)
        for component, group in (('Breakable', 'doors'), ('TransitionPoint', 'gates'),
                                 ('CameraLockArea', 'camera_locks'),
                                 ('HazardRespawnTrigger', 'hazard_respawns')):
            if component in parts:
                groups[group].append(row)
        if ('DamageHero' in parts and 'HealthManager' not in parts
                and 'StalactiteControl' not in parts):
            groups['hazards'].append(row)
    return groups


def source_lifecycle(row, geometry, rows, read):
    """Require complete collider ancestry and retain inactive ancestors explicitly."""
    go = row['parts']['GameObject'][0]
    shapes = [g for g in geometry if g['go'] == go]
    if not shapes:
        raise ValueError(f'{go}: gameplay object has no exported collider ancestry')
    owners = shapes[0]['owners']
    if not owners or owners[0] != go or len(set(owners)) != len(owners):
        raise ValueError(f'{go}: invalid object ancestry')
    if any(g['owners'] != owners for g in shapes):
        raise ValueError(f'{go}: inconsistent collider ancestry')
    # Reconstruct from Transform references as well: a truncated owners list must
    # not hide an inactive parent. File IDs here refer to the same scene export.
    chain = shapes[0]['chain']
    if len(chain) != len(owners):
        raise ValueError(f'{go}: incomplete transform ancestry')
    for i, (owner, transform) in enumerate(zip(owners, chain)):
        prefix = owner.rsplit('-', 1)[0]
        t = read(transform)
        if t['m_GameObject']['m_PathID'] != int(owner.rsplit('-', 1)[1].split('.')[0]):
            raise ValueError(f'{go}: transform owner mismatch')
        father = t['m_Father']
        expected = chain[i + 1] if i + 1 < len(chain) else None
        actual = f"{prefix}-{father['m_PathID']}.json" if father['m_PathID'] else None
        if father['m_FileID'] or actual != expected:
            raise ValueError(f'{go}: incomplete or external parent hierarchy')
    inactive = [owner for owner in owners if not read(owner)['m_IsActive']]
    controls = []
    for parent in rows:
        if parent['parts']['GameObject'][0] not in owners:
            continue
        for kind in ('ActivateChildrenOnContact', 'DeactivateIfPlayerdataTrue'):
            for key in parent['parts'].get(kind, []):
                if read(key)['m_Enabled']:
                    controls.append(key)
    return dict(source_go=go, owners=owners, inactive_owners=inactive,
                initial_active=not inactive, controls=sorted(controls))


def require_lifecycle(state, record, allow_activation=False):
    go = state['source_go']
    if type(record.get('active')) is not bool or record['active'] != state['initial_active']:
        raise ValueError(f'{go}: missing or changed initial activity')
    activation = record.get('activation')
    handled = []
    if activation is not None:
        if not allow_activation or not isinstance(activation, dict):
            raise ValueError(f'{go}: unsupported activation metadata')
        handled = sorted([activation.get('source', ''),
                          activation.get('deactivation_source', '')])
        if (not activation.get('disable_if_visited_dirtmouth')
                or len(state['owners']) < 2
                or activation.get('parent') != state['owners'][1]):
            raise ValueError(f'{go}: unsupported activation parent/condition')
    if handled != state['controls']:
        raise ValueError(f'{go}: unhandled activation controls {state["controls"]}')
    if not state['initial_active']:
        # The supported rule enables direct children, not an arbitrary inactive
        # ancestor. Anything else requires research and a runtime implementation.
        if not activation or state['inactive_owners'] != [go]:
            raise ValueError(f'{go}: unresearched inactive object/ancestor')


def validate_tutorial_objects(manifest, rows, geometry, read, complete_rows=None):
    """Run at extraction AND packaging so edits/stale manifests cannot drop actors."""
    audit = {}
    groups = tutorial_groups(rows)
    if complete_rows is not None:
        # The curated rules input must not itself omit an enemy/hazard. Geometry
        # inventory has one row per shape, so deduplicate by GameObject first.
        unique = {r['go']: r for r in complete_rows}
        complete = tutorial_groups(unique.values())
        for group in groups:
            if group == 'doors':
                # Decorative Breakables have a separate artwork export. Only
                # tutorial doors are in the room-rules contract.
                complete[group] = [r for r in complete[group]
                                   if read(r['go'])['m_Name'].startswith('Tute Door')]
            require_coverage((r['parts']['GameObject'][0] for r in complete[group]),
                             (r['parts']['GameObject'][0] for r in groups[group]),
                             'source inventory '+group)
    for group, candidates in groups.items():
        records = manifest[group]
        require_coverage((r['parts']['GameObject'][0] for r in candidates),
                         (r.get('source_go') for r in records), group)
        by_id = {r['source_go']: r for r in records}
        audit[group] = []
        for row in candidates:
            state = source_lifecycle(row, geometry, rows, read)
            require_lifecycle(state, by_id[state['source_go']], group == 'crawlers')
            audit[group].append(state)
    return audit
