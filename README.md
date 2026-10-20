# pathscope

Restores shell **path auto-completion** in Windows file dialogs, and suppresses
the text-suggestion popup over the filename box.

Works in the **Open** and **Save As** dialogs and the **folder picker**
(`IFileOpenDialog` + `FOS_PICKFOLDERS`, which Electron apps such as Claude
Desktop use). Analysed on Windows 11 **10.0.29648.1000** (2026-09-14) and
**10.0.29683.1000** (2026-10-20).

---

## The bug

`shell32!CAutoComplete::_EditWndProc`, handling `WM_IME_NOTIFY` (`0x282`):

```c
case 0x282u:
  if (wParam == 5) { flags |= 0x40u; goto LABEL_35; }   // IMN_OPENCANDIDATE -> _HideDropDown
  if (wParam == 4)   flags &= ~0x40u;                   // IMN_CLOSECANDIDATE
```

and `shell32!CAutoComplete::_ShowDropDown`:

```c
if (hwndDropDown == 0 || _IsComboboxDropped(this) || (flags & 0x40) != 0)
    return;                                             // refuses to show
```

That guard exists so autocomplete steps aside for a **CJK IME candidate list**.
Windows 11 delivers ordinary text suggestions through the *same*
candidate-window channel, so typing a path raises `IMN_OPENCANDIDATE` over and
over — measured at **16 times across a 7-character path**. Each one hides the
dropdown and blocks it from returning.

It fires **even when no suggestion is displayed**: merely opening the candidate
session is enough. That is why the dropdown vanishes even when nothing appears
on screen.

## The suggestion popup

The strip of word suggestions (`geri · guri · Geri` over `e:\gdri`) does **not**
come from the edit's IMM context or its TSF document. A call-stack capture of
the `IMN_OPENCANDIDATE` that reaches the edit shows the sender:

```
USER32!SendMessageW <- textinputframework!CUIElementAdapter::OpenCandidateListUI
  <- CUIElementAdapter::OnCandidateMessage <- CInputContextAdapter::OnCandidateMessage
  <- CTSF3CandidateMessage::Execute <- ... (same thread, not a cross-thread send)
```

`textinputframework.dll` (TSF3) receives the candidate list from
`TextInputHost.exe`, which draws the strip, and notifies `GetFocus()` itself.
Detaching the IME (`ImmAssociateContext(hwnd, NULL)`) or TSF
(`ITfThreadMgr::AssociateFocus(hwnd, NULL)`) therefore changes nothing — both
were tested on 29683 and removed.

What TSF3 *does* honour is `TextInputClient::IsPrivateField`:

```c
if ((info->flags & 0xC0000) != 0) return 1;
scope = info->inputScope;                  // from the edit's own input context
if (scope - 31 <= 0x22 && _bittest64(&0x740000001, scope - 31)) return 1;  // 31 = IS_PASSWORD, 61, 63-65
return scope == 66;
```

A private field gets no suggestions at all. The scope is read from the edit's
own input context (`GUID_PROP_INPUTSCOPE`, which `msctf!SetInputScope` feeds),
but `IsPrivateField` runs only in `EditControlFocusChangeInternal` — **as focus
enters the edit**. Setting the scope at `WM_SETFOCUS` is already too late.

## What does not work

- **`IS_FILE_FULLFILEPATH`** — what `shell32!CRunDlg::InitRunDlg` sets on the
  Run dialog. `comdlg32` never sets it; supplying it returns `S_OK` and changes
  nothing.
- **Detaching the IME or TSF** — see above. Worse, with the IME detached TSF3
  falls back to a context whose scope the hook does not own, so the private
  scope is ignored.
- **`IS_PASSWORD` at `WM_SETFOCUS`** — TSF3 has already recorded the edit as
  not private for that focus session.
- **Stripping `ISC_SHOWUIALL`** from `WM_IME_SETCONTEXT` — no effect.

`msctf!SetInputScope` refuses cross-thread callers (`E_INVALIDARG`), so no
external utility can set it on another app's window. Hence a hook.

## What this does

A global `WH_CBT` hook.

1. **At `HCBT_SETFOCUS`** — just before focus moves — subclass any `Edit`
   inside a `#32770`, decide whether it is a file dialog, and if so
   `SetInputScope(hwnd, IS_PASSWORD)` → TSF3 marks the filename box private →
   **no suggestions, no candidate session**.
2. **On `WM_IME_NOTIFY`/`IMN_OPENCANDIDATE`** — drop it for file dialogs so bit
   `0x40` is never set → **the dropdown survives** any candidate session that
   still opens (a CJK IME, say).
3. **Stay in front of autocomplete** — in the folder picker, autocomplete
   subclasses the edit *after* focus and lands in front of the hook, seeing
   `IMN_OPENCANDIDATE` first. When that happens a second, minimal layer is put
   on top. One layer only.

Documented APIs only. No binary patching, no hardcoded offsets, so a Windows
update will not break it the way a `shell32` patch would.

### Timing

| Moment | Result |
|---|---|
| **`HCBT_SETFOCUS`** | Open / Save / folder picker expose comdlg32 IDs → recognised; scope set before TSF3 evaluates the field |
| `WM_SETFOCUS` / first keystroke | Fallback for a dialog not built yet: the dropdown fix still applies; the scope takes effect from the next focus entry |

A negative answer before the first keystroke is **not** cached — it may only
mean the dialog is still constructing.

### Detection notes

- The legacy dialog exposes comdlg32 control IDs (`cmb13` 1148, `edt1` 1152,
  `cmb1` 1136); the **modern `IFileDialog` exposes none of them** — its controls
  sit inside DirectUI containers with ID `0`. It is identified by a
  `DUIViewWndClassName` / `SHELLDLL_DefView` descendant instead.
- `FindWindowEx` only walks **immediate** children; the shell view is several
  levels down, so the search must recurse (`EnumChildWindows`).

### Trade-off

To the input stack the filename box is a password field: **no text
suggestions and no CJK composition there**. The text itself is displayed
normally and autocomplete is unaffected. Every other control on the system is
untouched.

---

## Files

| File | Purpose |
|---|---|
| `pathscope_x64.dll` / `pathscope_x86.dll` | the hook |
| `pshostw_x64.exe` / `pshostw_x86.exe` | windowless host (autostart) |
| `pshost_x64.exe` / `pshost_x86.exe` | console host (testing; Ctrl+C to stop) |
| `pathscope.c`, `pshost.c`, `pshostw.c`, `pathscope.def` | source |
| `CMakeLists.txt`, `deploy.ps1` | build and deploy |
| `test/pickfolder.c` | test harness: opens the folder picker (`pickfolder`) or file picker (`pickfolder file`) |

Both architectures are required: a global hook only injects into processes of
matching bitness. The x86 build **needs `pathscope.def`** — without it
`__stdcall` decoration exports `_CbtProc@16` and `GetProcAddress` fails with
error 127.

## Build

```powershell
cmake -B build/x64 -A x64   -S . ; cmake --build build/x64 --config Release
cmake -B build/x86 -A Win32 -S . ; cmake --build build/x86 --config Release
```

Requires Visual Studio 2026 + Windows SDK. The hook links the **static CRT** so
its imports stay `user32` + `kernel32` only — it is injected into every GUI
process and must not need redistributable runtime DLLs.

## Install

`deploy.ps1` (elevated) copies to `C:\Tools\pathscope` and hash-verifies every
file. A loaded DLL cannot be overwritten but *can* be renamed, so it renames the
old one aside first.

```powershell
# status
Get-Process pshostw_x64, pshostw_x86 -ErrorAction SilentlyContinue

# stop (removes the hook from every process immediately)
taskkill /IM pshostw_x64.exe /IM pshostw_x86.exe

# start
C:\Tools\pathscope\pshostw_x64.exe pathscope_x64.dll
C:\Tools\pathscope\pshostw_x86.exe pathscope_x86.dll

# Running apps keep the DLL they already loaded until they restart: Windows
# matches a loaded module by path, even after the file was renamed. To apply a
# new build to running apps immediately, start the hosts on a fresh file name:
#   copy pathscope_x64.dll pathscope_x64.new.dll ; pshostw_x64.exe pathscope_x64.new.dll

# remove autostart
Unregister-ScheduledTask -TaskName 'pathscope (x64)' -Confirm:$false
Unregister-ScheduledTask -TaskName 'pathscope (x86)' -Confirm:$false
```

Diagnostic log: `%TEMP%\pathscope.log` (rotates to `.old` at 2 MB). Every line
has a timestamp, PID and exe name:

| Line | Meaning |
|---|---|
| `HOST x64 \| hook installed` | the host started and the hook is live (one per sign-in, per arch) |
| `HOST ... FAILED ...` | the host could not install the hook - nothing is fixed |
| `HOOK` / `SUBCLASS` | an `Edit` in a `#32770` got focus and was subclassed |
| `VERDICT ... filedialog=1 at=<when> by=<probe>` | recognised as a file dialog; `at=cbt` means before focus arrived |
| `PRIVATE-SCOPE ... hr=` | `SetInputScope(IS_PASSWORD)` result |
| `TOP ... re-layered` | autocomplete attached after the hook; second layer put in front |
| `DIAG` / `PROPS` | per-keystroke state of a file-dialog edit (capped): `outer`, `imc`, dropdown `dd=count/visible` |
| `STACK` | a candidate session opened on a file-dialog edit anyway — who sent it |
| `VERDICT ... filedialog=0` | judged not a file dialog, with the dialog's class and title |
| `PASS-CANDIDATE` | a suggestion session opened on an edit judged *not* a file dialog - a misjudged file dialog shows up here |
| `SKIP` | an `Edit` outside a `#32770` got focus - not handled (once per window) |

## Limits

- Does not reach **elevated** apps unless the host itself runs elevated.
- Only while running — no hook, no fix.
- Covers shell file dialogs; apps with custom-drawn pickers that bypass
  comdlg32 are unaffected.
- The Browse-For-Folder dialog (`SHBrowseForFolder`) and Explorer's address bar
  are **not** covered — different control layouts, not yet investigated.

## Status

This is a workaround for a Microsoft regression. The proper fix belongs in
`comdlg32`/`shell32`. Not yet reported upstream.
