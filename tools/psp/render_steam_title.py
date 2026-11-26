"""Bake the authored Menu_Title backdrop from the original PC data."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image
import UnityPy


SIZE = (1920, 1080)
# Menu_Title's settled framing, also used by HK3DS's offline title render.
CENTER = (14.6, 8.4)
INPUTS = ('level1', 'resources.assets', 'resources.assets.resS',
          'sharedassets1.assets', 'sharedassets1.assets.resS')
BACKDROP = 'menu-background-source.png'
METHOD = 'offline-menu-title-v1'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def transform_of(obj):
    return obj.m_GameObject.read().m_Component[0].component.read()


def planar_transform(transform):
    rotation = transform.m_LocalRotation
    if abs(rotation.x) + abs(rotation.y) > 1e-6:
        raise ValueError('Title renderer requires planar sprite transforms')
    scale, position = transform.m_LocalScale, transform.m_LocalPosition
    cosine = 1 - 2 * rotation.z**2
    sine = 2 * rotation.z * rotation.w
    local = np.array(((cosine * scale.x, -sine * scale.y, 0, position.x),
                      (sine * scale.x, cosine * scale.y, 0, position.y),
                      (0, 0, scale.z, position.z), (0, 0, 0, 1)))
    if transform.m_Father:
        return planar_transform(transform.m_Father.read()) @ local
    return local


def title_layers(scene):
    style = scene.objects[4769].read()
    group = transform_of(style).m_Father.read()
    if group.m_GameObject.read().m_Name != 'Modern_Style':
        raise ValueError('Expected the authored blue title style in Menu_Title')
    # This style is selected at runtime in the reference. The scene's other
    # menu styles, UI, logos and independently drawn particles stay out of the bake.
    layers = []
    for child in group.m_Children:
        transform = child.read()
        owner = transform.m_GameObject.read()
        if not owner.m_IsActive:
            continue
        for part in owner.m_Component:
            if part.component.type.name == 'SpriteRenderer':
                renderer = part.component.read()
                if renderer.m_Enabled:
                    layers.append((renderer, planar_transform(transform)))
    if len(layers) != 8:
        raise ValueError(f'Expected eight blue title sprites, found {len(layers)}')
    return sorted(layers, key=lambda layer: (layer[0].m_SortingLayer,
                                             layer[0].m_SortingOrder, -layer[1][2, 3]))


def project_sprite(renderer, world, focal_length, camera_z, textures):
    pointer = renderer.m_Sprite
    identity = (pointer.m_FileID, pointer.path_id)
    if identity not in textures:
        sprite = pointer.read()
        image = sprite.image.convert('RGBA')
        offset, rect, pivot = sprite.m_RD.textureRectOffset, sprite.m_Rect, sprite.m_Pivot
        units = 1 / sprite.m_PixelsToUnits
        # Unity's trim offset is measured from the bottom left of the untrimmed sprite.
        x = (offset.x - rect.width * pivot.x) * units
        y = (offset.y + image.height - rect.height * pivot.y) * units
        pixels_to_local = np.array(((units, 0, x), (0, -units, y), (0, 0, 1)))
        textures[identity] = image, pixels_to_local
    image, pixels_to_local = textures[identity]
    if renderer.m_FlipX or renderer.m_FlipY:
        axes = np.diag((-1 if renderer.m_FlipX else 1, -1 if renderer.m_FlipY else 1, 1))
        pixels_to_local = axes @ pixels_to_local
    depth = world[2, 3] - camera_z
    if depth <= 0:
        raise ValueError('Title sprite is behind the projection camera')
    scale = focal_length / depth
    width, height = SIZE
    projection = np.array(((scale, 0, width / 2 - CENTER[0] * scale),
                           (0, -scale, height / 2 + CENTER[1] * scale),
                           (0, 0, 1)))
    pixels_to_screen = projection @ world[np.ix_((0, 1, 3), (0, 1, 3))] @ pixels_to_local
    inverse = np.linalg.inv(pixels_to_screen)
    return image.transform(SIZE, Image.Transform.AFFINE, inverse[:2].ravel(),
                           Image.Resampling.BILINEAR)


def composite(destination, image, renderer):
    shader = renderer.m_Materials[0].read().m_Shader.read().m_ParsedForm.m_Name
    tint = renderer.m_Color
    pixels = np.asarray(image, dtype=np.float32) / 255
    pixels *= (tint.r, tint.g, tint.b, tint.a)
    alpha = pixels[..., 3:4]
    rgb = destination[..., :3]
    coverage = destination[..., 3:4]
    if shader == 'UI/BlendModes/Screen':
        # The screen shader discards alpha below 0.01, samples the backdrop,
        # then blends with SrcAlpha / OneMinusSrcAlpha for both RGB and alpha.
        alpha[alpha < 0.01] = 0
        rgb += pixels[..., :3] * (1 - rgb) * alpha
        coverage[:] = alpha * alpha + coverage * (1 - alpha)
    elif shader == 'Sprites/Default':
        rgb[:] = pixels[..., :3] * alpha + rgb * (1 - alpha)
        coverage[:] = alpha + coverage * (1 - alpha)
    else:
        raise ValueError(f'Unsupported title shader: {shader}')


def render(original):
    scene = UnityPy.load(str(original / 'level1')).file
    camera = scene.objects[3518].read()
    if camera.orthographic or camera.m_GameObject.read().m_Name != 'tk2dCamera':
        raise ValueError('Expected the perspective Menu_Title camera')
    camera_z = planar_transform(transform_of(camera))[2, 3]
    focal_length = SIZE[1] / (2 * math.tan(math.radians(camera.field_of_view / 2)))
    background = camera.m_BackGroundColor
    canvas = np.empty((SIZE[1], SIZE[0], 4), dtype=np.float32)
    canvas[:] = (background.r, background.g, background.b, background.a)
    textures = {}
    for renderer, transform in title_layers(scene):
        sprite = project_sprite(renderer, transform, focal_length, camera_z, textures)
        composite(canvas, sprite, renderer)
    return Image.fromarray(np.rint(np.clip(canvas, 0, 1) * 255).astype(np.uint8))


def prepare(original, source):
    source = source.absolute()
    if source.resolve() != source:
        raise ValueError(f'Title export must use a direct directory: {source}')
    provenance_path = source / 'provenance.json'
    backdrop = source / BACKDROP
    if any(path.is_symlink() for path in (provenance_path, backdrop)):
        raise ValueError(f'Title export requires regular destination files: {source}')
    provenance = json.loads(provenance_path.read_text(encoding='utf-8'))
    inputs = {name: digest(original / name) for name in INPUTS}
    if backdrop.is_file():
        for entry in provenance['assets']:
            if (entry['file'] == BACKDROP and entry.get('method') == METHOD
                    and entry.get('inputs') == inputs and entry['sha256'] == digest(backdrop)):
                print('Offline title backdrop already prepared')
                return
    with tempfile.TemporaryDirectory(prefix='.title-backdrop-', dir=source) as temporary:
        pending = Path(temporary) / BACKDROP
        render(original).save(pending)
        entry = dict(collection='Menu_Title', name='Modern_Style', file=BACKDROP,
                     method=METHOD, inputs=inputs, sha256=digest(pending))
        provenance['assets'] = [asset for asset in provenance['assets'] if asset['file'] != BACKDROP]
        provenance['assets'].append(entry)
        pending_provenance = Path(temporary) / provenance_path.name
        pending_provenance.write_text(json.dumps(provenance, indent=2) + '\n', encoding='utf-8')
        pending.replace(backdrop)
        pending_provenance.replace(provenance_path)
    print('Rendered Menu_Title backdrop from the Steam assets')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.original, args.source)
