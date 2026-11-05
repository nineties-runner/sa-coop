#pragma once
#include <string>
#include <vector>

namespace coop::music {

struct TrackInfo {
    std::wstring path;
    std::string title;
};

// Start or stop playing audio
void PlayTrack(int index);
void TogglePause();
void Stop();
void Next();
void Prev();
void SetVolume(float vol); // 0.0 .. 1.0
float Volume();
bool IsPlaying();
bool IsPaused();
int CurrentTrackIndex();
std::string CurrentTrackTitle();
float CurrentPositionSeconds();
float DurationSeconds();

// Playlist management
void AddFile(const std::wstring& path);
void AddDirectory(const std::wstring& dir);
void ClearPlaylist();
const std::vector<TrackInfo>& Playlist();

// Streaming toggle
bool IsStreamingEnabled();
void SetStreamingEnabled(bool on);

}  // namespace coop::music

namespace coop::ui {
void SetMusicMenuVisible(bool visible);
bool IsMusicMenuVisible();
void ToggleMusicMenu();
void RenderMusicMenu();
}  // namespace coop::ui
