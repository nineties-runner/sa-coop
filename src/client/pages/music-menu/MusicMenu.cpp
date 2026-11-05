#include "pages/music-menu/MusicMenu.h"

#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/mp3/minimp3_ex.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "features/radio/Radio.h"
#include "features/voice/Adpcm.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop {

namespace {

using namespace game;
using namespace ui::w;
constexpr uint32_t kText = 0xFFE6E9EF;

// Audio constants
constexpr int kMusicRate = 22050; // 22.05 kHz stereo
constexpr int kMusicChannels = 2;
constexpr int kFrameSamplesPerChan = 441; // 20 ms per channel
constexpr int kFrameTotalSamples = kFrameSamplesPerChan * kMusicChannels; // 882 samples total
constexpr int kOutBuffers = 16;
constexpr size_t kStartBuffer = 4;
constexpr size_t kMaxBuffer = 20;

struct AudioFrame {
    int16_t samples[kFrameTotalSamples];
};

struct DecodedTrack {
    std::vector<int16_t> pcm; // interleaved 16-bit PCM resampled to kMusicRate stereo
    size_t totalSamples = 0; // in samples (each stereo sample is 2 int16_t)
};

// Global audio player state
std::mutex g_playerMx;
std::vector<music::TrackInfo> g_playlist;
int g_currentIndex = -1;
std::atomic<bool> g_isPlaying{false};
std::atomic<bool> g_isPaused{false};
std::atomic<float> g_volume{0.85f};
std::atomic<bool> g_streamToPassengers{true};

std::atomic<bool> g_driverActive{false};
std::atomic<bool> g_hasPassengers{false};
std::atomic<bool> g_passengerActive{false};
std::atomic<float> g_gameRadioVol{1.0f};

float g_driverFade = 0.0f;
float g_recvFade = 0.0f;

DecodedTrack g_loadedTrack;
size_t g_playbackSamplePos = 0;
uint16_t g_streamSeq = 0;

// Diagnostics & metrics
std::atomic<uint32_t> g_txPackets{0};
std::atomic<uint32_t> g_rxPackets{0};
std::atomic<uint32_t> g_rxQueueSize{0};
std::atomic<int> g_riderCount{0};
std::mutex g_rxTitleMx;
std::string g_rxTrackTitle;

// WinMM waveOut device for local music playback
HWAVEOUT g_waveOut = nullptr;
WAVEHDR g_waveHdr[kOutBuffers] = {};
int16_t g_waveBuf[kOutBuffers][kFrameTotalSamples] = {};

// Receiver state (passenger)
std::mutex g_recvMx;
std::deque<AudioFrame> g_recvQueue;
HWAVEOUT g_recvWaveOut = nullptr;
WAVEHDR g_recvWaveHdr[kOutBuffers] = {};
int16_t g_recvWaveBuf[kOutBuffers][kFrameTotalSamples] = {};
bool g_recvPlaying = false;
uint64_t g_lastRecvMs = 0;

WAVEFORMATEX FormatMusic() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = kMusicChannels;
    f.nSamplesPerSec = kMusicRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = kMusicChannels * 2;
    f.nAvgBytesPerSec = kMusicRate * f.nBlockAlign;
    return f;
}

// Simple resampler & converter to 22050 Hz stereo
void ResampleToStandard(const int16_t* inSamples, int inHz, int inChannels, size_t totalInputFrames, std::vector<int16_t>& out) {
    if (!inSamples || totalInputFrames == 0 || inHz <= 0 || inChannels <= 0) return;
    double ratio = (double)inHz / (double)kMusicRate;
    size_t outFrames = (size_t)((double)totalInputFrames / ratio);
    out.resize(outFrames * kMusicChannels);

    for (size_t i = 0; i < outFrames; ++i) {
        double srcIdx = i * ratio;
        size_t idx0 = (size_t)srcIdx;
        size_t idx1 = std::min(idx0 + 1, totalInputFrames - 1);
        double frac = srcIdx - (double)idx0;

        int16_t left = 0, right = 0;
        if (inChannels == 1) {
            double s0 = inSamples[idx0];
            double s1 = inSamples[idx1];
            int16_t val = (int16_t)(s0 + (s1 - s0) * frac);
            left = right = val;
        } else {
            double l0 = inSamples[idx0 * inChannels];
            double l1 = inSamples[idx1 * inChannels];
            double r0 = inSamples[idx0 * inChannels + 1];
            double r1 = inSamples[idx1 * inChannels + 1];
            left = (int16_t)(l0 + (l1 - l0) * frac);
            right = (int16_t)(r0 + (r1 - r0) * frac);
        }
        out[i * 2] = left;
        out[i * 2 + 1] = right;
    }
}

bool LoadMp3File(const std::wstring& path, DecodedTrack& out) {
    mp3dec_t dec;
    mp3dec_file_info_t info{};
    mp3dec_init(&dec);
    if (mp3dec_load_w(&dec, path.c_str(), &info, nullptr, nullptr) != 0 || !info.buffer || info.samples == 0) {
        if (info.buffer) free(info.buffer);
        return false;
    }
    size_t totalFrames = info.samples / info.channels;
    ResampleToStandard(info.buffer, info.hz, info.channels, totalFrames, out.pcm);
    out.totalSamples = out.pcm.size() / kMusicChannels;
    free(info.buffer);
    return !out.pcm.empty();
}

void CloseLocalWaveOut() {
    if (!g_waveOut) return;
    waveOutReset(g_waveOut);
    for (WAVEHDR& h : g_waveHdr) {
        if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_waveOut, &h, sizeof(h));
    }
    waveOutClose(g_waveOut);
    g_waveOut = nullptr;
    g_driverFade = 0.0f;
}

void CloseRecvWaveOut() {
    if (!g_recvWaveOut) return;
    waveOutReset(g_recvWaveOut);
    for (WAVEHDR& h : g_recvWaveHdr) {
        if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_recvWaveOut, &h, sizeof(h));
    }
    waveOutClose(g_recvWaveOut);
    g_recvWaveOut = nullptr;
    g_recvPlaying = false;
    g_recvFade = 0.0f;
}

// Background playback / pump thread
std::atomic<bool> g_threadRunning{true};
std::thread g_audioThread;

void AudioThreadFunc() {
    adpcm::State encStateL;
    adpcm::State encStateR;

    while (g_threadRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

        // 1. Local playback & stream sending (Driver on USER TRACK PLAYER)
        bool driverActive = g_driverActive.load();
        if (!driverActive) {
            g_driverFade = 0.0f;
        } else if (g_isPlaying.load() && !g_isPaused.load()) {
            std::lock_guard<std::mutex> lock(g_playerMx);
            if (!g_loadedTrack.pcm.empty() && g_playbackSamplePos < g_loadedTrack.totalSamples) {
                // Ensure waveOut open
                if (!g_waveOut) {
                    WAVEFORMATEX f = FormatMusic();
                    if (waveOutOpen(&g_waveOut, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
                        for (WAVEHDR& h : g_waveHdr) h.dwFlags = WHDR_DONE;
                    }
                }

                float gameVol = g_gameRadioVol.load();
                float userVol = g_volume.load();
                float masterVol = gameVol * userVol;
                constexpr float kFadeStep = 1.0f / (float)(kMusicRate * 1.2f); // ~1.2s fade-in

                // Check free buffers in waveOut
                for (int i = 0; i < kOutBuffers; ++i) {
                    if (g_playbackSamplePos >= g_loadedTrack.totalSamples) break;
                    WAVEHDR& h = g_waveHdr[i];
                    if (!(h.dwFlags & WHDR_DONE)) continue;

                    if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_waveOut, &h, sizeof(h));

                    // Fill frame
                    int16_t chanLocalL[kFrameSamplesPerChan];
                    int16_t chanLocalR[kFrameSamplesPerChan];
                    int16_t chanStreamL[kFrameSamplesPerChan];
                    int16_t chanStreamR[kFrameSamplesPerChan];

                    for (int s = 0; s < kFrameSamplesPerChan; ++s) {
                        if (g_playbackSamplePos < g_loadedTrack.totalSamples) {
                            int16_t l = g_loadedTrack.pcm[g_playbackSamplePos * 2];
                            int16_t r = g_loadedTrack.pcm[g_playbackSamplePos * 2 + 1];

                            if (g_driverFade < 1.0f) {
                                g_driverFade = std::min(1.0f, g_driverFade + kFadeStep);
                            }
                            float curVol = masterVol * g_driverFade;

                            chanLocalL[s] = (int16_t)std::clamp((float)l * curVol, -32768.0f, 32767.0f);
                            chanLocalR[s] = (int16_t)std::clamp((float)r * curVol, -32768.0f, 32767.0f);

                            // Unscaled PCM for streaming: passengers apply their own game radio volume & fade
                            chanStreamL[s] = l;
                            chanStreamR[s] = r;

                            g_waveBuf[i][s * 2] = chanLocalL[s];
                            g_waveBuf[i][s * 2 + 1] = chanLocalR[s];
                            ++g_playbackSamplePos;
                        } else {
                            chanLocalL[s] = chanLocalR[s] = 0;
                            chanStreamL[s] = chanStreamR[s] = 0;
                            g_waveBuf[i][s * 2] = g_waveBuf[i][s * 2 + 1] = 0;
                        }
                    }

                    h = {};
                    h.lpData = (LPSTR)g_waveBuf[i];
                    h.dwBufferLength = sizeof(g_waveBuf[i]);
                    waveOutPrepareHeader(g_waveOut, &h, sizeof(h));
                    waveOutWrite(g_waveOut, &h, sizeof(h));

                    // Send audio frame to passengers if we are driver and streaming enabled
                    if (g_streamToPassengers.load() && Net().Online()) {
                        auto encL = adpcm::Encode(chanStreamL, kFrameSamplesPerChan, encStateL);
                        auto encR = adpcm::Encode(chanStreamR, kFrameSamplesPerChan, encStateR);

                        Writer w(proto::G_RADIO_STREAM);
                        w.U16(g_streamSeq++);
                        bool sendTitle = (g_streamSeq % 50 == 1);
                        w.U8(sendTitle ? 1 : 0);
                        if (sendTitle) {
                            std::string curTitle = music::CurrentTrackTitle();
                            w.Str(curTitle.empty() ? "User Track" : curTitle);
                        }
                        w.U16((uint16_t)encL.size());
                        w.Bytes(encL.data(), encL.size());
                        w.U16((uint16_t)encR.size());
                        w.Bytes(encR.data(), encR.size());
                        Net().Send(proto::CH_VOICE, w);
                        g_txPackets++;
                    }
                }

                if (g_playbackSamplePos >= g_loadedTrack.totalSamples) {
                    music::Next();
                }
            }
        }

        // 2. Passenger receiver playback (in car with remote driver on USER TRACK PLAYER)
        bool passengerActive = g_passengerActive.load();
        if (!passengerActive) {
            g_recvFade = 0.0f;
        } else {
            std::vector<AudioFrame> toPlay;
            {
                std::lock_guard<std::mutex> lock(g_recvMx);
                if (NowMs() - g_lastRecvMs > 1500) {
                    g_recvQueue.clear();
                    g_recvPlaying = false;
                    g_recvFade = 0.0f;
                } else if (!g_recvQueue.empty()) {
                    if (g_recvPlaying || g_recvQueue.size() >= kStartBuffer) {
                        g_recvPlaying = true;
                        while (!g_recvQueue.empty() && toPlay.size() < 4) {
                            toPlay.push_back(g_recvQueue.front());
                            g_recvQueue.pop_front();
                        }
                    }
                }
            }

            if (!toPlay.empty()) {
                if (!g_recvWaveOut) {
                    WAVEFORMATEX f = FormatMusic();
                    if (waveOutOpen(&g_recvWaveOut, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) {
                        for (WAVEHDR& h : g_recvWaveHdr) h.dwFlags = WHDR_DONE;
                    }
                }
                if (g_recvWaveOut) {
                    size_t played = 0;
                    float gameVol = g_gameRadioVol.load();
                    float userVol = g_volume.load();
                    float masterVol = gameVol * userVol;
                    constexpr float kFadeStep = 1.0f / (float)(kMusicRate * 1.2f); // ~1.2s fade-in

                    for (int i = 0; i < kOutBuffers && played < toPlay.size(); ++i) {
                        WAVEHDR& h = g_recvWaveHdr[i];
                        if (!(h.dwFlags & WHDR_DONE)) continue;
                        if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_recvWaveOut, &h, sizeof(h));

                        const AudioFrame& src = toPlay[played++];
                        for (int k = 0; k < kFrameSamplesPerChan; ++k) {
                            if (g_recvFade < 1.0f) {
                                g_recvFade = std::min(1.0f, g_recvFade + kFadeStep);
                            }
                            float curVol = masterVol * g_recvFade;

                            int16_t l = src.samples[k * 2];
                            int16_t r = src.samples[k * 2 + 1];
                            g_recvWaveBuf[i][k * 2] = (int16_t)std::clamp((float)l * curVol, -32768.0f, 32767.0f);
                            g_recvWaveBuf[i][k * 2 + 1] = (int16_t)std::clamp((float)r * curVol, -32768.0f, 32767.0f);
                        }
                        h = {};
                        h.lpData = (LPSTR)g_recvWaveBuf[i];
                        h.dwBufferLength = sizeof(g_recvWaveBuf[i]);
                        waveOutPrepareHeader(g_recvWaveOut, &h, sizeof(h));
                        waveOutWrite(g_recvWaveOut, &h, sizeof(h));
                    }
                }
            }
        }
    }
}

}  // namespace

namespace music {

void PlayTrack(int index) {
    std::lock_guard<std::mutex> lock(g_playerMx);
    if (index < 0 || index >= (int)g_playlist.size()) return;
    g_currentIndex = index;
    if (LoadMp3File(g_playlist[index].path, g_loadedTrack)) {
        g_playbackSamplePos = 0;
        g_driverFade = 0.0f;
        g_isPlaying = true;
        g_isPaused = false;
        ui::TheChat().AddSystem(Format(TR("Играет: %s", "Playing: %s"), g_playlist[index].title.c_str()));
        if (radio::CarRadio()) {
            CPlayerPed* me = FindPlayerPed(0);
            CVehicle* v = me ? me->VehicleIfIn() : nullptr;
            if (v && v->Driver() == me) {
                radio::TuneToUserTracks();
            }
        }
    } else {
        ui::TheChat().AddError(Format(TR("Не удалось открыть MP3: %s", "Failed to load MP3: %s"), g_playlist[index].title.c_str()));
    }
}

void TogglePause() {
    if (!g_isPlaying) return;
    g_isPaused = !g_isPaused;
    if (!g_isPaused) g_driverFade = 0.0f;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_playerMx);
    g_isPlaying = false;
    g_isPaused = false;
    g_playbackSamplePos = 0;
    g_driverFade = 0.0f;
    CloseLocalWaveOut();
}

void Next() {
    std::lock_guard<std::mutex> lock(g_playerMx);
    if (g_playlist.empty()) return;
    int next = (g_currentIndex + 1) % (int)g_playlist.size();
    lock.~lock_guard();
    PlayTrack(next);
}

void Prev() {
    std::lock_guard<std::mutex> lock(g_playerMx);
    if (g_playlist.empty()) return;
    int prev = (g_currentIndex - 1 + (int)g_playlist.size()) % (int)g_playlist.size();
    lock.~lock_guard();
    PlayTrack(prev);
}

void SetVolume(float vol) { g_volume = std::clamp(vol, 0.0f, 1.0f); }
float Volume() { return g_volume.load(); }
bool IsPlaying() { return g_isPlaying.load(); }
bool IsPaused() { return g_isPaused.load(); }
int CurrentTrackIndex() { return g_currentIndex; }

std::string CurrentTrackTitle() {
    std::lock_guard<std::mutex> lock(g_playerMx);
    if (g_currentIndex >= 0 && g_currentIndex < (int)g_playlist.size()) {
        return g_playlist[g_currentIndex].title;
    }
    return "";
}

float CurrentPositionSeconds() {
    std::lock_guard<std::mutex> lock(g_playerMx);
    return (float)g_playbackSamplePos / (float)kMusicRate;
}

float DurationSeconds() {
    std::lock_guard<std::mutex> lock(g_playerMx);
    return (float)g_loadedTrack.totalSamples / (float)kMusicRate;
}

void AddFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_playerMx);
    TrackInfo ti;
    ti.path = path;
    size_t sep = path.find_last_of(L"\\/");
    std::wstring fn = (sep != std::wstring::npos) ? path.substr(sep + 1) : path;
    ti.title = WideToUtf8(fn);
    g_playlist.push_back(ti);
}

void AddDirectory(const std::wstring& dir) {
    std::wstring search = dir;
    if (!search.empty() && search.back() != L'\\') search += L'\\';
    search += L"*.mp3";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                std::wstring full = dir;
                if (!full.empty() && full.back() != L'\\') full += L'\\';
                full += fd.cFileName;
                AddFile(full);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void AutoScanUserTracks() {
    wchar_t myDocs[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, SHGFP_TYPE_CURRENT, myDocs) == S_OK) {
        std::wstring ut = std::wstring(myDocs) + L"\\GTA San Andreas User Files\\User Tracks";
        AddDirectory(ut);
    }
    if (Playlist().empty() && myDocs[0]) {
        std::wstring dl = std::wstring(myDocs);
        size_t pos = dl.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            std::wstring dlDir = dl.substr(0, pos + 1) + L"Downloads";
            AddDirectory(dlDir);
        }
    }
}

void ClearPlaylist() {
    Stop();
    std::lock_guard<std::mutex> lock(g_playerMx);
    g_playlist.clear();
    g_currentIndex = -1;
}

const std::vector<TrackInfo>& Playlist() { return g_playlist; }

bool IsStreamingEnabled() { return g_streamToPassengers.load(); }
void SetStreamingEnabled(bool on) { g_streamToPassengers = on; }

}  // namespace music

// UI Implementation
namespace ui {

namespace {
bool g_musicVisible = false;
int g_musicScroll = 0;
int g_musicSelected = -1;

void OpenFileDialog() {
    wchar_t filename[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)game::GameWindow();
    ofn.lpstrFilter = L"MP3 Audio (*.mp3)\0*.mp3\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        music::AddFile(filename);
        if (!music::IsPlaying()) music::PlayTrack((int)music::Playlist().size() - 1);
    }
}

bool Hovered(float x, float y, float w, float h) {
    auto& st = In().State();
    return st.mouseX >= x && st.mouseX < x + w && st.mouseY >= y && st.mouseY < y + h;
}
}  // namespace

void SetMusicMenuVisible(bool visible) {
    if (visible == g_musicVisible) return;
    g_musicVisible = visible;
    if (visible && TheChat().IsOpen()) TheChat().Close();
    if (!visible && core::ConnectMenuVisible()) return;
    In().SetKeyboardCapture(visible);
    In().SetMouseCapture(visible);
}

bool IsMusicMenuVisible() { return g_musicVisible; }

void ToggleMusicMenu() {
    SetMusicMenuVisible(!g_musicVisible);
}

void RenderMusicMenu() {
    if (!g_musicVisible) return;
    if (!core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible()) {
        return SetMusicMenuVisible(false);
    }

    auto& g = Gfx();
    float s = g.Scale();
    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU);

    float W = 620 * s;
    float H = 480 * s;
    float x = (g.Width() - W) * 0.5f, y = (g.Height() - H) * 0.5f;
    float pad = 18 * s;
    float cx = x + pad, cy = y + 16 * s, cw = W - 2 * pad;

    w::BeginFrame();
    g.Rect(x, y, W, H, 0xF0000000);
    g.RectOutline(x, y, W, H, kSaLine, std::max(1.0f, s));

    // Title
    g.Text(FONT_MENU, cx, cy, TR("User Tracks / MP3 Плеер", "User Tracks / MP3 Player"), kSaBlue, TEXT_SHADOW);
    cy += mh + 12 * s;

    // Controls bar: Play/Pause, Stop, Prev, Next, Open File
    float btnH = lh + 10 * s;
    float bw = 75 * s;

    std::string playText = music::IsPaused() ? TR("Пуск", "Resume") : music::IsPlaying() ? TR("Пауза", "Pause") : TR("Играть", "Play");
    if (w::Button("btn_mus_play", cx, cy, bw, btnH, playText)) {
        if (!music::IsPlaying() && !music::Playlist().empty()) {
            music::PlayTrack(g_musicSelected >= 0 ? g_musicSelected : 0);
        } else {
            music::TogglePause();
        }
    }
    if (w::Button("btn_mus_stop", cx + bw + 8 * s, cy, bw, btnH, TR("Стоп", "Stop"))) {
        music::Stop();
    }
    if (w::Button("btn_mus_prev", cx + (bw + 8 * s) * 2, cy, 40 * s, btnH, "<")) {
        music::Prev();
    }
    if (w::Button("btn_mus_next", cx + (bw + 8 * s) * 2 + 48 * s, cy, 40 * s, btnH, ">")) {
        music::Next();
    }

    float openW = 150 * s;
    if (w::Button("btn_mus_open", cx + cw - openW, cy, openW, btnH, TR("Открыть MP3...", "Open MP3..."))) {
        OpenFileDialog();
    }
    cy += btnH + 12 * s;

    // Status: Now playing
    std::string title = music::CurrentTrackTitle();
    if (title.empty()) title = TR("Ничего не играет", "Nothing playing");
    float curSec = music::CurrentPositionSeconds();
    float durSec = music::DurationSeconds();
    std::string timeStr = Format("%02d:%02d / %02d:%02d", (int)curSec / 60, (int)curSec % 60, (int)durSec / 60, (int)durSec % 60);

    g.Text(FONT_NORMAL, cx, cy, Format(TR("Трек: %s", "Track: %s"), title.c_str()), kSaBlue, TEXT_SHADOW);
    g.Text(FONT_SMALL, cx + cw, cy + 2 * s, timeStr, kSaMuted, TEXT_RIGHT | TEXT_SHADOW);
    cy += lh + 10 * s;

    // Stream to passengers checkbox
    bool stream = music::IsStreamingEnabled();
    if (w::Checkbox("chk_stream", cx, cy, TR("Стримить радио пассажирам в машине", "Stream radio to car passengers"), stream)) {
        music::SetStreamingEnabled(stream);
    }
    std::string diag = Format("TX: %u | RX: %u", (unsigned)g_txPackets.load(), (unsigned)g_rxPackets.load());
    g.Text(FONT_SMALL, cx + cw, cy + 2 * s, diag, kSaMuted, TEXT_RIGHT | TEXT_SHADOW);
    cy += lh + 12 * s;

    // Playlist header
    g.Text(FONT_MENU_SMALL, cx, cy, TR("Список треков", "Playlist"), kSaBlue, TEXT_SHADOW);
    if (w::Button("btn_mus_clear", cx + cw - 110 * s, cy - 4 * s, 110 * s, lh + 6 * s, TR("Очистить", "Clear"))) {
        music::ClearPlaylist();
    }
    cy += sh + 8 * s;

    // Playlist box
    float listH = H - (cy - y) - 48 * s;
    g.Rect(cx, cy, cw, listH, 0xFF090C10);
    g.RectOutline(cx, cy, cw, listH, kSaLine, std::max(1.0f, s));

    const auto& plist = music::Playlist();
    float itemH = lh + 6 * s;
    int visibleCount = (int)(listH / itemH);

    if (Hovered(cx, cy, cw, listH)) {
        if (In().State().wheel > 0) g_musicScroll = std::max(0, g_musicScroll - 2);
        else if (In().State().wheel < 0) g_musicScroll = std::max(0, std::min((int)plist.size() - visibleCount, g_musicScroll + 2));
    }
    if (g_musicScroll > (int)plist.size() - visibleCount) g_musicScroll = std::max(0, (int)plist.size() - visibleCount);

    float itemY = cy;
    for (int k = 0; k < visibleCount && (g_musicScroll + k) < (int)plist.size(); ++k) {
        int idx = g_musicScroll + k;
        bool isSel = (g_musicSelected == idx);
        bool isCur = (music::CurrentTrackIndex() == idx);
        bool hover = Hovered(cx, itemY, cw, itemH);

        if (isSel) g.Rect(cx, itemY, cw, itemH, 0xE026303B);
        else if (hover) g.Rect(cx, itemY, cw, itemH, 0x8016202A);

        if (hover && In().State().lmbPressed) {
            g_musicSelected = idx;
            music::PlayTrack(idx);
        }

        std::string num = Format("%d. ", idx + 1);
        uint32_t col = isCur ? kSaGreen : isSel ? kSaBlue : (hover ? 0xFFFFFFFF : kText);
        g.Text(FONT_NORMAL, cx + 8 * s, itemY + 3 * s, num + plist[idx].title, col, TEXT_SHADOW);

        itemY += itemH;
    }

    if (plist.empty()) {
        g.Text(FONT_SMALL, cx + cw * 0.5f, cy + listH * 0.45f,
               TR("Плейлист пуст. Нажмите «Открыть MP3...» для выбора любого трека с диска.",
                  "Playlist is empty. Click \"Open MP3...\" to select any track."),
               kSaMuted, TEXT_CENTER | TEXT_SHADOW);
    }

    // Bottom close button
    float by = y + H - 38 * s;
    const char* close = TR("Закрыть", "Close");
    float room = mh * 1.4f;
    float closeBtnW = g.TextWidth(FONT_MENU, close) + 2 * room;
    if (w::Button("btn_mus_close", cx + cw - closeBtnW, by, closeBtnW, btnH, close)) {
        SetMusicMenuVisible(false);
    }

    w::EndFrame();
    w::DrawCursor();
}

}  // namespace ui

// Module registration & packet handlers
class MusicRadioModule : public Module {
public:
    const char* Name() const override { return "MusicRadio"; }

    void OnInstallHooks() override {
        // Start background audio thread
        g_threadRunning = true;
        g_audioThread = std::thread(AudioThreadFunc);

        Net().SetHandler(proto::G_RADIO_STREAM, [](uint8_t from, Reader& r) {
            uint16_t seq = r.U16(); (void)seq;
            uint8_t flags = r.U8();
            if (flags & 1) {
                std::string title = r.Str(128);
                if (!title.empty()) {
                    std::lock_guard<std::mutex> lock(g_rxTitleMx);
                    g_rxTrackTitle = title;
                }
            }
            uint16_t lenL = r.U16();
            std::vector<uint8_t> dataL = r.Bytes(lenL);
            uint16_t lenR = r.U16();
            std::vector<uint8_t> dataR = r.Bytes(lenR);

            if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady()) return;

            // Only accept if we are in the same vehicle as the sender who is driving
            CPlayerPed* me = FindPlayerPed(0);
            CVehicle* v = me ? me->VehicleIfIn() : nullptr;
            if (!v || v->Driver() == me || !radio::CarRadio(v)) return;

            const RemotePlayer* rp = radio::RemoteDriver();
            if (!rp || rp->id != from) return; // not passenger of this driver

            // Ensure passenger radio is tuned to USER TRACK PLAYER
            if (radio::RequestedStation() != radio::RADIO_USER_TRACKS) {
                radio::TuneToUserTracks();
            }

            std::vector<int16_t> pcmL, pcmR;
            if (!adpcm::Decode(dataL.data(), dataL.size(), pcmL) || !adpcm::Decode(dataR.data(), dataR.size(), pcmR)) return;
            if (pcmL.size() != kFrameSamplesPerChan || pcmR.size() != kFrameSamplesPerChan) return;

            AudioFrame f;
            for (int s = 0; s < kFrameSamplesPerChan; ++s) {
                f.samples[s * 2] = pcmL[s];
                f.samples[s * 2 + 1] = pcmR[s];
            }

            std::lock_guard<std::mutex> lock(g_recvMx);
            if (g_recvQueue.size() >= kMaxBuffer) g_recvQueue.pop_front();
            g_recvQueue.push_back(f);
            g_lastRecvMs = NowMs();
            g_rxPackets++;
            g_rxQueueSize.store((uint32_t)g_recvQueue.size());
        });
    }

    void OnPostProcess() override {
        if (!core::IsPlayerReady()) {
            g_driverActive.store(false);
            g_hasPassengers.store(false);
            g_passengerActive.store(false);
            return;
        }

        bool driverActive = radio::IsDriverOnUserTracks();
        CPlayerPed* me = FindPlayerPed(0);
        CVehicle* v = me ? me->VehicleIfIn() : nullptr;
        const RemotePlayer* driver = radio::RemoteDriver();
        bool isPassenger = v && driver != nullptr && radio::CarRadio(v);
        bool passengerActive = isPassenger && (NowMs() - g_lastRecvMs < 3000);
        float gameVol = radio::GameRadioVolume();

        g_driverActive.store(driverActive);
        g_passengerActive.store(passengerActive);
        g_gameRadioVol.store(gameVol);

        // Notify passenger on connect/disconnect
        static bool s_wasPassengerActive = false;
        if (passengerActive && !s_wasPassengerActive) {
            std::string trackName;
            {
                std::lock_guard<std::mutex> lock(g_rxTitleMx);
                trackName = g_rxTrackTitle;
            }
            if (trackName.empty()) trackName = TR("Поток водителя", "Driver's stream");
            ui::TheChat().AddSystem(Format(TR("Радиопоток водителя подключён: %s", "Connected to driver's radio stream: %s"), trackName.c_str()));
        } else if (!passengerActive && s_wasPassengerActive && isPassenger) {
            ui::TheChat().AddSystem(TR("Радиопоток водителя отключён.", "Driver's radio stream disconnected."));
        }
        s_wasPassengerActive = passengerActive;

        // Check if car has riders
        int riderCount = 0;
        if (driverActive && Net().Online()) {
            if (v) {
                for (auto& [id, rp] : Players::Instance().All()) {
                    if (rp.ped && rp.ped->VehicleIfIn() == v) {
                        riderCount++;
                    }
                }
            }
        }
        g_hasPassengers.store(riderCount > 0);
        g_riderCount.store(riderCount);

        // Auto-scan and auto-resume driver's track when tuning into USER TRACK PLAYER
        static bool s_wasDriverActive = false;
        if (driverActive && !s_wasDriverActive) {
            if (music::Playlist().empty()) {
                music::AutoScanUserTracks();
            }
            std::lock_guard<std::mutex> lock(g_playerMx);
            if (g_currentIndex < 0 && !g_playlist.empty()) {
                lock.~lock_guard();
                music::PlayTrack(0);
            } else if (g_currentIndex >= 0 && !g_loadedTrack.pcm.empty() && !g_isPlaying.load()) {
                g_isPlaying = true;
                g_isPaused = false;
                g_driverFade = 0.0f;
            }
        }
        s_wasDriverActive = driverActive;

        // Cleanup receiver if passenger left car or driver tuned away
        if (!passengerActive) {
            std::lock_guard<std::mutex> lock(g_recvMx);
            if (!g_recvQueue.empty() || g_recvPlaying) {
                g_recvQueue.clear();
                g_recvPlaying = false;
                CloseRecvWaveOut();
            }
        }
    }

    void OnRender() override {
        if (!core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        CVehicle* v = me ? me->VehicleIfIn() : nullptr;
        if (!v || !radio::CarRadio(v)) return;

        auto& g = ui::Gfx();
        float s = g.Scale();
        float x = 24 * s;
        float y = g.Height() * 0.56f;

        if (radio::IsDriverOnUserTracks()) {
            std::string title = music::CurrentTrackTitle();
            if (title.empty()) title = TR("Трек не выбран", "No track selected");
            int riders = g_riderCount.load();
            uint32_t tx = g_txPackets.load();
            std::string text = Format(TR("Радио: %s (стрим: %d пасс, %u пкт)",
                                         "Radio: %s (stream: %d riders, %u pkts)"),
                                      title.c_str(), riders, tx);
            g.Text(ui::FONT_SMALL, x, y, text, 0xFFE6D050, ui::TEXT_OUTLINE);
        } else if (radio::RemoteDriver() != nullptr) {
            bool pActive = g_passengerActive.load();
            if (pActive) {
                std::string title;
                {
                    std::lock_guard<std::mutex> lock(g_rxTitleMx);
                    title = g_rxTrackTitle;
                }
                if (title.empty()) title = TR("Поток водителя", "Driver's stream");
                uint32_t rx = g_rxPackets.load();
                uint32_t q = g_rxQueueSize.load();
                int volPct = (int)(g_gameRadioVol.load() * g_volume.load() * 100.0f);
                std::string text = Format(TR("Радио водителя: %s (приём: %u пкт, буфер: %u, звук: %d%%)",
                                             "Driver's radio: %s (rx: %u pkts, buf: %u, vol: %d%%)"),
                                          title.c_str(), rx, q, volPct);
                g.Text(ui::FONT_SMALL, x, y, text, 0xFF70E090, ui::TEXT_OUTLINE);
            } else if (radio::RequestedStation() == radio::RADIO_USER_TRACKS || radio::ActiveStation() == radio::RADIO_USER_TRACKS) {
                std::string text = TR("Радио водителя: ожидание аудиопотока...",
                                      "Driver's radio: waiting for audio stream...");
                g.Text(ui::FONT_SMALL, x, y, text, 0xFFAAAAAA, ui::TEXT_OUTLINE);
            }
        }
    }

    void OnSessionEnd() override {
        music::Stop();
        CloseRecvWaveOut();
    }

    void OnGameShutdown() override {
        g_threadRunning = false;
        if (g_audioThread.joinable()) g_audioThread.join();
        CloseLocalWaveOut();
        CloseRecvWaveOut();
    }

    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd == "radio" || cmd == "music" || cmd == "mp3") {
            ui::ToggleMusicMenu();
            return true;
        }
        return false;
    }

    // Rendered from Core.cpp via ui::RenderMusicMenu()

    static MusicRadioModule& Instance() {
        static MusicRadioModule m;
        return m;
    }
};

struct MusicRadioReg {
    MusicRadioReg() { core::RegisterModule(&MusicRadioModule::Instance()); }
} g_musicRadioReg;

}  // namespace coop
