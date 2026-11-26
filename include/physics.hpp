#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hk3ds
{
struct Vec2 { float x = 0, y = 0; };
struct Rect { float left, bottom, right, top; };
struct PhysicsMaterial { float friction=0, bounce=0; };
struct Solid { Rect bounds; bool wallSlide = true; bool hardLanding = true; };
struct TerrainShape
{
    static constexpr std::uint32_t queryOnly=32;
    static constexpr std::uint32_t heroExcluded=256, kinematic=512;
    static constexpr std::uint32_t breakableWall=1024;
    static constexpr std::uint32_t leverGate=2048;
    static constexpr std::uint32_t nailQueryExcluded=64;
    static constexpr std::uint32_t terrainQueryExcluded=128;
    std::uint32_t id=0, kind=0, flags=0, first=0, count=0;
    PhysicsMaterial material{};
    bool enabled=true; // Runtime activation; HKT1 stores the intact snapshot.
    bool queryBoundsValid=false; // Opt in only after bounds are computed from vertices.
    float radius=0;
    Rect bounds{};
    // kind: 0 filled CCW polygon, 1 two-sided edge chain (no implicit closure).
    // flags: 1 trigger, 2 NonSlider, 4 suppress hard landing, 8 active NonThunker, 16 Roof,
    // 32 excluded from every exported actor collision layer; 64 outside nail query layers 8/25;
    // 128 outside Terrain layer 8 (Crawler wall/ground/edge rays).
    bool actorContact(bool hero=false) const { return enabled && !(flags & (1u|queryOnly)) && !(hero && (flags&heroExcluded)); }
};
// Defaults from the audited Physics2DSettings of build 22529139.
// Optional caller-owned diagnostics; no storage or allocations in runtime bodies.
// Feature numbers are this port's geometric keys, not Unity's internal IDs.
struct ContactPointSample
{
    // feature low nibble: terrain endpoint 0/1 or edge 2. High nibble:
    // box vertices BL/BR/TR/TL 0..3 or faces left/right/bottom/top 4..7.
    // Together with shape/child this identifies a geometric constraint.
    std::uint32_t shape=0, child=0, feature=0;
    Vec2 point{}, normal{};
    float separation=0;
    bool toi=false;
};
struct ContactDiagnostics
{
    ContactPointSample* points=nullptr;
    std::size_t capacity=0, count=0;
    bool limited=false;
};
struct ContactSettings
{
    bool enabled=false, continuous=true;
    float offset=0.01f, bodyRadius=0, baumgarte=0.2f, toiBaumgarte=0.75f;
    float maxCorrection=0.2f, maxTranslation=100;
    unsigned positionIterations=3, velocityIterations=8;
    ContactDiagnostics* diagnostics=nullptr;
};
struct World
{
    const Solid* solids = nullptr; std::size_t count = 0;
    const TerrainShape* shapes=nullptr; std::size_t shapeCount=0;
    const Vec2* vertices=nullptr;
    bool queriesHitTriggers=true, queriesStartInside=false;
    bool heroContacts=false; // IgnoreHeroCollision affects body contact, not terrain rays.
    ContactSettings contact{};
};
struct SurfaceContact
{
    std::uint32_t id=0; Vec2 normal{}; float normalImpulse=0;
    float separation=0; unsigned points=0; bool toi=false;
    unsigned flags=0;
};
struct Contacts
{
    bool ground = false, roof = false, left = false, right = false;
    bool nonSlider = false, hardLanding = true;
    std::array<SurfaceContact,16> surfaces{};
    std::size_t count=0;
    bool limited=false;
};
Vec2 interpolatePosition(Vec2 previous, Vec2 current, float alpha);
Rect bodyBounds(Vec2 position);
Rect boxBounds(Vec2 position, Rect local, float scaleX = 1);
// Legacy synthetic rectangles retain their X/Y solver. Source terrain uses
// contact manifolds when World.contact.enabled; no full Unity equivalence claim.
Contacts moveBody(Vec2& position, Vec2& velocity, float dt, const World& world);
Contacts moveBox(Vec2& position, Vec2& velocity, Rect local, float dt, const World& world,
                 PhysicsMaterial material = {}, unsigned colliderCopies = 1, float edgeRadius = 0);
// Synthetic Solid rectangles represent Terrain. Contour query exclusions are
// independent of actor contacts; zero retains the unfiltered query policy.
bool rayTerrain(Vec2 origin, Vec2 direction, float length, const World& world, float& distance,
                std::uint32_t excludedFlags=0);
bool sweepBox(Vec2 position, Rect local, Vec2 direction, float length, const World& world, float& distance);
// Exact swept AABB against contour segments; returns the nearest fixture,
// on the exported terrain nail layers (8/25), including triggers/NonThunker
// so callers can apply the source query policy. Query-only Geo Rocks are excluded.
bool castTerrainBox(Vec2 center, Vec2 half, Vec2 direction, float length,
                    const World& world, float& distance, unsigned& flags);
bool overlaps(Rect a, Rect b);
// Box2D box sensors include the two polygon skins (0.01 world units each).
// This is geometric overlap, independent of Enter/Stay lifecycle dispatch.
bool boxSensorOverlap(Rect a, Rect b);
// Geometric box overlap on enabled exported Terrain, including triggers.
// Unlike a ray, a collision sensor ignores queriesHitTriggers/StartInside.
// The caller supplies any collider skin in the box bounds.
bool terrainSensorOverlap(Rect box, const World& world);
// Counterclockwise convex quad, with any collider skin supplied by the caller.
bool terrainQuadOverlap(const std::array<Vec2,4>& quad, const World& world);
}
