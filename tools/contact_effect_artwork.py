"""Offline source adapter for Tutorial_01 WaterDrip and DustyPlatform artwork."""
import hashlib
import json
from pathlib import Path
import struct

from PIL import Image
from convert_hero import decode_sprite
from convert_room_visuals import project

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '_private/research/platform-effects-source'
WATER_AUDIO = (183, 136, 109, 175)
DUST_AUDIO = (170, 164, 100)
AUDIO_FIRST = 44  # EnemyClip::WaterDrip1; asserted by the runtime and clip-bank test.


class ContactSource:
    def __init__(self, source=SOURCE):
        self.source = source
        self.manifest = json.loads((source/'manifest.json').read_text())
        if (self.manifest['source_build_id'], self.manifest['scene']) != (22529139, 'Tutorial_01'):
            raise ValueError('Unexpected contact effects source')
        self.hashes = {}
        for record in self.manifest['asset_exports']:
            path=source/record['file']
            if hashlib.sha256(path.read_bytes()).hexdigest()!=record['sha256']:
                raise ValueError('Changed contact asset source: '+str(path))
        for owner in self.manifest['owners'].values():
            for record in owner['files']:
                path = source/record['snapshot']
                if hashlib.sha256(path.read_bytes()).hexdigest() != record['sha256']:
                    raise ValueError('Changed contact scene source: '+str(path))

    def path(self, key):
        return self.source/'assets'/key.replace('.assets-', '-')

    def has(self, key):
        return self.path(key).exists()

    def read(self, key):
        path = self.path(key)
        self.hashes[str(path.relative_to(self.source))] = hashlib.sha256(path.read_bytes()).hexdigest()
        return json.loads(path.read_text())

    def texture(self, key):
        stem = key.removesuffix('.json').replace('.assets-', '-')
        candidates = [self.source/'media/asset-ripper'/(stem+'.png'), self.source/'assets'/(stem+'.png')]
        path = next((p for p in candidates if p.exists()), candidates[0])
        self.hashes[str(path.relative_to(self.source))] = hashlib.sha256(path.read_bytes()).hexdigest()
        return Image.open(path).convert('RGBA')

    def components(self, owner):
        data = self.read(owner)
        result = {}
        prefix = owner.rsplit('-', 1)[0]
        for c in data['m_Components']:
            key = prefix+'-'+str(c['m_Component']['m_PathID'])+'.json'
            d = self.read(key)
            if 'm_LocalPosition' in d: kind = 'Transform'
            elif 'm_InitialModule' in d: kind = 'ParticleSystem'
            elif 'm_RenderMode' in d: kind = 'ParticleSystemRenderer'
            else: continue
            result.setdefault(kind, []).append(key)
        return result

    def scene(self, owner, kind):
        row = next(r for r in self.manifest['owners']['level6:'+str(owner)]['files'] if r['type']==kind)
        return json.loads((self.source/row['snapshot']).read_text())


def compile_bank(contact, image, emitter, read, chain, root_transform):
    """Append to the existing atlas/pool; return exact scene and clip records."""
    ids = [emitter('resources.assets-5214.json', contact=True),
           emitter('resources.assets-8116.json', contact=True),
           emitter('resources.assets-5215.json', contact=True)]
    library = contact.read('sharedassets6.assets-1095.json')['m_Structure']['clips']
    collection = contact.read('sharedassets6.assets-1111.json')['m_Structure']
    clips = []
    for name in ('Idle','Drip','Fall','Impact'):
        clip = next(c for c in library if c['name']==name)
        frames = []
        for frame in clip['frames']:
            sprite = collection['spriteDefinitions'][frame['spriteId']]
            bitmap, bounds = decode_sprite(contact.texture('sharedassets6.assets-55.json'), sprite)
            frames.append(image(('water-drip',frame['spriteId']),bitmap,bounds))
        clips.append(dict(name=name,fps=clip['fps'],sprites=frames))
    drips = []
    for row in sorted(contact.manifest['water_drips'], key=lambda r:r['go']):
        owner = row['go']; transforms = chain(root_transform(f'level6-{owner}.json'))
        origin = project((0,0,0),transforms)
        x, y = project((1,0,0),transforms), project((0,1,0),transforms)
        collider = contact.scene(owner,'BoxCollider2D'); size, offset = collider['m_Size'], collider['m_Offset']
        component = contact.scene(owner,'WaterDrip')['m_Structure']
        renderer = contact.scene(owner,'MeshRenderer')
        values = [*origin,x[0]-origin[0],y[0]-origin[0],x[1]-origin[1],y[1]-origin[1],
                  offset['m_X']-size['m_X']/2,offset['m_Y']-size['m_Y']/2,
                  offset['m_X']+size['m_X']/2,offset['m_Y']+size['m_Y']/2,
                  component['idleTimeMin'],component['idleTimeMax'],component['fallVelocity'],
                  component['impactTranslation'],renderer['m_SortingLayer'],renderer['m_SortingOrder']]
        drips.append(dict(owner=owner,values=values))
    platforms = []
    for row in sorted(contact.manifest['dusty_platforms'], key=lambda r:r['go']):
        owner = row['go']; component = contact.scene(owner,'DustyPlatform')['m_Structure']
        collider = row['colliders'][0]
        origin = project((0,0,0),chain(root_transform(f'level6-{owner}.json')))
        offset = component['streamOffset']
        values = [*origin[:2],*collider['size'],component['inset'],component['dustRateAreaFactor'],
                  component['dustRateConstant'],component['cooldownDuration'],component['rocksChance'],
                  offset['x'],offset['y'],offset['z'],-.1,component['dustIgnoredLayers']['m_Bits']]
        platforms.append(dict(owner=owner,collider=collider['path_id'],values=values))
    sounds = []
    for table_id, audio_ids, first in [(1061,WATER_AUDIO,AUDIO_FIRST),(1058,DUST_AUDIO,AUDIO_FIRST+4)]:
        table = contact.read(f'sharedassets6.assets-{table_id}.json')['m_Structure']
        entries = [(first+audio_ids.index(o['Clip']['m_PathID']),o['Weight']) for o in table['options']]
        source=(contact.scene(drips[0]['owner'],'AudioSource') if table_id==1061 else
                contact.read('resources.assets-13920.json'))
        keys=source['m_RolloffCustomCurve']['m_Curve']
        if source['m_RolloffMode']!=2 or len(keys)!=2:raise ValueError('Unsupported contact audio rolloff')
        spatial=[source['m_MaxDistance'],keys[0]['m_Time'],keys[0]['m_Value'],keys[0]['m_OutSlope'],
                 keys[1]['m_Time'],keys[1]['m_Value'],keys[1]['m_InSlope'],
                 source['m_PanLevelCustomCurve']['m_Curve'][0]['m_Value']]
        sounds.append(dict(pitch=[table['pitchMin'],table['pitchMax']],entries=entries,spatial=spatial))
    if len(drips)!=9 or len(platforms)!=18:raise ValueError('Incomplete contact effect scene')
    binary = bytearray(struct.pack('<4s3I',b'CFX1',*ids))
    for d in drips: binary.extend(struct.pack('<I17f',d['owner'],*d['values']))
    for p in platforms: binary.extend(struct.pack('<2I14f',p['owner'],p['collider'],*p['values']))
    for c in clips:
        binary.extend(struct.pack('<fI',c['fps'],len(c['sprites'])))
        binary.extend(struct.pack('<'+'I'*len(c['sprites']),*c['sprites']))
    for s in sounds:
        binary.extend(struct.pack('<2fI',*s['pitch'],len(s['entries'])))
        for entry in s['entries']: binary.extend(struct.pack('<If',*entry))
        binary.extend(struct.pack('<8f',*s['spatial']))
    return dict(emitters=ids,drips=drips,platforms=platforms,clips=clips,sounds=sounds,
                source_sha256=contact.hashes,binary=binary,
                approximations=['Source box volumes are projected into XY; native 3D motion is omitted.',
                                'Stream texture-sheet playback and eight curve samples approximate native particles.',
                                'Source particles without collision may pass through terrain; the port adds no invented bounce.'])
