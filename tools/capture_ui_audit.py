"""Original-player UI fixtures for the native-resolution presentation audit.

Only bootstrap/profile isolation is patched. State injection is recorded explicitly;
these samples establish appearance, not natural interaction/trigger acceptance.
"""
import json
from pathlib import Path

from PIL import Image
from probe_unity_physics import run
from verify_probe_sources import verify

ROOT = Path(__file__).resolve().parents[1]

SETUP = r'''
 IEnumerator CaptureFrames(){
  yield return UnityEngine.SceneManagement.SceneManager.LoadSceneAsync("Menu_Title");
  for(int i=0;i<10;i++)yield return null;
  var language=AppDomain.CurrentDomain.GetAssemblies().Select(a=>a.GetType("TeamCherry.Localization.Language")).First(t=>t!=null);
  var change=language.GetMethods().Where(m=>m.Name=="SwitchLanguage"&&m.IsStatic).OrderBy(m=>m.GetParameters().Length).First();
  var parameters=change.GetParameters();var values=new object[parameters.Length];
  values[0]=parameters[0].ParameterType==typeof(string)?(object)"EN":Enum.Parse(parameters[0].ParameterType,"EN");
  for(int i=1;i<values.Length;i++)values[i]=parameters[i].DefaultValue;
  change.Invoke(null,values);
  yield return UnityEngine.SceneManagement.SceneManager.LoadSceneAsync("Knight_Pickup",LoadSceneMode.Additive);
  for(int i=0;i<10;i++)yield return null;
  Screen.SetResolution(int.Parse(Arg("-hk3ds-width")),int.Parse(Arg("-hk3ds-height")),FullScreenMode.Windowed);
  for(int i=0;i<10;i++)yield return null;
  GameManager.instance.OnWillActivateFirstLevel();GameManager.instance.LoadScene("Tutorial_01");
'''

MENUS = r'''
  for(int i=0;i<180;i++)yield return null;
  // Startup settings can restore the profile language after the first switch.
  change.Invoke(null,values);
  yield return UnityEngine.SceneManagement.SceneManager.LoadSceneAsync("Menu_Title");
  for(int i=0;i<120;i++)yield return null;
  // Loading Menu_Title applies the saved settings again. Select English only
  // after that load, so the reference records the actual displayed language.
  change.Invoke(null,values);
  GameManager.instance.RefreshLocalization();
  foreach(var font in FindObjectsOfType<ChangeFontByLanguage>())font.SetFont();
  for(int i=0;i<60;i++)yield return null;
  UIManager.instance.UIGoToMainMenu();
  for(int i=0;i<120;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("main-menu");
  UIManager.instance.UIGoToOptionsMenu();
  for(int i=0;i<120;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("options");
  // Bake the original blue sprite/beam composition separately from its UI.
  // Retain world renderers, postprocessing and particles; hide only menu art.
  var menuGraphics=FindObjectsOfType<UnityEngine.UI.Graphic>().Where(g=>g.enabled).ToArray();
  var menuLogos=FindObjectsOfType<Renderer>().Where(r=>r.enabled&&(r.name=="LogoTitle"||r.name=="Hidden_Dreams_Logo")).ToArray();
  foreach(var graphic in menuGraphics)graphic.enabled=false;
  foreach(var logo in menuLogos)logo.enabled=false;
  yield return null;yield return new WaitForEndOfFrame();Save("menu-background");
  foreach(var graphic in menuGraphics)graphic.enabled=true;
  foreach(var logo in menuLogos)logo.enabled=true;
  GameManager.instance.OnWillActivateFirstLevel();GameManager.instance.LoadScene("Tutorial_01");
  for(int i=0;i<780;i++)yield return null;
  var hero=HeroController.instance;
  if(hero){var rb=hero.GetComponent<Rigidbody2D>();rb.velocity=Vector2.zero;rb.constraints=RigidbodyConstraints2D.FreezeAll;hero.enabled=false;}
  UIManager.instance.UIGoToPauseMenu();
  for(int i=0;i<120;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("pause");
  Finish();
 }
'''

HUD = r'''
  for(int i=0;i<780;i++)yield return null;
  logStage="runtime";
  var hero=HeroController.instance;hero.transform.position=new Vector3(75,60.6f,0);
  var body=hero.GetComponent<Rigidbody2D>();body.velocity=Vector2.zero;
  body.constraints=RigidbodyConstraints2D.FreezeAll;body.simulated=true;hero.enabled=false;
  for(int i=0;i<180;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("scene");
  // Keep physics/lighting callbacks live; isolate only the image behind HudCamera.
  foreach(var camera in Camera.allCameras)if(camera.name!="HudCamera"){
   camera.cullingMask=0;camera.clearFlags=CameraClearFlags.SolidColor;camera.backgroundColor=Color.black;
  }
  var pd=PlayerData.instance;pd.MPCharge=0;pd.geo=0;
  GameCameras.instance.soulOrbFSM.SendEvent("MP DRAIN");
  for(int i=0;i<60;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("empty");
  foreach(int soul in new[]{11,33,66,99}){
   pd.MPCharge=soul;GameCameras.instance.soulOrbFSM.SendEvent("MP GAIN");
   for(int i=0;i<45;i++)yield return null;
   yield return new WaitForEndOfFrame();Save("soul-"+soul);
  }
  // Clear Orb Full through the real loss event before testing a cracked vessel.
  // Directly changing 99 -> 66 then sending MP GAIN retains the full overlay.
  pd.MPCharge=0;GameCameras.instance.soulOrbFSM.SendEvent("MP LOSE");
  for(int i=0;i<60;i++)yield return null;
  pd.soulLimited=true;pd.maxMP=66;pd.MPCharge=66;
  PlayMakerFSM.BroadcastEvent("SOUL LIMITER UP");
  GameCameras.instance.soulOrbFSM.SendEvent("MP GAIN");
  for(int i=0;i<120;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("soul-limiter");
  pd.soulLimited=false;pd.maxMP=99;
  PlayMakerFSM.BroadcastEvent("SOUL LIMITER DOWN");
  for(int i=0;i<120;i++)yield return null;
  pd.MPCharge=33;GameCameras.instance.soulOrbFSM.SendEvent("MP LOSE");
  for(int i=0;i<=60;i++){
   if(new[]{0,6,18,30,60}.Contains(i)){yield return new WaitForEndOfFrame();Save("spend-"+i);}
   yield return null;
  }
  hero.TakeDamage(null,CollisionSide.left,1,1);
  for(int i=0;i<=30;i++){
   if(new[]{0,3,18,30}.Contains(i)){yield return new WaitForEndOfFrame();Save("damage-"+i);}
   yield return null;
  }
  hero.AddHealth(1);
  for(int i=0;i<=30;i++){
   if(new[]{0,3,18,30}.Contains(i)){yield return new WaitForEndOfFrame();Save("refill-"+i);}
   yield return null;
  }
  var counter=FindObjectOfType<GeoCounter>();pd.geo=128;counter.AddGeo(128);
  for(int i=0;i<=150;i++){
   if(new[]{0,30,90,120,150}.Contains(i)){yield return new WaitForEndOfFrame();Save("geo-"+i);}
   yield return null;
  }
  var reminder=FindObjectsOfType<PlayMakerFSM>().Single(f=>f.name=="Control Reminder"&&f.FsmName=="Display");
  foreach(string state in new[]{"Attack","First Jump","Interact","Focus","Down"}){
   reminder.SetState(state);
   for(int i=0;i<60;i++)yield return null;
   yield return new WaitForEndOfFrame();Save("reminder-"+state.Replace(" ","-"));
  }
  PlayMakerFSM.BroadcastEvent("BOX UP");
  var dialogue=FindObjectsOfType<DialogueBox>().First(b=>b.name=="Text");
  foreach(string key in new[]{"TUT_TAB_01","TUT_TAB_02","TUT_TAB_03"}){
   dialogue.StartConversation(key,"Lore Tablets");
   for(int i=0;i<360;i++)yield return null;
   yield return new WaitForEndOfFrame();Save("lore-"+key);
   if(key=="TUT_TAB_02"){
    typeof(DialogueBox).GetMethod("ShowPage",InstanceFields).Invoke(dialogue,new object[]{2});
    for(int i=0;i<360;i++)yield return null;
    yield return new WaitForEndOfFrame();Save("lore-TUT_TAB_02-page2");
   }
  }
  PlayMakerFSM.BroadcastEvent("BOX DOWN");
  for(int i=0;i<60;i++)yield return null;
  DialogueVisible(false);
  var focus=Resources.FindObjectsOfTypeAll<GameObject>().FirstOrDefault(g=>g.scene.IsValid()&&g.name=="Focus_prompt_temp");
  if(focus){
   focus.SetActive(true);
   foreach(var f in focus.GetComponentsInChildren<PlayMakerFSM>(true))if(f.Fsm.States.Any(s=>s.Name=="Up"))f.SetState("Up");
   foreach(var f in focus.GetComponentsInChildren<ColorFader>(true)){f.gameObject.SetActive(true);f.Fade(true);}
   for(int i=0;i<=300;i++){
    if(new[]{0,180,210,240,300}.Contains(i)){yield return new WaitForEndOfFrame();Save("focus-panel-"+i);}
    yield return null;
   }
   foreach(var f in focus.GetComponentsInChildren<ColorFader>(true))f.Fade(false);
   for(int i=0;i<60;i++){
    if(new[]{0,3,6,9,12}.Contains(i)){yield return new WaitForEndOfFrame();Save("focus-panel-down-"+i);}
    yield return null;
   }
  }
  DialogueVisible(true);
  foreach(string key in new[]{"MR_MUSHROOM_SHROOMISH1","MR_MUSHROOM_FINAL","MR_MUSHROOM_DREAM"}){
   PlayMakerFSM.BroadcastEvent("BOX UP");dialogue.StartConversation(key,"Minor NPC");
   for(int i=0;i<420;i++)yield return null;
   yield return new WaitForEndOfFrame();Save(key);
   PlayMakerFSM.BroadcastEvent("BOX DOWN");for(int i=0;i<60;i++)yield return null;
  }
  DialogueVisible(false);
  var chest=FindObjectsOfType<PlayMakerFSM>().FirstOrDefault(f=>f.name=="Chest"&&f.Fsm.States.Any(s=>s.Name=="Open"));
  if(chest){
   chest.SetState("Open");for(int i=0;i<180;i++)yield return null;
   var shiny=Resources.FindObjectsOfTypeAll<PlayMakerFSM>().FirstOrDefault(f=>f.gameObject.scene.IsValid()&&f.FsmName=="Shiny Control"&&f.transform.parent&&f.transform.parent.name=="Item");
   if(shiny){
    shiny.gameObject.SetActive(true);
    var inspect=FindObjectsOfType<PlayMakerFSM>().Single(f=>f.name=="Inspect Region"&&f.FsmName=="inspect");
    if(inspect){
     hero.transform.position=new Vector3(shiny.transform.position.x-1,shiny.transform.position.y,0);
     inspect.SetState("In Range");
     for(int i=0;i<90;i++)yield return null;
     yield return new WaitForEndOfFrame();Save("fury-inspect");
    }
    shiny.SetState("Tute");
    for(int i=0;i<=330;i++){
     if(new[]{0,15,30,60,90,120,180,210,240,300,330}.Contains(i)){yield return new WaitForEndOfFrame();Save("fury-"+i);}
     yield return null;
    }
   }
  }
  Finish();
 }
'''

OPENING = r'''
  for(int i=0;i<=3300;i++){
   yield return new WaitForEndOfFrame();
   if(new[]{0,120,320,375,420,600,1420,1520,1628,1748,1850,1972,2092,2240,2320,2440,2640,2700,2798,2900,3000,3020,3150,3240}.Contains(i))Save("opening-"+i.ToString("D4"));
   yield return null;
  }
  Finish();
 }
'''

# Observe the live source animator clock; scene-loading frames are not time zero.
BLANKER = r'''
  bool dumped=false;var saved=new HashSet<int>();
  File.WriteAllText(Path.Combine(output,"blanker.tsv"),"frame\tseconds\tname\tenabled\tr\tg\tb\ta\theroY\tfadeState\trealSeconds\n");
  File.WriteAllText(Path.Combine(output,"opening-scene.tsv"),"frame\tseconds\tcameraX\tcameraY\theroY\tmask\talpha\tstate\n");
  for(int i=0;i<3600;i++){
   yield return new WaitForEndOfFrame();
   var credits=FindObjectsOfType<Animator>().FirstOrDefault(a=>a.name=="tutorial_credits");
   if(credits){
    var info=credits.GetCurrentAnimatorStateInfo(0);float seconds=info.normalizedTime*info.length;
    var fade=GameCameras.instance.cameraFadeFSM;
    foreach(var mask in FindObjectsOfType<SpriteRenderer>().Where(r=>r.name=="Inverse Remasker (2)"||r.name=="Inverse Remasker")){
     var fsm=mask.GetComponent<PlayMakerFSM>();var cam=Camera.main.transform.position;
     File.AppendAllText(Path.Combine(output,"opening-scene.tsv"),Time.frameCount+"\t"+seconds.ToString("R")+"\t"+cam.x.ToString("R")+"\t"+cam.y.ToString("R")+"\t"+HeroController.instance.transform.position.y.ToString("R")+"\t"+mask.name+"\t"+(mask.color.a*mask.sharedMaterial.color.a).ToString("R")+"\t"+(fsm?fsm.ActiveStateName:"")+"\n");
    }
    foreach(var blanker in FindObjectsOfType<SpriteRenderer>().Where(r=>r.name.Contains("Blanker"))){
     var c=blanker.color*blanker.sharedMaterial.color;
     File.AppendAllText(Path.Combine(output,"blanker.tsv"),Time.frameCount+"\t"+seconds.ToString("R")+"\t"+blanker.name+"\t"+blanker.enabled+"\t"+c.r.ToString("R")+"\t"+c.g.ToString("R")+"\t"+c.b.ToString("R")+"\t"+c.a.ToString("R")+"\t"+HeroController.instance.transform.position.y.ToString("R")+"\t"+fade.ActiveStateName+"\t"+Time.realtimeSinceStartup.ToString("R")+"\n");
     if(!dumped){
      dumped=true;
      using(var w=new StreamWriter(Path.Combine(output,"fade-source.txt"))){
       foreach(var f in FindObjectsOfType<PlayMakerFSM>().Where(f=>f==fade||f.name.Contains("Blanker"))){
        w.WriteLine(f.name+" / "+f.FsmName);
        foreach(var s in f.FsmStates){w.WriteLine("STATE "+s.Name);
         foreach(var t in s.Transitions)w.WriteLine(" -> "+t.EventName+" / "+t.ToState);
         foreach(var a in s.Actions){w.WriteLine(a.GetType().FullName+" enabled="+a.Enabled);
          foreach(var field in a.GetType().GetFields(BindingFlags.Public|BindingFlags.Instance))w.WriteLine("  "+field.Name+" = "+field.GetValue(a));
         }
        }
       }
      }
     }
    }
    foreach(var action in fade.Fsm.ActiveState.Actions){
     var field=action.GetType().GetField("colorLerp",BindingFlags.Instance|BindingFlags.NonPublic);
     if(field!=null){var c=(Color)field.GetValue(action);File.AppendAllText(Path.Combine(output,"blanker.tsv"),Time.frameCount+"\t"+seconds.ToString("R")+"\tGUI\tTrue\t"+c.r.ToString("R")+"\t"+c.g.ToString("R")+"\t"+c.b.ToString("R")+"\t"+c.a.ToString("R")+"\t"+HeroController.instance.transform.position.y.ToString("R")+"\t"+fade.ActiveStateName+"\n");}
    }
    int tick=Mathf.RoundToInt(seconds*60);
    if(new[]{OPENING_TICKS}.Contains(tick)&&saved.Add(tick))Save("blanker-"+tick.ToString("D4"));
    if(seconds>6.05f)break;
   }
   else if(i%180==179)GameManager.instance.SkipCutscene();
   yield return null;
  }
  Finish();
 }
'''

HELPERS = r'''
 static string Hierarchy(Transform t){return t.parent?Hierarchy(t.parent)+"/"+t.name:t.name;}
 static void DialogueVisible(bool visible){
  var manager=GameObject.Find("DialogueManager");
  if(manager)foreach(var renderer in manager.GetComponentsInChildren<Renderer>(true))renderer.enabled=visible;
  File.AppendAllText(Path.Combine(output,"isolation.tsv"),Time.frameCount+"\tdialogue renderers\t"+visible+"\n");
 }
 static void Finish(){
  File.WriteAllText(Path.Combine(output,"complete.txt"),"Original UI fixture complete");Application.Quit();
 }
 static string Clean(string s){return (s??"").Replace("\t"," ").Replace("\r","").Replace("\n"," ");}
 static void Save(string name){
  var texture=new Texture2D(Screen.width,Screen.height,TextureFormat.RGBA32,false);
  texture.ReadPixels(new Rect(0,0,Screen.width,Screen.height),0,0);texture.Apply();
  File.WriteAllBytes(Path.Combine(output,name+".png"),texture.EncodeToPNG());Destroy(texture);
  var hero=HeroController.instance;var rb=hero?hero.GetComponent<Rigidbody2D>():null;
  // Canvas text does not appear in Renderer inventories. Record its displayed
  // strings as well so a reference cannot silently retain another language.
  using(var w=new StreamWriter(Path.Combine(output,name+"-ui-text.tsv"))){
   foreach(var component in FindObjectsOfType<MonoBehaviour>()){
    if(!component.enabled)continue;
    var property=component.GetType().GetProperty("text");
    if(property==null||property.PropertyType!=typeof(string)||!property.CanRead||property.GetIndexParameters().Length!=0)continue;
    w.WriteLine(Clean(component.name)+"\t"+Clean(property.GetValue(component,null) as string));
   }
  }
  File.AppendAllText(Path.Combine(output,"states.tsv"),name+"\t"+Time.frameCount+"\t"+Time.time+"\t"+Screen.width+"\t"+Screen.height+"\t"+PlayerData.instance.health+"\t"+PlayerData.instance.MPCharge+"\t"+PlayerData.instance.geo+"\t"+(rb?rb.simulated:false)+"\t"+(rb?rb.constraints.ToString():"None")+"\t"+(hero?hero.transform.position.ToString():"None")+"\n");
  using(var w=new StreamWriter(Path.Combine(output,name+"-renderers.tsv"))){
   File.AppendAllText(Path.Combine(output,"soul-states.tsv"),name+"\t"+PlayerData.instance.MPCharge+"\t"+PlayerData.instance.maxMP+"\t"+PlayerData.instance.soulLimited+"\n");
   w.WriteLine("camera\tname\ttype\tx\ty\twidth\theight\ttext\tcolor");
   foreach(var camera in Camera.allCameras)foreach(var renderer in FindObjectsOfType<Renderer>()){
    if(!renderer.enabled||(camera.cullingMask&(1<<renderer.gameObject.layer))==0)continue;
    var b=renderer.bounds;var lo=camera.WorldToScreenPoint(b.min);var hi=camera.WorldToScreenPoint(b.max);
    var text=renderer.GetComponent<TMProOld.TextMeshPro>();var sprite=renderer.GetComponent<tk2dBaseSprite>();var unitySprite=renderer as SpriteRenderer;
    w.WriteLine(camera.name+"\t"+Clean(renderer.name)+"\t"+renderer.GetType().Name+"\t"+Math.Min(lo.x,hi.x)+"\t"+(Screen.height-Math.Max(lo.y,hi.y))+"\t"+Math.Abs(hi.x-lo.x)+"\t"+Math.Abs(hi.y-lo.y)+"\t"+Clean(text?text.text:"")+"\t"+(text?text.color.ToString():sprite?sprite.color.ToString():unitySprite?unitySprite.color.ToString():""));
   }
  }
  using(var w=new StreamWriter(Path.Combine(output,name+"-fsms.tsv")))foreach(var f in FindObjectsOfType<PlayMakerFSM>())w.WriteLine(Clean(f.name)+"\t"+Clean(f.FsmName)+"\t"+Clean(f.ActiveStateName));
 }
}
'''


SOUL_LIMITER = r'''
  var pd=PlayerData.instance;pd.MPCharge=0;pd.geo=0;
  GameCameras.instance.soulOrbFSM.SendEvent("MP DRAIN");
  for(int i=0;i<60;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("empty");
  pd.soulLimited=true;pd.maxMP=66;
  PlayMakerFSM.BroadcastEvent("SOUL LIMITER UP");
  float started=Time.time;
  for(int i=0;i<=60;i++){
   if(new[]{0,6,18,30,60}.Contains(i)){
    yield return new WaitForEndOfFrame();Save("limiter-up-"+i);
    File.AppendAllText(Path.Combine(output,"limiter-ages.tsv"),"limiter-up-"+i+"\t"+(Time.time-started)+"\n");
   }
   yield return null;
  }
  pd.MPCharge=66;GameCameras.instance.soulOrbFSM.SendEvent("MP GAIN");
  for(int i=0;i<180;i++)yield return null;
  yield return new WaitForEndOfFrame();Save("limited-fill");
  // Shade Death Start restores capacity; Give Geo delivers DOWN after the
  // source .5 + .75 + 1 second waits. Keep those observations separate.
  GameManager.instance.EndSoulLimiter();
  started=Time.time;
  for(int i=0;i<135;i++){
   if(new[]{0,30,75,120,132}.Contains(i)){
    yield return new WaitForEndOfFrame();Save("recovery-wait-"+i);
    File.AppendAllText(Path.Combine(output,"limiter-ages.tsv"),"recovery-wait-"+i+"\t"+(Time.time-started)+"\n");
   }
   yield return null;
  }
  PlayMakerFSM.BroadcastEvent("SOUL LIMITER DOWN");
  started=Time.time;
  for(int i=0;i<=60;i++){
   if(new[]{0,6,18,30,60}.Contains(i)){
    yield return new WaitForEndOfFrame();Save("limiter-down-"+i);
    File.AppendAllText(Path.Combine(output,"limiter-ages.tsv"),"limiter-down-"+i+"\t"+(Time.time-started)+"\n");
   }
   yield return null;
  }
  Finish();
 }
'''


def template(mode):
    base = (ROOT / 'tools/unity_frame_capture_probe.cs.txt').read_text()
    body = HUD
    if mode == 'soul-limiter':
        body = HUD[:HUD.index('  var pd=')] + SOUL_LIMITER
    if mode == 'panels':
        body = HUD[:HUD.index('  var pd=')] + HUD[HUD.index('  var reminder='):]
        # Reading fixtures represent normal play after the opening sequence.
        # Isolate only that unrelated overlay; opening/blanker audits retain it.
        body = body.replace('  logStage="runtime";', '''  logStage="runtime";
  foreach(var credits in FindObjectsOfType<Animator>().Where(a=>a.name=="tutorial_credits"))
   credits.gameObject.SetActive(false);''')
    if mode == 'inspect':
        body = HUD[:HUD.index('  // Keep physics/lighting')] + HUD[HUD.index('  var chest='):]
    setup = SETUP
    if mode == 'menus':
        setup = setup.replace('  GameManager.instance.OnWillActivateFirstLevel();GameManager.instance.LoadScene("Tutorial_01");', '')
        body = MENUS
    if mode == 'blanker':
        setup = setup.replace('  yield return UnityEngine.SceneManagement.SceneManager.LoadSceneAsync("Knight_Pickup",LoadSceneMode.Additive);', '')
        setup = setup.replace('  GameManager.instance.OnWillActivateFirstLevel();GameManager.instance.LoadScene("Tutorial_01");', r'''
  GameManager.instance.StartNewGame();
''')
    return base[:base.index(' IEnumerator CaptureFrames()')] + setup + (
        BLANKER.replace('OPENING_TICKS', ','.join(map(str, json.loads(
            (ROOT / 'tests/opening-blanker-capture.json').read_text())[0]['frames'])))
        if mode == 'blanker' else OPENING if mode == 'opening' else body) + HELPERS


def capture_ui(args, fixture_source=None, timeout_seconds=600, validator=None, player_root=None,
               *, source_audit=None, converted_inputs=True, cecil=None):
    if (args.width, args.height) != (1920, 1080):
        raise ValueError('UI reference captures require native 1920x1080')
    parent = (ROOT / '_private/converted' if player_root is not None else
              Path(getattr(args, 'output_root', None) or ROOT / '_private/converted/ui-audit'))
    if not (parent.resolve().is_relative_to((ROOT / '_private/converted').resolve()) or parent.resolve().is_relative_to(Path('C:/Projects/HK3DS/_private/laptop-reference-frames').resolve())):
        raise ValueError('UI references must remain in private converted or laptop-reference-frames')
    player = Path(player_root).resolve() if player_root is not None else (parent / args.name).resolve()
    if player == parent.resolve() or not player.is_relative_to(parent.resolve()) or player.exists():
        raise ValueError('UI capture name must select a fresh private player directory')
    audit = source_audit
    if audit is None:
        audit = verify(ROOT / '_private/tutorial-intro-probe/results/provenance.json',
                       args.steam, ROOT / '_private/original/Hollow Knight')
    player.mkdir(parents=True)
    fixture = player / 'fixture.cs.txt'
    fixture.write_text(template(args.ui_audit) if fixture_source is None else fixture_source)
    patch = player / 'patch.cs.txt'
    source = (ROOT / 'tools/unity_frame_capture_patch.cs.txt').read_text()
    # Isolate preference accesses in the firstpass helper assembly as well as
    # Assembly-CSharp; language selection uses the original localization API.
    source = source.replace('  asm.Write(a[0]+"/Assembly-CSharp.patched.dll");', r'''
  using(var first=AssemblyDefinition.ReadAssembly(a[0]+"/Assembly-CSharp-firstpass.dll",new ReaderParameters{AssemblyResolver=r})){
   foreach(var t in first.MainModule.Types.SelectMany(Flatten))foreach(var m in t.Methods.Where(m=>m.HasBody))
    foreach(var ins in m.Body.Instructions)if(ins.Operand is MethodReference mr){
     if(mr.DeclaringType.FullName=="UnityEngine.PlayerPrefs"){
      var replacement=probe.MainModule.Types.Single(t=>t.Name=="HKMemoryPrefs").Methods.Single(x=>x.Name==mr.Name&&x.Parameters.Select(p=>p.ParameterType.FullName).SequenceEqual(mr.Parameters.Select(p=>p.ParameterType.FullName)));
      ins.Operand=first.MainModule.ImportReference(replacement);prefs++;
     }
     if(mr.DeclaringType.FullName=="UnityEngine.Application"&&mr.Name=="get_persistentDataPath"){
      ins.Operand=first.MainModule.ImportReference(entry.Methods.Single(x=>x.Name=="PersistentPath"));paths++;
     }
    }
   first.Write(a[0]+"/Assembly-CSharp-firstpass.isolated.dll");
  }
  File.Move(a[0]+"/Assembly-CSharp-firstpass.isolated.dll",a[0]+"/Assembly-CSharp-firstpass.dll",true);
  asm.Write(a[0]+"/Assembly-CSharp.patched.dll");''')
    patch.write_text(source)
    assets = ()
    if args.ui_audit == 'blanker':
        data = ROOT / '_private/original/Hollow Knight/hollow_knight_Data'
        assets = ('level5', *(p.name for p in data.glob('sharedassets5.*')))
    run(ROOT / '_private/original/Hollow Knight', player, args.dotnet, fixture,
        True, patch,
        ['-hk3ds-width', str(args.width), '-hk3ds-height', str(args.height)],
        timeout=timeout_seconds, graphics=True, extra_libraries=('PlayMaker.dll', 'Unity.TextMeshPro.dll'), extra_assets=assets,
        converted_inputs=converted_inputs, cecil=cecil)
    (validator or validate_ui)(player / 'results', args.ui_audit)
    (player / 'results/source-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
    return player / 'results'


def validate_menu_language(directory):
    labels = {line.partition('\t')[2].strip() for line in
              (directory / 'main-menu-ui-text.tsv').read_text(encoding='utf-8').splitlines()}
    if not {'Start Game', 'Options'}.issubset(labels):
        raise ValueError('Menu reference did not display English Start Game / Options labels')


def validate_ui(directory, mode):
    rows = [row.split('\t') for row in (directory / 'states.tsv').read_text().splitlines()]
    if not rows:
        raise ValueError('No UI samples')
    limiter = directory / 'soul-limiter-renderers.tsv'
    if limiter.exists() and any('\tOrb Full\tSpriteRenderer\t' in line for line in limiter.read_text().splitlines()):
        raise ValueError('Limiter reference retains the stale Orb Full overlay')
    for row in rows:
        if tuple(map(int, row[3:5])) != (1920, 1080):
            raise ValueError('UI sample resolution must be native 1920x1080: ' + row[0])
        if mode not in ('opening', 'blanker', 'menus') and row[8:10] != ['True', 'FreezeAll']:
            raise ValueError('HUD fixture must preserve simulated, frozen hero physics: ' + row[0])
        with Image.open(directory / (row[0] + '.png')) as image:
            if image.size != (1920, 1080):
                raise ValueError('UI screenshot dimensions differ: ' + row[0])
    if mode == 'menus':
        validate_menu_language(directory)
    if mode == 'soul-limiter':
        observations = {r[0]:r[1:] for r in (line.split('\t') for line in (directory/'soul-states.tsv').read_text().splitlines())}
        for row in rows:
            name=row[0]
            if name=='scene':
                continue
            cracked=name.startswith(('limiter-up-', 'recovery-wait-')) or name=='limited-fill'
            expected='Cracked' if cracked else 'Normal' if name.startswith('limiter-down-') else 'Appear Normal'
            fsms=(directory/(name+'-fsms.tsv')).read_text().splitlines()
            if 'HUD_frame\tLoad Animation\t'+expected not in fsms:
                raise ValueError('Wrong native limiter frame state: '+name)
            if name.startswith('recovery-wait-') and observations[name]!=['66','99','False']:
                raise ValueError('Recovery fixture must restore capacity before DOWN: '+name)
    return rows
