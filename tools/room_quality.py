"""Explicit, reversible Tutorial_01 artwork cuts for the 30 fps package."""
from PIL import Image

PROFILE = 'new3ds-30fps'
DROP_FOG = frozenset()
DROP_HAZE = frozenset()
# Tablet ColorFaders remain required by RoomAmbient::bind.
DROP_GLOW = frozenset()
RUNTIME_TABLET_LIGHTS = frozenset((906, 1581, 3542))
DROP_BACKGROUND = frozenset()
SOFT_SPRITES = frozenset(('resources.assets-2563.json',
                          'resources.assets-3023.json'))
FRINGE_ALPHA = 0  # Trimming soft edges produced hard fog/haze edges.
BACKGROUND_SPRITES = frozenset(('resources.assets-2171.json',
    'sharedassets6.assets-347.json', 'sharedassets6.assets-381.json',
    'sharedassets6.assets-259.json', 'sharedassets6.assets-348.json',
    'sharedassets6.assets-372.json', 'sharedassets6.assets-237.json'))
HALF_SIZE_SPRITES = frozenset()
PARTICLE_DENSITY = .5
AMBIENT_DENSITY = .5  # Half the source emission rate.


def cut_groups():
    return {'fog': DROP_FOG, 'haze': DROP_HAZE, 'glow': DROP_GLOW,
            'background': DROP_BACKGROUND}


def select_cuts(mapping, read):
    if mapping['build'] != 22529139 or mapping['scene'] != 'Tutorial_01':
        raise ValueError('30 fps artwork cuts require the reviewed Tutorial_01 build')
    if set().union(*cut_groups().values()) & RUNTIME_TABLET_LIGHTS:
        raise ValueError('30 fps cuts must retain runtime-bound tablet lights')
    selected = []
    for item in mapping['inventory']:
        owner = int(item['go'].rsplit('-', 1)[1].split('.')[0])
        group = next((key for key, ids in cut_groups().items() if owner in ids), None)
        if group is None:
            continue
        name = read(item['go'])['m_Name']
        prefixes = {'fog': ('fog ',), 'haze': ('haze',),
                    'glow': ('cd_room_beam_glow',),
                    'background': ('BG_twist',)}
        sprites = {'fog': SOFT_SPRITES, 'haze': SOFT_SPRITES,
                   'glow': {'resources.assets-2327.json', 'resources.assets-3077.json'},
                   'background': {'resources.assets-2171.json'}}
        if not item['active'] or item['sprite'] not in sprites[group] or not name.startswith(prefixes[group]):
            raise ValueError(f'Changed 30 fps atmosphere owner: {owner}')
        selected.append(dict(owner=owner, name=name, sprite=item['sprite'], group=group))
    if {row['owner'] for row in selected} != set().union(*cut_groups().values()):
        raise ValueError('Missing reviewed 30 fps atmosphere owners')
    return selected


def trim_fringe(image):
    """Apply the configured cutoff; zero preserves source soft edges (AP48/AP54)."""
    result = image.copy()
    result.putalpha(image.getchannel('A').point(lambda a: 0 if a <= FRINGE_ALPHA else a))
    return result


def resize_background(image):
    """Half each texture dimension; caller retains the source world bounds."""
    return image.convert('RGBa').resize((max(1, (image.width+1)//2),
        max(1, (image.height+1)//2)), Image.Resampling.BOX).convert('RGBA')


def thin_particles(data):
    """Reduce shared decorative bursts/rates, retaining at least one particle."""
    for emitter in data['emitters']:
        emitter['count'] = (emitter['count']+1)//2
        emitter['rate'] *= PARTICLE_DENSITY
    if data['grass']:
        data['grass']['count'] = data['emitters'][data['grass']['emitter']]['count']
