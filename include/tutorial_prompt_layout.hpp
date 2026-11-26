#pragma once

namespace hk3ds::promptlayout {
// LCD adaptation: preserve local source proportions, enlarged for reading.
// Native Focus ink 465x434 maps at .4 px/source px, starting at LCD y=58.
inline constexpr float centerX=160, buttonRadius=10;
inline constexpr float reminderY=211, reminderBaseline=216, reminderScale=1.15f;
inline constexpr float reminderGap=12, holdBaseline=198, holdScale=.75f;
// Native "Backboard" (black_fader 126x128 px at 33.6 px/unit = 3.75x3.81 units)
// has local scale 2.958x.615: 11.09x2.34 units centred at x 0.02 (the prompt
// root is x 0). Its y is within 0.1 unit of the prompt row. The native button
// sprite is 0.95 units and the port's button sprite 24 px: 25.26 px/unit.
inline constexpr float reminderBackboardY=212, reminderBackboardWidth=280.2f, reminderBackboardHeight=59.2f;
inline constexpr float focusArtY=136, focusArtScale=2.46f, focusTextScale=.92f;
// Approximation: fit the illustration between the lore frame and instructions
// when both are visible on the 240px-high top screen.
inline constexpr float loreFocusArtY=148, loreFocusArtScale=.66f;
inline constexpr float modalDimOpacity=.92f;
inline constexpr float focusLine1=162, focusLine2=180, focusLastLine=230;
inline constexpr float focusHoldX=140, focusHoldBaseline=207, focusHoldScale=1.15f;
inline constexpr float focusButtonX=182, focusButtonY=202;
}
