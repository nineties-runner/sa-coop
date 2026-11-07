#include "pages/anims-menu/AnimsMenu.h"

#include <windows.h>

#include <algorithm>

#include <string>

#include <vector>

#include "common/lib/util/Util.h"

#include "entities/player/Players.h"

#include "features/anims/Emotes.h"

#include "shared/game/Anim.h"

#include "shared/config/Config.h"

#include "shared/game/Game.h"

#include "shared/game/Script.h"

#include "shared/lib/core/Core.h"

#include "shared/ui/chat/Chat.h"

#include "shared/ui/input/Input.h"

#include "shared/ui/renderer/Renderer.h"

#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {

using w::kSaBlue, w::kSaMuted, w::kSaGreen, w::kSaRed, w::kSaLine;

constexpr uint32_t kText = 0xFFE6E9EF;

bool g_visible = false;

std::wstring g_search;

std::wstring g_lastSearch;

std::vector<int> g_filtered;

int g_scroll = 0;

int g_selected = -1;       // index into g_filtered

int g_assignSlot = -1;     // 0..9 if assigning to slot, or -1

bool g_looped = true;

float g_rotAngle = 0.0f;

bool g_camAttached = false;

bool g_dragRot = false;

float g_dragLastX = 0.0f;

int g_previewPedHandle = -1;

game::CVector g_previewPos{};

float g_previewHeading = 0.0f;

std::string g_previewBlock;

std::string g_previewAnim;

bool g_previewLooped = true;

bool g_previewStarted = false;  // the animation was given to the ped: re-giving it every frame froze it on frame 0

void DestroyPreviewPed() {

    if (g_previewPedHandle >= 0) {

        if (game::GetPed(g_previewPedHandle)) {

            game::script::Cmd(0x009B).I(g_previewPedHandle).Run();  // DELETE_CHAR

        }

        g_previewPedHandle = -1;

    }

}

void UpdatePreviewPedAnim() {
    if (g_previewPedHandle < 0 || g_previewBlock.empty() || g_previewAnim.empty()) return;
    int blockIdx = game::Call<0x4D3990, int>(g_previewBlock.c_str());
    if (blockIdx < 0) return;
    if (!game::anim::BlockLoaded(blockIdx)) {
        game::RequestModel(game::anim::kIfpBase + blockIdx, 2);
        return;
    }
    if (g_previewStarted) return;
    g_previewStarted = true;
    game::script::Cmd(0x0792).I(g_previewPedHandle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    game::script::Cmd(0x0605).I(g_previewPedHandle).S(g_previewAnim.c_str()).S(g_previewBlock.c_str()).F(4.0f).I(g_previewLooped ? 1 : 0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM
}
void PlayPreviewAnim(const char* block, const char* anim, bool loop) {

    if (!block || !anim) return;

    g_previewBlock = block;

    g_previewAnim = anim;

    g_previewLooped = loop;

    g_previewStarted = false;

    UpdatePreviewPedAnim();

}

void SetupPreviewCamera() {

    if (!core::IsPlayerReady()) return;

    auto* me = game::FindPlayerPed(0);

    if (!me) return;

    if (me->InVehicle() && me->Vehicle()) {
        me->Vehicle()->MoveSpeed() = {0.0f, 0.0f, 0.0f};
        me->Vehicle()->TurnSpeed() = {0.0f, 0.0f, 0.0f};
    }

    if (g_previewPedHandle < 0) {

        // Spawn clone in front of player

        float h = me->CurrentRotation();

        game::CVector p = (me->InVehicle() && me->Vehicle()) ? me->Vehicle()->GetPosition() : me->GetPosition();

        float fwdDist = 3.5f;

        g_previewPos = {p.x + std::sin(h) * fwdDist, p.y - std::cos(h) * fwdDist, p.z};

        g_previewHeading = h + 3.14159265f; // facing towards original camera

        int model = me->ModelIndex();

        if (!game::IsModelLoaded(model)) game::LoadModelNow(model);

        game::script::Cmd(0x009A).I(24).I(model).F(g_previewPos.x).F(g_previewPos.y).F(g_previewPos.z - 1.0f).Out(&g_previewPedHandle).Run();  // CREATE_CHAR

        if (g_previewPedHandle >= 0) {

            game::script::Cmd(0x060B).I(g_previewPedHandle).I(65542).Run();  // SET_CHAR_DECISION_MAKER Empty

            game::script::Cmd(0x02AB).I(g_previewPedHandle).I(1).I(1).I(1).I(1).I(1).Run();  // SET_CHAR_PROOFS

            game::script::Cmd(0x0619).I(g_previewPedHandle).I(0).Run();  // SET_CHAR_COLLISION off

            game::script::Cmd(0x04B8).I(g_previewPedHandle).I(1).Run();  // SET_CHAR_STAY_IN_SAME_PLACE

            game::script::Cmd(0x0173).I(g_previewPedHandle).F(g_previewHeading * 57.29578f).Run();  // SET_CHAR_HEADING

        }

    }

    if (g_previewPedHandle >= 0) {

        float rotH = g_previewHeading + g_rotAngle;

        game::script::Cmd(0x0173).I(g_previewPedHandle).F(rotH * 57.29578f).Run();

        float dist = 3.6f;

        float camX = g_previewPos.x - std::sin(g_previewHeading) * dist;

        float camY = g_previewPos.y + std::cos(g_previewHeading) * dist;

        float camZ = g_previewPos.z + 0.05f;

        float lookX = g_previewPos.x;

        float lookY = g_previewPos.y;

        float lookZ = g_previewPos.z - 0.15f;

        // slide the view left so the ped sits in the right-hand preview window, not behind the list
        float sx = std::cos(g_previewHeading) * 1.35f, sy = std::sin(g_previewHeading) * 1.35f;
        camX += sx, lookX += sx, camY += sy, lookY += sy;
        game::CVector src{camX, camY, camZ}, look{lookX, lookY, lookZ};
        if (!g_camAttached) {  // opcodes once (as Pool): repeating CAMERA_RESET_NEW_SCRIPTABLES each frame blanked the camera to 0,0,0
            game::script::Cmd(0x015F).F(camX).F(camY).F(camZ).F(0.0f).F(0.0f).F(0.0f).Run();  // SET_FIXED_CAMERA_POSITION
            game::script::Cmd(0x0160).F(lookX).F(lookY).F(lookZ).I(2).Run();                  // POINT_CAMERA_AT_POINT
            game::script::Cmd(0x0925).Run();                                                   // CAMERA_RESET_NEW_SCRIPTABLES
            game::script::Cmd(0x0930).I(1).Run();                                              // CAMERA_PERSIST_POS
            game::script::Cmd(0x092F).I(1).Run();                                              // CAMERA_PERSIST_TRACK
        }
        game::Ref<game::CVector>(game::addr::TheCamera + 0xCE0) = src;   // m_vecMoveLinear
        game::Ref<game::CVector>(game::addr::TheCamera + 0xC80) = look;  // m_vecTrackLinear
        g_camAttached = true;

    }

}

void RestorePreviewCamera() {

    DestroyPreviewPed();

    g_previewBlock.clear();

    g_previewAnim.clear();

    if (!g_camAttached) return;

    g_camAttached = false;

    game::script::Cmd(0x0930).I(0).Run();

    game::script::Cmd(0x092F).I(0).Run();

    game::script::Cmd(0x0925).Run();

    game::script::Cmd(0x02EB).Run(); // RESTORE_CAMERA_JUMPCUT

    game::script::Cmd(0x0373).Run(); // SET_CAMERA_BEHIND_PLAYER

}

std::string g_statusMsg;

uint64_t g_statusUntil = 0;

void Status(const std::string& msg) {

    g_statusMsg = msg;

    g_statusUntil = NowMs() + 4000;

}

std::wstring ToLowerW(const std::wstring& s) {

    std::wstring out = s;

    if (!out.empty()) CharLowerBuffW(out.data(), (DWORD)out.size());

    return out;

}

void UpdateFilter() {

    g_filtered.clear();

    std::string qUtf8 = ToLower(Trim(WideToUtf8(g_search)));

    int total = emotes::AllAnimsCount();

    if (qUtf8.empty()) {

        g_filtered.resize(total);

        for (int i = 0; i < total; ++i) g_filtered[i] = i;

    } else {

        for (int i = 0; i < total; ++i) {

            const char* block = nullptr;

            const char* anim = nullptr;

            emotes::GetAllAnim(i, block, anim);

            std::string combined = ToLower(std::string(block) + " " + anim);

            if (combined.find(qUtf8) != std::string::npos) {

                g_filtered.push_back(i);

            }

        }

    }

    g_scroll = 0;

    if (g_selected >= (int)g_filtered.size()) g_selected = g_filtered.empty() ? -1 : 0;

}

bool Hovered(float x, float y, float w, float h) {

    auto& st = In().State();

    return st.mouseX >= x && st.mouseX < x + w && st.mouseY >= y && st.mouseY < y + h;

}

}  // namespace

void SetAnimsMenuVisible(bool visible) {

    if (visible == g_visible) return;

    g_visible = visible;

    g_assignSlot = -1;

    if (visible) {

        if (TheChat().IsOpen()) TheChat().Close();

        g_rotAngle = 0.0f;

        g_dragRot = false;

        UpdateFilter();

        SetupPreviewCamera();

    } else {

        RestorePreviewCamera();

    }

    if (!visible && core::ConnectMenuVisible()) return;

    In().SetKeyboardCapture(visible);

    In().SetMouseCapture(visible);

}

bool IsAnimsMenuVisible() { return g_visible; }

void ToggleAnimsMenu() {

    if (g_visible) return SetAnimsMenuVisible(false);

    if (game::IsForeground() && core::IsPlayerReady() && !game::MenuActive() && !core::ConnectMenuVisible()) {

        SetAnimsMenuVisible(true);

    }

}

void RenderAnimsMenu() {

    UpdatePreviewPedAnim();

    if (!g_visible) return;

    if (!core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible()) {

        return SetAnimsMenuVisible(false);

    }

    if (g_search != g_lastSearch) {

        g_lastSearch = g_search;

        UpdateFilter();

    }

    auto& g = Gfx();

    float s = g.Scale();

    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU);

    SetupPreviewCamera();

    float W = std::min(680 * s, g.Width() * 0.58f);

    float H = std::min(640 * s, g.Height() - 40 * s);

    float x = 24 * s, y = (g.Height() - H) * 0.5f;

    float pad = 18 * s;

    float cx = x + pad, cy = y + 16 * s, cw = W - 2 * pad;

    // 3D Viewport on the right

    float viewX = x + W + 16 * s;

    float viewY = y;

    float viewW = g.Width() - viewX - 24 * s;

    float viewH = H;

    auto& inSt = In().State();

    if (Hovered(viewX, viewY, viewW, viewH)) {

        if (inSt.lmbPressed) {

            g_dragRot = true;

            g_dragLastX = inSt.mouseX;

        }

    }

    if (!inSt.lmb) g_dragRot = false;

    if (g_dragRot) {

        float dx = inSt.mouseX - g_dragLastX;

        g_dragLastX = inSt.mouseX;

        g_rotAngle += (float)dx * 0.015f;

    }

    w::BeginFrame();

    g.Rect(x, y, W, H, 0xF0000000);

    g.RectOutline(x, y, W, H, kSaLine, std::max(1.0f, s));

    g.Rect(viewX, viewY, viewW, viewH, 0x20000000);

    g.RectOutline(viewX, viewY, viewW, viewH, 0xFF4A5A6B, std::max(1.0f, s));

    g.Text(FONT_MENU_SMALL, viewX + 12 * s, viewY + 10 * s, TR("3D Превью (тяни мышью для поворота)", "3D Live Preview (drag mouse to rotate)"), kSaBlue, TEXT_SHADOW);

    // Title

    g.Text(FONT_MENU, cx, cy, TR("Анимации и Колесо Эмоций", "Animations & Emote Wheel"), kSaBlue, TEXT_SHADOW);

    std::string countStr = Format(TR("Найдено: %d / %d", "Found: %d / %d"), (int)g_filtered.size(), emotes::AllAnimsCount());

    g.Text(FONT_SMALL, cx + cw, cy + (mh - sh) * 0.5f, countStr, kSaMuted, TEXT_RIGHT | TEXT_SHADOW);

    cy += mh + 12 * s;

    // Search bar & Looped checkbox

    float searchW = cw * 0.65f;

    float rowH = lh + 10 * s;

    w::TextBox("anim_search", cx, cy, searchW, rowH, g_search, 40, false, false, TR("Поиск (блок или имя анимации)...", "Search (block or animation name)..."));

    float chkX = cx + searchW + 16 * s;

    w::Checkbox("anim_looped", chkX, cy + (rowH - lh) * 0.5f, TR("Зацикленно", "Looped"), g_looped);

    cy += rowH + 8 * s;

    // Movement / Walk-Run-Idle Animation Set selector
    float walkLabelW = g.TextWidth(FONT_SMALL, TR("Сет походки / бега:", "Walk / Run set:")) + 10 * s;
    g.Text(FONT_SMALL, cx, cy + (rowH - sh) * 0.5f, TR("Сет походки / бега:", "Walk / Run set:"), kSaMuted, TEXT_SHADOW);
    int curStyle = std::clamp(Cfg().animStyle, 0, Players::AnimStyleCount() - 1);
    std::string styleLabel = Format("%d. %s", curStyle, Players::AnimStyleName(curStyle));
    if (int d = w::Selector("walk_style", cx + walkLabelW, cy, cw - walkLabelW, rowH, styleLabel)) {
        int count = Players::AnimStyleCount();
        int next = (curStyle + d + count) % count;
        Players::Instance().SetLocalAnimStyle(next);
        auto* me = game::FindPlayerPed(0);
        const char* pb = nullptr;
        const char* pa = nullptr;
        Players::GetAnimStylePreview(next, Cfg().skin, me ? me->ModelIndex() : 0, pb, pa);
        PlayPreviewAnim(pb, pa, true);
        Status(Format(TR("Сет анимаций походки/бега: %s", "Walk/run animation set: %s"), Players::AnimStyleName(next)));
    }

    cy += rowH + 10 * s;

    // List area & Side panel (Wheel slots)

    float sideW = 210 * s;

    float listW = cw - sideW - 14 * s;

    float bottomH = 56 * s;

    float listH = H - (cy - y) - bottomH;

    // --- DRAW ANIMATION LIST ---

    g.Rect(cx, cy, listW, listH, 0xFF090C10);

    g.RectOutline(cx, cy, listW, listH, kSaLine, std::max(1.0f, s));

    float itemH = lh + 6 * s;

    int visibleCount = (int)(listH / itemH);

    // Mouse wheel scrolling

    if (Hovered(cx, cy, listW, listH)) {

        if (In().State().wheel > 0) g_scroll = std::max(0, g_scroll - 3);

        else if (In().State().wheel < 0) g_scroll = std::max(0, std::min((int)g_filtered.size() - visibleCount, g_scroll + 3));

    }

    if (g_scroll > (int)g_filtered.size() - visibleCount) g_scroll = std::max(0, (int)g_filtered.size() - visibleCount);

    float itemY = cy;

    for (int k = 0; k < visibleCount && (g_scroll + k) < (int)g_filtered.size(); ++k) {

        int idx = g_filtered[g_scroll + k];

        const char* block = nullptr;

        const char* anim = nullptr;

        emotes::GetAllAnim(idx, block, anim);

        bool isSel = (g_selected == g_scroll + k);

        bool hover = Hovered(cx, itemY, listW, itemH);

        if (isSel) g.Rect(cx, itemY, listW, itemH, 0xE026303B);

        else if (hover) g.Rect(cx, itemY, listW, itemH, 0x8016202A);

        if (hover && In().State().lmbPressed) {

            if (g_selected != g_scroll + k) {

                g_selected = g_scroll + k;

                const char* pb = nullptr;

                const char* pa = nullptr;

                emotes::GetAllAnim(g_filtered[g_selected], pb, pa);

                PlayPreviewAnim(pb, pa, g_looped);

            }

        }

        std::string bStr = block;

        std::string aStr = anim;

        g.Text(FONT_NORMAL, cx + 8 * s, itemY + 3 * s, aStr, isSel ? kSaBlue : (hover ? 0xFFFFFFFF : kText), TEXT_SHADOW);

        g.Text(FONT_SMALL, cx + listW - 8 * s, itemY + 4 * s, bStr, kSaMuted, TEXT_RIGHT | TEXT_SHADOW);

        itemY += itemH;

    }

    // Scrollbar indicator

    if ((int)g_filtered.size() > visibleCount && visibleCount > 0) {

        float barH = std::max(12 * s, listH * visibleCount / (float)g_filtered.size());

        float barY = cy + (listH - barH) * (g_scroll / (float)(g_filtered.size() - visibleCount));

        g.Rect(cx + listW - 4 * s, barY, 4 * s, barH, kSaMuted);

    }

    // --- DRAW SIDE PANEL (WHEEL SLOTS & ACTIONS) ---

    float spX = cx + listW + 16 * s;

    float spY = cy;

    g.Text(FONT_MENU_SMALL, spX, spY, TR("Колесо (10 слотов)", "Wheel (10 slots)"), kSaBlue, TEXT_SHADOW);

    spY += sh + 6 * s;

    const auto& slots = emotes::WheelSlots();

    float actionBtnsH = (rowH + 4 * s) * 2;

    float slotRowH = std::max(18 * s, (listH - (sh + 6 * s) - actionBtnsH - 12 * s) / 10.0f);

    for (int i = 0; i < emotes::kWheelSlots && i < (int)slots.size(); ++i) {

        bool hover = Hovered(spX, spY, sideW, slotRowH);

        bool isAssigning = (g_assignSlot == i);

        g.Rect(spX, spY, sideW, slotRowH, isAssigning ? 0xC04A5A6B : (hover ? 0xFF16202A : 0xFF0C1117));

        g.RectOutline(spX, spY, sideW, slotRowH, isAssigning ? kSaBlue : (hover ? kSaBlue : kSaLine), std::max(1.0f, s));

        std::string slotLabel = Format("%d: %s", (i + 1) % 10, slots[i].name.c_str());

        g.Text(FONT_SMALL, spX + 6 * s, spY + (slotRowH - sh) * 0.5f, slotLabel, isAssigning ? kSaBlue : (hover ? 0xFFFFFFFF : kText));

        if (hover && In().State().lmbPressed) {

            if (g_selected >= 0 && g_selected < (int)g_filtered.size()) {

                const char* b = nullptr;

                const char* a = nullptr;

                emotes::GetAllAnim(g_filtered[g_selected], b, a);

                emotes::SetWheelSlot(i, a, b, a, g_looped);

                Status(Format(TR("Слот %d: установлен %s", "Slot %d: set to %s"), (i + 1) % 10, a));

                g_assignSlot = -1;

            } else {

                g_assignSlot = (g_assignSlot == i) ? -1 : i;

            }

        }

        spY += slotRowH + 2 * s;

    }

    spY += 4 * s;

    // Play selected button

    std::string playLabel = TR("Проиграть (Enter)", "Play (Enter)");

    bool canPlay = (g_selected >= 0 && g_selected < (int)g_filtered.size());

    if (w::Button("btn_play", spX, spY, sideW, rowH, playLabel, canPlay, canPlay)) {

        if (canPlay) {

            const char* b = nullptr;

            const char* a = nullptr;

            emotes::GetAllAnim(g_filtered[g_selected], b, a);

            emotes::PlayRaw(b, a, g_looped);

            Status(Format(TR("Играет: %s/%s", "Playing: %s/%s"), b, a));

        }

    }

    spY += rowH + 4 * s;

    // Reset wheel button

    std::string resetLabel = TR("Сбросить колесо", "Reset wheel");

    if (w::Button("btn_reset", spX, spY, sideW, rowH, resetLabel)) {

        emotes::ResetWheelSlots();

        Status(TR("Колесо сброшено по умолчанию", "Wheel reset to default"));

    }

    // Enter key plays selected

    for (int vk : In().State().keyDowns) {

        if (vk == VK_RETURN && canPlay) {

            const char* b = nullptr;

            const char* a = nullptr;

            emotes::GetAllAnim(g_filtered[g_selected], b, a);

            emotes::PlayRaw(b, a, g_looped);

            Status(Format(TR("Играет: %s/%s", "Playing: %s/%s"), b, a));

        } else if (vk == VK_UP) {

            if (g_selected > 0) {

                --g_selected;

                if (g_selected < g_scroll) g_scroll = g_selected;

                const char* pb = nullptr;

                const char* pa = nullptr;

                emotes::GetAllAnim(g_filtered[g_selected], pb, pa);

                PlayPreviewAnim(pb, pa, g_looped);

            }

        } else if (vk == VK_DOWN) {

            if (g_selected + 1 < (int)g_filtered.size()) {

                ++g_selected;

                if (g_selected >= g_scroll + visibleCount) g_scroll = g_selected - visibleCount + 1;

                const char* pb = nullptr;

                const char* pa = nullptr;

                emotes::GetAllAnim(g_filtered[g_selected], pb, pa);

                PlayPreviewAnim(pb, pa, g_looped);

            }

        }

    }

    // Bottom bar: Status & Close button

    float by = y + H - 42 * s;

    const char* close = TR("Закрыть (F3)", "Close (F3)");

    float room = mh * 1.4f;

    float closeBtnW = g.TextWidth(FONT_MENU, close) + 2 * room;

    float closeX = cx + cw - closeBtnW;

    bool closing = w::Button("btn_close", closeX, by, closeBtnW, rowH, close);

    if (NowMs() < g_statusUntil && !g_statusMsg.empty()) {

        g.Text(FONT_SMALL, cx, by + 6 * s, g_statusMsg, kSaGreen, TEXT_SHADOW);

    } else {

        std::string hint = TR("Стрелки / мышь: выбор. Клик по слоту: назначить.",

                              "Arrows / mouse: select. Click slot: assign.");

        g.Text(FONT_SMALL, cx, by + 6 * s, hint, kSaMuted, TEXT_SHADOW);

    }

    w::EndFrame();

    w::DrawCursor();

    if (closing) SetAnimsMenuVisible(false);

}

}  // namespace coop::ui

