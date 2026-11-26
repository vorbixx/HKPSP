"""Convert Tutorial_01 enemy and grass clips from preserved local OGG exports.

Normal conversion is offline. ``--extract`` asks the local AssetRipper API to
export any missing OGGs from the copied Unity assets, never the Steam install.
"""
import converted_files  # Install hardlink copy-on-write before asset writes.
import argparse
import hashlib
import json
import struct
import subprocess
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "_private" / "converted" / "hero" / "romfs"
SOURCE = ROOT / "_private" / "converted" / "enemy" / "source"
FFMPEG = ROOT / "_private" / "tools" / "ffmpeg-7.1.1" / "ffmpeg-7.1.1-full_build-shared" / "bin" / "ffmpeg.exe"
CAP_BYTES = 15 * 1024 * 1024  # resident PCM; matches runtime and retains the 1 MiB linear reserve
STREAMED_CLIPS = {"S75 Opening Sting-08", "break_wall_after_tutorial_area", "secret_discovered_temp", "hero_death_extra_details"}

CLIPS = (
    # collection, path ID, name, PCM bytes, authored gain, pitch range, role
    ("resources.assets", 1248, "enemy_damage", 26562, 1.0, (0.75, 1.25), "injury and infected death damage layer"),
    ("resources.assets", 1308, "enemy_death_sword", 86982, 1.0, (0.75, 1.25), "infected death sword layer"),
    ("sharedassets6.assets", 129, "crawler", 333286, 1.0, (1.0, 1.0), "crawler loop"),
    ("sharedassets6.assets", 151, "buzzer_fly", 701532, 0.8, (1.0, 1.25), "buzzer flight loop; pitch 1 in Idle/Stop, 1.25 in Chase"),
    ("sharedassets6.assets", 178, "buzzer_startle_01", 52922, 1.0, (1.2, 1.4), "weighted buzzer startle candidate"),
    ("sharedassets6.assets", 172, "buzzer_startle_02", 42338, 1.0, (1.2, 1.4), "weighted buzzer startle candidate"),
    ("sharedassets6.assets", 182, "buzzer_startle_03", 35282, 1.0, (1.2, 1.4), "weighted buzzer startle candidate"),
    ("sharedassets6.assets", 166, "splat_01", 56450, 1.0, (1.0, 1.0), "buzzer corpse first eligible ground landing candidate"),
    ("sharedassets6.assets", 152, "Splat_02", 59978, 1.0, (1.0, 1.0), "buzzer corpse first eligible ground landing candidate"),
    ("sharedassets6.assets", 117, "Splat_03", 49394, 1.0, (1.0, 1.0), "buzzer corpse first eligible ground landing candidate"),
    ("sharedassets6.assets", 89, "grass_move_6", 135488, 1.0, (1.0, 1.0), "grass whole movement"),
    ("sharedassets6.assets", 137, "grass_move_2", 167476, 1.0, (1.0, 1.0), "grass whole movement"),
    ("sharedassets6.assets", 132, "grass_move_5", 103492, 1.0, (1.0, 1.0), "grass whole movement"),
    ("sharedassets6.assets", 119, "grass_move_1", 172852, 1.0, (1.0, 1.0), "grass whole movement"),
    ("sharedassets6.assets", 147, "grass_move_3", 145136, 1.0, (1.0, 1.0), "grass whole movement"),
    ("sharedassets6.assets", 124, "grass_move_4", 198856, 1.0, (1.0, 1.0), "grass whole movement"),
    ("sharedassets6.assets", 185, "props_grass_stud_move_1", 16972, 1.0, (1.0, 1.0), "grass stub movement"),
    ("sharedassets6.assets", 118, "props_grass_stud_move_4", 15356, 1.0, (1.0, 1.0), "grass stub movement"),
    ("sharedassets6.assets", 148, "props_grass_stud_move_5", 20558, 1.0, (1.0, 1.0), "grass stub movement"),
    ("sharedassets6.assets", 111, "props_grass_stud_move_3", 18194, 1.0, (1.0, 1.0), "grass stub movement"),
    ("sharedassets6.assets", 121, "props_grass_stud_move_2", 15140, 1.0, (1.0, 1.0), "grass stub movement"),
    ("sharedassets6.assets", 94, "grass_cut_4", 27592, 1.0, (1.0, 1.0), "grass cut"),
    ("sharedassets6.assets", 139, "grass_cut_2", 31656, 1.0, (1.0, 1.0), "grass cut"),
    ("sharedassets6.assets", 174, "grass_cut_3", 34710, 1.0, (1.0, 1.0), "grass cut"),
    ("sharedassets6.assets", 143, "grass_cut_5", 47360, 1.0, (1.0, 1.0), "grass cut"),
    ("sharedassets6.assets", 146, "health_cocoon_move", 460800, 1.0, (1.0, 1.0), "Health Cocoon sweat cue"),
    ("resources.assets", 1359, "health_cocoon_break", 207796, 1.0, (1.0, 1.0), "Health Cocoon break cue"),
    ("sharedassets6.assets", 92, "breakable_wall_hit_1", 39820, 1.0, (0.85, 1.15), "Great Door accepted nail hit variant 1"),
    ("sharedassets6.assets", 99, "breakable_wall_hit_2", 70074, 1.0, (0.85, 1.15), "Great Door accepted nail hit variant 2"),
    ("sharedassets6.assets", 102, "breakable_wall_death", 233600, 1.0, (0.85, 1.15), "Great Door stage break; inherits the accepted hit pitch"),
    ("sharedassets6.assets", 161, "stalactite_start", 206488, 1.0, (1.0, 1.0), "Stalactite Fall startFallSound: stalactite_break"),
    ("sharedassets6.assets", 95, "stalactite_hit", 117012, 1.0, (1.0, 1.0), "Stalactite nail deflection: stalactite_impact"),
    ("sharedassets6.assets", 112, "stalactite_break", 74808, 1.0, (0.9, 1.1), "Stalactite upward break and embedded stal_death table"),
    ("resources.assets", 1254, "hollow_shade_startle", 146368, 1.0, (1.0, 1.0), "Shade startle"),
    ("resources.assets", 1148, "hollow_shade_fly", 741158, 1.0, (1.0, 1.0), "Shade flight loop"),
    ("resources.assets", 1210, "hollow_shade_sword", 231810, 1.15, (0.85, 1.0), "Shade slash anticipation"),
    ("resources.assets", 1219, "boss_explode_clean", 305566, 1.0, (1.0, 1.0), "Shade recovery explosion"),
    ("resources.assets", 1296, "hero_void_scream_spell", 284450, 1.0, (1.0, 1.0), "Shade recovery scream"),
    ("sharedassets6.assets", 158, "chest_open", 119908, 1.0, (1.0, 1.0), "Fury chest open"),
    ("sharedassets6.assets", 141, "barrel_death_2", 85336, 1.0, (1.0, 1.0), "Fury chest open break layer"),
    ("sharedassets6.assets", 150, "shiny_item_loop", 398946, 1.0, (1.0, 1.0), "Fury shiny idle loop"),
    ("resources.assets", 1337, "shiny_item_pickup", 121438, 1.0, (1.0, 1.0), "Fury charm acquisition"),
    ("sharedassets6.assets", 114, "tablet_hum", 410880, 0.8740000129, (1.0, 1.0), "tablet Awake hum; proximity gain fade"),
    ("sharedassets6.assets", 159, "tablet_wake", 311124, 1.0, (1.0, 1.0), "tablet Close entry accent"),
    ("sharedassets6.assets",183,"water_drip_1",17450,1.0,(1.0,1.0),"WaterDrip impact candidate"),
    ("sharedassets6.assets",136,"water_drip_2",29662,1.0,(1.0,1.0),"WaterDrip impact candidate"),
    ("sharedassets6.assets",109,"water_drip_3",11274,1.0,(1.0,1.0),"WaterDrip impact candidate"),
    ("sharedassets6.assets",175,"water_drip_4",17450,1.0,(1.0,1.0),"WaterDrip impact candidate"),
    ("sharedassets6.assets",170,"platform_dirt_fall_1",121544,1.0,(1.0,1.0),"DustyPlatform top-contact candidate"),
    ("sharedassets6.assets",164,"platform_dirt_fall_2",139946,1.0,(1.0,1.0),"DustyPlatform top-contact candidate"),
    ("sharedassets6.assets",100,"platform_dirt_fall_3",119560,1.0,(1.0,1.0),"DustyPlatform top-contact candidate"),
    ("sharedassets6.assets", 120, "health_bug_wall_hit", 31144, 0.5, (0.9, 1.1), "Health Scuttler wall bounce; pooled source"),
    ("sharedassets6.assets", 96, "tablet_inspect", 322560, 1.0, (1.0, 1.0), "tablet inspection accent"),
)

# Source event FSM audio actions (sharedassets6, build 22529139).
CLIPS += (('sharedassets6.assets',
  108,
  'Mr_Mush_talk_02',
  285770,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  168,
  'mr_mush_takeoff',
  922308,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  88,
  'mage_knight_teleport',
  89740,
  1.0,
  (1.25, 1.25),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  116,
  'flamebearer_dash_prepare',
  49884,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  135,
  'flamebearer_dash_dash',
  124976,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  126,
  'flamebearer_teleport_out',
  179780,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  153,
  'flamebearer_death_and_absorb',
  824968,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  130,
  'flamebearer_shoot',
  146488,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  165,
  'flamebearer_fire_spiral_prepare',
  79196,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  101,
  'flamebearer_fire_spiral_cast',
  174562,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  177,
  'accordion_sting_04',
  647560,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  167,
  'nightmare_lantern_powering_up',
  348520,
  1.0,
  (1.0, 1.0),
  'Tutorial conditional event'),
 ('sharedassets6.assets',
  144,
  'nightmare_lantern_flame_burst',
  531368,
  0.75,
  (1.0, 1.0),
  'Tutorial conditional event'))

# Death cues append after all stable main IDs.
CLIPS += (
    ('resources.assets', 1293, 'hero_death_v2', 750504, 1.0, (1.0, 1.0), 'hero death Start'),
    ('resources.assets', 1267, 'hero_damage', 292068, 1.0, (1.0, 1.0), 'hero death Start damage layer'),
    ('resources.assets', 1280, 'hero_death_extra_details', 981184, 1.0, (1.0, 1.0), 'hero death Initiate'),
)

# Opening cues append after every stable main sound ID.
CLIPS += (
    ("sharedassets6.assets", 171, "false_knight_land_1st_time", 290340, 1.0, (1.0, 1.0), "initial Land entry"),
    ("sharedassets6.assets", 169, "pigeon_fly_away_3", 201272, 1.0, (1.0, 1.0), "initial Flock entry"),
    ("sharedassets6.assets", 104, "pigeon_fly_away_2", 120586, 1.0, (1.0, 1.0), "initial Flock .75 second delayed bird"),
    ("sharedassets6.assets", 125, "S75 Opening Sting-08", 2642012, 1.0, (1.0, 1.0), "initial Flock opening sting"),
    ("resources.assets", 1278, "sword_hit_reject", 23128, 1.0, (.85, 1.15), "Fury chest and standalone spike TinkEffect"),
)

# Append: stable IDs; source Collapser Break layers reuse 102 and 171.
CLIPS += (("sharedassets6.assets", 97, "break_wall_after_tutorial_area", 1175500, 1.0, (1.0, 1.0), "Collapser Break long layer; streamed"),)

COLLECTIONS = {
    "resources.assets": {
        "asset_sha256": "6d49ac5a50a282a645acc32518b28e7a29d0b94a0c3b5a4cda18dade3e6fe511",
        "resource": "resources.resource",
    },
    "sharedassets6.assets": {
        "asset_sha256": "346324bc1e4c8aa44bb5f2f6d72ea3234dbfea6b1a6d106531a34fa314e42df8",
        "resource": "sharedassets6.resource",
    },
    "sharedassets7.assets": {
        "asset_sha256": "3c9cde7a84b9bc6d5375da9d68d727f524ac834712350443f639981f38d92919",
        "resource": "sharedassets7.resource",
    },
}

# Verified AudioClip m_Resource offsets and compressed FSB payload sizes from
# asset-target-metadata.json. All clips are mono 44100 Hz PCM16 after decode.
PAYLOADS = {
    ("sharedassets6.assets", 157): (2889632, 86144),
    ("sharedassets6.assets", 97): (104608, 208192),
    ('resources.assets', 1278): (9413728, 2752),
    ('resources.assets', 1293): (10380320, 57824),
    ('resources.assets', 1267): (8723424, 49248),
    ('resources.assets', 1280): (9433888, 119072),
    ("sharedassets6.assets", 120): (1411936, 7328),
    ("resources.assets", 1254): (8287136, 14272),
    ("resources.assets", 1148): (33344, 72192),
    ("resources.assets", 1210): (4160544, 21088),
    ("resources.assets", 1219): (4350720, 28800),
    ("resources.assets", 1296): (10527616, 27200),
    ("sharedassets6.assets", 158): (2975776, 11808),
    ("sharedassets6.assets", 141): (2336736, 8576),
    ("sharedassets6.assets", 150): (2651168, 29824),
    ("resources.assets", 1337): (11923104, 13152),
    ("resources.assets", 1248): (8231712, 3552),
    ("resources.assets", 1308): (10706848, 11232),
    ("sharedassets6.assets", 129): (1996800, 51456),
    ("sharedassets6.assets", 151): (2680992, 63264),
    ("sharedassets6.assets", 178): (3604576, 5632),
    ("sharedassets6.assets", 172): (3418656, 4736),
    ("sharedassets6.assets", 182): (3656672, 3808),
    ("sharedassets6.assets", 166): (3185280, 7456),
    ("sharedassets6.assets", 152): (2744256, 7360),
    ("sharedassets6.assets", 117): (1383328, 5408),
    ("sharedassets6.assets", 89): (9600, 14848),
    ("sharedassets6.assets", 137): (2289408, 22560),
    ("sharedassets6.assets", 132): (2241856, 11392),
    ("sharedassets6.assets", 119): (1390400, 21536),
    ("sharedassets6.assets", 147): (2615968, 17440),
    ("sharedassets6.assets", 124): (1545024, 25248),
    ("sharedassets6.assets", 185): (3675424, 1664),
    ("sharedassets6.assets", 118): (1388736, 1664),
    ("sharedassets6.assets", 148): (2633408, 2112),
    ("sharedassets6.assets", 111): (1309248, 1952),
    ("sharedassets6.assets", 121): (1419264, 1696),
    ("sharedassets6.assets", 94): (50368, 3168),
    ("sharedassets6.assets", 139): (2323968, 3616),
    ("sharedassets6.assets", 174): (3439776, 4096),
    ("sharedassets6.assets", 143): (2353024, 5216),
    ("sharedassets6.assets", 146): (2539392, 76576),
    ("resources.assets", 1359): (13440224, 53312),
    ("sharedassets6.assets", 92): (41056, 4960),
    ("sharedassets6.assets", 99): (320704, 7904),
    ("sharedassets6.assets", 102): (358016, 24512),
    ("sharedassets6.assets", 161): (3106112, 41280),
    ("sharedassets6.assets", 95): (53536, 20736),
    ("sharedassets6.assets", 112): (1311200, 16864),
    ("sharedassets6.assets", 114): (1333280, 40384),
    ("sharedassets6.assets", 159): (2987584, 29600),
    ("sharedassets6.assets", 171): (3360128, 58528),
    ("sharedassets6.assets", 169): (3326432, 21184),
    ("sharedassets6.assets", 104): (398176, 13216),
    ("sharedassets6.assets", 125): (1570272, 350208),
    ("sharedassets6.assets", 96): (74272, 30336),
    ("sharedassets6.assets", 183): (3660480, 2016),
    ("sharedassets6.assets", 136): (2285728, 3680),
    ("sharedassets6.assets", 109): (1272032, 1408),
    ("sharedassets6.assets", 175): (3443872, 1952),
    ("sharedassets6.assets", 170): (3347616, 12512),
    ("sharedassets6.assets", 164): (3162976, 14624),
    ("sharedassets6.assets", 100): (328608, 12352),
}

PAYLOADS.update({('sharedassets6.assets', 108): (1244352, 27680),
 ('sharedassets6.assets', 168): (3236384, 90048),
 ('sharedassets6.assets', 88): (0, 9600),
 ('sharedassets6.assets', 116): (1378528, 4800),
 ('sharedassets6.assets', 135): (2273376, 12352),
 ('sharedassets6.assets', 126): (1920480, 25824),
 ('sharedassets6.assets', 153): (2751616, 120320),
 ('sharedassets6.assets', 130): (2048256, 31936),
 ('sharedassets6.assets', 165): (3177600, 7680),
 ('sharedassets6.assets', 101): (340960, 17056),
 ('sharedassets6.assets', 177): (3513568, 91008),
 ('sharedassets6.assets', 167): (3192736, 43648),
 ('sharedassets6.assets', 144): (2358240, 74976)})


# TinkEffect -> Block Hit v2 AudioSource13938, copied build22529139.
PAYLOADS[("resources.assets",1278)] = (9413728,2752)
# HeroAudioController.DASH -> source13891, clip1186, copied build22529139.
CLIPS += (("resources.assets",1186,"hero_dash",23424,1.0,(1.0,1.0),"accepted hero dash; source13891"),)
PAYLOADS[("resources.assets",1186)] = (2860704,2496)

# Keep the shared dash ID stable when appending discovery playback.
CLIPS += (("sharedassets6.assets", 157, "secret_discovered_temp", 710872, 1.0, (1.0, 1.0), "Secret Sound Region discovery; streamed"),)
CLIPS += (("resources.assets",1361,"hero_fireball",740148,1.0,(1.0,1.0),"Vengeful Spirit cast"),)
PAYLOADS[("resources.assets",1361)] = (14438976,67392)
CLIPS += (("sharedassets7.assets",106,"switch_gate_switch",117350,1.0,(1.0,1.0),"Town Mines Lever FSM3919 accepted Hit"),)
PAYLOADS[("sharedassets7.assets",106)] = (2913760,11648)
TOWN_LEVER_SOURCE = ROOT / '_private/minimum-obstacles/town-lever-feedback/lever-hit.ogg'
TOWN_LEVER_SHA256 = '5fb57181dd92e0443b6e85ed6b45c6b4c9d0512a0c4862c48ad8215599213dfb'

# Break Floor 1 FSM12191 Hit 1/2/Break: native barrel_death_1, fixed pitch 1.
CLIPS += (("sharedassets6.assets",107,"barrel_death_1",73016,1.0,(1.0,1.0),"Tutorial break floor accepted hit"),)
PAYLOADS[("sharedassets6.assets",107)] = (1236352,8000)

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def validate_manifest_outputs(manifest, output_dir):
    """Reject a manifest that names a clip without its generated PCM WAV."""
    missing = [entry["name"] for entry in manifest.get("clips", ())
               if not (output_dir / entry["output"]).is_file()]
    if missing:
        raise ValueError("Manifest clips missing source WAV: " + ", ".join(missing))


def wav_bytes(pcm, rate, channels=1):
    import io
    with io.BytesIO() as buffer:
        with wave.open(buffer, "wb") as out:
            out.setnchannels(channels)
            out.setsampwidth(2)
            out.setframerate(rate)
            out.writeframes(pcm)
        return buffer.getvalue()


def extract_sources(source_dir=SOURCE, port=58291, *, client=None):
    """Export missing OGGs from the local AssetRipper session, keyed by exact source ID."""
    import sys
    sys.path.insert(0, str((ROOT / "tools").resolve()))
    from assetripper_client import Client
    source_dir.mkdir(parents=True, exist_ok=True)
    client = client or Client(port)
    exported = []
    for collection, path_id, name, *_ in CLIPS:
        path = source_dir / f"{collection.replace('.', '-')}-{path_id}-{name}.ogg"
        if path.exists():
            continue
        source_name = {"stalactite_start":"stalactite_break","stalactite_hit":"stalactite_impact","stalactite_break":"stalactite_death",'tablet_hum':'Lore_tablet_idle_audio_temp','tablet_wake':'soul_totem_awake','tablet_inspect':'Lore_Tablet_activate_temp'}.get(name,name)
        rows = [row for row in client.search(source_name)
                if row["type"] == "AudioClip" and row["collection"] == collection
                and row["path"]["D"] == path_id]
        if len(rows) != 1:
            raise ValueError(f"Expected exact AudioClip {collection}:{path_id} {name}, got {len(rows)}")
        path.write_bytes(client.request("/Assets/Audio", Path=json.dumps(rows[0]["path"], separators=(",", ":"))))
        exported.append(path.name)
    return exported


def convert(source_dir=SOURCE, output_dir=OUTPUT, decoder=FFMPEG):
    if decoder == FFMPEG and not decoder.is_file():
        import shutil
        decoder = shutil.which('ffmpeg') or decoder
    prepared = []
    total_pcm_bytes = 0
    streamed_pcm_bytes = 0
    for collection, path_id, name, expected_bytes, gain, pitch, role in CLIPS:
        offset, size = PAYLOADS[(collection, path_id)]
        source_path = source_dir / f"{collection.replace('.', '-')}-{path_id}-{name}.ogg"
        if name=='switch_gate_switch' and not source_path.exists():source_path=TOWN_LEVER_SOURCE
        if not source_path.exists():
            raise FileNotFoundError(f"Missing extracted source {source_path}; run with --extract first")
        encoded = source_path.read_bytes()
        if name=='switch_gate_switch' and sha256(encoded)!=TOWN_LEVER_SHA256:
            raise ValueError('Changed original Town lever OGG export')
        if not encoded.startswith(b"OggS"):
            raise ValueError(f"Invalid OGG payload for {collection}:{path_id} {name}")
        # Refuse an unexpected native format before the explicitly configured decoder.
        # Keep each clip's native channel count; this must never silently downmix/resample.
        identification = encoded.find(b"\x01vorbis", 0, 512)
        if identification < 0 or len(encoded) < identification + 16:
            raise ValueError(f"Unexpected native Vorbis format for {name}; quality change requires review")
        channels = encoded[identification+11]
        rate = struct.unpack_from("<I", encoded, identification+12)[0]
        expected_rate = 48000 if name in ('tablet_hum','tablet_inspect') else 44100
        if channels not in (1, 2) or rate != expected_rate or (name in STREAMED_CLIPS and channels != 2):
            raise ValueError(f"Unexpected native Vorbis format for {name}: {channels} channels at {rate} Hz; quality change requires review")
        # approximation: keep the original 8.39 s spell tail at 22.05 kHz.
        # Native 44.1 kHz exceeds the fixed 15 MiB resident bank by 71,050 B.
        if name == 'hero_fireball':rate=22050
        decoded = subprocess.run(
            [str(decoder), "-v", "error", "-i", "pipe:0", "-f", "s16le",
             "-acodec", "pcm_s16le", "-ac", str(channels), "-ar", str(rate), "pipe:1"],
            input=encoded, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if decoded.returncode:
            raise ValueError(f"Vorbis decode failed for {name}: {decoded.stderr.decode(errors='replace')}")
        pcm = decoded.stdout
        pcm_size = len(pcm)
        wav = wav_bytes(pcm, rate, channels)
        total_pcm_bytes += pcm_size
        if name in STREAMED_CLIPS:
            streamed_pcm_bytes += pcm_size
        prepared.append((collection, path_id, name, expected_bytes, pcm_size, gain, pitch, role,
                         offset, size, encoded, channels, rate, wav))

    resident_pcm_bytes = total_pcm_bytes - streamed_pcm_bytes
    if resident_pcm_bytes > CAP_BYTES:
        raise ValueError(f"Enemy/grass PCM bank exceeds runtime cap: {resident_pcm_bytes} > {CAP_BYTES}")

    output_dir.mkdir(parents=True, exist_ok=True)
    entries = []
    for (collection, path_id, name, estimated_pcm_bytes, pcm_size, gain, pitch, role, offset, size,
         encoded, clip_channels, rate, wav) in prepared:
        category = "grass" if name.startswith("grass_") or name.startswith("props_grass_") else "enemy"
        filename = f"sfx-{category}-{name.replace(' ', '_')}.wav"
        # Make's RomFS prerequisite list cannot represent the original sting's spaces.
        old_filename = f"sfx-{category}-{name}.wav"
        if old_filename != filename:
            (output_dir / old_filename).unlink(missing_ok=True)
        (output_dir / filename).write_bytes(wav)
        entries.append({
            "source_id": f"{collection}:{path_id}", "name": name,
            "unity_resource_file": COLLECTIONS[collection]["resource"],
            "unity_resource_offset": offset, "unity_resource_size": size,
            "exported_ogg_bytes": len(encoded),
            "source_ogg_sha256": sha256(encoded), "output": filename,
            "output_wav_sha256": sha256(wav), "channels": clip_channels,
            "sample_rate": rate, "pcm16_bytes": pcm_size,
            "metadata_pcm16_estimate_bytes": estimated_pcm_bytes,
            "streamed": name in STREAMED_CLIPS,
            "gain": gain, "pitch": list(pitch), "role": role,
        })
    manifest = {
        "format": "PCM16 WAV; native channels and rates except hero_fireball at 22050 Hz",
        "source_asset_collection_sha256": {k: v["asset_sha256"] for k, v in COLLECTIONS.items()},
        "source_payloads_are": "AssetRipper AudioClip exports from the copied Unity assets, preserved as OGG in _private/converted/enemy/source",
        "pcm_resident_budget_bytes": resident_pcm_bytes,
        "pcm_streamed_bytes": streamed_pcm_bytes,
        "pcm_cap_bytes": CAP_BYTES,
        "cap_excludes": ["allocator overhead", "voice state", "temporary decoder memory"],
        "clips": entries,
    }
    validate_manifest_outputs(manifest, output_dir)
    (output_dir / "enemy-audio-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--decoder", type=Path, default=FFMPEG)
    parser.add_argument("--extract", action="store_true", help="export any missing source OGGs from local AssetRipper")
    parser.add_argument("--port", type=int, default=58291)
    args = parser.parse_args()
    if args.extract:
        extract_sources(args.source, args.port)
    manifest = convert(args.source, args.output, args.decoder)
    print(json.dumps({"clips": len(manifest["clips"]),
                      "pcm_resident_budget_bytes": manifest["pcm_resident_budget_bytes"],
                      "output": str(args.output)}))


if __name__ == "__main__":
    main()
