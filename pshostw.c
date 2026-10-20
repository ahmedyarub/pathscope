/*
 * pshostw - windowless host for the pathscope hook (for autostart).
 *
 * Same job as pshost.exe but built as a GUI-subsystem binary, so no console
 * window appears at sign-in. A global hook needs a message loop on the
 * installing thread; that is all this does.
 *
 * Usage:  pshostw_x64.exe pathscope_x64.dll
 * Stop:   taskkill /IM pshostw_x64.exe   (removes the hook everywhere)
 *
 * A named mutex prevents two copies of the same architecture running.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef _WIN64
#define ARCH "x64"
#else
#define ARCH "x86"
#endif

/* Appends "HOST <arch> ..." to the same %TEMP%\pathscope.log the hook writes,
   so each sign-in shows whether the hook was actually installed. */
static void HostLog(const char *fmt, ...)
{
    WCHAR      path[MAX_PATH];
    char       buf[512];
    DWORD      n, wrote;
    int        m;
    HANDLE     f;
    va_list    ap;
    SYSTEMTIME st;

    n = GetTempPathW(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH - 20) return;
    lstrcatW(path, L"pathscope.log");

    GetLocalTime(&st);
    m = wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u %5lu HOST " ARCH " | ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, GetCurrentProcessId());
    va_start(ap, fmt);
    m += wvsprintfA(buf + m, fmt, ap);
    va_end(ap);
    if (m > (int)sizeof(buf) - 2) m = (int)sizeof(buf) - 2;
    buf[m] = '\r'; buf[m + 1] = '\n'; m += 2;

    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, buf, (DWORD)m, &wrote, NULL);
    CloseHandle(f);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdline, int show)
{
    HMODULE  dll;
    HOOKPROC proc;
    HHOOK    hook;
    MSG      msg;
    WCHAR    path[MAX_PATH];
    WCHAR    mutexName[128];
    HANDLE   mutex;
    DWORD    n;

    UNREFERENCED_PARAMETER(hInst);
    UNREFERENCED_PARAMETER(hPrev);
    UNREFERENCED_PARAMETER(show);

#ifdef _WIN64
    lstrcpyW(mutexName, L"Local\\pathscope.host.x64");
#else
    lstrcpyW(mutexName, L"Local\\pathscope.host.x86");
#endif
    HostLog("start");
    mutex = CreateMutexW(NULL, TRUE, mutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        HostLog("another host already running - exiting");
        return 0;
    }

    /* Resolve the DLL next to this executable. */
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 1;
    while (n > 0 && path[n - 1] != L'\\') n--;
    path[n] = 0;

    if (cmdline && cmdline[0]) {
        /* strip surrounding quotes if present */
        WCHAR *p = cmdline;
        if (*p == L'"') p++;
        lstrcatW(path, p);
        n = lstrlenW(path);
        if (n && path[n - 1] == L'"') path[n - 1] = 0;
    } else {
#ifdef _WIN64
        lstrcatW(path, L"pathscope_x64.dll");
#else
        lstrcatW(path, L"pathscope_x86.dll");
#endif
    }

    dll = LoadLibraryW(path);
    if (!dll) { HostLog("FAILED LoadLibrary %S err=%lu", path, GetLastError()); return 1; }

    proc = (HOOKPROC)GetProcAddress(dll, "CbtProc");
    if (!proc) { HostLog("FAILED GetProcAddress(CbtProc) err=%lu", GetLastError()); return 1; }

    hook = SetWindowsHookExW(WH_CBT, proc, dll, 0);
    if (!hook) { HostLog("FAILED SetWindowsHookEx err=%lu", GetLastError()); return 1; }
    HostLog("hook installed (%S)", path);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hook);
    HostLog("message loop ended - hook removed, exiting");
    return 0;
}
