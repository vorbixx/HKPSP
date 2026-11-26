"""Export room BG event receivers and their persistent battle triggers."""
import math
import struct
import warnings
from battle_waves import export as export_waves
from scene_activation import fnv1a32, owner_id
from convert_terrain import paths, transform_point


def export(mapping, read, visuals, visual_read, spawns, target):
    gates=[]; encounters=[]; seen=set()
    for row in mapping['inventory']:
        go=read(row['go'])
        if go.get('m_TagString')=='Battle Gate':
            owner=owner_id(row['go'])
            owners={owner}
            owners.update(owner_id(r['go']) for r in visuals['inventory'] if row['go'] in r['owners'])
            gates.append(dict(collider=owner_id(row['collider']),owners=sorted(owners)))
        for key in row['components'][0].get('PlayMakerFSM',[]):
            if key in seen:continue
            seen.add(key)
            # Saved components without a graph cannot receive battle events.
            fsm=read(key).get('m_Structure',{}).get('fsm')
            if not fsm:
                warnings.warn(f"{mapping.get('scene', 'room')}: no serialized battle FSM in {key} ({go.get('m_Name', row['go'])})")
                continue
            strings={p.get('value') for s in fsm.get('states',[]) for p in s['actionData'].get('fsmStringParams',[])}
            if not {'BG CLOSE','BG OPEN'}<=strings:continue
            if not read(row['collider'])['m_IsTrigger']:continue
            points=[transform_point(p,[read(t) for t in row['chain']])
                    for path in paths(read(row['collider']),row['type']) for p in path]
            rect=[min(p[0] for p in points),min(p[1] for p in points),max(p[0] for p in points),max(p[1] for p in points)]
            persistent=row['components'][0].get('PersistentBoolItem',[])
            save=[0,0]
            if persistent:
                item=read(persistent[0])['m_Structure']; data=item['persistentBoolData']
                if not item['dontSave'] and not item['semiPersistent']:
                    save=[fnv1a32(data['sceneName'] or mapping['scene']),fnv1a32(data['id'] or go['m_Name'])]
            refs={v['value']['m_PathID'] for v in fsm['variables']['gameObjectVariables']}
            enemies=[s['owner'] for s in spawns if s['owner'] in refs or row['go'] in s.get('owners',[])]
            if not enemies:
                # Older Battle Scene controllers count source-tagged enemies.
                enemies=[s['owner'] for s in spawns if any(read(o).get('m_TagString')=='Battle Enemy' for o in s.get('owners',[]))]
            delay=0
            for state in fsm['states']:
                if state['name'] not in ('End Wait','Drop Item'):continue
                ad=state['actionData']
                for i,name in enumerate(ad['paramName']):
                    if name=='time' and ad['paramDataType'][i]==15:
                        pos=ad['paramDataPos'][i];raw=bytes(ad['byteData'][pos:pos+ad['paramByteDataSize'][i]])
                        if len(raw)==5 and not raw[4]:delay+=struct.unpack('<f',raw[:4])[0]
            if not math.isfinite(delay) or not 0<=delay<=60:raise ValueError('Invalid battle ending delay')
            waves=export_waves(fsm,spawns,read)
            if waves:
                enemies=list(dict.fromkeys(s['owner'] for w in waves for s in w['spawns']))
            encounters.append(dict(key=save,trigger=rect,enemies=enemies,end_delay=delay,source=key,waves=waves))
    if len(gates)>32 or len(encounters)>16:raise ValueError('Room battle budget exceeded')
    data=bytearray(struct.pack('<4sII',b'HKG2',len(gates),len(encounters)))
    for gate in gates:
        data+=struct.pack('<II',gate['collider'],len(gate['owners']))
        data+=struct.pack('<'+'I'*len(gate['owners']),*gate['owners'])
    for encounter in encounters:
        enemies=encounter['enemies']
        data+=struct.pack('<II5fI',*encounter['key'],*encounter['trigger'],encounter['end_delay'],len(enemies))
        data+=struct.pack('<'+'I'*len(enemies),*enemies)
        waves=encounter['waves']
        if len(waves)>16:raise ValueError('Battle wave budget exceeded')
        data+=struct.pack('<I',len(waves))
        for wave in waves:
            data+=struct.pack('<2fIiI',wave['delay'],wave['timeout'],wave['health_owner'],wave['health_below'],len(wave['spawns']))
            for spawn in wave['spawns']:
                data+=struct.pack('<I2fI',spawn['owner'],*spawn['position'],int(spawn['reposition']))
    (target/'battle-gates.bin').write_bytes(data)
    return dict(gates=gates,encounters=encounters)
