"""Measure isolated bodies using Physics2D in a separate private source-player copy.

This is not an instrumented original Crawler FSM/Update run. Only the probe
copy is patched; the Steam installation and archived private original are read
only. Requires an installed .NET SDK and private ILSpy's Mono.Cecil.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path
from assetripper_client import ROOT


def dotnet_toolchain(root):
    def version(name):
        numeric = tuple(int(part) for part in name.split('-', 1)[0].split('.') if part.isdigit())
        return numeric, '-' not in name

    sdks = list((root / 'sdk').glob('*/Roslyn/bincore/csc.dll'))
    runtimes = [p for p in (root / 'shared/Microsoft.NETCore.App').iterdir() if p.is_dir()]
    references = list((root / 'packs/Microsoft.NETCore.App.Ref').glob('*/ref/net*'))
    compatible = [(ref, runtime) for ref in references for runtime in runtimes
                  if version(ref.name.removeprefix('net'))[0] == version(runtime.name)[0][:2]]
    if not sdks or not compatible:
        raise ValueError('A .NET SDK and matching reference pack/runtime are required')
    refs, runtime = max(compatible, key=lambda pair: (version(pair[0].parents[1].name), version(pair[1].name)))
    csc = max(sdks, key=lambda path: version(path.parents[2].name))
    return csc, refs, runtime.name


def digest(path):
    with path.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()


def dotnet_version(name):
    """Order SDK/reference/runtime versions numerically, including .NET 10+."""
    return tuple(int(part) for part in name.split('-')[0].split('.'))


def version_key(value):
    """Order either version paths or strings using the shared numeric key."""
    try:
        return dotnet_version(getattr(value, 'name', value))
    except ValueError:
        return ()


def latest_dotnet_version(root, *, major=None):
    versions = [path for path in Path(root).iterdir()
                if path.is_dir() and version_key(path)]
    if major is not None:
        versions = [path for path in versions if version_key(path)[0] == major]
    if not versions:
        raise RuntimeError(f'No installed .NET version under {root} matches major {major}')
    return max(versions, key=lambda path: (version_key(path), '-' not in path.name))


def private_roots():
    # Converted data has its own configured cold-storage junction per worktree.
    roots = [(ROOT / '_private').resolve(), (ROOT / '_private/converted').resolve()]
    git_entry = ROOT / '.git'
    if git_entry.is_file():
        entry = git_entry.read_text().strip()
        if not entry.startswith('gitdir:'):
            raise ValueError('Invalid worktree Git directory')
        git_dir = (ROOT / entry[len('gitdir:'):].strip()).resolve()
    elif git_entry.is_dir():
        git_dir = git_entry.resolve()
    else:
        return roots
    common_entry = git_dir / 'commondir'
    common_dir = ((git_dir / common_entry.read_text().strip()).resolve()
                  if common_entry.is_file() else git_dir)
    # A normal main checkout stores its common repository in <checkout>/.git.
    # Do not turn arbitrary per-entry junction targets into trusted roots.
    if common_dir.name == '.git':
        roots.append((common_dir.parent / '_private').resolve())
    return roots


def resolve_private_player_paths(original, output):
    original=original.absolute();output=output.absolute()
    allowed_roots=private_roots()
    archive=(ROOT / '_private/original/Hollow Knight').absolute()
    archive_root=(ROOT / '_private/original').resolve()
    for is_source,path in ((True,original),(False,output)):
        if '..' in path.parts:
            raise ValueError('Both player paths must be private')
        resolved=path.resolve()
        # The archive directory or configured player can be a cold-storage
        # junction. Archived inputs never authorize writable output roots.
        steam_path=any(part.casefold()=="steamapps" for part in resolved.parts)
        if is_source and steam_path:
            raise ValueError('Both player paths must be private')
        archive_input=is_source and (path==archive or
            (resolved != archive_root and resolved.is_relative_to(archive_root)))
        if not archive_input and not any(resolved != root and resolved.is_relative_to(root) for root in allowed_roots):
            raise ValueError('Both player paths must be private')
    original=original.resolve();output=output.resolve()
    if original==output or original.is_relative_to(output) or output.is_relative_to(original):
        raise ValueError('Probe and original must be separate directories')
    return original,output


def gameplay_shared_assets(source, graphics=False):
    # Global prefabs reference these indirectly. Missing grass materials can render
    # magenta without a player error, even when the room's direct assets exist.
    required = {f'sharedassets{index}{suffix}' for index in (13, 19)
                for suffix in ('.assets', '.assets.resS', '.resource')} if graphics else set()
    missing = sorted(name for name in required if not (source/name).is_file())
    if missing:
        raise ValueError('Missing original gameplay dependencies: ' + ', '.join(missing))
    return sorted(required | {p.name for p in source.iterdir()
                              if p.is_file() and p.name.startswith(('sharedassets1.', 'sharedassets4.', 'sharedassets6.'))})


def run(original, output, dotnet_root, template=None, gameplay=False, patch_template=None, extra_args=(), timeout=60, graphics=False, extra_libraries=(), offline=False, extra_assets=(), converted_inputs=True, cecil=None):
    if any(Path(name).name != name for name in extra_assets):
        raise ValueError('Extra probe assets must be basenames')
    if any(Path(name).name != name or not name.endswith('.dll') for name in extra_libraries):
        raise ValueError('Extra probe libraries must be DLL basenames')
    template=template or ROOT/"tools/unity_physics_probe.cs.txt"
    template=template.resolve()
    original_requested=original.absolute()
    original,output=resolve_private_player_paths(original,output)
    # prune_stale_player_copies(output)  # disabled by the orchestrator 2026-10-27: it stripped shared probe players (reverted in main 1d616a5)
    output.mkdir(parents=True,exist_ok=True)
    data=output/'hollow_knight_Data';data.mkdir(exist_ok=True)
    source=original/'hollow_knight_Data'
    # Exact selected assets plus native built-in resources. No hard links.
    for name in ('hollow_knight.exe','UnityPlayer.dll'):
        shutil.copyfile(original/name,output/name)
    shutil.copytree(original/'MonoBleedingEdge',output/'MonoBleedingEdge',dirs_exist_ok=True)
    for name in ('Managed','Resources','Plugins','StreamingAssets'):
        shutil.copytree(source/name,data/name,dirs_exist_ok=True)
    names=['app.info','boot.config','Config.ini','globalgamemanagers','globalgamemanagers.assets','globalgamemanagers.assets.resS',
           'ScriptingAssemblies.json','RuntimeInitializeOnLoads.json','resources.assets','resources.assets.resS','level0','sharedassets0.assets','sharedassets0.assets.resS','sharedassets0.resource']
    if gameplay:
        names += ["resources.resource","level1","level4","level6"]
        names += gameplay_shared_assets(source, graphics=graphics)
    names += list(extra_assets)
    names = list(dict.fromkeys(names))
    for name in names: shutil.copyfile(source/name,data/name)
    if gameplay or offline:
        (data/"Plugins/x86_64/steam_api64.dll").unlink(missing_ok=True)
    managed=data/'Managed';build=output/'patcher';build.mkdir(exist_ok=True)
    shutil.copyfile(template,output/'Probe.cs')
    patch=(patch_template or ROOT/('tools/unity_crawler_trace_patch.cs.txt' if gameplay else 'tools/unity_probe_patch.cs.txt')).resolve()
    shutil.copyfile(patch,build/'Patch.cs')
    csc,refs,runtime=dotnet_toolchain(dotnet_root)
    cecil=cecil or next((ROOT/'_private/tools/ilspy').rglob('Mono.Cecil.dll'))
    dotnet=dotnet_root/'dotnet.exe'
    def compile(args): subprocess.run([str(dotnet),str(csc),'-nologo',*map(str,args)],check=True,cwd=ROOT)
    compile(['-target:exe','-out:'+str(build/'patcher.dll'),*['-r:'+str(p) for p in refs.glob('*.dll')],'-r:'+str(cecil),'-r:'+str(cecil.with_name('Mono.Cecil.Rocks.dll')),build/'Patch.cs'])
    shutil.copyfile(cecil,build/'Mono.Cecil.dll')
    shutil.copyfile(cecil.with_name('Mono.Cecil.Rocks.dll'),build/'Mono.Cecil.Rocks.dll')
    (build/'patcher.runtimeconfig.json').write_text(json.dumps(dict(runtimeOptions=dict(tfm=refs.name,framework=dict(name='Microsoft.NETCore.App',version=runtime)))))
    libraries=['mscorlib.dll','netstandard.dll','System.dll','System.Core.dll','UnityEngine.CoreModule.dll','UnityEngine.Physics2DModule.dll']
    if gameplay: libraries=[p.name for p in (source/'Managed').glob('*.dll')]
    libraries=list(dict.fromkeys([*libraries,*extra_libraries]))
    compile(['-target:library','-nostdlib','-out:'+str(managed/'HK3DSPhysicsProbe.dll'),*['-r:'+str(managed/n) for n in libraries],output/'Probe.cs'])
    subprocess.run([str(dotnet),str(build/'patcher.dll'),str(managed)],check=True,cwd=ROOT)
    shutil.copyfile(managed/'Assembly-CSharp.patched.dll',managed/'Assembly-CSharp.dll')
    results=output/'results';results.mkdir(exist_ok=True)
    for name in ('complete.txt','error.txt','unity-body-trace.csv','unity-contact-trace.csv','crawler-events.csv','runtime-errors.txt','stage.txt','contact-gap.csv','solver-controls.csv','observer-preservation.json'):
        (results/name).unlink(missing_ok=True)
    if gameplay: shutil.copyfile(managed/'observer-preservation.json',results/'observer-preservation.json')
    pack=ROOT/'_private/converted/hero/romfs'
    with (output/'stdout.log').open('w') as stdout:
        graphics_args=['-force-d3d11','-screen-width','640','-screen-height','360','-screen-fullscreen','0'] if graphics else ['-nographics']
        mode_args=[] if graphics else ['-batchmode']
        process=subprocess.run([str(output/'hollow_knight.exe'),*mode_args,*graphics_args,'-logFile',str(output/'player.log'),
                                '-hk3ds-pack',str(pack),'-hk3ds-output',str(results),*map(str,extra_args)],cwd=output,stdout=stdout,stderr=subprocess.STDOUT,timeout=timeout)
    if process.returncode or not (results/'complete.txt').exists():
        raise RuntimeError('Unity probe failed; inspect private player.log/results/error.txt')
    inputs=[original/'hollow_knight.exe',original/'UnityPlayer.dll',source/'Managed/Assembly-CSharp.dll',source/'Managed/UnityEngine.Physics2DModule.dll',source/'globalgamemanagers',template,patch,ROOT/'tools/probe_unity_physics.py']
    # Original room views do not read the port's reconstructed physics banks.
    # Keep those provenance requirements for probes which actually use them.
    if converted_inputs:
        inputs += [pack/'tutorial.bin',pack/'crawler.bin']
    inputs += [source/name for name in names]
    inputs += [source/'Managed'/name for name in extra_libraries]
    if gameplay:
        inputs += list((source/'Managed').glob('*.dll'))
        inputs += [ROOT/'tools/probe_crawler_fsm.py',ROOT/'tools/analyze_crawler_traces.py']
    inputs=list(dict.fromkeys(inputs))
    def input_key(path):
        if path.is_relative_to(original):
            path=original_requested/path.relative_to(original)
        shared_private=(ROOT/'_private/original').resolve().parent
        if not path.is_relative_to(ROOT) and path.resolve().is_relative_to(shared_private):
            path=ROOT/'_private'/path.resolve().relative_to(shared_private)
        return str(path.relative_to(ROOT))
    report=dict(method=('source Tutorial_01, original gameplay callbacks; graphics enabled' if graphics else 'source Tutorial_01 serialized Crawler and Knight; original gameplay callbacks, paced headless' if gameplay else 'isolated reconstructed PhysicsScene2D; original player, no original FSM'),
                graphics=graphics,offline=gameplay or offline,player_arguments=mode_args+graphics_args+list(map(str,extra_args)),
                source_sha256={input_key(p):digest(p) for p in inputs},
                patched_sha256=digest(managed/'Assembly-CSharp.dll'),
                outputs={p.name:digest(p) for p in results.iterdir() if p.is_file() and p.name!='provenance.json'},exit_code=process.returncode)
    (results/'provenance.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(dict(result=str(results),exit_code=process.returncode)))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--original',type=Path,default=ROOT/'_private/original/Hollow Knight')
    p.add_argument('--output',type=Path,default=ROOT/'_private/physics-probe')
    p.add_argument('--dotnet-root',type=Path,default=Path('C:/Program Files/dotnet'))
    p.add_argument('--template',type=Path)
    a=p.parse_args();run(a.original,a.output,a.dotnet_root,a.template)
