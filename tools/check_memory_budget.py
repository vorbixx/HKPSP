"""Check the resident Tutorial_01 RomFS allocation against the ARM heap budget."""
from __future__ import annotations

import argparse
import ast
import json
import re
import struct
import wave
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MIB = 1024 * 1024

# Remaining linear bytes observed after the complete Tutorial_01 load on 2026-10-22.
MEASURED_FULL_LOAD_LINEAR_FREE = 1_218_048
# At that capture, manifest-backed texture/PCM plus the ScreenEffects target used
# 40,920,322 bytes. The residual is the fixed graphics/runtime allocation omitted
# from converter manifests, kept constant as packages change.
CALIBRATED_REFERENCE_LINEAR_HEAP = 42 * MIB + MIB // 2
CALIBRATED_REFERENCE_PACKAGE_BYTES = 40_920_322
CALIBRATED_RUNTIME_LINEAR_BYTES = (
    CALIBRATED_REFERENCE_LINEAR_HEAP
    - MEASURED_FULL_LOAD_LINEAR_FREE
    - CALIBRATED_REFERENCE_PACKAGE_BYTES
)
# The measured 85.38 MiB ordinary-heap reference was taken at the 36 MiB base split.
ORDINARY_HEAP_FREE_AT_BASE_SPLIT = 85.38 * MIB


class MemoryBudgetError(RuntimeError):
    pass


def _ambient_pool_bound(manifest: dict, romfs: Path, runtime_capacity: int) -> int:
    """Check every selectable bank against the single reserved runtime pool."""
    default = manifest.get("ambient_particles", {})
    declared = int(default.get("capacity", -1))
    banks = {default.get("config", "ambient-particles.bin"): declared}
    banks.update({name: None for name in manifest.get("deepnest_ambient", {}).values()})
    bound = 0
    for name, expected in banks.items():
        try:
            with (romfs / name).open("rb") as stream:
                header = stream.read(16)
            magic, version, emitters, total = struct.unpack("<4s3I", header)
        except (OSError, struct.error) as error:
            raise MemoryBudgetError(f"Invalid ambient particle bank {name}: {error}") from error
        if magic != b"HAP1" or version not in (2, 3) or emitters == 0 or total == 0:
            raise MemoryBudgetError(f"Invalid ambient particle bank header: {name}")
        if expected is not None and total != expected:
            raise MemoryBudgetError(
                f"Ambient particle bank {name} differs: manifest={expected}, bank={total}")
        if total > runtime_capacity:
            raise MemoryBudgetError(
                f"Ambient particle bank {name} capacity {total} exceeds runtime capacity {runtime_capacity}")
        bound = max(bound, total)
    return bound


@dataclass(frozen=True)
class PackageUsage:
    texture_bytes: int
    texture_pages: int
    pcm_bytes: int
    enemy_pcm_bytes: int
    grass_pcm_bytes: int
    great_door_pcm_bytes: int
    geo_texture_bytes: int
    great_door_texture_bytes: int
    hud_texture_bytes: int
    decor_pool_bound: int
    decor_pool_capacity: int
    ambient_pool_capacity: int


@dataclass(frozen=True)
class BudgetResult:
    usage: PackageUsage
    constants: dict[str, int]
    fixed_linear_bytes: int
    linear_free_bytes: int
    linear_reserve_bytes: int
    ordinary_heap_bytes: float


def _read_json(path: Path) -> dict:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MemoryBudgetError(f"Cannot read converter manifest {path}: {exc}") from exc


def _cpp_constants(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    expressions = dict(re.findall(
        r"constexpr\s+std::size_t\s+(\w+)\s*=\s*([^;]+);", text))
    resolved: dict[str, int] = {}

    def evaluate(name: str, active: set[str]) -> int:
        if name in resolved:
            return resolved[name]
        if name in active or name not in expressions:
            raise MemoryBudgetError(f"Cannot resolve memory constant {name} in {path}")
        active.add(name)
        source = re.sub(r"(?<=\d)[uUlL]+\b", "", expressions[name])
        node = ast.parse(source, mode="eval").body

        def visit(item: ast.AST) -> int:
            if isinstance(item, ast.Constant) and isinstance(item.value, int):
                return item.value
            if isinstance(item, ast.Name):
                return evaluate(item.id, active)
            if isinstance(item, ast.BinOp) and isinstance(item.op, (ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div)):
                left, right = visit(item.left), visit(item.right)
                if isinstance(item.op, ast.Add): return left + right
                if isinstance(item.op, ast.Sub): return left - right
                if isinstance(item.op, ast.Mult): return left * right
                if right == 0: raise MemoryBudgetError(f"Division by zero in {name}")
                return left // right
            raise MemoryBudgetError(f"Unsupported expression for {name}: {expressions[name]}")

        value = visit(node)
        active.remove(name)
        resolved[name] = value
        return value

    for key in expressions:
        evaluate(key, set())
    return resolved


def _pool_capacity(path: Path, class_name: str) -> int:
    source = path.read_text(encoding="utf-8")
    match = re.search(rf"class\s+{re.escape(class_name)}\b[\s\S]*?capacity\s*=\s*(\d+)", source)
    if not match:
        raise MemoryBudgetError(f"Cannot find {class_name} capacity in {path}")
    return int(match.group(1))


def _check_files(romfs: Path, prefix: str, count: int) -> None:
    expected = {f"{prefix}{index}.t3x" for index in range(count)}
    found = {path.name for path in romfs.glob(f"{prefix}*.t3x")}
    missing, extra = sorted(expected - found), sorted(found - expected)
    if missing or extra:
        details = []
        if missing: details.append("missing " + ", ".join(missing))
        if extra: details.append("unlisted " + ", ".join(extra))
        raise MemoryBudgetError(f"Packaged {prefix} texture pages: {'; '.join(details)}")
    for name in expected:
        if (romfs / name).stat().st_size <= 0:
            raise MemoryBudgetError(f"Packaged texture page is empty: {romfs / name}")


INVENTORY_PAGE_SIZE = (256, 256)


def _texture_group(converted: Path, romfs: Path, *, name: str, manifest_name: str,
                   prefix: str, page_count_key: str | None, default_pages: int,
                   page_size: tuple[int, int] | tuple[tuple[int, int], ...], texture_format: str | tuple[str, ...], bytes_per_pixel: int,
                   page_bytes_key: str | None = None) -> tuple[int, int]:
    manifest = _read_json(converted / manifest_name)
    pages = int(manifest.get(page_count_key, default_pages)) if page_count_key else default_pages
    if pages < 1:
        raise MemoryBudgetError(f"{name} manifest has no texture pages")
    fmt = manifest.get("texture_format", manifest.get("format"))
    formats = texture_format if isinstance(texture_format, tuple) else (texture_format,) * pages
    if isinstance(texture_format, tuple) and tuple(manifest.get('texture_formats', ())) != formats:
        raise MemoryBudgetError(f"{name} page formats differ from the fixed allocation")
    if not isinstance(texture_format, tuple) and fmt is not None and fmt != texture_format:
        raise MemoryBudgetError(f"{name} texture format is {fmt!r}, expected {texture_format!r}")
    sizes = page_size if isinstance(page_size[0], tuple) else (page_size,) * pages
    if len(sizes) != pages or len(formats) != pages:
        raise MemoryBudgetError(f"{name} page count differs from the fixed allocation")
    pixel_sizes = {'rgba8': 4, 'rgba4': 2, 'la8': 2, 'la4': 1}
    expected_bytes = sum(width * height * pixel_sizes[format] for (width, height), format in zip(sizes, formats))
    if page_bytes_key and int(manifest.get(page_bytes_key, sizes[0][0]*sizes[0][1]*bytes_per_pixel)) != sizes[0][0]*sizes[0][1]*bytes_per_pixel:
        raise MemoryBudgetError(f"{name} page byte size differs from the fixed allocation")
    total = int(manifest.get("texture_bytes", -1))
    if total != expected_bytes:
        raise MemoryBudgetError(
            f"{name} manifest reports {total} texture bytes, but {pages} pages require {expected_bytes}")
    _check_files(romfs, prefix, pages)
    # Inspect the packaged header too: a smaller manifest must not conceal an
    # unchanged, larger GPU allocation. Tex3DS stores log2(size)-3 in 3 bits.
    for index, size in enumerate(sizes):
        with (romfs / f"{prefix}{index}.t3x").open('rb') as stream:
            header = stream.read(4)
        actual = ((8 << (header[2] & 7), 8 << ((header[2] >> 3) & 7))
                  if len(header) == 4 else None)
        if actual != size or header[3] != {'rgba8': 0, 'rgba4': 4, 'la8': 5, 'la4': 9}[formats[index]]:
            raise MemoryBudgetError(f"{name} packaged page {index} dimensions/format differ from the fixed allocation")
    return total, pages


def _lighting_textures(converted: Path, romfs: Path) -> tuple[int, int]:
    manifest = _read_json(converted / "hero-light/manifest.json")
    if manifest.get("format") not in ("rgba8", "mixed"):
        raise MemoryBudgetError("Unsupported lighting manifest format")
    sprites = manifest.get("sprites", [])
    if {sprite.get("name") for sprite in sprites} != {"hero-light", "vignette", "hero-glow"}:
        raise MemoryBudgetError("Lighting manifest must contain hero-light, vignette and hero-glow")
    total = 0
    for sprite in sprites:
        name = sprite["name"]
        width, height = map(int, sprite["size"])
        padded_width = 1 << (width - 1).bit_length()
        padded_height = 1 << (height - 1).bit_length()
        path = romfs / f"{name}.t3x"
        if not path.is_file() or path.stat().st_size <= 0:
            raise MemoryBudgetError(f"Lighting texture is missing or empty: {path}")
        fmt = sprite.get("format", "rgba8")
        if fmt != "rgba8" and (name != "vignette" or fmt != "la8" or manifest.get("format") != "mixed"):
            raise MemoryBudgetError(f"Unsupported lighting texture format: {name} {fmt}")
        header = path.read_bytes()[:4]
        if len(header) != 4 or header[3] != {"rgba8": 0, "la8": 5}[fmt]:
            raise MemoryBudgetError(f"Lighting texture format mismatch: {path}")
        total += padded_width * padded_height * (2 if fmt == "la8" else 4)
    return total, len(sprites)


def _pcm_bytes(path: Path) -> int:
    try:
        with wave.open(str(path), "rb") as wav:
            if wav.getcomptype() != "NONE" or wav.getsampwidth() != 2:
                raise MemoryBudgetError(f"Expected PCM16 WAV: {path}")
            return wav.getnframes() * wav.getnchannels() * wav.getsampwidth()
    except (OSError, wave.Error) as exc:
        raise MemoryBudgetError(f"Cannot read PCM WAV {path}: {exc}") from exc


def _audio_clip_names(source: str, method: str) -> list[str]:
    # Room-local banks are not resident in the Tutorial_01 startup budget.
    body = re.search(r"\bAudioPlayer::" + re.escape(method)
                     + r"\([^)]*\)\s*\{(.*?)(?=^\})", source, re.S | re.M)
    names = re.search(r"const\s+char\s*\*\s*names\[\]\s*=\s*\{(.*?)\};",
                      body[1], re.S) if body else None
    if not names:
        raise MemoryBudgetError(f"AudioPlayer::{method} clip-name array is missing")
    return re.findall(r'"([^"]+)"', names[1])


def _base_pcm_assets(romfs: Path, audio_source: Path) -> int:
    source = audio_source.read_text(encoding="utf-8")
    names = _audio_clip_names(source, "load")
    if not names:
        raise MemoryBudgetError('Empty base AudioPlayer clip table')
    return sum(_pcm_bytes(romfs / f'sfx-{name}.wav') for name in names)


FALSE_KNIGHT_STREAMS = (
    'false_knight_damage_armour', 'false_knight_head_damage_2', 'false_knight_jump',
    'false_knight_strike_ground', 'Fknight_daze', 'FKnight_Rage', 'FKnight_death',
    'false_knight_swing', 'Fknight_flump_01')


def _false_knight_assets(romfs: Path) -> None:
    manifest = _read_json(romfs / 'false-knight-audio-manifest.json')
    clips = manifest.get('clips', [])
    if (manifest.get('resident_pcm_bytes') != 0
            or [c.get('name') for c in clips] != list(FALSE_KNIGHT_STREAMS) + ['Boss Battle 1']):
        raise MemoryBudgetError('False Knight manifest must contain ten nonresident native streams')
    for clip in clips:
        music = clip['name'] == 'Boss Battle 1'
        output = 'music-false-knight.bin' if music else f"sfx-enemy-{clip['name']}.wav"
        frames = clip.get('frames', 0)
        if (clip.get('output') != output or clip.get('rate') != 44100
                or clip.get('channels') != 2 or not frames):
            raise MemoryBudgetError(f'Invalid False Knight stream metadata: {output}')
        path = romfs / output
        if music:
            with path.open('rb') as stream:
                valid = stream.read(16) == struct.pack('<4sIII', b'HKAW', 44100, 2, frames)
            valid = valid and path.stat().st_size == 16 + frames * 4
        else:
            with wave.open(str(path), 'rb') as wav:
                valid = (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (2, 2, 44100, frames)
            valid = valid and _pcm_bytes(path) == frames * 4
        if not valid:
            raise MemoryBudgetError(f'Invalid native False Knight stream: {output}')


def _pcm_assets(romfs: Path, audio_source: Path, constants: dict[str, int]) -> tuple[int, int, int, int]:
    source = audio_source.read_text(encoding="utf-8")
    base_names = _audio_clip_names(source, "load")
    enemy_names = _audio_clip_names(source, "loadEnemies")
    if not base_names or not enemy_names:
        raise MemoryBudgetError("AudioPlayer clip-name arrays are empty")
    if any(name in enemy_names for name in FALSE_KNIGHT_STREAMS):
        if not set(FALSE_KNIGHT_STREAMS).issubset(enemy_names):
            raise MemoryBudgetError('Incomplete False Knight runtime stream table')
        _false_knight_assets(romfs)
        enemy_names = [name for name in enemy_names if name not in FALSE_KNIGHT_STREAMS]

    base_pcm = 0
    for name in base_names:
        path = romfs / f"sfx-{name}.wav"
        if not path.is_file():
            raise MemoryBudgetError(f"Resident base PCM is missing: {path}")
        base_pcm += _pcm_bytes(path)

    manifest = _read_json(romfs / "enemy-audio-manifest.json")
    clips = list(manifest.get("clips", []))
    expected_pcm = int(manifest.get("pcm_resident_budget_bytes", -1))
    if any(name.startswith("stag-") for name in enemy_names):
        stag = _read_json(romfs / "stag-audio-manifest.json")
        clips += stag.get("clips", [])
        expected_pcm += int(stag.get("pcm_resident_budget_bytes", -1))
    if len(clips) != len(enemy_names):
        raise MemoryBudgetError(f"Enemy PCM manifest has {len(clips)} clips, runtime loads {len(enemy_names)}")
    manifest_names = set()
    enemy_pcm = grass_pcm = great_door_pcm = streamed_pcm = 0
    for clip in clips:
        output = str(clip.get("output", ""))
        if Path(output).name != output or not output.endswith(".wav"):
            raise MemoryBudgetError(f"Invalid enemy PCM output name: {output!r}")
        manifest_names.add(re.sub(r"^sfx-(?:enemy|grass)-", "", output.removesuffix(".wav")))
        actual_bytes = _pcm_bytes(romfs / output)
        if actual_bytes != int(clip.get("pcm16_bytes", -1)):
            raise MemoryBudgetError(f"PCM size mismatch for {output}: WAV={actual_bytes}, manifest={clip.get('pcm16_bytes')}")
        streamed = output in {"sfx-enemy-S75_Opening_Sting-08.wav", "sfx-enemy-break_wall_after_tutorial_area.wav", "sfx-enemy-secret_discovered_temp.wav", "sfx-enemy-hero_death_extra_details.wav"}
        if bool(clip.get("streamed")) != streamed:
            raise MemoryBudgetError(f"Unexpected streaming policy for {output}")
        if streamed:
            with wave.open(str(romfs / output), "rb") as wav:
                if wav.getnchannels() != 2 or wav.getframerate() != 44100:
                    raise MemoryBudgetError("Streamed room cues must retain native stereo 44.1 kHz PCM")
            streamed_pcm += actual_bytes
        else:
            enemy_pcm += actual_bytes
        if output.startswith("sfx-grass-"):
            grass_pcm += actual_bytes
        if "great door" in str(clip.get("role", "")).lower():
            great_door_pcm += actual_bytes
    if manifest_names != set(enemy_names):
        raise MemoryBudgetError("Enemy PCM manifest clip names differ from AudioPlayer::loadEnemies")
    if enemy_pcm != expected_pcm:
        raise MemoryBudgetError("Enemy PCM clip sum differs from pcm_resident_budget_bytes")
    if streamed_pcm != int(manifest.get("pcm_streamed_bytes", -1)):
        raise MemoryBudgetError("Opening streamed PCM sum differs from pcm_streamed_bytes")
    if int(manifest.get("pcm_cap_bytes", -1)) != constants["enemyAudioBankCapBytes"]:
        raise MemoryBudgetError("Enemy audio manifest cap differs from render_memory.hpp")
    return base_pcm, enemy_pcm, grass_pcm, great_door_pcm


def _wind_assets(romfs: Path, constants: dict[str, int]) -> None:
    manifest = _read_json(romfs / 'wind-audio-manifest.json')
    # Packaged clips are not concurrent voices: the worker still reserves only
    # three fixed queues/cache slots. Validate each authored native rate here.
    expected = (('cave_wind_loop', 44100), ('dirtmouth_wind_loop_a', 44100),
                ('dirtmouth_wind_loop_b_grass', 44100), ('green_path_atmos_loop', 48000),
                ('cave_noises', 48000))
    clips = manifest.get('clips', [])
    # Older exports contain identical PCM but predate this descriptive field.
    # Runtime constants always charge the cache; reject an explicit mismatch.
    if ('pcm_start_cache_bytes' in manifest
            and manifest['pcm_start_cache_bytes'] != constants['windStreamStartCacheBytes']):
        raise MemoryBudgetError('Wind startup cache differs from runtime ordinary-heap allocation')
    if (manifest.get('pcm_resident_bytes') != 0 or len(clips) != len(expected)
            or [c.get('name') for c in clips] != [name for name, _ in expected]):
        raise MemoryBudgetError('Wind stream manifest must contain the five authored nonresident native loops')
    total = 0
    for clip, (name, rate) in zip(clips, expected):
        output = f'wind-{name}.bin'
        if clip.get('output') != output:
            raise MemoryBudgetError('Unexpected wind stream output')
        try:
            with (romfs/output).open('rb') as file:
                header = file.read(16)
            valid = (len(header) == 16 and struct.unpack('<4sIII', header)
                     == (b'HKAW', rate, 2, clip.get('frames')))
            size = (romfs/output).stat().st_size-16
        except OSError as exc:
            raise MemoryBudgetError(f'Missing wind stream: {output}') from exc
        if (not valid or not clip.get('frames') or size != clip['frames']*4
                or size != clip.get('pcm16_bytes') or clip.get('channels') != 2
                or clip.get('sample_rate') != rate or clip.get('gain') != 1 or clip.get('pitch') != 1):
            raise MemoryBudgetError(f'Invalid native wind stream: {output}')
        total += size
    if total != manifest.get('pcm_streamed_bytes'):
        raise MemoryBudgetError('Wind streamed byte count differs from packaged PCM')


def _ambient_pool_capacity(manifest: dict, runtime_capacity: int) -> int:
    bound = int(manifest.get("ambient_particles", {}).get("capacity", -1))
    if not 0 < bound <= runtime_capacity:
        raise MemoryBudgetError(
            f"Invalid ambient particle bank bound: manifest={bound}, runtime={runtime_capacity}")
    # A smaller bank still owns the whole fixed runtime pool.
    return runtime_capacity


def read_package_usage(converted: Path, romfs: Path, root: Path = ROOT) -> tuple[PackageUsage, dict[str, int]]:
    constants = _cpp_constants(root / "include/render_memory.hpp")
    texture_bytes = texture_pages = 0

    groups = [
        ("conditional events", "conditional-events/manifest.json", "events-page-", "pages", 2, ((1024, 1024), (1024, 512)), "rgba4", 2, None),
        ("hero", "hero/manifest.json", "page-", "pages", 5, ((1024, 1024),) * 5, "rgba4", 2, None),
        ("resident room", "room-resident/manifest.json", "room-resident-page-", "pages", constants['roomTextureBudget']//(2*MIB), (1024, 1024), resident_texture_formats(_read_json(converted/'room-resident/manifest.json')), 2, None),
        ("Fury", "fury-chest/manifest.json", "fury-page-", "pages", 4, ((512,256),(256,256),(256,128),(512,128)), ("rgba8","rgba8","rgba8","la8"), 4, None),
        ("geo", "geo-visuals/manifest.json", "geo-page-", "page_count", 2, (512, 512), "rgba8", 4, "texture_page_bytes"),
        ("Great Door", "great-door-visuals/manifest.json", "great-door-page-", None, 1, (1024, 512), "rgba8", 4, None),
        ("HUD", "hud/manifest.json", "hud-page-", "pages", 2, ((512,256),(512,256)), ("rgba8","la8"), 4, None),
        ("map", "map/manifest.json", "map-page-", "pages", 1, (512,512), "la4", 1, None),
        ("Inventory", "inventory/manifest.json", "inventory-page-", None, 1, INVENTORY_PAGE_SIZE, "rgba4", 2, None),
        ("Shade", "shade/manifest.json", "shade-page-", "pages", 2, ((512, 512), (512, 256)), "rgba4", 2, None),
        ("buzzer", "buzzer/manifest.json", "buzzer-page-", None, 1, (512, 512), "rgba8", 4, None),
        ("crawler", "crawler/manifest.json", "crawler-page-", None, 1, (256, 512), "rgba8", 4, None),
        ("decor effects", "decor-effects/manifest.json", "decor-fx-page-", "pages", 1, (512, 512), "rgba4", 2, None),
    ]
    resource_bytes: dict[str, int] = {}
    if (converted/'ability-pickups/manifest.json').exists():
        groups.append(("abilities", "ability-pickups/manifest.json", "ability-page-", "pages", 1, (512,512), "rgba4", 2, None))
    if (converted/'cornifer-original/manifest.json').exists():
        size, count = _texture_group(
            converted, romfs, name='Cornifer',
            manifest_name='cornifer-original/manifest.json', prefix='cornifer-page-',
            page_count_key='pages', default_pages=1, page_size=(512, 512),
            texture_format='rgba4', bytes_per_pixel=2)
        resource_bytes['Cornifer'] = size
        texture_bytes += size
        texture_pages += count
    if (converted/'greenpath-enemies/manifest.json').exists():
        groups.append(("Greenpath", "greenpath-enemies/manifest.json", "greenpath-page-", "pages", 1, (1024,1024), "rgba4", 2, None))
    for name, manifest, prefix, count_key, default_count, dims, fmt, bpp, page_key in groups:
        size, count = _texture_group(
            converted, romfs, name=name, manifest_name=manifest, prefix=prefix,
            page_count_key=count_key, default_pages=default_count, page_size=dims,
            texture_format=fmt, bytes_per_pixel=bpp, page_bytes_key=page_key)
        resource_bytes[name] = size
        texture_bytes += size
        texture_pages += count

    lighting_bytes, lighting_pages = _lighting_textures(converted, romfs)
    resource_bytes["lighting"] = lighting_bytes
    texture_bytes += lighting_bytes
    texture_pages += lighting_pages

    audio_source = root / "src/audio_player.cpp"
    base_pcm, enemy_pcm, grass_pcm, great_door_pcm = _pcm_assets(romfs, audio_source, constants)
    _wind_assets(romfs, constants)
    decor_manifest = _read_json(converted / "decor-effects/manifest.json")
    decor_pool_bound = int(decor_manifest.get("pool_bound", -1))
    decor_capacity = _pool_capacity(root / "include/breakable.hpp", "DecorEffects")
    _ambient_pool_bound(decor_manifest, romfs, _pool_capacity(root / "include/ambient_particles.hpp", "AmbientParticles"))
    ambient_capacity = _ambient_pool_capacity(
        decor_manifest, _pool_capacity(root / "include/ambient_particles.hpp", "AmbientParticles"))
    if decor_pool_bound < 0 or decor_pool_bound > decor_capacity:
        raise MemoryBudgetError(f"Decor effect pool bound {decor_pool_bound} exceeds runtime capacity {decor_capacity}")


    usage = PackageUsage(
        texture_bytes=texture_bytes,
        texture_pages=texture_pages,
        pcm_bytes=base_pcm + enemy_pcm,
        enemy_pcm_bytes=enemy_pcm,
        grass_pcm_bytes=grass_pcm,
        great_door_pcm_bytes=great_door_pcm,
        geo_texture_bytes=resource_bytes["geo"],
        great_door_texture_bytes=resource_bytes["Great Door"],
        hud_texture_bytes=resource_bytes["HUD"],
        decor_pool_bound=decor_pool_bound,
        decor_pool_capacity=decor_capacity,
        ambient_pool_capacity=ambient_capacity,
    )
    return usage, constants


def evaluate_budget(usage: PackageUsage, constants: dict[str, int], *, extra_texture_bytes: int = 0) -> BudgetResult:
    fixed_linear_bytes = (constants["screenEffectsLinearTargetBytes"] + constants["windStreamBufferBytes"]
                          + constants.get("openingStreamBufferBytes", 0) + constants.get("secretStreamBufferBytes", 0)
                          + constants.get("corniferStreamBufferBytes", 0)
                          # Calibration already includes Citro3D's default buffer.
                          + constants["gpuCommandBufferBytes"] - 256 * 1024)
    reserve = max(constants["hudLinearReserveBytes"], constants["enemyAudioLinearReserveBytes"])
    linear_free = (constants["gameLinearHeapBytes"] - usage.texture_bytes - usage.pcm_bytes
                   - fixed_linear_bytes - CALIBRATED_RUNTIME_LINEAR_BYTES - extra_texture_bytes)

    if usage.enemy_pcm_bytes > constants["enemyAudioBankCapBytes"]:
        raise MemoryBudgetError(
            f"Enemy PCM {usage.enemy_pcm_bytes} exceeds cap {constants['enemyAudioBankCapBytes']}")
    if usage.grass_pcm_bytes > constants["grassLinearHeapBytes"]:
        raise MemoryBudgetError(
            f"Grass PCM {usage.grass_pcm_bytes} exceeds budget {constants['grassLinearHeapBytes']}")
    if usage.great_door_texture_bytes + usage.great_door_pcm_bytes > constants["greatDoorLinearHeapBytes"]:
        raise MemoryBudgetError("Great Door atlas and PCM exceed greatDoorLinearHeapBytes")
    if usage.hud_texture_bytes > constants["hudTextureBudgetBytes"]:
        raise MemoryBudgetError("HUD atlas exceeds hudTextureBudgetBytes")
    if usage.geo_texture_bytes > 2 * constants["geoTexturePageBytes"]:
        raise MemoryBudgetError("Geo pages exceed the two-page admission described by geoTexturePageBytes")
    if linear_free < reserve:
        raise MemoryBudgetError(
            f"Linear heap estimate leaves {linear_free} bytes; admission reserve is {reserve} bytes")

    ordinary_heap = ORDINARY_HEAP_FREE_AT_BASE_SPLIT - (
        constants["gameLinearHeapBytes"] - constants["baseLinearHeapBytes"])
    ordinary_heap -= constants.get("roomDynamicsStateBudgetBytes", 0)
    ordinary_heap -= constants.get("passivePlantStateBudgetBytes", 0)
    ordinary_heap -= constants.get("heroDeathStateBudgetBytes", 0)
    ordinary_heap -= constants.get("shadePresentationStateBudgetBytes", 0)
    ordinary_heap -= constants.get("windStreamStartCacheBytes", 0)
    if ordinary_heap <= 0:
        raise MemoryBudgetError(f"Ordinary heap estimate is not positive: {ordinary_heap:.0f} bytes")
    return BudgetResult(usage, constants, fixed_linear_bytes, linear_free, reserve, ordinary_heap)


def resident_texture_formats(manifest: dict) -> str | tuple[str, ...]:
    if manifest.get('format') != 'mixed':
        return 'rgba4'
    formats = tuple(manifest.get('texture_formats', ()))
    if len(formats) != manifest.get('pages') or any(fmt not in ('rgba4', 'rgba8') for fmt in formats):
        raise MemoryBudgetError('Invalid resident texture formats')
    return formats


def check_scene_texture_scale(manifests: list[dict], budget: int) -> None:
    if any(m.get('texture_scale') != 1 or m.get('quality_cuts',{}).get('half_size_sprites') for m in manifests):
        raise MemoryBudgetError('New scene texture_scale must explicitly be 1; regenerate source artwork')
    resident=manifests[-1]
    pages=resident.get('pages',0)
    formats = resident_texture_formats(resident)
    formats = formats if isinstance(formats, tuple) else (formats,)*pages
    expected_bytes = sum((4 if fmt == 'rgba8' else 2)*MIB for fmt in formats)
    if not 0<pages<=budget//(2*MIB) or resident.get('texture_bytes')!=expected_bytes or expected_bytes>budget:
        raise MemoryBudgetError('New scene resident pages exceed the room texture budget')


def scene_actor_banks(spawns: list[dict], scene: str = "") -> set[str]:
    # Upper estimate includes initially disabled actors, as save state can change
    # activation. Existing Tutorial actor textures remain resident across scenes.
    banks={'Climber':'tiktik','Fly':'gruzzer','Buzzer':'vengefly','Hatcher Baby':'aspid-hatchling',
           'Zombie Runner':'enemy-b','Zombie Hornhead':'enemy-b','Zombie Leaper':'enemy-b','Worm':'enemy-b',
           'Zombie Barger':'enemy-c','Roller':'enemy-c','Hatcher':'enemy-c','Mawlek Body':'enemy-c',
           'Mushroom Brawler':'enemy-c','Ruins Sentry':'enemy-c','Ruins SentryB':'enemy-c',
           'Ruins Sentry Fat':'enemy-c','Ruins Sentry FatB':'enemy-c','Ruins Sentry Fat B':'enemy-c',
           'Ruins Flying Sentry':'enemy-c','Ruins Flying SentryB':'enemy-c',
           'Giant Fly':'boss','False Knight New':'boss','Blocker':'elder-baldur'}
    names={re.sub(r' (?:[0-9]+|\([0-9]+\))$','',e['prefab']) for e in spawns}
    implicit = {"Ruins2_03": "watcher", "Fungus2_15": "mantis"}
    return {banks[name] for name in names if name in banks} | ({implicit[scene]} if scene in implicit else set())


def actor_texture_bytes(header: bytes) -> int:
    # Native actor banks use RGBA8, RGB5A1 (enemy-c), or RGBA4. Charge the
    # decompressed GPU allocation, independently of Tex3DS file compression.
    if len(header)!=4 or header[2]&0xc0 or header[3] not in (0,2,4):
        raise MemoryBudgetError('Invalid actor texture header')
    return (8<<(header[2]&7))*(8<<((header[2]>>3)&7))*(4 if header[3]==0 else 2)


def old_scene_actor_banks(scene: str, spawns: list[dict]) -> set[str]:
    banks=scene_actor_banks(spawns,scene)
    if any(e['prefab'].startswith('False Knight New') for e in spawns):
        if not any(e['prefab'].startswith('Giant Fly') for e in spawns):banks.discard('boss')
        banks.add('false-knight')
    return banks


def source_actor_banks(scene: str, source: Path) -> set[str]:
    """Conservative graph dependencies, before a room has been converted."""
    from convert_greenpath_enemies import kind_for_prefab
    inventory=source/'terrain/terrain-map.json'
    if not inventory.is_file():return set()
    # Metadata also admits placed actors without HealthManager. Include every
    # owner conservatively; unrelated object names select no actor bank.
    owners={owner for row in json.loads(inventory.read_text())['inventory']
            for owner in row['owners']}
    spawns=[{'prefab':json.loads((source/'terrain'/owner).read_text())['m_Name']}
            for owner in sorted(owners)]
    if scene=='Crossroads_10':spawns.append({'prefab':'False Knight New'})
    if scene=='Fungus1_04':spawns.append({'prefab':'Hornet Boss 1'})
    banks=old_scene_actor_banks(scene,spawns)
    if any(kind_for_prefab(e['prefab']) is not None for e in spawns):banks.add('greenpath')
    return banks


def actor_metadata_file(bank: str) -> str:
    return f'{bank}-art.bin' if bank in {'boss','false-knight','watcher','mantis','greenpath','elder-baldur'} else f'{bank}-anim.bin'


def old_scene_actor_paths(scene: str, spawns: list[dict], romfs: Path,
                          sidecar: Path) -> set[Path]:
    """Banks sharing Old's room allowance; startup banks are charged separately."""
    banks=old_scene_actor_banks(scene,spawns)
    if sidecar.is_file():
        header=sidecar.read_bytes()[:8]
        if len(header)<8:raise MemoryBudgetError(f'{scene} invalid Greenpath sidecar')
        if struct.unpack_from('<I',header,4)[0]:banks.add('greenpath')
    paths=set()
    for bank in banks:
        pages=set(romfs.glob(f'{bank}-page-*.t3x'))
        if not pages:raise MemoryBudgetError(f'{scene} missing actor atlas {bank}')
        metadata=romfs/actor_metadata_file(bank)
        if not metadata.is_file():raise MemoryBudgetError(f'{scene} missing actor atlas {bank} metadata')
        header=metadata.read_bytes()[:8]
        if len(header)!=8 or header[:4]!=b'HKA1':
            raise MemoryBudgetError(f'{scene} invalid actor atlas {bank} metadata')
        count=struct.unpack_from('<I',header,4)[0]
        if not 0<count<=32 or pages!={romfs/f'{bank}-page-{i}.t3x' for i in range(count)}:
            raise MemoryBudgetError(f'{scene} incomplete actor atlas {bank}')
        paths.update(pages)
    return paths


def parse_scene_list(value: str) -> tuple[str, ...]:
    from extract_scenes import SCENES
    from greenpath_scenes import SCENES as GREENPATH_SCENES
    from deepnest_scenes import SCENES as DEEPNEST_SCENES
    from scene_sources import saved_scenes
    # Match converter discovery, including complete saved regional exports.
    known = set(SCENES) | set(GREENPATH_SCENES) | set(DEEPNEST_SCENES)
    known.update(saved_scenes(ROOT / "_private/research/crossroads-scenes"))
    scenes = tuple(part.strip() for part in value.split(','))
    if any(not scene for scene in scenes):
        raise MemoryBudgetError('Scene audit list must contain non-empty scene names')
    unknown = set(scenes) - known - {'Tutorial_01'}
    if unknown:
        raise MemoryBudgetError('Unknown scene audit entries: ' + ', '.join(sorted(unknown)))
    return tuple(dict.fromkeys(scenes))


def audit_scene_textures(converted: Path, romfs: Path, constants: dict[str,int],
                         scene_list: str | list[str] | None = None, *, scenes: list[str] | None = None) -> int:
    if isinstance(scene_list, list):
        if scenes is not None:
            raise MemoryBudgetError('Specify only one scene scope')
        scenes = scene_list
        scene_list = None
    if scenes is not None:
        if scene_list is not None:
            raise MemoryBudgetError('Specify only one scene scope')
        scene_list = ','.join(scenes)
    # The default audit admits every scene actually packaged, including rooms
    # outside the extraction catalog. Explicit diagnostic runs use their route.
    if scene_list is None:
        packaged = romfs/'scenes'
        if not packaged.exists():
            return 0
        scenes = [p.name for p in sorted(packaged.iterdir()) if p.is_dir()]
    else:
        scenes = parse_scene_list(scene_list)
    largest=0
    for scene in scenes:
        if scene_list is not None and scene == 'Tutorial_01':
            continue
        directory=converted/'scenes'/scene
        scene_manifest=_read_json(directory/'manifest.json')
        resident_manifest=_read_json(directory/'resident/manifest.json')
        check_scene_texture_scale([scene_manifest,_read_json(directory/'art/manifest.json'),resident_manifest],
                                  constants['roomTextureBudget'])
        size,pages=_texture_group(converted,romfs/'scenes'/scene,name=scene,
            manifest_name=f'scenes/{scene}/resident/manifest.json',prefix='room-resident-page-',
            page_count_key='pages',default_pages=0,page_size=(1024,1024),texture_format=resident_texture_formats(resident_manifest),bytes_per_pixel=2)
        metadata=(romfs/'scenes'/scene/'room-resident-visual.bin').read_bytes()
        if len(metadata)<20 or struct.unpack_from('<I',metadata,4)[0]!=pages:
            raise MemoryBudgetError(f'{scene} packaged resident page count differs')
        actor_banks=scene_actor_banks(scene_manifest.get('enemy_spawns',[]),scene)
        if scene_manifest.get('stag'):
            actor_banks.add('stag')
        for bank in actor_banks:
            textures=list(romfs.glob(f'{bank}-page-*.t3x'))
            if not textures:raise MemoryBudgetError(f'{scene} missing actor atlas {bank}')
            for texture in textures:
                with texture.open('rb') as stream:header=stream.read(4)
                try:size+=actor_texture_bytes(header)
                except MemoryBudgetError as exc:raise MemoryBudgetError(f'Invalid actor texture {texture}') from exc
        # Six shared transient voices can retain room PCM concurrently; one
        # decoder adds ordinary scratch. No extra real NDSP channels are used.
        sound_bytes=max((p.stat().st_size for p in (romfs/'scenes'/scene).glob('room-sfx-*.wav')),default=0)
        if sound_bytes>1024*1024+44:
            raise MemoryBudgetError(f'{scene} breakable PCM exceeds the 1 MiB clip budget')
        size+=7*sound_bytes
        largest=max(largest,size)
    return largest


def _old_profile_knobs(root: Path):
    profile=(root/'include/graphics_profile.hpp').read_text(encoding='utf-8')
    knobs=re.search(r'\{"Old 3DS","([^"]+)",oldRoomTextureBudget,false,false,false,true,([\d.]+)f,2,oldEnemyAudioBankCapBytes,false,([\d.]+)f,([\d.]+)f,4\}',profile)
    if not knobs:raise MemoryBudgetError('Update Old memory audit for changed profile knobs')
    return knobs


def old_profile_knobs(root: Path = ROOT) -> tuple[float, str]:
    knobs = _old_profile_knobs(root)
    return float(knobs.group(2)), knobs.group(1)


def old_profile_depths(root: Path = ROOT) -> tuple[float, float, float]:
    knobs = _old_profile_knobs(root)
    return tuple(float(knobs.group(i)) for i in (2, 3, 4))


def old_room_pages(metadata: bytes, max_depth: float) -> set[int]:
    """Match native room admission, retaining hidden draws that may activate later."""
    if len(metadata)<20 or metadata[:4] not in (b'HKR2',b'HKR3',b'HKR4'):
        raise MemoryBudgetError('Invalid Old profile room metadata')
    pages,sprites,draws,sparse=struct.unpack_from('<4I',metadata,4)
    if len(metadata)!=20+sprites*36+draws*72+sparse*24+(4 if metadata[:4]==b'HKR4' else 0):
        raise MemoryBudgetError('Invalid Old profile room metadata size')
    wanted=set()
    for offset in range(20+sprites*36,20+sprites*36+draws*72,72):
        sprite=struct.unpack_from('<I',metadata,offset)[0]
        if sprite>=sprites:raise MemoryBudgetError('Old profile draw sprite out of range')
        page=struct.unpack_from('<I',metadata,20+sprite*36)[0]
        if page>=pages:raise MemoryBudgetError('Old profile sprite page out of range')
        if abs(struct.unpack_from('<f',metadata,offset+8+6*4)[0])<max_depth:wanted.add(page)
    return wanted


def audit_old_package(converted: Path, romfs: Path, root: Path, scene_list: str | None) -> BudgetResult:
    constants=_cpp_constants(root/'include/render_memory.hpp')
    depth, room_prefix = old_profile_knobs(root)
    def texture_bytes(path):
        header=path.read_bytes()[:4]
        if len(header)!=4 or header[2]&0xc0 or header[3] not in (0,2,4,5,9):
            raise MemoryBudgetError(f'Invalid texture header: {path}')
        return (8<<(header[2]&7))*(8<<((header[2]>>3)&7))*({0:4,2:2,4:2,5:2,9:1}[header[3]])
    # All startup banks still loaded by main, including gameplay FX/hidden actors.
    prefixes=('page-','ability-page-','crawler-page-','buzzer-page-','shade-page-',
              'events-page-','cornifer-page-','fury-page-','geo-page-','great-door-page-',
              'hud-page-','map-page-','inventory-page-','decor-fx-page-')
    core=set()
    for prefix in prefixes:
        paths=set(romfs.glob(prefix+'*.t3x'))
        if not paths:raise MemoryBudgetError(f'Missing Old startup bank: {prefix}')
        core.update(paths)
    scenes=parse_scene_list(scene_list) if scene_list is not None else ('Tutorial_01',)+tuple(p.name for p in sorted((romfs/'scenes').iterdir()) if p.is_dir())
    core_bytes=sum(map(texture_bytes,core))
    worst=0;worst_pages=0
    for scene in scenes:
        directory=romfs if scene=='Tutorial_01' else romfs/'scenes'/scene
        pages=old_room_pages((directory/f'{room_prefix}visual.bin').read_bytes(),depth)
        size=sum(texture_bytes(directory/f'{room_prefix}page-{i}.t3x') for i in pages)
        if size>constants['oldRoomTextureBudget']:
            raise MemoryBudgetError(f'Old {scene}: {len(pages)} near pages exceed 16 MiB room cap')
        actors=set()
        # Old releases optional actors before each room load; New's sticky
        # atlas policy is unchanged. Charge Greenpath only with its own room.
        manifest=(_read_json(converted/'scenes'/scene/'manifest.json')
                  if scene!='Tutorial_01' else {})
        actors.update(old_scene_actor_paths(scene,manifest.get('enemy_spawns',[]),romfs,
                                             directory/'greenpath-enemies.bin'))
        size+=sum(texture_bytes(p) for p in actors-core)
        if size>worst:worst=size;worst_pages=len(pages)+len(actors-core)
    pcm=_base_pcm_assets(romfs,root/'src/audio_player.cpp')+constants['oldEnemyAudioBankCapBytes']
    fixed=(constants['gpuCommandBufferBytes']-256*1024+constants['windStreamBufferBytes']
           +constants['openingStreamBufferBytes']+constants['secretStreamBufferBytes'])
    free=constants['oldLinearHeapBytes']-core_bytes-worst-pcm-fixed-CALIBRATED_RUNTIME_LINEAR_BYTES
    reserve=2*MIB # RoomRenderer admission reserve (room_visual.hpp).
    if free<reserve:raise MemoryBudgetError(f'Old linear estimate leaves {free} B; reserve is {reserve} B')
    usage=PackageUsage(core_bytes+worst,len(core)+worst_pages,pcm,constants['oldEnemyAudioBankCapBytes'],0,0,0,0,0,0,0,0)
    # Ordinary value is an upper bound before executable/static/runtime allocations.
    return BudgetResult(usage,constants,fixed,free,reserve,64*MIB-constants['oldLinearHeapBytes'])


def audit_package(converted: Path | None = None, romfs: Path | None = None, root: Path = ROOT,
                  scene_list: str | list[str] | None = None, *, scenes: list[str] | None = None) -> BudgetResult:
    if isinstance(scene_list, list):
        if scenes is not None:
            raise MemoryBudgetError('Specify only one scene scope')
        scenes = scene_list
        scene_list = None
    if scenes is not None:
        if scene_list is not None:
            raise MemoryBudgetError('Specify only one scene scope')
        scene_list = ','.join(scenes)
    if scene_list is not None:
        parse_scene_list(scene_list)
    converted = converted or root / "_private/converted"
    romfs = romfs or converted / "hero/romfs"
    usage, constants = read_package_usage(converted, romfs, root)
    result=evaluate_budget(usage, constants)
    check_scene_texture_scale([_read_json(converted/'room-plants/manifest.json'),
                               _read_json(converted/'room-resident/manifest.json')],constants['roomTextureBudget'])
    largest=audit_scene_textures(converted,romfs,constants,scene_list)
    tutorial=_read_json(converted/'room-resident/manifest.json')['texture_bytes']
    # Charge the full admitted enemy PCM cap and retain the stricter room reserve.
    scene_free=result.linear_free_bytes-max(0,largest-tutorial)-(constants['enemyAudioBankCapBytes']-usage.enemy_pcm_bytes)
    if scene_free<2*MIB:
        raise MemoryBudgetError(f'Worst room/actor/audio-cap estimate leaves {scene_free} B; room reserve is {2*MIB} B')
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converted", type=Path, default=ROOT / "_private/converted")
    parser.add_argument("--romfs", type=Path, default=ROOT / "_private/converted/hero/romfs")
    parser.add_argument("--profile", choices=("new","old"), default="new")
    parser.add_argument("--scenes", help="Explicit comma-separated diagnostic replay rooms; core assets are always audited")
    parser.add_argument("--room-page-limit", action="store_true", help="Print the compiled New room limit without scanning resources")
    args = parser.parse_args()
    if args.room_page_limit:
        print(_cpp_constants(ROOT / 'include/render_memory.hpp')['roomTextureBudget']//(2*MIB))
        return 0
    audit_label = 'Diagnostic scope memory budget' if args.scenes is not None else 'Memory budget'
    try:
        if args.scenes is not None:
            scenes = parse_scene_list(args.scenes)
            print('Diagnostic scope memory audit: ' + ','.join(scenes) + ' (plus all core assets)')
        result = (audit_old_package(args.converted,args.romfs,ROOT,args.scenes) if args.profile=="old"
                  else audit_package(args.converted, args.romfs, scene_list=args.scenes))
    except MemoryBudgetError as exc:
        print(f"{audit_label} FAILED: {exc}")
        return 1

    usage = result.usage
    if args.profile=='old':print('Old 3DS: native bounded audio, no effects/lighting; ordinary heap shown below is a pre-executable upper bound')
    print(
        f"{audit_label} PASS: "
        f"textures={usage.texture_bytes} B/{usage.texture_pages} pages, "
        f"PCM={usage.pcm_bytes} B (enemy={usage.enemy_pcm_bytes} B), "
        f"linear target={result.fixed_linear_bytes} B, "
        f"estimated linear free={result.linear_free_bytes} B "
        f"(reserve={result.linear_reserve_bytes} B), "
        f"ordinary heap after split={result.ordinary_heap_bytes / MIB:.2f} MiB; "
        f"fixed pools decor={usage.decor_pool_bound}/{usage.decor_pool_capacity}, "
        f"ambient={usage.ambient_pool_capacity}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
