"""Eastern room admission and private source ownership.

Indices are zero-based BuildSettings entries from build 22529139. Candidates
are deliberately separate from admitted packages: a catalog entry alone does
not prove that its terrain, artwork or room scripts are supported.
"""
from pathlib import Path

# Recovered exports include camera, border and blur-plane dependencies together.
# The legacy eastern directory predates those exports and must not shadow them.
SOURCE = Path('_private/research/crossroads-scenes')
B2_CANDIDATES = {
    'RestingGrounds_07': 242,
    'RestingGrounds_08': 243,
    'RestingGrounds_09': 244,
    'RestingGrounds_10': 245,
    'RestingGrounds_17': 247,
}
# Crossroads_10 is an original-resolution room admission, not an eastern-room export.
NATIVE_RESOLUTION = {'Crossroads_10'}
# Add a candidate only after source export and native-resolution admission.
ADMITTED = {scene: B2_CANDIDATES[scene] for scene in (
    'RestingGrounds_07', 'RestingGrounds_08', 'RestingGrounds_09')}


def scene_source(scene):
    """Use the complete canonical export for room artwork and dependencies."""
    if scene == 'Dream_Nailcollection':
        return Path('_private/converted-psp/route-scenes-source') / scene
    # Targeted gate recovery stages a complete room without mutating the shared
    # source export. All converters and validation use this same ownership map.
    obstacle_rooms = {
        'Ruins1_05': 'ruins-gate-05-room',
        'Ruins1_05b': 'ruins-gate-room',
        'Ruins1_05c': 'ruins-gate-05c-room',
        'Fungus2_14': 'mantis-14-room',
        'Fungus2_15': 'mantis-15-room',
        'Fungus2_18': 'mantis-18-room',
    }
    if scene in obstacle_rooms:
        return Path('_private/minimum-obstacles') / obstacle_rooms[scene]
    if scene == 'Room_ruinhouse':
        return Path('_private/minimum-world-quests/scenes') / scene
    return SOURCE / scene
