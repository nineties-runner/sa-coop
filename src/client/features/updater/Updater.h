#pragma once
#include <string>

namespace coop::updater {

enum class State {
    Idle,
    Checking,
    Available,      // new version found
    Downloading,    // download in progress
    ReadyToRestart, // downloaded, waiting for restart
    UpToDate,       // already latest version
    Failed          // check or download error
};

void CheckForUpdates(bool silent = true);
void StartDownload();
void ApplyAndRestart();

State CurrentState();
std::string LatestVersion();
std::string StatusMessage();
int DownloadProgress();  // 0 .. 100%

}  // namespace coop::updater
