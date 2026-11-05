#include "features/updater/Updater.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/config/version/Version.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/chat/Chat.h"

namespace coop::updater {

namespace {

// Default update check URL (can be customized via coop.ini [game] update_url)
constexpr const char* kDefaultUpdateUrl = "https://raw.githubusercontent.com/nineties-runner/sa-coop/refs/heads/main/version.json";

std::atomic<State> g_state{State::Idle};
std::string g_latestVersion;
std::string g_downloadUrl;
std::string g_statusMsg;
std::atomic<int> g_progress{0};
std::atomic<bool> g_workerRunning{false};

void SetStatus(State s, const std::string& msg) {
    g_statusMsg = msg;
    g_state = s;
}

// Simple version comparison: returns true if 'remote' is strictly greater than 'local'
// E.g. "0.3.3" > "0.3.2" or "v0.4.0" > "0.3.2"
bool IsNewerVersion(std::string remote, std::string local) {
    if (!remote.empty() && (remote[0] == 'v' || remote[0] == 'V')) remote = remote.substr(1);
    if (!local.empty() && (local[0] == 'v' || local[0] == 'V')) local = local.substr(1);
    auto p1 = Split(remote, '.');
    auto p2 = Split(local, '.');
    for (size_t i = 0; i < 3; ++i) {
        int v1 = i < p1.size() ? atoi(p1[i].c_str()) : 0;
        int v2 = i < p2.size() ? atoi(p2[i].c_str()) : 0;
        if (v1 > v2) return true;
        if (v1 < v2) return false;
    }
    return false;
}

// Parse simple JSON or key-value fields: "version": "...", "url": "..."
std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

bool DownloadFile(const std::string& url, const std::wstring& outPath, std::atomic<int>& progress) {
    std::wstring w = Utf8ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return false;

    std::wstring sHost(host, uc.dwHostNameLength);
    std::wstring sPath(path, uc.dwUrlPathLength);
    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET ses = WinHttpOpen(L"SA-COOP/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"SA-COOP/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;

    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 60000);
    HINTERNET con = WinHttpConnect(ses, sHost.c_str(), uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", sPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (!req) {
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (!ok) {
        WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    if (status != 200) {
        WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }

    DWORD totalLen = 0;
    sz = sizeof(totalLen);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &totalLen, &sz, nullptr);

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }

    DWORD downloaded = 0;
    char buf[16384];
    DWORD readBytes = 0;
    while (WinHttpReadData(req, buf, sizeof(buf), &readBytes) && readBytes > 0) {
        DWORD written = 0;
        WriteFile(hFile, buf, readBytes, &written, nullptr);
        downloaded += readBytes;
        if (totalLen > 0) {
            progress = (int)((uint64_t)downloaded * 100 / totalLen);
        }
    }

    CloseHandle(hFile);
    WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    progress = 100;
    return true;
}

}  // namespace

void CheckForUpdates(bool silent) {
    if (g_workerRunning.exchange(true)) return;
    SetStatus(State::Checking, TR("Проверка обновлений...", "Checking for updates..."));

    std::thread([silent]() {
        std::string url = Cfg().updateUrl.empty() ? kDefaultUpdateUrl : Cfg().updateUrl;
        LOGI("Checking for updates from %s", url.c_str());
        std::string headers = "User-Agent: SA-COOP-Client\r\nAccept: application/vnd.github.v3+json\r\n";
        auto resp = nat::HttpRequest("GET", url, "", headers, 7000);
        if (resp.status != 200 || resp.body.empty()) {
            LOGW("Update check failed (HTTP %d)", resp.status);
            SetStatus(State::Failed, TR("Не удалось проверить обновления.", "Failed to check for updates."));
            if (!silent) ui::TheChat().AddError(TR("Ошибка проверки обновлений.", "Failed to check for updates."));
            g_workerRunning = false;
            return;
        }

        std::string ver = ExtractJsonString(resp.body, "tag_name");
        if (ver.empty()) ver = ExtractJsonString(resp.body, "version");
        std::string dl = ExtractJsonString(resp.body, "browser_download_url");
        if (dl.empty()) dl = ExtractJsonString(resp.body, "url");
        if (ver.empty()) {
            ver = Trim(resp.body);
        }
        if (dl.empty() && !ver.empty()) {
            dl = "https://github.com/nineties-runner/sa-coop/releases/download/" + ver + "/SA-COOP.zip";
        }

        g_latestVersion = ver;
        g_downloadUrl = dl;

        std::string curVer = COOP_VERSION_STR;
        if (IsNewerVersion(ver, curVer)) {
            LOGI("New version available: %s (current: %s)", ver.c_str(), curVer.c_str());
            SetStatus(State::Available, Format(TR("Доступна версия %s!", "Version %s is available!"), ver.c_str()));
            ui::TheChat().AddSystem(Format(TR("Доступна новая версия SA-COOP: %s! Для обновления напишите /update",
                                              "New SA-COOP version available: %s! Type /update to install"),
                                           ver.c_str()));
        } else {
            LOGI("Already on the latest version (%s)", curVer.c_str());
            SetStatus(State::UpToDate, TR("У вас установлена последняя версия.", "You have the latest version."));
            if (!silent) ui::TheChat().AddSystem(TR("У вас последняя версия мода.", "You have the latest version."));
        }
        g_workerRunning = false;
    }).detach();
}

void StartDownload() {
    if (g_state != State::Available || g_downloadUrl.empty()) return;
    if (g_workerRunning.exchange(true)) return;

    SetStatus(State::Downloading, TR("Скачивание обновления...", "Downloading update..."));
    g_progress = 0;
    ui::TheChat().AddSystem(TR("Скачивание обновления началось...", "Update download started..."));

    std::thread([]() {
        std::wstring zipPath = Cfg().coopDir + L"update.zip";
        bool ok = DownloadFile(g_downloadUrl, zipPath, g_progress);
        if (!ok) {
            LOGE("Failed to download update from %s", g_downloadUrl.c_str());
            SetStatus(State::Failed, TR("Не удалось скачать обновление.", "Failed to download update."));
            ui::TheChat().AddError(TR("Не удалось скачать обновление.", "Failed to download update."));
            g_workerRunning = false;
            return;
        }

        LOGI("Update downloaded successfully to %ls", zipPath.c_str());
        SetStatus(State::ReadyToRestart, TR("Обновление скачано! Перезапустите игру для установки.",
                                            "Update downloaded! Restart game to install."));
        ui::TheChat().AddSystem(TR("Обновление скачано! Напишите /update restart для перезапуска.",
                                   "Update downloaded! Type /update restart to apply."));
        g_workerRunning = false;
    }).detach();
}

void ApplyAndRestart() {
    std::wstring gameDir = Cfg().gameDir;
    std::wstring coopDir = Cfg().coopDir;
    std::wstring zipPath = coopDir + L"update.zip";
    std::wstring batPath = coopDir + L"apply_update.bat";

    // Write batch script to wait for gta_sa.exe exit, unpack via tar, and restart
    std::string sGameDir = WideToUtf8(gameDir);
    std::string sZipPath = WideToUtf8(zipPath);

    std::string script =
        "@echo off\r\n"
        "chcp 65001 >nul\r\n"
        "timeout /t 2 /nobreak >nul\r\n"
        ":wait_loop\r\n"
        "tasklist /fi \"imagename eq gta_sa.exe\" 2>nul | find /i \"gta_sa.exe\" >nul\r\n"
        "if not errorlevel 1 (\r\n"
        "    timeout /t 1 /nobreak >nul\r\n"
        "    goto wait_loop\r\n"
        ")\r\n"
        "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + sZipPath + "' -DestinationPath '" + sGameDir + "' -Force\"\r\n"
        "del /f /q \"" + sZipPath + "\" >nul 2>&1\r\n"
        "start \"\" \"" + sGameDir + "gta_sa.exe\"\r\n"
        "del /f /q \"%~f0\" >nul 2>&1\r\n"
        "exit\r\n";
std::ofstream out(batPath.c_str(), std::ios::binary);
    out.write(script.data(), (std::streamsize)script.size());
    out.close();

    // Launch updater script detached
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, batPath.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, gameDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOGI("Updater script launched, exiting game...");
        // Terminate process
        ExitProcess(0);
    } else {
        LOGE("Failed to launch updater script %ls", batPath.c_str());
        ui::TheChat().AddError(TR("Не удалось запустить скрипт обновления.", "Failed to launch updater script."));
    }
}

State CurrentState() { return g_state; }
std::string LatestVersion() { return g_latestVersion; }
std::string StatusMessage() { return g_statusMsg; }
int DownloadProgress() { return g_progress; }

}  // namespace coop::updater
