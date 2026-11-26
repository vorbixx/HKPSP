"""Export observed Tutorial_01 room rules separately from diagnostic fixtures."""
import converted_files  # Install hardlink copy-on-write before asset writes.
import json,struct,hashlib,argparse
from pathlib import Path
from assetripper_client import ROOT
from convert_terrain import transform_point,paths
from inspect_fsm import decode_fsm
from export_objects import validate_tutorial_objects

ORIGINAL_DATA = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
# These are the source files for the reviewed 22529139 new-game defaults and
# Tutorial_01 bounds used below. Their hashes were recorded by the source probe.
TUTORIAL_INPUTS = {
    'level6': '60e9b83d79fdcb51e8b9c1f2530d189a67cace982ff17d8a039c232d7ae8ee8e',
    'Managed/Assembly-CSharp.dll': 'e9048ef6a633970f735e01ec166d3959f610eaea7a88d827d48d67b1e5fb87bd',
}


def validate_original(original, inputs=TUTORIAL_INPUTS):
    for name, expected in inputs.items():
        with (original / name).open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != expected:
            raise ValueError(f'Tutorial_01 rules need the reviewed Steam build 22529139. Source changed: {name}')


def convert(source=ROOT/"_private/research/tutorial",output=ROOT/"_private/converted/tutorial",
            original=ORIGINAL_DATA):
    validate_original(original)
    read=lambda k:json.loads((source/k).read_text())
    rows=json.loads((source/"room-reliable-export.json").read_text())
    geometry=json.loads((source/"collider-geometry.json").read_text())
    bygo={}
    for r in geometry:bygo.setdefault(r["go"],[]).append(r)
    def point(r):return transform_point([0,0],[read(k) for k in r["chain"]])
    def rect(r):
        c=read(r["collider"]);pts=[transform_point(p,[read(k) for k in r["chain"]]) for path in paths(c,r["type"]) for p in path]
        return [min(p[0] for p in pts),min(p[1] for p in pts),max(p[0] for p in pts),max(p[1] for p in pts)]
    def active(r):return all(read(k)["m_IsActive"] for k in r["owners"])
    def script(r,t):return read(r["parts"][t][0])["m_Structure"]
    def add(group, row, record):
        go = row['parts']['GameObject'][0]
        if not bygo.get(go):
            raise ValueError(f'{go}: gameplay object has no exported collider')
        record.update(source_go=go, active=active(bygo[go][0]))
        result[group].append(record)
    def activation(g):
        parents=[r for r in rows if r['parts']['GameObject'][0] in g['owners'] and 'ActivateChildrenOnContact' in r['parts']]
        if not parents:return None
        assert len(parents)==1
        parent=parents[0];a=script(parent,'ActivateChildrenOnContact');d=script(parent,'DeactivateIfPlayerdataTrue')
        assert read(parent['parts']['GameObject'][0])['m_Layer']==13 and d['boolName']=='visitedDirtmouth'
        assert read(parent['parts']['ActivateChildrenOnContact'][0])['m_Enabled'] and read(parent['parts']['DeactivateIfPlayerdataTrue'][0])['m_Enabled']
        assert not a['circleCollider']['m_PathID'] and a['boxCollider']['m_PathID']
        trigger=next(r for r in geometry if r['collider']==f"level6-{a['boxCollider']['m_PathID']}.json")
        c=read(trigger['collider']);assert c['m_Enabled'] and c['m_IsTrigger']
        return dict(trigger=rect(trigger),ultrawide_threshold=a['ultrawideThreshold'] if a['skipInUltrawide'] else 0,disable_if_visited_dirtmouth=True,parent=parent['parts']['GameObject'][0],source=parent['parts']['ActivateChildrenOnContact'][0],deactivation_source=parent['parts']['DeactivateIfPlayerdataTrue'][0])
    fixture=next(r for r in rows if r["name"]=="Crawler 1")
    result=dict(build=22529139,scene="Tutorial_01",bounds=[0,0,201,80],initial_gate="top1",initial_spawn=[34.5,97.0],playable_respawn=[35.46,11.37],respawn_facing_right=True,initial_health=5,initial_soul=0,nail_damage=5,abilities=dict(dash=False,walljump=False,doublejump=False,spell=False),crawlers=[],doors=[],gates=[],hazards=[],camera_locks=[],hazard_respawns=[])
    for r in rows:
        go=r["parts"]["GameObject"][0];gs=bygo.get(go,[])
        if "HealthManager" in r["parts"] and 'LineOfSightDetector' not in r['parts']:
            g=gs[0];h=script(r,"HealthManager");f=decode_fsm(script(r,"PlayMakerFSM")["fsm"])
            assert all(script(r,t)==script(fixture,t) for t in ["PlayMakerFSM","Recoil","EnemyDeathEffects"]), "Crawler branch changed"
            walk=next(a["parameters"] for s in f["states"] if s["name"]=="Walk" for a in s["actions"] if a["type"].endswith("WalkLeftRight"))
            assert h["hp"]==8 and h["smallGeoDrops"]==2 and walk["walkSpeed"]==4 and walk["turnDelay"]==1
            add("crawlers",r,dict(name=r["name"],spawn=point(g),active=active(g),scale_x=read(r["parts"]["Transform"][0])["m_LocalScale"]["m_X"],collider_count=len(gs),fsm_matches_fixture=True,recoil_matches_fixture=True,death_matches_fixture=True,hp=h["hp"],damage=script(r,"DamageHero")["damageDealt"],body=rect(g),speed=4,turn_delay=1,shared_config="crawler.bin"))
            result['crawlers'][-1]['activation']=activation(g)
        if 'Breakable' in r['parts']:
            d=script(r,"Breakable");whole={v["m_PathID"] for v in d["wholeParts"]};trigger=next(g for g in gs if read(g["collider"])["m_IsTrigger"])
            ids=[int(trigger["collider"].split("-")[-1].split(".")[0])]
            for g in geometry:
                if whole & {int(k.split("-")[-1].split(".")[0]) for k in g["owners"]} and active(g) and read(g["go"])["m_Layer"]==8:ids.append(int(g["collider"].split("-")[-1].split(".")[0]))
            ids=list(dict.fromkeys(ids));add("doors",r,dict(name=r["name"],trigger=ids[0],disabled_shapes=ids,layer=read(go)["m_Layer"],rect=rect(trigger),persistent=True))
        if "TransitionPoint" in r["parts"]:
            d=script(r,"TransitionPoint");add("gates",r,dict(name=r["name"],rect=rect(gs[0]),position=point(gs[0]),target=d["targetScene"],entry=d["entryPoint"],offset=[d["entryOffset"]["x"],d["entryOffset"]["y"]],entry_delay=d["entryDelay"]))
        if ('DamageHero' in r['parts'] and 'HealthManager' not in r['parts']
                and 'StalactiteControl' not in r['parts']):
            d=script(r,"DamageHero");g=next(g for g in gs if read(g["collider"])["m_IsTrigger"]);add("hazards",r,dict(name=r["name"],rect=rect(g),damage=d["damageDealt"],type=d["hazardType"]))
        if "CameraLockArea" in r["parts"] and gs:
            d=script(r,"CameraLockArea");add("camera_locks",r,dict(name=r["name"],rect=rect(gs[0]),limits=[d[k] for k in ["cameraXMin","cameraYMin","cameraXMax","cameraYMax"]],prevent_up=d["preventLookUp"],prevent_down=d["preventLookDown"],priority=d["maxPriority"],active=active(gs[0])))
        if "HazardRespawnTrigger" in r["parts"] and gs:
            d=script(r,"HazardRespawnTrigger");marker=read("level6-"+str(d["respawnMarker"]["m_PathID"])+".json");owner="level6-"+str(marker["m_GameObject"]["m_PathID"])+".json";obj=read(owner)
            tr=next(read("level6-"+str(v["m_Component"]["m_PathID"])+".json") for v in obj["m_Components"] if "m_LocalPosition" in read("level6-"+str(v["m_Component"]["m_PathID"])+".json"));chain=[tr]
            while chain[-1]["m_Father"]["m_PathID"]:chain.append(read("level6-"+str(chain[-1]["m_Father"]["m_PathID"])+".json"))
            add("hazard_respawns",r,dict(name=r["name"],rect=rect(gs[0]),position=transform_point([0,0],chain),facing_right=marker["m_Structure"]["respawnFacingRight"],fire_once=d["fireOnce"]))
    result["entry_semantics"]=dict(source="GameManager.OnWillActivateFirstLevel; HeroController.EnterScene; TutorialEntryPauser.Start",hidden_delays=[0.165,1.0,0.2],drop_wait=0.33,first_credits_input_release="Collect Normal 3 FINISHED after concurrent .75 s Land and 3.25 s Flock waits",death_marker="Death Respawn Marker",mandatory_enemy_kills=False)
    collapse=next(r for r in rows if r["name"]=="Collapser Tute 01")
    cg=bygo[collapse["parts"]["GameObject"][0]]
    trigger=next(g for g in cg if read(g["collider"])["m_IsTrigger"])
    result["collapse"]=dict(trigger_rect=rect(trigger),delay=0.4,trigger_id=int(trigger["collider"].split("-")[-1].split(".")[0]),disabled_shapes=[int(g["collider"].split("-")[-1].split(".")[0]) for g in geometry if collapse["parts"]["GameObject"][0] in g["owners"]],camera_lock_name="CameraLockArea Collapser",force_hard_landing_next_frame=True,persistent=True)
    result['object_inventory'] = validate_tutorial_objects(
        result, rows, geometry, read, read('inventory.json')['rows'])
    # HKS1 v1. Rect values are minX,minY,maxX,maxY; all rows are fixed width.
    binary=bytearray(struct.pack("<4s8I",b"HKS1",1,len(result["crawlers"]),len(result["doors"]),len(result["gates"]),len(result["hazards"]),len(result["camera_locks"]),len(result["hazard_respawns"]),0))
    binary+=struct.pack("<8f4I",*result["bounds"],*result["initial_spawn"],*result["playable_respawn"],
                        result["initial_health"],result["initial_soul"],result["nail_damage"],
                        result["respawn_facing_right"])
    for r in result["crawlers"]:binary+=struct.pack("<3f5I",*r["spawn"],r["scale_x"],int(r["active"]),r["collider_count"],r["hp"],r["damage"],0)
    for r in result["doors"]:
        ids=r["disabled_shapes"];assert len(ids)<=8
        binary+=struct.pack("<12I4f",r["trigger"],len(ids),r["layer"],1,*(ids+[0]*(8-len(ids))),*r["rect"])
    for r in result["gates"]:
        # target enum 1 Town, 2 Cliffs_02; entry enum 1 left1, 2 bot2, 3 bot1.
        binary+=struct.pack("<9f3I",*r["rect"],*r["position"],*r["offset"],r["entry_delay"],1 if r["target"]=="Town" else 2,{"left1":1,"bot2":2,"bot1":3}[r["entry"]],{"right1":1,"top1":2,"top2":3}[r["name"]])
    for r in result["hazards"]:binary+=struct.pack("<4f2I",*r["rect"],r["damage"],r["type"])
    for r in result["camera_locks"]:binary+=struct.pack("<8f4I",*r["rect"],*r["limits"],int(r["prevent_up"]),int(r["prevent_down"]),int(r["priority"]),int(r["active"]))
    for r in result["hazard_respawns"]:binary+=struct.pack("<6f2I",*r["rect"],*r["position"],int(r["facing_right"]),int(r["fire_once"]))
    result["original_sha256"]=TUTORIAL_INPUTS
    result["binary_sha256"]=hashlib.sha256(binary).hexdigest()
    result["source_sha256"]={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in source.glob("*.json")}
    output.mkdir(parents=True,exist_ok=True);(output/"tutorial-room.bin").write_bytes(binary);(output/"manifest.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps({k:len(result[k]) for k in ["crawlers","doors","gates","hazards","camera_locks","hazard_respawns"]}))
    return result
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / '_private/research/tutorial')
    parser.add_argument('--output', type=Path, default=ROOT / '_private/converted/tutorial')
    parser.add_argument('--original', type=Path, default=ORIGINAL_DATA)
    args = parser.parse_args()
    convert(args.source, args.output, args.original)
