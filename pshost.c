/*
 * pshost - installs the pathscope WH_CBT hook and pumps messages.
 *
 * A global hook needs a message loop in the installing thread; Windows uses it
 * to marshal hook callbacks. Closing this program (Ctrl+C or closing the
 * window) removes the hook and unloads the DLL from every process.
 *
 * Only reaches processes at or below this program's integrity level, so
 * elevated apps need this run elevated too.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static HHOOK g_hook;

static BOOL WINAPI CtrlHandler(DWORD type)
{
    (void)type;
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    PostQuitMessage(0);
    return TRUE;
}

int main(int argc, char **argv)
{
    HMODULE  dll;
    HOOKPROC proc;
    MSG      msg;
    WCHAR    path[MAX_PATH];
    DWORD    n;

    (void)argc; (void)argv;

    /* Load the hook DLL from this executable's own directory. */
    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { printf("cannot resolve own path\n"); return 1; }
    while (n > 0 && path[n - 1] != L'\\') n--;
    path[n] = 0;
    if (argc > 1) {
        WCHAR wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide, MAX_PATH);
        lstrcatW(path, wide);
    } else {
        lstrcatW(path, L"pathscope64.dll");
    }

    dll = LoadLibraryW(path);
    if (!dll) { wprintf(L"LoadLibrary(%s) failed: %lu\n", path, GetLastError()); return 1; }

    proc = (HOOKPROC)GetProcAddress(dll, "CbtProc");
    if (!proc) { printf("GetProcAddress(CbtProc) failed: %lu\n", GetLastError()); return 1; }

    g_hook = SetWindowsHookExW(WH_CBT, proc, dll, 0);
    if (!g_hook) { printf("SetWindowsHookEx failed: %lu\n", GetLastError()); return 1; }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    printf("pathscope active.\n");
    printf("  Fixes path auto-completion in file dialogs by declaring the filename\n");
    printf("  box as IS_FILE_FULLFILEPATH - the same call shell32!CRunDlg makes.\n");
    printf("\n");
    printf("  Open any app's file dialog (Ctrl+O) and type a path.\n");
    printf("  Press Ctrl+C to remove the hook and exit.\n\n");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    printf("pathscope removed.\n");
    return 0;
}
