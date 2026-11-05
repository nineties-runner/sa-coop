#pragma once

namespace coop::ui {

// The F3 / animations window: browse and search all 1745 animations of the game across 129 IFP blocks.
// Players can search by name/block, play any animation (looped or once), assign animations to any of the 10 slots
// on the emote wheel (J), or reset the wheel to defaults. Wheel customization is saved in coop\coop.ini ([wheel]).
void SetAnimsMenuVisible(bool visible);
bool IsAnimsMenuVisible();
void ToggleAnimsMenu();
void RenderAnimsMenu();

}  // namespace coop::ui
