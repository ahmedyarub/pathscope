/*
 * pathscope - restores shell path auto-completion in Windows file dialogs,
 *             and suppresses text suggestions over the filename box.
 *
 * THE BUG (analysed on Windows 11 build 10.0.29648.1000, Canary)
 * -------------------------------------------------------------
 * shell32!CAutoComplete::_EditWndProc, on WM_IME_NOTIFY / IMN_OPENCANDIDATE:
 *
 *     case 0x282u:
 *       if (wParam == 5) { flags |= 0x40u; goto LABEL_35; }   // _HideDropDown
 *       if (wParam == 4)   flags &= ~0x40u;                   // IMN_CLOSECANDIDATE
 *
 * and shell32!CAutoComplete::_ShowDropDown:
 *
 *     if (hwndDropDown == 0 || _IsComboboxDropped(this) || (flags & 0x40) != 0)
 *         return;                                             // refuses to show
 *
 * That guard was written for CJK IMEs: while an IME candidate list is on
 * screen, autocomplete gets out of the way. Windows 11 delivers ordinary text
 * suggestions through the SAME candidate-window channel, so typing a path
 * raises IMN_OPENCANDIDATE repeatedly (measured: 16 times over a 7-character
 * path) and the completion list is killed each time - even when no suggestion
 * is actually displayed, because opening the candidate session is enough.
 *
 * WHAT DOESN'T WORK
 * -----------------
 * shell32!CRunDlg::InitRunDlg protects the Run dialog with
 *     SetInputScope(pcbi.hwndItem, IS_FILE_FULLFILEPATH);
 * and comdlg32 never makes that call for its filename box. Supplying the
 * missing call from this hook was verified to return S_OK and to change
 * nothing: IS_FILE_FULLFILEPATH does not suppress the predictor on this build.
 *
 * WHAT THIS DOES
 * --------------
 *  1. Subclasses the filename edit and drops WM_IME_NOTIFY/IMN_OPENCANDIDATE
 *     so bit 0x40 is never set  -> the dropdown survives.
 *  2. Detaches the input method context (ImmAssociateContext(hwnd, NULL))
 *     -> no candidate session can attach, so no suggestion UI.
 *
 * Documented APIs only; no binary patching and no hardcoded offsets, so it is
 * not tied to a particular shell32 build.
 *
 * Trade-off: CJK IME composition is disabled in the filename box specifically.
 * Every other control on the system is untouched.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WM_IME_NOTIFY_     0x0282
#define IMN_OPENCANDIDATE_ 5

static const WCHAR PROP_OLDPROC[] = L"PathScope.OldProc";
static const WCHAR PROP_OLDIMC[]  = L"PathScope.OldIMC";

/* ------------------------------------------------------------------ logging */
static WCHAR g_logPath[MAX_PATH];
static BOOL  g_logReady = FALSE;

static void LogInit(void)
{
    DWORD n = GetTempPathW(MAX_PATH, g_logPath);
    if (n == 0 || n > MAX_PATH - 20) { g_logPath[0] = 0; return; }
    lstrcatW(g_logPath, L"pathscope.log");
    g_logReady = TRUE;
}

static void Log(const char *fmt, ...)
{
    char    buf[512];
    DWORD   wrote;
    HANDLE  f;
    int     n;
    va_list ap;

    if (!g_logReady) return;
    va_start(ap, fmt);
    n = wvsprintfA(buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    buf[n] = '\r'; buf[n + 1] = '\n'; n += 2;

    f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, buf, (DWORD)n, &wrote, NULL);
    CloseHandle(f);
}

/* ------------------------------------------------------------ window helpers */
static BOOL ClassIs(HWND h, const WCHAR *want)
{
    WCHAR buf[64];
    if (!h) return FALSE;
    if (!GetClassNameW(h, buf, ARRAYSIZE(buf))) return FALSE;
    return lstrcmpiW(buf, want) == 0;
}

/*
 * The filename box of both the modern and legacy shell file dialogs is
 *      Edit  <  ComboBox  <  ComboBoxEx32  <  #32770
 * A ComboBoxEx32 inside a dialog is, in practice, only this control (and the
 * Run dialog's, which shell32 already protects - acting on it is harmless).
 *
 * Note: probing for DUIViewWndClassName / SHELLDLL_DefView does NOT work here.
 * At HCBT_SETFOCUS time the shell view does not exist yet, so the probe always
 * fails and the control is never matched.
 */
static BOOL IsFileDialogEdit(HWND h)
{
    HWND parent;

    if (!ClassIs(h, L"Edit")) return FALSE;

    parent = GetParent(h);
    if (ClassIs(parent, L"ComboBox")) parent = GetParent(parent);
    if (!ClassIs(parent, L"ComboBoxEx32")) return FALSE;

    return ClassIs(GetAncestor(h, GA_ROOT), L"#32770");
}

/* ------------------------------------------------- fix 1: keep the dropdown */
static LRESULT CALLBACK ScopeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, PROP_OLDPROC);

    if (msg == WM_IME_NOTIFY_ && wp == IMN_OPENCANDIDATE_)
        return 0;                       /* never reaches CAutoComplete */

    if (msg == WM_NCDESTROY) {
        if (old) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
        RemovePropW(h, PROP_OLDPROC);
        RemovePropW(h, PROP_OLDIMC);
    }
    return old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

static void ApplySwallow(HWND h)
{
    WNDPROC old;
    if (GetPropW(h, PROP_OLDPROC)) return;
    old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ScopeProc);
    if (!old) { Log("SUBCLASS %p FAILED err=%lu", (void *)h, GetLastError()); return; }
    SetPropW(h, PROP_OLDPROC, (HANDLE)old);
    Log("SUBCLASS %p ok", (void *)h);
}

/* --------------------------------------------- fix 2: no suggestion popup */
typedef HIMC (WINAPI *PFN_ImmAssociateContext)(HWND, HIMC);
static PFN_ImmAssociateContext g_pImmAssociateContext = NULL;
static BOOL g_triedImm = FALSE;

static void ApplyDetachIme(HWND h)
{
    HIMC old;
    if (!g_triedImm) {
        HMODULE m;
        g_triedImm = TRUE;
        m = LoadLibraryW(L"imm32.dll");
        if (m) g_pImmAssociateContext =
            (PFN_ImmAssociateContext)GetProcAddress(m, "ImmAssociateContext");
    }
    if (!g_pImmAssociateContext) return;
    if (GetPropW(h, PROP_OLDIMC)) return;

    old = g_pImmAssociateContext(h, (HIMC)NULL);
    Log("DETACH-IME %p old=%p", (void *)h, (void *)old);
    if (old) SetPropW(h, PROP_OLDIMC, (HANDLE)old);
}

/* ------------------------------------------------------------------- hook */
__declspec(dllexport) LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam)
{
    /* HCBT_SETFOCUS fires before focus is assigned, so we are in place early. */
    if (code == HCBT_SETFOCUS) {
        HWND h = (HWND)wParam;
        if (h && !GetPropW(h, PROP_OLDPROC) && IsFileDialogEdit(h)) {
            ApplySwallow(h);
            ApplyDetachIme(h);
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        LogInit();
    }
    return TRUE;
}
