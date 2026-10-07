#ifndef TEST_WINDOWS_H
#define TEST_WINDOWS_H
// The small Win32 surface used by zune_upload.cpp, implemented by tests/upload.cpp.
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
typedef uint32_t DWORD;
typedef int BOOL;
typedef wchar_t WCHAR;
typedef void *HANDLE;
typedef void *LPVOID;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_SIZE ((DWORD)0xFFFFFFFF)
enum {
    GENERIC_READ = 1, FILE_SHARE_READ = 2, FILE_SHARE_WRITE = 4, OPEN_EXISTING = 8,
    FILE_ATTRIBUTE_NORMAL = 16, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3,
    ERROR_ACCESS_DENIED = 5
};
HANDLE CreateFile(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
DWORD GetLastError();
DWORD GetFileSize(HANDLE, DWORD *);
BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
BOOL CloseHandle(HANDLE);
BOOL DeleteFile(const WCHAR *);
void Sleep(DWORD);
DWORD GetTickCount();
// Windows' unsigned long is 32-bit; adapt the format arguments for the Linux host.
int _snwprintf(WCHAR *, size_t, const WCHAR *, DWORD, DWORD = 0);
#endif
