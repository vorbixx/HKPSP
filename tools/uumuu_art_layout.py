"""Decoded native RGBA4 page plans used by the guarded Uumuu GPU adapter."""

BODY_PAGES = ((1024, 1024),) * 5 + ((512, 512),)
QUIRREL_PAGES = ((1024, 1024),) * 2 + ((256, 256),)


def validate_runtime_layout(manifest, expected):
    dimensions = tuple(tuple(page) for page in manifest.get('page_dimensions', ()))
    if dimensions != expected or manifest.get('pages') != len(expected):
        raise ValueError('Packed Uumuu art differs from its guarded runtime page layout')
    texture_bytes = sum(width * height * 2 for width, height in dimensions)
    if manifest.get('texture_bytes') != texture_bytes:
        raise ValueError('Packed Uumuu art differs from its decoded RGBA4 allocation cost')
    return texture_bytes
