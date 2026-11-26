"""Capture original Inventory and Charms UI states from a private player copy."""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import subprocess
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import capture_ui_audit as ui_audit  # noqa: E402
from capture_ui_audit import HELPERS, SETUP  # noqa: E402
import probe_unity_physics as unity_probe  # noqa: E402


BODY = r'''
  Screen.SetResolution(int.Parse(Arg("-hk3ds-width")),int.Parse(Arg("-hk3ds-height")),FullScreenMode.Windowed);
  for(int i=0;i<10;i++)yield return null;
  for(int i=0;i<780;i++)yield return null;
  logStage="runtime";
  var hero=HeroController.instance;
  hero.transform.position=new Vector3(75,60.6f,0);
  var body=hero.GetComponent<Rigidbody2D>();body.velocity=Vector2.zero;
  body.constraints=RigidbodyConstraints2D.FreezeAll;body.simulated=true;hero.enabled=false;
  var fsm=GameManager.instance.inventoryFSM;
  if(!fsm)throw new Exception("GameManager.inventoryFSM is unavailable");
  var inventory=fsm.gameObject;
  inventory.SetActive(true);
  File.AppendAllText(Path.Combine(output,"inventory-fsm.tsv"),"state-index\tstate\ttransitions\n");
  var states=fsm.Fsm.States;
  for(int index=0;index<states.Length;index++){
   var state=states[index];
   var transitions="";
   foreach(var field in state.GetType().GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic)){
    if(!field.Name.ToLowerInvariant().Contains("transition"))continue;
    var entries=field.GetValue(state) as Array;
    if(entries==null)continue;
    foreach(var entry in entries){
     if(entry==null)continue;
     var eventName="";var targetName="";
     foreach(var member in entry.GetType().GetFields(BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic)){
      var value=member.GetValue(entry);if(value==null)continue;
      if(member.Name.ToLowerInvariant().Contains("event"))eventName=value.ToString();
      if(member.Name.ToLowerInvariant().Contains("state"))targetName=value.ToString();
     }
     if(transitions.Length>0)transitions+=";";
     transitions+=eventName+" -> "+targetName;
    }
   }
   File.AppendAllText(Path.Combine(output,"inventory-fsm.tsv"),index+"\t"+state.Name+"\t"+transitions.Replace("\t"," ")+"\n");
  }
  var childFsms=inventory.GetComponentsInChildren<PlayMakerFSM>(true);
  using(var writer=new StreamWriter(Path.Combine(output,"inventory-components.tsv"))){
   writer.WriteLine("object\tfsm\tstates\trenderers");
   foreach(var child in childFsms){
    var names=String.Join(",",child.Fsm.States.Select(s=>s.Name));
    var renderers=String.Join(",",child.GetComponentsInChildren<Renderer>(true).Where(r=>r).Select(r=>r.name).Distinct().Take(50));
    writer.WriteLine(child.gameObject.name+"\t"+child.FsmName+"\t"+names+"\t"+renderers);
   }
  }
  // Enter through Open so the root FSM initializes children and pause timing.
  // SetIsInventoryOpen(true) alone pauses time before that setup can run.
  fsm.SetState("Open");
  for(int frame=0;frame<240;frame++)yield return null;
  yield return new WaitForEndOfFrame();Save("candidate-open");
  Finish();
 }
'''


def validate_capture(directory, mode):
    if not (directory / "complete.txt").is_file():
        raise ValueError("Original inventory fixture did not finish")
    if not (directory / "inventory-fsm.tsv").is_file():
        raise ValueError("Original inventory FSM state list is missing")
    rows = (directory / "states.tsv").read_text().splitlines()
    if not rows:
        raise ValueError("Original inventory fixture saved no images")
    for row in rows:
        values = row.split("\t")
        if len(values) < 5 or tuple(map(int, values[3:5])) != (1920, 1080):
            raise ValueError("Original inventory capture is not native 1920x1080")
        image = directory / (values[0] + ".png")
        if not image.is_file() or image.stat().st_size == 0:
            raise ValueError("Missing image for state " + values[0])


def verify_private_copy(provenance, steam, archived):
    """Verify the preserved original against its capture provenance hashes."""
    archive = archived.resolve()
    hashes = json.loads(provenance.read_text())["source_sha256"]
    files = []
    for name, expected in hashes.items():
        path = (ROOT / name.replace("\\", "/")).resolve()
        if not path.is_relative_to(archive):
            continue
        if not path.is_file():
            raise ValueError("Provenance input is missing from archived original: " + name)
        with path.open("rb") as stream:
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual != expected:
            raise ValueError("Archived original differs from its provenance hash: " + name)
        files.append({"file": path.relative_to(archive).as_posix(), "sha256": actual,
                      "matches_provenance": True})
    if not files:
        raise ValueError("No provenance inputs resolved inside the archived original")
    return {"build_id": "22529139 (recorded research baseline)",
            "archived_copy": str(archive),
            "steam_manifest": "unavailable; archived copy checked against provenance SHA-256",
            "files": files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default="inventory-reference-20261026")
    parser.add_argument("--output", type=Path, default=ROOT / "build/inventory-reference")
    parser.add_argument("--steam", type=Path,
                        default=ROOT / "_private/original/Hollow Knight",
                        help="Kept for capture_ui compatibility; validation uses archived provenance hashes")
    parser.add_argument("--dotnet", type=Path, default=Path("C:/Program Files/dotnet"))
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    base = (ROOT / "tools/unity_frame_capture_probe.cs.txt").read_text()
    fixture = base[:base.index(" IEnumerator CaptureFrames()")]+SETUP+BODY+HELPERS
    capture_args = SimpleNamespace(name=args.name, width=1920, height=1080,
                                   steam=args.steam, dotnet=args.dotnet,
                                   ui_audit="panels")
    old_verify = ui_audit.verify
    old_subprocess_run = subprocess.run

    def run_with_net10_patcher(command, *run_args, **run_kwargs):
        if len(command) > 1 and Path(str(command[1])).name == "patcher.dll":
            runtime_config = Path(str(command[1])).with_suffix(".runtimeconfig.json")
            settings = json.loads(runtime_config.read_text())
            settings["runtimeOptions"]["framework"]["version"] = "10.0.5"
            runtime_config.write_text(json.dumps(settings))
        return old_subprocess_run(command, *run_args, **run_kwargs)

    ui_audit.verify = verify_private_copy
    unity_probe.subprocess.run = run_with_net10_patcher
    try:
        # probe_unity_physics picks runtime directories lexically, pairing the
        # net10.0 reference pack with runtime 8.0.23 on this machine. Correct
        # only the generated private patcher's runtime config before launching it.
        os.environ["DOTNET_ROLL_FORWARD"] = "Major"
        results = ui_audit.capture_ui(capture_args, fixture_source=fixture,
                                     timeout_seconds=args.timeout, validator=validate_capture)
    finally:
        ui_audit.verify = old_verify
        unity_probe.subprocess.run = old_subprocess_run
    args.output.mkdir(parents=True, exist_ok=True)
    report = args.output / args.name
    if report.exists():
        raise ValueError("Output directory already exists: " + str(report))
    report.mkdir()
    for source in results.iterdir():
        if source.is_file() and (source.suffix.lower() in (".png", ".tsv", ".txt", ".json")):
            shutil.copy2(source, report / source.name)
    manifest = {"source": str(results), "resolution": [1920, 1080],
                "state_images": sorted(p.name for p in report.glob("candidate-open.png")),
                "state_file": "inventory-fsm.tsv",
                "note": "Candidate only: the original Inventory root FSM is injected to Open and settled for 240 frames, without preemptively pausing time. Physics and lighting remain active. Visual inspection is required before accepting a page; Charms and Map pane switching is not captured by this helper."}
    (report / "capture.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(report)


if __name__ == "__main__":
    main()
