// Emotes and gestures (see Emotes.h).
//
// An emote is TASK_PLAY_ANIM on the local player: the task keeps the pad's walk away while it runs, and an emote
// stops (CLEAR_CHAR_TASKS) when the player moves, jumps, aims, fires or gets into a car. Only a press made after the
// emote began counts: a key still held from picking it, or a pad field the game keeps up all along (the first version
// took every emote away a frame after a 250 ms grace), stops nothing. Its IFP block is streamed in
// first (the task needs the animation in memory) and let go once the emote is over, unless it was in memory before.
// The other games see it through Anims.cpp: the animation of the emote's block counts as an activity there.
#include "features/anims/Emotes.h"

#include <windows.h>

#include <cstdlib>
#include <set>
#include <vector>

#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/anims/AllAnims.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::emotes {

namespace {
using namespace game;
using game::script::Cmd;

// the default wheel items clockwise from the top
constexpr Emote kEmotes[] = {
    {TR_BOTH("помахать,привет", "wave,hi"), "ON_LOOKERS", "wave_loop", false, true},
    {TR_BOTH("позвать,сюда", "come,beckon"), "GANGS", "Invite_Yes", false, true},
    {TR_BOTH("жест банды,банда,дэб", "gang sign,gang,dab"), "GHANDS", "gsign1", false, true},
    {TR_BOTH("фак", "finger,fu"), "RIOT", "RIOT_FUKU", false, true},
    {TR_BOTH("аплодировать,хлопать", "clap,applaud"), "DANCING", "bd_clap", true, true},
    {TR_BOTH("танцевать,танец", "dance"), "DANCING", "dnce_M_b", true, true},
    {TR_BOTH("сесть", "sit"), "BEACH", "ParkSit_M_loop", true, true},
    {TR_BOTH("лечь", "lie,lay"), "BEACH", "Lay_Bac_Loop", true, true},
    {TR_BOTH("засмеяться,смех", "laugh"), "RAPPING", "Laugh_01", false, true},
    {TR_BOTH("рука-лицо,фейспалм", "facepalm"), "MISC", "plyr_shkhead", false, true},
    {TR_BOTH("нет,отказ", "no,refuse"), "GANGS", "Invite_No", false, false},
    {TR_BOTH("указать", "point"), "ON_LOOKERS", "point_loop", true, false},
    {TR_BOTH("радоваться,ура", "cheer"), "OTB", "wtchrace_win", false, false},
    {TR_BOTH("тайчи", "tai chi,taichi"), "PARK", "Tai_Chi_Loop", true, false},
};
constexpr int kCount = (int)(sizeof(kEmotes) / sizeof(kEmotes[0]));

constexpr uint64_t kLoadTimeoutMs = 5000;
constexpr uint64_t kStartTimeoutMs = 1000;  // the task's animation shows up on the clump within it

std::wstring Lower(const std::string& utf8) {
    std::wstring w = Utf8ToWide(Trim(utf8));
    if (!w.empty()) CharLowerBuffW(w.data(), (DWORD)w.size());
    return w;
}

std::vector<std::string> Names(const char* list) { return Split(list, ','); }

bool IsStop(const std::wstring& what) {
    for (const char* w : {TR_BOTH("стоп", "stop")})
        if (Lower(w) == what) return true;
    return false;
}

int BlockIndex(const char* name) { return Call<0x4D3990, int>(name); }  // CAnimManager::GetAnimationBlockIndex

// the strongest association of that block on the clump, blending in or full
void* BlockAssociation(void* clump, int block) {
    void* best = nullptr;
    for (void* a = anim::First(clump); a; a = anim::Next(a)) {
        int b, idx;
        if (anim::BlendDelta(a) < 0.0f || !anim::Locate(anim::Hierarchy(a), b, idx) || b != block) continue;
        if (!best || anim::BlendAmount(a) > anim::BlendAmount(best)) best = a;
    }
    return best;
}

// the pad inputs that stop an emote
struct StopKey {
    const char* name;
    int16_t CControllerState::*field;
    int threshold;  // the sticks: a pad's drift is no walk
};
constexpr StopKey kStopKeys[] = {
    {"walk left/right", &CControllerState::LeftStickX, 64},
    {"walk forward/back", &CControllerState::LeftStickY, 64},
    {"jump", &CControllerState::ButtonSquare, 1},
    {"sprint", &CControllerState::ButtonCross, 1},
    {"aim", &CControllerState::RightShoulder1, 1},
    {"fire", &CControllerState::ButtonCircle, 1},
    {"enter a car", &CControllerState::ButtonTriangle, 1},
};

bool InScene() {
    return Ref<uint8_t>(0xB5F851) || cam::WideScreenOn() || GetPad(0)->DisablePlayerControls;  // CCutsceneMgr::ms_running
}

class Emotes : public Module {
public:
    const char* Name() const override { return "Emotes"; }

    void PlayEmote(int i) {
        if (i < 0 || i >= kCount) return;
        PlayAnimation(kEmotes[i].block, kEmotes[i].anim, kEmotes[i].looped, i, kEmotes[i].en);
    }

    void PlayRawAnimation(const char* block, const char* anim, bool looped) {
        PlayAnimation(block, anim, looped, -1, anim);
    }

    void PlayAnimation(const char* blockName, const char* animName, bool looped, int emoteIndex, const char* label) {
        if (!blockName || !animName || !*blockName || !*animName) return;
        if (m_current >= 0 && m_curBlockName == blockName && m_curAnimName == animName && m_curLooped)
            return StopEmote("by picking it again");
        if (const char* why = Blocked()) {
            if (*why) ui::TheChat().AddError(why);
            return;
        }
        StopEmote("for another one");
        m_current = emoteIndex >= 0 ? emoteIndex : 1000;
        m_curBlockName = blockName;
        m_curAnimName = animName;
        m_curLooped = looped;
        m_curLabel = label ? label : animName;
        m_block = BlockIndex(blockName);
        m_since = NowMs();
        if (m_block < 0) {
            LOGW("Emote %s: no IFP block %s", m_curLabel.c_str(), blockName);
            Finish();
        }
    }

    void StopEmote(const char* why = "by the player") {
        if (m_current < 0) return;
        if (m_started) {
            ClearTask();
            LOGI("Emote %s stopped %s", m_curLabel.c_str(), why);
        }
        Finish();
    }

    void OnPostProcess() override {
        if (m_current < 0) return;
        if (!core::IsPlayerReady()) return Finish();
        CPlayerPed* me = FindPlayerPed(0);
        uint64_t now = NowMs();
        if (!m_started) {
            if (Blocked()) return Finish();
            if (!anim::BlockLoaded(m_block)) {
                RequestModel(anim::kIfpBase + m_block, 2);
                m_ours.insert(m_block);
                if (now - m_since < kLoadTimeoutMs) return;
                LOGW("Emote %s: IFP block %s did not load", m_curLabel.c_str(), m_curBlockName.c_str());
                ui::TheChat().AddError(TR("Анимация не загрузилась.", "The animation did not load."));
                return Finish();
            }
            // TASK_PLAY_ANIM: a looped one until stopped, a single one ends with its last frame
            Cmd(0x0605).I(GetPedRef(me)).S(m_curAnimName.c_str()).S(m_curBlockName.c_str()).F(4.0f).I(m_curLooped ? 1 : 0).I(0).I(0).I(0).I(-1).Run();
            m_started = true;
            m_since = now;
            m_released = 0;
            LOGI("Emote %s (%s/%s)", m_curLabel.c_str(), m_curBlockName.c_str(), m_curAnimName.c_str());
            return;
        }
        void* a = BlockAssociation(me->RwObject(), m_block);
        if (a) m_seen = true;
        if (!a && (m_seen || now - m_since > kStartTimeoutMs)) {
            if (!m_seen) LOGW("Emote %s: its animation never showed up (primary task %d)", m_curLabel.c_str(), ActivePrimaryTaskType(me));
            else if (m_curLooped) LOGW("Emote %s: its animation was taken away (primary task %d)", m_curLabel.c_str(), ActivePrimaryTaskType(me));
            else LOGI("Emote %s played to its end", m_curLabel.c_str());
            return Finish();
        }
        if (me->InVehicle()) return StopEmote("in a car");
        if (InScene()) return StopEmote("by a scene");
        const CControllerState& pad = GetPad(0)->NewState;
        for (int k = 0; k < (int)(sizeof(kStopKeys) / sizeof(kStopKeys[0])); ++k) {
            int v = pad.*kStopKeys[k].field;
            if (std::abs(v) < kStopKeys[k].threshold) {
                m_released |= 1u << k;
            } else if (m_released & (1u << k)) {
                LOGI("Emote %s: %s pressed (%d)", m_curLabel.c_str(), kStopKeys[k].name, v);
                return StopEmote("by the pad");
            }
        }
    }

    void OnGameStarted() override {
        Reset();
        LoadWheelCustomization();
    }
    void OnGameShutdown() override { Reset(); }  // the world goes with what it streamed

    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        std::string rest;
        for (auto& a : args) rest += (rest.empty() ? "" : " ") + a;
        if (cmd == "e" || cmd == "emote" || cmd == "emotes") {
            std::wstring what = Lower(rest);
            if (what.empty()) List();
            else if (IsStop(what)) StopEmote();
            else if (int i = Find(rest); i >= 0) PlayEmote(i);
            else if (!rest.empty()) {
                // Try to find raw anim by block and name, e.g. "DANCING dnce_m_b" or "bd_clap"
                auto parts = Split(rest, ' ');
                if (parts.size() >= 2) {
                    PlayRawAnimation(parts[0].c_str(), parts[1].c_str(), true);
                    return true;
                }
                // Try searching across all anims by name
                for (int k = 0; k < (int)(sizeof(kAllGameAnims) / sizeof(kAllGameAnims[0])); ++k) {
                    if (!_stricmp(kAllGameAnims[k].anim, rest.c_str())) {
                        PlayRawAnimation(kAllGameAnims[k].block, kAllGameAnims[k].anim, true);
                        return true;
                    }
                }
                ui::TheChat().AddError(Format(TR("Нет такой эмоции: %s. Список: /e", "There is no such emote: %s. The list: /e"),
                                              rest.c_str()));
            }
            return true;
        }
        if (!rest.empty()) return false;
        int i = Find(cmd);  // the short ones: /dance, /wave, /танец
        if (i < 0) return false;
        PlayEmote(i);
        return true;
    }

    int Current() const { return m_current; }
    int PlayingBlock() const { return m_current >= 0 && m_started ? m_block : -1; }

    const std::vector<WheelItem>& GetWheelSlots() {
        if (m_wheelSlots.empty()) LoadWheelCustomization();
        return m_wheelSlots;
    }

    void SetWheelSlot(int slot, const std::string& name, const std::string& block, const std::string& anim, bool looped) {
        if (m_wheelSlots.empty()) LoadWheelCustomization();
        if (slot < 0 || slot >= kWheelSlots) return;
        m_wheelSlots[slot] = WheelItem{name, block, anim, looped};
        SaveWheelCustomization();
    }

    void ResetWheelSlots() {
        InitDefaultWheelSlots();
        SaveWheelCustomization();
    }

    void PlayWheelSlot(int slot) {
        if (m_wheelSlots.empty()) LoadWheelCustomization();
        if (slot < 0 || slot >= (int)m_wheelSlots.size()) return;
        const auto& it = m_wheelSlots[slot];
        PlayAnimation(it.block.c_str(), it.anim.c_str(), it.looped, -1, it.name.c_str());
    }

    static Emotes& Instance() {
        static Emotes e;
        return e;
    }

private:
    void InitDefaultWheelSlots() {
        m_wheelSlots.clear();
        for (int i = 0; i < kCount && (int)m_wheelSlots.size() < kWheelSlots; ++i) {
            if (kEmotes[i].wheel) {
                m_wheelSlots.push_back(WheelItem{
                    emotes::Name(i),
                    kEmotes[i].block,
                    kEmotes[i].anim,
                    kEmotes[i].looped
                });
            }
        }
    }

    void LoadWheelCustomization() {
        InitDefaultWheelSlots();
        Ini ini;
        if (!ini.Load(Cfg().iniPath)) return;
        bool hasCustom = false;
        for (int i = 0; i < kWheelSlots; ++i) {
            std::string key = "slot" + std::to_string(i);
            std::string val = ini.Get("wheel", key, "");
            if (val.empty()) continue;
            // format: "Name|BLOCK|anim|looped"
            auto parts = Split(val, '|');
            if (parts.size() >= 3) {
                hasCustom = true;
                std::string name = parts[0];
                std::string block = parts[1];
                std::string anim = parts[2];
                bool looped = parts.size() > 3 ? (parts[3] == "1" || parts[3] == "true") : true;
                if (i < (int)m_wheelSlots.size()) m_wheelSlots[i] = WheelItem{name, block, anim, looped};
            }
        }
        if (hasCustom) LOGI("Loaded custom emote wheel from coop.ini");
    }

    void SaveWheelCustomization() {
        Ini ini;
        ini.Load(Cfg().iniPath);
        for (int i = 0; i < kWheelSlots && i < (int)m_wheelSlots.size(); ++i) {
            std::string key = "slot" + std::to_string(i);
            const auto& item = m_wheelSlots[i];
            std::string val = item.name + "|" + item.block + "|" + item.anim + "|" + (item.looped ? "1" : "0");
            ini.Set("wheel", key, val);
        }
        ini.Save(Cfg().iniPath);
        LOGI("Saved custom emote wheel to coop.ini");
    }

    // each emote with its commands, a few to a line
    void List() {
        ui::TheChat().AddSystem(Format(TR("Эмоции: напишите команду или /e <название>, /e стоп останавливает. Колесо: зажмите %s.",
                                          "Emotes: type the command or /e <name>, /e stop stops. The wheel: hold %s."),
                                       KeyName(Cfg().emoteKey).c_str()));
        std::string line;
        for (int i = 0; i < kCount; ++i) {
            line += (line.empty() ? "" : "; ") + emotes::Name(i) + " — " + Commands(i);
            if (i % 4 == 3 || i == kCount - 1) {
                ui::TheChat().AddSystem(line);
                line.clear();
            }
        }
    }

    static void ClearTask() {
        if (CPlayerPed* me = FindPlayerPed(0)) Cmd(0x0687).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS: blends it out
    }

    // the emote is over: its block goes back to the streamer when we brought it in and nothing of ours plays it
    void Finish() {
        if (m_block >= 0 && m_ours.count(m_block)) {
            Call<0x409C10>(anim::kIfpBase + m_block);  // CStreaming::SetModelIsDeletable (the association holds it)
            m_ours.erase(m_block);
        }
        m_current = m_block = -1;
        m_curBlockName.clear();
        m_curAnimName.clear();
        m_curLabel.clear();
        m_started = m_seen = false;
    }

    void Reset() {
        m_current = m_block = -1;
        m_curBlockName.clear();
        m_curAnimName.clear();
        m_curLabel.clear();
        m_started = m_seen = false;
        m_ours.clear();
    }

    int m_current = -1, m_block = -1;
    std::string m_curBlockName, m_curAnimName, m_curLabel;
    bool m_curLooped = true;
    bool m_started = false, m_seen = false;
    uint64_t m_since = 0;
    uint32_t m_released = 0;  // the stop inputs let go since the emote began: pressing one again stops it
    std::set<int> m_ours;  // blocks we requested that the game did not have in memory
    std::vector<WheelItem> m_wheelSlots;
};

struct Reg {
    Reg() { core::RegisterModule(&Emotes::Instance()); }
} g_reg;
}  // namespace

std::string KeyName(int vk) {
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return Format("F%d", vk - VK_F1 + 1);
    wchar_t buf[64];
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), buf, 64) > 0) return WideToUtf8(buf);
    return Format("0x%02X", vk);
}

int Count() { return kCount; }
const Emote& Get(int i) { return kEmotes[i]; }

std::string Name(int i) { return Names(Cfg().english ? kEmotes[i].en : kEmotes[i].ru)[0]; }

std::string Commands(int i, bool first) {
    std::string out;
    const char* ru = kEmotes[i].ru;
    const char* en = kEmotes[i].en;
    // both with the Latin one first, or only the interface language's
    const char* order[] = {first && !Cfg().english ? ru : en, first && !Cfg().english ? en : ru};
    for (const char* list : order) {
        for (auto& n : Names(list))
            if (n.find(' ') == std::string::npos) {
                out += (out.empty() ? "/" : ", /") + n;
                break;
            }
        if (first && !out.empty()) break;
    }
    return out;
}

int Find(const std::string& utf8) {
    std::wstring want = Lower(utf8);
    if (want.empty()) return -1;
    for (int i = 0; i < kCount; ++i)
        for (const char* list : {kEmotes[i].ru, kEmotes[i].en})
            for (auto& n : Names(list))
                if (Lower(n) == want) return i;
    return -1;
}

const char* Blocked() {
    if (!core::IsPlayerReady() || MenuActive()) return "";
    CPlayerPed* me = FindPlayerPed(0);
    if (me->Health() <= 0.0f) return "";
    if (me->InVehicle()) return TR("В машине эмоции недоступны.", "Emotes are not available in a car.");
    if (Cmd(0x04AD).I(GetPedRef(me)).Run())  // IS_CHAR_IN_WATER
        return TR("В воде эмоции недоступны.", "Emotes are not available in the water.");
    if (!(me->PedFlags() & 1) || me->IsInTheAir())  // CPed::bIsStanding
        return TR("В прыжке и в падении эмоции недоступны.", "Emotes are not available while jumping or falling.");
    if (InScene()) return TR("Во время сцены эмоции недоступны.", "Emotes are not available during a scene.");
    return nullptr;
}

void Play(int i) { Emotes::Instance().PlayEmote(i); }
void PlayRaw(const char* block, const char* anim, bool looped) { Emotes::Instance().PlayRawAnimation(block, anim, looped); }
void Stop() { Emotes::Instance().StopEmote(); }
int Current() { return Emotes::Instance().Current(); }
int PlayingBlock() { return Emotes::Instance().PlayingBlock(); }

const std::vector<WheelItem>& WheelSlots() { return Emotes::Instance().GetWheelSlots(); }
void SetWheelSlot(int slot, const std::string& name, const std::string& block, const std::string& anim, bool looped) {
    Emotes::Instance().SetWheelSlot(slot, name, block, anim, looped);
}
void ResetWheelSlots() { Emotes::Instance().ResetWheelSlots(); }
void PlayWheelSlot(int slot) { Emotes::Instance().PlayWheelSlot(slot); }

int AllAnimsCount() { return (int)(sizeof(kAllGameAnims) / sizeof(kAllGameAnims[0])); }
void GetAllAnim(int i, const char*& block, const char*& anim) {
    if (i < 0 || i >= AllAnimsCount()) {
        block = anim = "";
        return;
    }
    block = kAllGameAnims[i].block;
    anim = kAllGameAnims[i].anim;
}

}  // namespace coop::emotes
