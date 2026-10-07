// With ZUNE_LOG=on, leaving the game (the three-finger hold) sends its log, and the profile
// if there is one, over Wi-Fi to the computer that runs `./sm64zune logs`
// (tools/receive_logs.py listens on ZUNE_LOG_HOST, port 8767):
//   POST /sm64zune/<run>/<file>/<offset>   the next piece of a file, at most 64 KiB
//   POST /sm64zune/<run>/done              everything was sent
// <run> is eight hex digits that tell this upload from any other; <offset> is in hex too.
// Once the computer has everything, the files are deleted here, so each log is sent once. If
// anything fails they stay, and the next run adds to the same log. Wi-Fi only works while
// the Zune is unplugged from USB.
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <string.h>
#include <zdk.h>
#include "zune_settings.h"
#include "zune_platform.h"

#define WIDE_TEXT2(text) L##text
#define WIDE_TEXT(text) WIDE_TEXT2(text)
static const WCHAR HOST[] = WIDE_TEXT(ZUNE_LOG_HOST);
static const INTERNET_PORT PORT = 8767;
static const WCHAR LOG_FILE[] = L"\\Flash2\\SM64\\log.txt";
static const WCHAR PROFILE_FILE[] = L"\\Flash2\\SM64\\profile.bin";
enum { CHUNK = 64 * 1024, MAX_FILE = 4 << 20 };

static char chunk[CHUNK];

static bool Post(HINTERNET session, const WCHAR *path, const void *data, DWORD size)
{
    static const WCHAR headers[] = L"Content-Type: application/octet-stream\r\n";
    for (int attempt = 0; attempt < 3; ++attempt) {
        DWORD status = 0;
        HINTERNET host = InternetConnect(session, HOST, PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
        HINTERNET request = host ? HttpOpenRequest(host, L"POST", path, L"HTTP/1.0", NULL, NULL,
                                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0) : NULL;
        // HttpSendRequestW rejects -1 for a non-null header; the length is in WCHARs.
        if (request && HttpSendRequest(request, headers, (DWORD)wcslen(headers), (LPVOID)data, size)) {
            DWORD length = sizeof(status);
            HttpQueryInfo(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &length, NULL);
        }
        if (request) InternetCloseHandle(request);
        if (host) InternetCloseHandle(host);
        if (status == 200) return true;
        Sleep(500);
    }
    return false;
}

static DWORD FileSize(const WCHAR *path)
{
    HANDLE file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(file, NULL);
    CloseHandle(file);
    return size == 0xFFFFFFFF ? 0 : size;
}

// Sends a file in pieces; `url` is the request path with %08lx for the run and %lx for the
// offset. Adds what it sent to `done` and redraws the progress bar.
static bool PostFile(HINTERNET session, const WCHAR *path, const WCHAR *url, DWORD run, DWORD &done, DWORD total)
{
    HANDLE file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    // Keep the file stable while sending it. An unreadable, oversized or incomplete file
    // must not be acknowledged as delivered and deleted by ZuneUploadLogs.
    DWORD high = 0;
    DWORD size = GetFileSize(file, &high);
    if (size == INVALID_FILE_SIZE || high != 0 || size > MAX_FILE) {
        CloseHandle(file);
        return false;
    }
    bool ok = true;
    for (DWORD offset = 0; offset < size; offset += CHUNK) {
        DWORD wanted = size - offset < CHUNK ? size - offset : (DWORD)CHUNK;
        DWORD got = 0;
        if (!ReadFile(file, chunk, wanted, &got, NULL) || got != wanted) { ok = false; break; }
        WCHAR request[96];
        _snwprintf(request, 95, url, run, offset);
        request[95] = 0;
        if (!Post(session, request, chunk, got)) { ok = false; break; }
        done += got;
        ZuneDrawProgress(total ? (float)done / total : 1.0f, 0);
    }
    CloseHandle(file);
    return ok;
}

bool ZuneUploadLogs()
{
    if (!HOST[0]) return false;
    DWORD logSize = FileSize(LOG_FILE), profileSize = ZUNE_PROFILE ? FileSize(PROFILE_FILE) : 0;
    if (logSize > MAX_FILE) logSize = MAX_FILE;
    if (profileSize > MAX_FILE) profileSize = MAX_FILE;
    DWORD total = logSize + profileSize, done = 0;
    ZuneDrawProgress(0, 0);
    ZuneLog("exit upload: connecting Wi-Fi (%lu KiB to send)", total / 1024);
    BOOL connected = FALSE;
    if (FAILED(ZDKCloud_IsConnected(&connected)) || !connected) {
        ZDKCloud_Connect();
        DWORD start = GetTickCount();
        do {
            Sleep(250);
            ZDKCloud_IsConnected(&connected);
            ZuneDrawProgress(0, 0);
        } while (!connected && GetTickCount() - start < 30000);
    }
    if (!connected) {
        ZuneLog("exit upload: no Wi-Fi (USB still plugged in?); the log stays on the Zune");
        ZuneDrawProgress(0, -1);
        Sleep(1500);
        return false;
    }
    HINTERNET session = InternetOpen(L"ZuneSM64/1", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!session) return false;
    DWORD timeout = 8000;
    InternetSetOption(session, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOption(session, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOption(session, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    DWORD run = GetTickCount();
    bool ok = PostFile(session, LOG_FILE, L"/sm64zune/%08lx/log.txt/%lx", run, done, total);
    if (ok && ZUNE_PROFILE) ok = PostFile(session, PROFILE_FILE, L"/sm64zune/%08lx/profile.bin/%lx", run, done, total);
    if (ok) {
        WCHAR request[48];
        _snwprintf(request, 47, L"/sm64zune/%08lx/done", run);
        request[47] = 0;
        ok = Post(session, request, NULL, 0);
    }
    InternetCloseHandle(session);
    if (ok) {
        DeleteFile(LOG_FILE);
        if (ZUNE_PROFILE) DeleteFile(PROFILE_FILE);
    } else {
        ZuneLog("exit upload: failed after %lu of %lu KiB (is ./sm64zune logs running on %s?)", done / 1024,
                total / 1024, ZUNE_LOG_HOST);
    }
    ZuneDrawProgress(1, ok ? 1 : -1);
    Sleep(ok ? 500 : 1500);
    return ok;
}
