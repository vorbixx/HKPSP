#pragma once
namespace hk3ds::stagdata {
struct Dialogue {const char* const* pages;unsigned count;};
inline constexpr const char* greeting[] = {"@Stag/STAG_MEET.0","@Stag/STAG_MEET.1","@Stag/STAG_MEET.2"};
inline constexpr unsigned greetingCount = 3;
inline constexpr const char* town[] = {"@Stag/STAG_TOWN.0","@Stag/STAG_TOWN.1"};
inline constexpr unsigned townCount = 2;
inline constexpr const char* crossroads[] = {"@Stag/STAG_CROSSROADS.0","@Stag/STAG_CROSSROADS.1"};
inline constexpr unsigned crossroadsCount = 2;
inline constexpr const char* greenpath[] = {"@Stag/STAG_GREENPATH.0"};
inline constexpr unsigned greenpathCount = 1;
inline constexpr const char* resting[] = {"@Stag/STAG_RESTINGGROUNDS.0","@Stag/STAG_RESTINGGROUNDS.1"};
inline constexpr unsigned restingCount = 2;
inline constexpr const char* fungal[] = {"@Stag/STAG_FUNGUS.0","@Stag/STAG_FUNGUS.1"};
inline constexpr unsigned fungalCount = 2;
inline constexpr const char* gardens[] = {"@Stag/STAG_ROYALGARDENS.0","@Stag/STAG_ROYALGARDENS.1"};
inline constexpr unsigned gardensCount = 2;
inline constexpr const char* ruins1[] = {"@Stag/STAG_RUINS1.0","@Stag/STAG_RUINS1.1"};
inline constexpr unsigned ruins1Count = 2;
inline constexpr const char* ruins2[] = {"@Stag/STAG_RUINS2.0","@Stag/STAG_RUINS2.1"};
inline constexpr unsigned ruins2Count = 2;
inline constexpr const char* deepnest[] = {"@Stag/STAG_DEEPNEST.0"};
inline constexpr unsigned deepnestCount = 1;
inline constexpr const char* nest[] = {"@Stag/STAG_NEST.0","@Stag/STAG_NEST.1"};
inline constexpr unsigned nestCount = 2;
inline constexpr const char* hidden[] = {"@Stag/STAG_HIDDEN.0","@Stag/STAG_HIDDEN.1","@Stag/STAG_HIDDEN.2"};
inline constexpr unsigned hiddenCount = 3;
inline constexpr Dialogue local[]={{town,townCount},{crossroads,crossroadsCount},{greenpath,greenpathCount},{resting,restingCount},{fungal,fungalCount},{gardens,gardensCount},{ruins1,ruins1Count},{ruins2,ruins2Count},{deepnest,deepnestCount},{nest,nestCount},{hidden,hiddenCount}};
}
