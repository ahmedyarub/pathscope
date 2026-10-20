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
 * THE SUGGESTION POPUP (analysed on build 10.0.29683.1000)
 * ---------------------------------------------------------
 * Text suggestions come from textinputframework.dll (TSF3), not from the
 * edit's IMM context or its TSF document: CUIElementAdapter::
 * OpenCandidateListUI sends IMN_OPENCANDIDATE to GetFocus() itself, and the
 * suggestion strip is drawn by TextInputHost.exe. Detaching the IME
 * (ImmAssociateContext) or TSF (ITfThreadMgr::AssociateFocus) changes nothing
 * - verified, both removed. What TSF3 does honour is
 * TextInputClient::IsPrivateField(): an edit whose input scope is IS_PASSWORD
 * gets no suggestions at all. It is evaluated once, as focus ENTERS the edit,
 * from the edit's own input context - which msctf!SetInputScope feeds.
 *
 * WHAT THIS DOES
 * --------------
 *  1. SetInputScope(edit, IS_PASSWORD) at HCBT_SETFOCUS, before focus arrives
 *     -> TSF3 marks the filename box private: no candidate session, no
 *     suggestion strip, nothing to kill the dropdown.
 *  2. Subclasses the filename edit and drops WM_IME_NOTIFY/IMN_OPENCANDIDATE
 *     so bit 0x40 is never set -> the dropdown survives any candidate session
 *     that still opens (e.g. a CJK IME). Where autocomplete attaches after
 *     focus (the folder picker) a second layer keeps the drop in front.
 *
 * WHAT DOESN'T WORK
 * -----------------
 * IS_FILE_FULLFILEPATH (what CRunDlg::InitRunDlg uses) returns S_OK and
 * changes nothing. IS_PASSWORD set at WM_SETFOCUS is too late: TSF3 has
 * already recorded the edit as not private for that focus session.
 *
 * Documented APIs only; no binary patching and no hardcoded offsets, so it is
 * not tied to a particular shell32 build.
 *
 * Trade-off: the filename box is a "password" field to the input stack - no
 * text suggestions and no CJK composition there. The text itself is shown
 * normally; every other control on the system is untouched.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define WM_IME_NOTIFY_     0x0282
#define IMN_OPENCANDIDATE_ 5

static const WCHAR PROP_OLDPROC[] = L"PathScope.OldProc";
static const WCHAR PROP_SEEN[]    = L"PathScope.Seen";

/* ------------------------------------------------------------------ logging */
/*
 * Every line carries a timestamp, the PID and the exe name, so a report of
 * "it didn't work in X at 14:20" can be matched against the log. Rotates at
 * LOG_MAX_BYTES to pathscope.log.old; the hook is in every GUI process, so
 * without a cap the file would grow forever.
 */
#define LOG_MAX_BYTES (2 * 1024 * 1024)

static WCHAR g_logPath[MAX_PATH];
static WCHAR g_logOldPath[MAX_PATH];
static char  g_exeName[64];
static BOOL  g_logReady = FALSE;

static void LogInit(void)
{
    WCHAR exe[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, g_logPath);
    if (n == 0 || n > MAX_PATH - 20) { g_logPath[0] = 0; return; }
    lstrcatW(g_logPath, L"pathscope.log");
    lstrcpyW(g_logOldPath, g_logPath);
    lstrcatW(g_logOldPath, L".old");

    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        WCHAR *base = exe + n;
        while (base > exe && base[-1] != L'\\') base--;
        WideCharToMultiByte(CP_UTF8, 0, base, -1, g_exeName, sizeof(g_exeName), NULL, NULL);
    }
    g_logReady = TRUE;
}

static void Log(const char *fmt, ...)
{
    char       buf[1024];
    DWORD      wrote;
    HANDLE     f;
    int        n, m;
    va_list    ap;
    SYSTEMTIME st;
    LARGE_INTEGER size;

    if (!g_logReady) return;
    GetLocalTime(&st);
    n = wsprintfA(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u %5lu %s | ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds, GetCurrentProcessId(), g_exeName);
    va_start(ap, fmt);
    m = wvsprintfA(buf + n, fmt, ap);   /* wvsprintf caps output at 1024 */
    va_end(ap);
    if (m <= 0) return;
    n += m;
    if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n] = '\r'; buf[n + 1] = '\n'; n += 2;

    f = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, buf, (DWORD)n, &wrote, NULL);
    if (GetFileSizeEx(f, &size) && size.QuadPart > LOG_MAX_BYTES) {
        CloseHandle(f);
        MoveFileExW(g_logPath, g_logOldPath, MOVEFILE_REPLACE_EXISTING);
        return;
    }
    CloseHandle(f);
}

/* "class 'title'" of a window, UTF-8, for log lines. */
static const char *Describe(HWND h, char *out, int cch)
{
    WCHAR cls[64], title[100];
    char  c8[128], t8[300];
    cls[0] = title[0] = 0;
    if (h) {
        GetClassNameW(h, cls, ARRAYSIZE(cls));
        GetWindowTextW(h, title, ARRAYSIZE(title));
    }
    WideCharToMultiByte(CP_UTF8, 0, cls,   -1, c8, sizeof(c8), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, t8, sizeof(t8), NULL, NULL);
    if (cch < 16) { out[0] = 0; return out; }
    wsprintfA(out, "%.120s '%.300s'", c8, t8);   /* fits: out is >= 448 at every call site */
    return out;
}

/* ------------------------------------------------------------ window helpers */
static BOOL ClassIs(HWND h, const WCHAR *want)
{
    WCHAR buf[64];
    if (!h) return FALSE;
    if (!GetClassNameW(h, buf, ARRAYSIZE(buf))) return FALSE;
    return lstrcmpiW(buf, want) == 0;
}

/* comdlg32 control IDs (dlgs.h) */
#define ID_CMB13  1148   /* cmb13 - filename combo (Open)            */
#define ID_EDT1   1152   /* edt1  - filename edit  (Save As, legacy) */
#define ID_CMB1   1136   /* cmb1  - file type combo                  */

typedef struct { const WCHAR *cls; BOOL found; } FINDCTX;

static BOOL CALLBACK FindChildProc(HWND h, LPARAM lp)
{
    FINDCTX *c = (FINDCTX *)lp;
    if (ClassIs(h, c->cls)) { c->found = TRUE; return FALSE; }
    return TRUE;
}

/* FindWindowEx only walks immediate children; the shell view sits several
   levels down, so recurse. */
static BOOL HasDescendantOfClass(HWND root, const WCHAR *cls)
{
    FINDCTX c;
    c.cls = cls; c.found = FALSE;
    if (root) EnumChildWindows(root, FindChildProc, (LPARAM)&c);
    return c.found;
}

/*
 * Is this dialog a shell file dialog?
 *
 * A dialog that is still being built can fail every probe, so a NO is not
 * cached before the first keystroke (see EnsureVerdict). The legacy dialog exposes
 * comdlg32 control IDs; the modern IFileDialog exposes none of them (its
 * controls live inside DirectUI containers with ID 0) and is identified by its
 * shell view instead.
 */
/* *why names the probe that matched (or "none"), for the log. */
static BOOL IsFileDialogRoot(HWND root, const char **why)
{
    *why = "not-#32770";
    if (!ClassIs(root, L"#32770")) return FALSE;

    *why = "legacy-ids";
    if (GetDlgItem(root, ID_CMB13) ||
        GetDlgItem(root, ID_EDT1)  ||
        GetDlgItem(root, ID_CMB1)) return TRUE;                 /* legacy */

    *why = "DUIView";
    if (HasDescendantOfClass(root, L"DUIViewWndClassName")) return TRUE; /* modern */
    *why = "DefView";
    if (HasDescendantOfClass(root, L"SHELLDLL_DefView"))    return TRUE;

    *why = "none";
    return FALSE;
}

/*
 * Candidate at focus time: any Edit inside a dialog. Deliberately broad -
 * subclassing is harmless; EnsureVerdict makes the real decision.
 */
static BOOL IsCandidateEdit(HWND h)
{
    if (!ClassIs(h, L"Edit")) return FALSE;
    return ClassIs(GetAncestor(h, GA_ROOT), L"#32770");
}

/* ------------------------------------------------- fix 1: keep the dropdown */
static void ApplyPrivateScope(HWND h);       /* forward decl */

static const WCHAR PROP_VERDICT[] = L"PathScope.Verdict";
#define VERDICT_FILEDLG  ((HANDLE)(ULONG_PTR)1)
#define VERDICT_OTHER    ((HANDLE)(ULONG_PTR)2)

/*
 * Decide whether this edit belongs to a file dialog, and if so make it a
 * private field (ApplyPrivateScope).
 *
 * Tried first at HCBT_SETFOCUS - the only point early enough for TSF3 to see
 * the private scope. Dialogs that expose comdlg32's control IDs (Open, Save,
 * the folder picker) are recognisable there; a modern dialog that is not yet
 * built is caught again at WM_SETFOCUS and the first keystroke, which still
 * fixes the dropdown (the scope then applies from the next focus entry).
 *
 * allowNegative=FALSE means "don't cache a NO yet" - used before the first
 * keystroke, where a NO may only mean the dialog is still being constructed.
 */
static HANDLE EnsureVerdict(HWND h, BOOL allowNegative, const char *trigger)
{
    HANDLE      verdict = GetPropW(h, PROP_VERDICT);
    HWND        root;
    BOOL        isFd;
    const char *why;
    char        d[448];

    if (verdict) return verdict;

    root = GetAncestor(h, GA_ROOT);
    isFd = IsFileDialogRoot(root, &why);

    if (isFd) {
        SetPropW(h, PROP_VERDICT, VERDICT_FILEDLG);
        Log("VERDICT %p filedialog=1 at=%s by=%s root=%s", (void *)h, trigger, why,
            Describe(root, d, sizeof(d)));
        ApplyPrivateScope(h);
        return VERDICT_FILEDLG;
    }
    if (allowNegative) {
        SetPropW(h, PROP_VERDICT, VERDICT_OTHER);
        Log("VERDICT %p filedialog=0 at=%s by=%s root=%s", (void *)h, trigger, why,
            Describe(root, d, sizeof(d)));
        return VERDICT_OTHER;
    }
    return NULL;                        /* undecided; ask again later */
}

/* Logged once per edit: a candidate session opened on an edit we left alone.
   If a real file dialog was misjudged, this is the line that shows it. */
static const WCHAR PROP_CANDLOGGED[] = L"PathScope.CandLogged";

/* ------------------------------------------- diagnostics (file dialogs only) */
/*
 * What a file-dialog edit sees while the user types, capped at DIAG_MAX
 * per edit, so a regression on a future build shows up in the log:
 *   outer - 1 = ScopeProc outermost, 2 = via TopProc, 0 = something else is
 *           in front (CAutoComplete attached after us and not re-layered)
 *   imc   - whether the edit has an IMM input context
 *   dd    - Auto-Suggest Dropdown windows on this thread: count/visible
 */
#define DIAG_MAX 120
static const WCHAR PROP_DIAG[]  = L"PathScope.Diag";
static const WCHAR PROP_PROPS[] = L"PathScope.PropsLogged";

typedef HIMC (WINAPI *PFN_ImmGetContext)(HWND);
typedef BOOL (WINAPI *PFN_ImmReleaseContext)(HWND, HIMC);
static PFN_ImmGetContext     g_pImmGetContext     = NULL;
static PFN_ImmReleaseContext g_pImmReleaseContext = NULL;

static LRESULT CALLBACK ScopeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* ------------------------------------------- stay in front of CAutoComplete */
/*
 * ScopeProc only works while it is the OUTERMOST wndproc: it has to see
 * IMN_OPENCANDIDATE before CAutoComplete does. In the legacy Open dialog
 * autocomplete is attached before the edit gets focus, so the subclass made
 * at HCBT_SETFOCUS sits on top. In the folder picker (FOS_PICKFOLDERS, edit
 * 1152) autocomplete is attached AFTER focus, lands on top of ScopeProc, sees
 * IMN_OPENCANDIDATE first and hides the dropdown - observed in claude.exe's
 * picker: outer=0 on every keystroke, dropdown visible then hidden 177 ms
 * later by wp=5.
 *
 * So once a file-dialog edit is seen with something else on top, put a
 * second, minimal layer (TopProc) above it. One layer only.
 */
static const WCHAR PROP_TOPOLD[] = L"PathScope.TopOldProc";

/*
 * Diagnostic: with the private scope in place no candidate session should
 * open at all. If one does, log who sent it (InSendMessageEx + call stack as
 * module+offset, first STACK_MAX times per edit) - this is how the sender was
 * identified as textinputframework.dll in the first place.
 */
#define STACK_MAX 3
static const WCHAR PROP_STACKS[] = L"PathScope.Stacks";

static void LogStack(HWND h, const char *what)
{
    void   *frames[24];
    char    line[900];
    int     n, i, len;
    INT_PTR done = (INT_PTR)GetPropW(h, PROP_STACKS);
    if (done >= STACK_MAX) return;
    SetPropW(h, PROP_STACKS, (HANDLE)(done + 1));

    n   = CaptureStackBackTrace(1, 24, frames, NULL);
    len = wsprintfA(line, "STACK %p %s insend=%08lx:", (void *)h, what, (unsigned long)InSendMessageEx(NULL));
    for (i = 0; i < n && len < (int)sizeof(line) - 80; i++) {
        HMODULE m = NULL;
        char    mod[MAX_PATH], *base;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &m) && GetModuleFileNameA(m, mod, sizeof(mod))) {
            base = mod + lstrlenA(mod);
            while (base > mod && base[-1] != '\\') base--;
            len += wsprintfA(line + len, " %.40s+%lx", base,
                             (unsigned long)((ULONG_PTR)frames[i] - (ULONG_PTR)m));
        } else
            len += wsprintfA(line + len, " ?%p", frames[i]);
    }
    Log("%s", line);
}

static LRESULT CALLBACK TopProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, PROP_TOPOLD);

    if (msg == WM_IME_NOTIFY_ && wp == IMN_OPENCANDIDATE_) {
        LogStack(h, "imn-opencandidate");
        return 0;                       /* installed on file-dialog edits only */
    }

    if (msg == WM_NCDESTROY) {
        if (old && (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC) == TopProc)
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
        RemovePropW(h, PROP_TOPOLD);
        RemovePropW(h, PROP_STACKS);
    }
    return old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

static void EnsureOutermost(HWND h)
{
    WNDPROC cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    WNDPROC old;

    if (cur == ScopeProc || cur == TopProc || GetPropW(h, PROP_TOPOLD)) return;
    old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)TopProc);
    if (!old) { Log("TOP %p FAILED err=%lu", (void *)h, GetLastError()); return; }
    SetPropW(h, PROP_TOPOLD, (HANDLE)old);
    Log("TOP %p re-layered above %p", (void *)h, (void *)old);
}

static BOOL DiagBudget(HWND h)
{
    INT_PTR n = (INT_PTR)GetPropW(h, PROP_DIAG);
    if (n >= DIAG_MAX) return FALSE;
    SetPropW(h, PROP_DIAG, (HANDLE)(n + 1));
    return TRUE;
}

static int ImcState(HWND h)
{
    HIMC imc;
    if (!g_pImmGetContext) {
        HMODULE m = LoadLibraryW(L"imm32.dll");
        if (m) {
            g_pImmGetContext     = (PFN_ImmGetContext)GetProcAddress(m, "ImmGetContext");
            g_pImmReleaseContext = (PFN_ImmReleaseContext)GetProcAddress(m, "ImmReleaseContext");
        }
        if (!g_pImmGetContext) return -1;
    }
    imc = g_pImmGetContext(h);
    if (!imc) return 0;
    if (g_pImmReleaseContext) g_pImmReleaseContext(h, imc);
    return 1;
}

typedef struct { int count, visible; } DDCTX;

static BOOL CALLBACK DropdownProc(HWND w, LPARAM lp)
{
    DDCTX *c = (DDCTX *)lp;
    if (ClassIs(w, L"Auto-Suggest Dropdown")) {
        c->count++;
        if (IsWindowVisible(w)) c->visible++;
    }
    return TRUE;
}

static void Diag(HWND h, const char *what, WPARAM wp)
{
    DDCTX   dd = { 0, 0 };
    WNDPROC cur;
    int     outer;
    if (!DiagBudget(h)) return;
    EnumThreadWindows(GetCurrentThreadId(), DropdownProc, (LPARAM)&dd);
    cur   = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    outer = cur == ScopeProc ? 1 : cur == TopProc ? 2 : 0;   /* 2 = via TopProc */
    Log("DIAG %p %s wp=%u outer=%d imc=%d dd=%d/%d tid=%lu", (void *)h, what,
        (unsigned)wp, outer, ImcState(h), dd.count, dd.visible, GetCurrentThreadId());
}

typedef struct { char *buf; int len, cap; } PROPSCTX;

#define PS_IS_INTATOM(p) ((((ULONG_PTR)(p)) >> 16) == 0)

static BOOL CALLBACK PropsProc(HWND w, LPWSTR name, HANDLE data, ULONG_PTR lp)
{
    PROPSCTX *c = (PROPSCTX *)lp;
    char      one[80];
    int       n;
    UNREFERENCED_PARAMETER(w);
    UNREFERENCED_PARAMETER(data);
    if (PS_IS_INTATOM(name))
        n = wsprintfA(one, "#%u,", (unsigned)(ULONG_PTR)name);
    else {
        n = WideCharToMultiByte(CP_UTF8, 0, name, -1, one, sizeof(one) - 2, NULL, NULL);
        if (n <= 0) return TRUE;
        one[n - 1] = ','; one[n] = 0;
    }
    if (c->len + n < c->cap) { lstrcpyA(c->buf + c->len, one); c->len += n; }
    return TRUE;
}

/* Once per edit: its window properties. comctl32 subclassing (which
   CAutoComplete uses) leaves its own property, so this shows whether an
   autocomplete object is attached to the edit at all. */
static void DiagProps(HWND h)
{
    char     buf[600];
    PROPSCTX c;
    if (GetPropW(h, PROP_PROPS)) return;
    SetPropW(h, PROP_PROPS, (HANDLE)1);
    buf[0] = 0; c.buf = buf; c.len = 0; c.cap = sizeof(buf);
    EnumPropsExW(h, PropsProc, (LPARAM)&c);
    Log("PROPS %p %s", (void *)h, buf);
}

static LRESULT CALLBACK ScopeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, PROP_OLDPROC);

    if (GetPropW(h, PROP_VERDICT) == VERDICT_FILEDLG) {
        if (msg != WM_NCDESTROY) EnsureOutermost(h);
        if (msg == WM_IME_NOTIFY_) Diag(h, "ime-notify", wp);
        else if ((msg >= 0x010D && msg <= 0x010F) || (msg >= 0x0281 && msg <= 0x0291)) {
            char what[24];
            wsprintfA(what, "ime-msg-%04x", msg);
            Diag(h, what, wp);
        }
        else if (msg == WM_KILLFOCUS) {
            char d[448];
            if (DiagBudget(h))
                Log("DIAG %p killfocus to=%s", (void *)h, Describe((HWND)wp, d, sizeof(d)));
        } else if (msg == WM_CHAR) {
            LRESULT r = old ? CallWindowProcW(old, h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
            DiagProps(h);
            Diag(h, "after-char", wp);
            return r;
        }
    }

    switch (msg) {
    case WM_SETFOCUS:
        EnsureVerdict(h, FALSE, "setfocus");   /* act early when already detectable */
        break;
    case WM_CHAR:
    case WM_KEYDOWN:
        EnsureVerdict(h, TRUE, "key");         /* dialog is certainly built by now   */
        break;
    default:
        break;
    }

    if (msg == WM_IME_NOTIFY_ && wp == IMN_OPENCANDIDATE_) {
        if (EnsureVerdict(h, TRUE, "candidate") == VERDICT_FILEDLG)
            return 0;                   /* never reaches CAutoComplete */
        if (!GetPropW(h, PROP_CANDLOGGED)) {
            char d[448];
            SetPropW(h, PROP_CANDLOGGED, (HANDLE)1);
            Log("PASS-CANDIDATE %p root=%s", (void *)h, Describe(GetAncestor(h, GA_ROOT), d, sizeof(d)));
        }
    }

    if (msg == WM_NCDESTROY) {
        if (old) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)old);
        RemovePropW(h, PROP_OLDPROC);
        RemovePropW(h, PROP_VERDICT);
        RemovePropW(h, PROP_CANDLOGGED);
        RemovePropW(h, PROP_DIAG);
        RemovePropW(h, PROP_PROPS);
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
/*
 * textinputframework.dll (TSF3) decides per focused edit whether Windows text
 * suggestions may run: TextInputClient::IsPrivateField() returns TRUE when the
 * edit's input scope is IS_PASSWORD (31) - among a few others - and private
 * fields get no suggestions. The scope is read from the edit's OWN input
 * context (CInputContext::_GetInputScopeValue -> GUID_PROP_INPUTSCOPE, which
 * msctf!SetInputScope feeds), but IsPrivateField is evaluated only in
 * TextInputClient::EditControlFocusChangeInternal - i.e. as focus enters the
 * edit - so this must run before focus arrives (HCBT_SETFOCUS). The edit must
 * keep its own input context: with the IME detached, TSF3 falls back to a
 * context whose scope we don't own and the scope is ignored.
 */
#define PS_IS_PASSWORD 31

typedef HRESULT (WINAPI *PFN_SetInputScope)(HWND, int /* InputScope enum */);

static void ApplyPrivateScope(HWND h)
{
    static PFN_SetInputScope pSet = NULL;
    HRESULT hr;
    if (!pSet) {
        HMODULE m = LoadLibraryW(L"msctf.dll");
        if (m) pSet = (PFN_SetInputScope)GetProcAddress(m, "SetInputScope");
        if (!pSet) { Log("PRIVATE-SCOPE %p no SetInputScope", (void *)h); return; }
    }
    hr = pSet(h, PS_IS_PASSWORD);
    Log("PRIVATE-SCOPE %p hr=%08lx", (void *)h, (unsigned long)hr);
}

/* ------------------------------------------------------------------- hook */
__declspec(dllexport) LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam)
{
    /*
     * Subclass broadly at focus time (any Edit in a dialog), try to identify
     * the file dialog right away (see EnsureVerdict), and let ScopeProc retry
     * later for dialogs that are not fully built yet.
     */
    if (code == HCBT_SETFOCUS) {
        HWND h = (HWND)wParam;
        if (h && !GetPropW(h, PROP_OLDPROC)) {
            if (IsCandidateEdit(h)) {
                char d[448];
                Log("HOOK %p editId=%d root=%s", (void *)h, GetDlgCtrlID(h),
                    Describe(GetAncestor(h, GA_ROOT), d, sizeof(d)));
                ApplySwallow(h);
                /* Decide now if possible: TSF3 evaluates IsPrivateField only as
                   focus ENTERS the edit, so the private scope must be set
                   before focus arrives, not at WM_SETFOCUS. */
                EnsureVerdict(h, FALSE, "cbt");
            } else if (ClassIs(h, L"Edit") && !GetPropW(h, PROP_SEEN)) {
                /* An Edit outside a #32770 - not handled. Logged once per
                   window so a file picker hosted in some other root class
                   shows up in the log instead of failing silently. */
                char d[448];
                SetPropW(h, PROP_SEEN, (HANDLE)1);
                Log("SKIP %p editId=%d root=%s", (void *)h, GetDlgCtrlID(h),
                    Describe(GetAncestor(h, GA_ROOT), d, sizeof(d)));
            }
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
