"""Restore missing C# exports from the hash-verified original build22529139 DLL.

Run under validation-lock.ps1 -Exclusive: research is shared across worktrees.
The player is never executed; ILSpy reads the copied assembly and its references.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from extract_crawler import write_asset

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA256 = 'e9048ef6a633970f735e01ec166d3959f610eaea7a88d827d48d67b1e5fb87bd'
# Independent saved exports from the same original build bind live collection
# identities to preserved research, rather than trusting a port or asset name.
ASSET_ANCHORS = {
    'resources.assets': (487, 'dfa73b281d0191f4cf1f75627ac8368e1a0b10f9eef54d9b17e53705bb56d6bf'),
    'level6': (12037, '7bcf761f46a8084fa942b132a92bbf9a6d863e91de8d5e7f32d5c7982459faaa'),
    'sharedassets6.assets': (1150, 'c2f3a037db819476e2cec684bb375370413a45b03739ef35150660d25c4a1c23'),
}
GROUPS = {
    'uumuu': ('MegaJellyZap', 'GGCheckIfBossScene', 'HutongGames.PlayMaker.Actions.TransitionToAudioSnapshot'),
    'great-door': ('HutongGames.PlayMaker.Actions.ReceivedDamage', 'HutongGames.PlayMaker.Actions.BeginSceneTransition', 'PersistentBoolItem'),
    'room-entities': ('StalactiteControl', 'LineOfSightDetector', 'AlertRange', 'Recoil',
                      'HutongGames.PlayMaker.Actions.ChaseObject', 'HutongGames.PlayMaker.Actions.IdleBuzz',
                      'HutongGames.PlayMaker.Actions.Decelerate'),
    'geo': ('HealthManager', 'GeoControl', 'FlingUtils', 'ObjectBounce', 'EnemyDeathEffects'),
    'opening-blanker': ('HeroController',),
}
# Only these exact outputs of this task's earlier probe recovery may be replaced.
# A writable sidecar manifest cannot authorize overwriting another existing file.
PREVIOUS_SHA256 = {
    'all/MegaJellyZap.cs': '15916f01a03c870d52673a84f965336e772fd488e585aebceaa214339883e1bb',
    'all/GGCheckIfBossScene.cs': '569b53122f283ae31f4c8d3be2355578fe9f914915c3ee2d4fbb7e7c14de4b3c',
    'all/HutongGames.PlayMaker.Actions/TransitionToAudioSnapshot.cs': '1bf6c84edc8f39e1c2b5f53db7a2d2550ca5acd424ea323dc8fd998d4a8c6025',
    'all/HutongGames.PlayMaker.Actions/ReceivedDamage.cs': 'b7e5adc28d8afbed5715a8aec3139ba9eaa8f472b9a92300babf6fd9c45f0f78',
    'all/HutongGames.PlayMaker.Actions/BeginSceneTransition.cs': '296c236969d885f1c2706161003b282007cee321ffaa2469ea5a1f09f33ce7b6',
    'all/PersistentBoolItem.cs': '6695a0c95ba1ce7174d9e12a0ba65deb694671847fb747cfc92b34602553a306',
}


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def verify_collection(client, collection, name):
    ident, expected = ASSET_ANCHORS[name]
    if digest(client.asset({'C': collection, 'D': ident})) != expected:
        raise ValueError('Live collection differs from preserved original: ' + name)


def admit_source(name, raw):
    text = raw.decode('utf-8-sig')
    if any(mark in text for mark in ('HKMemoryPrefs', 'HK3DSPhysicsProbe', 'ICSharpCode.Decompiler.', 'Failed to resolve')):
        raise ValueError('Instrumented or incomplete source: ' + name)
    if 'class ' + name.rsplit('.', 1)[-1] not in text:
        raise ValueError('Missing decompiled class: ' + name)


def publish(output, group, files, manifest):
    manifest_path = output / ('restored-' + group + '.json')
    if manifest_path.exists() and json.loads(manifest_path.read_bytes())['original_sha256'] != ORIGINAL_SHA256:
        raise ValueError('Existing recovery belongs to a different source')
    files = dict(files)
    files[manifest_path] = (json.dumps(manifest, indent=2) + '\n').encode()
    for path, raw in files.items():
        if path.exists() and path.read_bytes() != raw:
            expected = PREVIOUS_SHA256.get(path.relative_to(output).as_posix())
            if digest(path.read_bytes()) != expected:
                raise ValueError('Refusing different existing source: ' + str(path))
    for path, raw in files.items():
        if not path.exists() or path.read_bytes() != raw:
            write_asset(path, raw)


def restore(root, group, assembly=None, stage=False):
    root = Path(root)
    assembly = Path(assembly) if assembly else root / '_private/converted/research-input/hollow_knight_Data/Managed/Assembly-CSharp.dll'
    if digest(assembly.read_bytes()) != ORIGINAL_SHA256:
        raise ValueError('Original assembly SHA-256 mismatch')
    output = root / ('_private/converted/research-managed' if stage else '_private/research/decompiled')
    files = {}
    for name in GROUPS[group]:
        raw = subprocess.run([str(root / '_private/tools/ilspy/ilspycmd.exe'), '-r', str(assembly.parent),
                              '-t', name, str(assembly)], check=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=180).stdout
        admit_source(name, raw)
        namespace, _, short = name.rpartition('.')
        files[output / 'all' / namespace / (short + '.cs')] = raw
    manifest = dict(build=22529139, original_sha256=ORIGINAL_SHA256,
                    source_sha256={p.relative_to(output).as_posix(): digest(v) for p, v in files.items()})
    publish(output, group, files, manifest)
    print(group + ': restored ' + str(len(files)) + ' original types; assembly SHA-256 verified; no source transformations')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', choices=GROUPS, required=True)
    parser.add_argument('--assembly', type=Path)
    parser.add_argument('--stage', action='store_true', help='Prepare in own converted directory under -Write before publishing')
    args = parser.parse_args()
    restore(ROOT, args.only, args.assembly, args.stage)
