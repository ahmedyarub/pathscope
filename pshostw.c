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
    mutex = CreateMutexW(NULL, TRUE, mutexName);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

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
    if (!dll) return 1;

    proc = (HOOKPROC)GetProcAddress(dll, "CbtProc");
    if (!proc) return 1;

    hook = SetWindowsHookExW(WH_CBT, proc, dll, 0);
    if (!hook) return 1;

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(hook);
    return 0;
}
