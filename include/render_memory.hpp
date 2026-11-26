#pragma once
#include <cstddef>

namespace hk3ds {
// New scene art retains source pixel dimensions. Town needs thirteen 2 MiB
// pages; one spare page preserves the existing transition admission reserve.
constexpr std::size_t roomTextureBudget=28u*1024u*1024u;
constexpr std::size_t oldRoomTextureBudget=16u*1024u*1024u;
constexpr std::size_t oldEnemyAudioBankCapBytes=768u*1024u;
constexpr std::size_t oldLinearHeapBytes=47u*1024u*1024u;
constexpr std::size_t oldApplicationMemoryBytes=64u*1024u*1024u;
constexpr bool highDetailRoomMemory(std::size_t commitLimit,unsigned memoryMode=6) {
    return memoryMode>=6&&memoryMode<=8&&commitLimit>oldApplicationMemoryBytes;
}
// Match libctru's commit accounting, including the already loaded image/stack.
// Zero means admission failed; never pass it to libctru (zero requests auto sizing).
constexpr std::size_t oldOrdinaryHeapBytes(std::size_t commitLimit,std::size_t committed) {
    const auto limit=commitLimit<oldApplicationMemoryBytes?commitLimit:oldApplicationMemoryBytes;
    if(committed>=limit)return 0;
    const auto available=(limit-committed)&~std::size_t(4095);
    return available>oldLinearHeapBytes?available-oldLinearHeapBytes:0;
}
// Selected before libctru allocates the heaps; defaults safely on query failure.
extern std::size_t activeRoomTextureBudget;
// Existing actors, resident room, screen effects and the decor page retain their
// allocation budget. Geo adds a separate lossless 512x512 RGBA8 texture.
constexpr std::size_t baseLinearHeapBytes=36u*1024u*1024u;
// Full Tutorial_01 and death/retry playback overflow Citro3D's 256 KiB default
// during bloom. Splits retain commands until frame completion; preserve every
// draw and admit the extra 128 KiB through the memory budget guard.
constexpr std::size_t gpuCommandBufferBytes=384u*1024u;
// 32 spike clocks plus eight SimpleRock bodies/configs; ordinary static heap.
// Additional source frames share the existing eight resident texture pages.
constexpr std::size_t roomDynamicsStateBudgetBytes=8u*1024u;
// Three scenery plant configs/clocks reuse resident sprites; ordinary RAM only.
constexpr std::size_t passivePlantStateBudgetBytes=2u*1024u;
// Geo plus cocoon use at most two 512x512 RGBA8 pages. The second page is
// admitted from existing headroom; the total linear heap remains 42 MiB.
constexpr std::size_t geoTexturePageBytes=512u*512u*4u;
// Geo chips share page zero: no added linear/VRAM allocations. Fixed CPU pool.
constexpr std::size_t geoChipsStateBudgetBytes=48u*1024u;
// Native grass PCM is 1.10 MiB. The previous ordinary heap measured 85.38 MiB
// free after loading; transfer 2 MiB and retain the audio admission reserve.
constexpr std::size_t grassLinearHeapBytes=2u*1024u*1024u;
// Great Door: one 1024x512 RGBA8 atlas (half source resolution, 32 texels/unit)
// plus ~344 KiB native PCM. Round up to 3 MiB to preserve admission reserves.
// Source VFX share the resident decor page. 1,152 extra fixed particle slots and
// sleep/spin state add <256 KiB ordinary heap; no extra linear allocation or SD IO.
constexpr std::size_t greatDoorLinearHeapBytes=3u*1024u*1024u;
// HUD: the complete limiter art and LCD text need 768 KiB across RGBA8/LA8
// pages. Keep the existing 512 KiB heap transfer; the additional 256 KiB uses
// measured headroom while preserving the mandatory 1 MiB linear reserve.
// Eight Sly stock icons add one 128x64 RGBA8 atlas (32 KiB).
constexpr std::size_t slyShopTextureBudgetBytes=128u*64u*4u;
constexpr std::size_t hudTextureBudgetBytes=768u*1024u+slyShopTextureBudgetBytes;
// Resident RGB8 shaft and RGBA4 font; excludes the title-only logo.
constexpr std::size_t hudMenuTextureBudgetBytes=512u*1024u;
constexpr std::size_t hudLinearHeapBytes=512u*1024u;
constexpr std::size_t hudLinearReserveBytes=1024u*1024u;
// RGBA8 map target uses VRAM, no depth/stencil buffer. Existing LCD
// double buffers were already allocated by gfxInitDefault.
constexpr std::size_t hudTargetVramBytes=0;
constexpr std::size_t mapTargetVramBytes=320u*240u*4u;
constexpr std::size_t lightVramReserveBytes=mapTargetVramBytes+512u*1024u;
constexpr bool lightVramFits(std::size_t freeBytes,std::size_t textureBytes) {
    return freeBytes>=lightVramReserveBytes && textureBytes<=freeBytes-lightVramReserveBytes;
}
// ScreenEffects keeps one 1024x512 RGBA8 target in linear memory; the other
// two targets and both bloom textures are allocated in VRAM.
constexpr std::size_t screenEffectsTargetWidth=1024u;
constexpr std::size_t screenEffectsTargetHeight=512u;
constexpr std::size_t screenEffectsLinearTargetBytes=screenEffectsTargetWidth*screenEffectsTargetHeight*4u;
// AudioPlayer admission limits are shared with the build-time resource audit.
// The native resident bank includes hero death and the short opening cues.
// OpeningSting uses the bounded stream below instead of resident PCM.
// The extra 512 KiB is supplied by tabletLinearHeapBytes below.
// Seven native drip/platform clips add about 446 KiB; transfer 512 KiB from
// ordinary heap to retain the measured linear admission reserve.
constexpr std::size_t contactEffectsLinearHeapBytes=512u*1024u;
constexpr std::size_t enemyAudioBankCapBytes=15u*1024u*1024u;
// Three source stereo stalactite cues add 389 KiB. Transfer 512 KiB from
// ordinary memory so the native PCM bank retains its 1 MiB admission reserve.
constexpr std::size_t stalactiteAudioLinearHeapBytes=512u*1024u;
constexpr std::size_t stalactiteSpriteCapacity=8;
constexpr std::size_t enemyAudioLinearReserveBytes=1024u*1024u;
// Three native stereo 44.1 kHz queues, 3 x 92.88 ms each; no resident full clips.
constexpr std::size_t windStreamChunkBytes=16u*1024u;
constexpr std::size_t windStreamBufferBytes=3u*3u*windStreamChunkBytes;
// One initial chunk per stream in ordinary heap removes startup/retry RomFS I/O.
constexpr std::size_t windStreamStartCacheBytes=3u*windStreamChunkBytes;
constexpr std::size_t windStreamStackBytes=16u*1024u;
// One native stereo 44.1 kHz one-shot, serviced by the existing wind worker.
// It borrows its logical voice's NDSP channel; no extra channel or thread.
constexpr std::size_t openingStreamBufferBytes=3u*windStreamChunkBytes;
// Discovery overlaps the opening sting without adding resident PCM.
constexpr std::size_t secretStreamBufferBytes=3u*windStreamChunkBytes;
// Streamed cues, including death details, prime 279 ms at audio load. Ordinary
// heap only; playback keeps the existing two linear queues and worker.
constexpr std::size_t oneShotStartCacheBytes=13u*openingStreamBufferBytes;
// One 1 MiB RGBA4 Shade page plus 1,709,352 B native PCM. The 3 MiB
// transfer retains the 1 MiB reserve; ordinary heap previously had 85.38 MiB.
// The lossless two-page Shade atlas returns 128 KiB to ordinary memory.
constexpr std::size_t shadeLinearHeapBytes=3u*1024u*1024u-128u*1024u;
// Native Shade particles share the existing atlas; fixed ordinary-memory pool.
constexpr std::size_t shadePresentationStateBudgetBytes=48u*1024u;
// Fury: RGBA8 art and LA8 text pages total 1 MiB; <1 MiB native PCM.
// The existing 3 MiB transfer preserves the linear reserve.
// The combined split's ordinary headroom is reported by the package audit.
constexpr std::size_t furyLinearHeapBytes=3u*1024u*1024u;
// Fury's 384 billboard particles share its existing atlas; ordinary heap only.
constexpr std::size_t furyParticleStateBudgetBytes=64u*1024u;
// Three tablets share hum/wake/inspection PCM (<1.1 MiB, native 48/44.1 kHz).
// Transfer 1.5 MiB from the ordinary heap; preserve the existing linear reserve.
constexpr std::size_t tabletLinearHeapBytes=1536u*1024u;
// Two RGBA4 event pages and native event voices. The combined package needs a
// 6 MiB transfer preserves the 1 MiB linear admission reserve.
// Bounded actors/projectiles allocate nothing per frame.
constexpr std::size_t conditionalLinearHeapBytes=6u*1024u*1024u;
// Native death PCM adds 2,023,756 bytes; artwork fits existing hero pages.
constexpr std::size_t heroDeathLinearHeapBytes=1024u*1024u;
constexpr std::size_t heroDeathStateBudgetBytes=96u*1024u;
// Compact decor/event pages recover 2.5 MiB without changing pixels. Return
// 1 MiB to ordinary memory; the rest admits the combined native death bank.
constexpr std::size_t compactAtlasHeapReturnBytes=1024u*1024u;
// Bounded sting streaming returns 3.75 MiB of the previous 4 MiB transfer.
// The combined room/actor transfers below consume part of that headroom;
// current linear and ordinary reserves are checked by the package audit.
constexpr std::size_t openingAudioLinearHeapBytes=256u*1024u;
// Ability art (512 KiB) and 22.05 kHz spell PCM (370,074 B). Transfer
// 1 MiB from ordinary RAM, retaining the package's 1 MiB linear reserve.
constexpr std::size_t abilityLinearHeapBytes=1024u*1024u;
// Cornifer: two 512x512 RGBA4 pages and 128 KiB LA4 map-atlas growth,
// plus a 16 KiB streamed PCM buffer.
// The shared boss/room allocation already covers this bank; retain the
// ordinary heap reserve instead of transferring another 1,196,032 B.
constexpr std::size_t corniferTextureHeapBytes=1152u*1024u;
constexpr std::size_t corniferStreamBufferBytes=16u*1024u;
// The combined Greenpath bank is one 1024x1024 RGBA4 page, loaded once.
constexpr std::size_t greenpathLinearHeapBytes=2u*1024u*1024u;
// Crossroads_04: seven room pages (14 MiB), three boss pages (6 MiB),
// Gruzzer (256 KiB), enemy-b (2 MiB) and enemy-c (512 KiB). Transfer 3 MiB
// from measured 22,897,920-byte entry headroom to retain the 2 MiB room reserve.
constexpr std::size_t bossSceneLinearHeapBytes=3u*1024u*1024u;
// Native False Knight adds 24 MiB of art and the room adds 2 MiB.
constexpr std::size_t falseKnightLinearHeapBytes=26u*1024u*1024u;
// Room peaks share capacity. The native boss transfer also covers the full
// source-scale room increment; transfer that memory only once.
static_assert(falseKnightLinearHeapBytes>=roomTextureBudget-oldRoomTextureBudget,
              "Source-scale rooms exceed shared boss-room capacity");
// Native Stag atlas plus station room art needs six MiB beyond the boss peak.
constexpr std::size_t stagLinearHeapBytes=8u*1024u*1024u;
constexpr std::size_t gameLinearHeapBytes=stagLinearHeapBytes+falseKnightLinearHeapBytes+bossSceneLinearHeapBytes+greenpathLinearHeapBytes+abilityLinearHeapBytes+heroDeathLinearHeapBytes-compactAtlasHeapReturnBytes+conditionalLinearHeapBytes+baseLinearHeapBytes+geoTexturePageBytes+grassLinearHeapBytes+greatDoorLinearHeapBytes+hudLinearHeapBytes+shadeLinearHeapBytes+furyLinearHeapBytes+tabletLinearHeapBytes+stalactiteAudioLinearHeapBytes+contactEffectsLinearHeapBytes+openingAudioLinearHeapBytes;
}
