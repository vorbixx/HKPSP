#pragma once
#include <cstdint>
#include <cstddef>

namespace hk3ds {
// Appended text capture ABI: old mailbox offsets stay stable.
struct TestTextBox {
    float left=0,top=0,right=0,bottom=0,glyphHeight=0;
    std::uint32_t color=0xffffffff;
    char text[96]{};
};
static_assert(sizeof(TestTextBox)==120,"text capture record ABI");
inline constexpr unsigned textCaptureCapacity=128;
// Test-only RPC mailbox. Freeze between requested rendered 60-Hz steps.
struct TestFrameCapture {
    std::uint32_t magic=0,steps=0,completed=0,rendered=0;
    float cameraX=0,cameraY=0;
    std::uint32_t skipCameraColor=0;
    std::uint32_t skipCameraBloom=0;
    float presentationAlpha=0; // Output: phase after the test-only capture boundary reset.
    float grassSeconds=-1; // Input: test override; negative uses the runtime clock.
    std::uint32_t excludeGrass=0; // Input: suppress grass draws for layer isolation.
    std::uint32_t followCamera=0; // Input: retain the gameplay camera while stepping.
    float observedCameraX=0,observedCameraY=0,commandBufferUsage=0;
    std::uint32_t roomDraws=0,roomDrawFailures=0;
    std::uint32_t reminderKind=0,promptFlags=0;
    float reminderAlpha=0,tabletAlpha=0;
    std::uint32_t skipIntermediateRenders=0;
    std::uint32_t inspectionPhase=0,inspectionOwner=0,inspectionPage=0,inspectionCharacters=0,inspectionLocked=0,inspectionSerial=0;
    float inspectionAlpha=0,inspectionHum=0;
    float heroX=0,heroY=0; // Output: rendered hero pose.
    std::uint32_t pinHero=0; // Input: source fixtures disable HeroController and Rigidbody2D.
    std::int32_t soulFixture=-1,maxSoulFixture=-1; // Input: capture-only HUD state; -1 disables.
    std::int32_t observedSoul=0,observedMaxSoul=0;
    std::int32_t hudHealthFixture=-1,hudGeoFixture=-1; // Presentation observations only.
    std::int32_t observedHudHealth=0,observedHudGeo=0;
    std::uint32_t excludeRoomOwner=0; // Input: capture-only room layer A/B; zero disables.
    float vignetteScale=0;
    std::int32_t darknessRegion=0;
    float heroLightScale=0,heroLightAlpha=0,heroLightAge=-1;
    std::uint32_t heroLightHits=0;
    std::int32_t heroLightHealth=0;
    char requestedScene[64]{}; // Input: diagnostic room load, consumed by command 22.
    char requestedGate[64]{}; // Input: entry gate for the diagnostic room load.
    std::uint32_t hideHero=0; // Input: omit sprite/slash, retain hero lighting.
    std::uint32_t textVersion=0;
    std::uint32_t textCount[2]{},textOverflow[2]{};
    std::uint32_t textSequence=0;
    TestTextBox textBoxes[2][textCaptureCapacity]{};
    // Output only: phase evidence for boss presentation captures.
    std::uint32_t bossKind=0,bossPhase=0,bossVisible=0;
    std::int32_t bossSprite=-1;
    float bossX=0,bossY=0;
    std::uint32_t bossParticles=0;
    std::int32_t bossHeroHealth=0;
    bool excludesRoomOwner(std::uint32_t owner) const volatile
    {return excludeRoomOwner!=0&&excludeRoomOwner==owner;}
};
static_assert(offsetof(TestFrameCapture,bossKind)==31072,"boss observation RPC offset");
static_assert(offsetof(TestFrameCapture,requestedScene)==196,"scene request RPC offset");
static_assert(offsetof(TestFrameCapture,hideHero)==324,"hero visibility RPC offset");
static_assert(offsetof(TestFrameCapture,textVersion)==328,"text capture RPC offset");
static_assert(offsetof(TestFrameCapture,textSequence)==348,"text publication RPC offset");
static_assert(sizeof(TestFrameCapture)==31104,"frame capture RPC ABI");
static_assert(offsetof(TestFrameCapture,heroLightScale)==176,"HeroLight diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,excludeRoomOwner)==164,"room exclusion RPC offset");
static_assert(offsetof(TestFrameCapture,vignetteScale)==168,"lighting diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,darknessRegion)==172,"darkness region RPC offset");
static_assert(offsetof(TestFrameCapture,hudHealthFixture)==148,"HUD fixture RPC offset");
static_assert(offsetof(TestFrameCapture,observedHudHealth)==156,"HUD diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,soulFixture)==132,"Soul fixture RPC offset");
static_assert(offsetof(TestFrameCapture,observedSoul)==140,"Soul diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,inspectionPhase)==88,"inspection diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,skipIntermediateRenders)==84,"capture checkpoint RPC offset");
static_assert(offsetof(TestFrameCapture,reminderKind)==68,"prompt diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,presentationAlpha)==32,"presentation phase RPC offset");
static_assert(offsetof(TestFrameCapture,grassSeconds)==36,"grass time RPC offset");
static_assert(offsetof(TestFrameCapture,excludeGrass)==40,"grass exclusion RPC offset");
static_assert(offsetof(TestFrameCapture,followCamera)==44,"camera mode RPC offset");
static_assert(offsetof(TestFrameCapture,observedCameraX)==48,"render diagnostics RPC offset");
static_assert(offsetof(TestFrameCapture,heroX)==120,"hero pose RPC offset");
static_assert(offsetof(TestFrameCapture,pinHero)==128,"hero pin RPC offset");
inline bool frameCaptureEnabled(const volatile TestFrameCapture& box,bool testCommands)
{return testCommands&&box.magic==0x484b4643u;}
inline bool frameCaptureExcludesOwner(const volatile TestFrameCapture& box,bool enabled,std::uint32_t owner)
{
    if(!enabled)return false;
    const auto excluded=box.excludeRoomOwner;
    return excluded!=0&&excluded==owner;
}
inline bool frameCaptureSoulFixture(const volatile TestFrameCapture& box,bool enabled,int& soul,int& maxSoul)
{
    const int value=box.soulFixture,maximum=box.maxSoulFixture;
    if(!enabled||value<0||(maximum!=66&&maximum!=99)||value>maximum)return false;
    soul=value;maxSoul=maximum;return true;
}
inline bool frameCaptureHudFixture(const volatile TestFrameCapture& box,bool enabled,int& health,
                                  int& maxHealth,int& blueHealth,std::uint32_t& geo)
{
    const int hp=box.hudHealthFixture,balance=box.hudGeoFixture;
    if(!enabled||hp<1||hp>5||balance<0)return false;
    health=hp;maxHealth=5;blueHealth=0;geo=static_cast<std::uint32_t>(balance);return true;
}
inline bool frameCaptureOwnerExcluded(const volatile TestFrameCapture& box,bool enabled,unsigned owner)
{return enabled&&box.excludeRoomOwner!=0&&box.excludeRoomOwner==owner;}
inline bool frameCaptureSkipRender(const volatile TestFrameCapture& box,bool enabled)
{return enabled&&box.skipIntermediateRenders==1&&box.steps>0;}
inline bool frameCaptureStartup(volatile TestFrameCapture& box,bool testCommands,bool replay,
                                std::uint32_t magic,std::uint32_t mode)
{
    if(!testCommands||!replay||magic!=0x484b4350u||mode>1)return false;
    box.magic=0x484b4643u;box.followCamera=1;box.skipIntermediateRenders=mode;
    return true;
}
inline double frameCaptureElapsed(volatile TestFrameCapture& box,bool enabled,double elapsed)
{
    if(!enabled)return elapsed;
    if(!box.steps)return 0;
    --box.steps;++box.completed;return 1.0/60.0;
}
}
