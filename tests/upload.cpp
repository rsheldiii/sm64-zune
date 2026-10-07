// Compile the real uploader with simulated Win32/WinINet calls. No Zune or network needed.
#include <assert.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <string.h>
#define ZUNE_PLATFORM_H  // Only these two platform hooks are used by the uploader.
void ZuneLog(const char *, ...);
void ZuneDrawProgress(float, int);
#include "../platform/zune_upload.cpp"

struct FakeFile {
    std::vector<char> data;
    std::vector<char> received;
    DWORD openError, high;
    bool sizeError, exists, deleted;
    unsigned reads, failRead, shortRead, zeroRead;
    size_t position;
    FakeFile() : openError(0), high(0), sizeError(false), exists(true), deleted(false),
                 reads(0), failRead(0), shortRead(0), zeroRead(0), position(0) {}
};
static FakeFile logFile, profileFile;
static DWORD lastError;
static std::wstring requestPath;
static unsigned doneRequests;
static bool failDataPost, failDonePost;
static int finalProgress;

static FakeFile &FindFile(const WCHAR *path)
{
    return wcscmp(path, LOG_FILE) == 0 ? logFile : profileFile;
}

HANDLE CreateFile(const WCHAR *path, DWORD, DWORD, void *, DWORD, DWORD, HANDLE)
{
    FakeFile &file = FindFile(path);
    lastError = file.exists ? file.openError : (DWORD)ERROR_FILE_NOT_FOUND;
    if (lastError) return INVALID_HANDLE_VALUE;
    file.position = file.reads = 0;
    return &file;
}
DWORD GetLastError() { return lastError; }
DWORD GetFileSize(HANDLE handle, DWORD *high)
{
    FakeFile &file = *(FakeFile *)handle;
    if (high) *high = file.high;
    return file.sizeError ? INVALID_FILE_SIZE : (DWORD)file.data.size();
}
BOOL ReadFile(HANDLE handle, void *buffer, DWORD wanted, DWORD *got, void *)
{
    FakeFile &file = *(FakeFile *)handle;
    ++file.reads;
    *got = 0;
    if (file.reads == file.failRead) return FALSE;
    size_t available = file.data.size() - file.position;
    *got = available < wanted ? (DWORD)available : wanted;
    if (file.reads == file.shortRead && *got) --*got;
    if (file.reads == file.zeroRead) *got = 0;
    if (*got) memcpy(buffer, &file.data[file.position], *got);
    file.position += *got;
    return TRUE;
}
BOOL CloseHandle(HANDLE) { return TRUE; }
BOOL DeleteFile(const WCHAR *path) { FindFile(path).deleted = true; return TRUE; }
void Sleep(DWORD) {}
DWORD GetTickCount() { return 1; }
int _snwprintf(WCHAR *buffer, size_t size, const WCHAR *format, DWORD run, DWORD offset)
{
    return swprintf(buffer, size, format, (unsigned long)run, (unsigned long)offset);
}
HINTERNET InternetConnect(HINTERNET, const WCHAR *, INTERNET_PORT, const WCHAR *, const WCHAR *, DWORD, DWORD, size_t)
{ return (HINTERNET)1; }
HINTERNET HttpOpenRequest(HINTERNET, const WCHAR *, const WCHAR *path, const WCHAR *, const WCHAR *, const WCHAR **, DWORD, size_t)
{ requestPath = path; return (HINTERNET)1; }
static bool IsDone() { return requestPath.find(L"/done") != std::wstring::npos; }
BOOL HttpSendRequest(HINTERNET, const WCHAR *, DWORD, LPVOID data, DWORD size)
{
    if (IsDone()) ++doneRequests;
    else if (!failDataPost) {
        FakeFile &file = requestPath.find(L"/log.txt/") != std::wstring::npos ? logFile : profileFile;
        const WCHAR *offsetText = wcsrchr(requestPath.c_str(), L'/') + 1;
        unsigned long offset = 0;
        assert(swscanf(offsetText, L"%lx", &offset) == 1);
        assert(offset == file.received.size());
        file.received.insert(file.received.end(), (char *)data, (char *)data + size);
    }
    return TRUE;
}
BOOL HttpQueryInfo(HINTERNET, DWORD, LPVOID result, DWORD *, DWORD *)
{ *(DWORD *)result = (IsDone() ? failDonePost : failDataPost) ? 500 : 200; return TRUE; }
BOOL InternetCloseHandle(HINTERNET) { return TRUE; }
HINTERNET InternetOpen(const WCHAR *, DWORD, const WCHAR *, const WCHAR *, DWORD) { return (HINTERNET)1; }
BOOL InternetSetOption(HINTERNET, DWORD, LPVOID, DWORD) { return TRUE; }
int ZDKCloud_IsConnected(BOOL *connected) { *connected = TRUE; return 0; }
int ZDKCloud_Connect() { return 0; }
void ZuneLog(const char *, ...) {}
void ZuneDrawProgress(float, int state) { finalProgress = state; }

static void Reset(size_t size = CHUNK + 17)
{
    logFile = FakeFile();
    profileFile = FakeFile();
    logFile.data.resize(size);
    for (size_t i = 0; i < size; ++i) logFile.data[i] = (char)(i % 251);
    profileFile.data.assign(19, 'p');
    doneRequests = 0;
    failDataPost = failDonePost = false;
    finalProgress = 0;
}

static void ExpectFailure()
{
    assert(!ZuneUploadLogs());
    assert(!logFile.deleted && !profileFile.deleted);
    assert(finalProgress == -1);
    assert(doneRequests == (failDonePost ? 3u : 0u));
}

int main()
{
    const size_t sizes[] = {0, 1, CHUNK, CHUNK + 17, MAX_FILE};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        Reset(sizes[i]);
        assert(ZuneUploadLogs());
        assert(logFile.received == logFile.data);
        assert(logFile.deleted && doneRequests == 1 && finalProgress == 1);
        assert(profileFile.deleted == (ZUNE_PROFILE != 0));
        if (ZUNE_PROFILE) assert(profileFile.received == profileFile.data);
        else assert(profileFile.received.empty());
    }
    Reset(); logFile.failRead = 1; ExpectFailure();
    Reset(); logFile.failRead = 2; ExpectFailure();
    assert(logFile.received.size() == CHUNK);  // Even after a successful first piece, keep the file.
    Reset(); logFile.shortRead = 2; ExpectFailure();
    Reset(); logFile.zeroRead = 2; ExpectFailure();
    Reset(); logFile.openError = ERROR_ACCESS_DENIED; ExpectFailure();
    Reset(); logFile.sizeError = true; ExpectFailure();
    Reset(); logFile.high = 1; ExpectFailure();
    Reset(MAX_FILE + 1); ExpectFailure();
    assert(logFile.received.empty());
    Reset(); failDataPost = true; ExpectFailure();
    Reset(); failDonePost = true; ExpectFailure();
#if ZUNE_PROFILE
    Reset(); profileFile.failRead = 1; ExpectFailure();
    assert(logFile.received == logFile.data);  // A failed profile preserves the log too.
    Reset(); profileFile.openError = ERROR_ACCESS_DENIED; ExpectFailure();
    Reset(); profileFile.exists = false;
    assert(ZuneUploadLogs());  // A missing optional profile is fine.
    Reset(); profileFile.openError = ERROR_PATH_NOT_FOUND;
    assert(ZuneUploadLogs());
#endif
    puts("upload tests passed");
}
