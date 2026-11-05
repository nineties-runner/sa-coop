#pragma once
// Emotes and gestures: animations of the game's own IFP blocks the local player plays on demand, from the wheel
// (EmoteWheel.cpp, a held key) or the chat (/e <name>). Anims.cpp replicates the one playing as an activity.
#include <string>
#include <vector>

namespace coop::emotes {

struct Emote {
    const char* ru;     // names in Russian, comma separated: the first one is shown, any one works in /e
    const char* en;     // the same in English
    const char* block;  // IFP block
    const char* anim;   // animation inside it
    bool looped;        // goes on until the player moves, jumps, aims or picks it again
    bool wheel;         // on the wheel (the others only from the chat)
};

int Count();
const Emote& Get(int i);
std::string Name(int i);              // the first name in the interface language
int Find(const std::string& utf8);    // by any name of either language, -1 when none
// its chat commands (its one-word names): "/dance, /танец", or only the one of the interface language
std::string Commands(int i, bool first = false);
const char* Blocked();                // why no emote can start now (nullptr: it can)
void Play(int i);                     // a looped one that plays now stops instead
void PlayRaw(const char* block, const char* anim, bool looped = true);
void Stop();
int Current();                        // the emote playing or about to, -1 when none
int PlayingBlock();                   // the IFP block of the emote animation playing now, -1 when none
std::string KeyName(int vk);          // a Windows key code as the keyboard names it

// Wheel customization (10 slots)
constexpr int kWheelSlots = 10;
struct WheelItem {
    std::string name;
    std::string block;
    std::string anim;
    bool looped = true;
};

const std::vector<WheelItem>& WheelSlots();
void SetWheelSlot(int slot, const std::string& name, const std::string& block, const std::string& anim, bool looped);
void ResetWheelSlots();
void PlayWheelSlot(int slot);

// Raw game animations access
int AllAnimsCount();
void GetAllAnim(int i, const char*& block, const char*& anim);

}  // namespace coop::emotes
