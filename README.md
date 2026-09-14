# pathscope

Restores shell **path auto-completion** in Windows file dialogs, and suppresses
the text-suggestion popup over the filename box.

Works in both the legacy **Open** dialog and the modern **Save As**
(`IFileDialog`). Analysed and verified on Windows 11 **10.0.29648.1000**
(Canary), 2026-09-14.

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

## What does not work

`shell32!CRunDlg::InitRunDlg` protects the Run dialog:

```c
if (GetComboBoxInfo(v27, &pcbi) && pcbi.hwndItem != nullptr)
    SetInputScope(pcbi.hwndItem, IS_FILE_FULLFILEPATH);
```

`comdlg32` never makes that call for its filename box — it does not even import
`SetInputScope`. Supplying the missing call from this hook was **tested and
returns `S_OK` while changing nothing**: `IS_FILE_FULLFILEPATH` does not
suppress the predictor on this build.

`msctf!SetInputScope` also refuses cross-thread callers —

```c
if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) { ... }
return -2147024809;   // 0x80070057 E_INVALIDARG
```

— so no external utility can set it on another app's window. Hence a hook.

## What this does

A global `WH_CBT` hook.

1. **At `HCBT_SETFOCUS`** — subclass any `Edit` inside a `#32770`. Deliberately
   broad; subclassing is harmless and the real decision is deferred.
2. **At `WM_SETFOCUS` / first keystroke** — decide whether the dialog is a file
   dialog, and if so `ImmAssociateContext(hwnd, NULL)` so no candidate session
   can attach → **no suggestion popup**.
3. **On `WM_IME_NOTIFY`/`IMN_OPENCANDIDATE`** — drop it for file dialogs so bit
   `0x40` is never set → **the dropdown survives**.

Documented APIs only. No binary patching, no hardcoded offsets, so a Windows
update will not break it the way a `shell32` patch would.

### Timing is the whole problem

Identifying the dialog is only possible in a narrow window:

| Moment | Result |
|---|---|
| `HCBT_SETFOCUS` | Modern `IFileDialog` is not built yet — unrecognisable |
| First `IMN_OPENCANDIDATE` | A suggestion has already shown; detaching is too late |
| **`WM_SETFOCUS` / first keystroke** | Dialog complete, ~160ms ahead of the suggestion |

A negative answer at `WM_SETFOCUS` is **not** cached — it may only mean the
dialog is still constructing — so the check repeats on the first keystroke.

### Detection notes

- The legacy dialog exposes comdlg32 control IDs (`cmb13` 1148, `edt1` 1152,
  `cmb1` 1136); the **modern `IFileDialog` exposes none of them** — its controls
  sit inside DirectUI containers with ID `0`. It is identified by a
  `DUIViewWndClassName` / `SHELLDLL_DefView` descendant instead.
- `FindWindowEx` only walks **immediate** children; the shell view is several
  levels down, so the search must recurse (`EnumChildWindows`).

### Trade-off

CJK IME composition is disabled **in the filename box only**. Every other
control on the system is untouched.

---

## Files

| File | Purpose |
|---|---|
| `pathscope_x64.dll` / `pathscope_x86.dll` | the hook |
| `pshostw_x64.exe` / `pshostw_x86.exe` | windowless host (autostart) |
| `pshost_x64.exe` / `pshost_x86.exe` | console host (testing; Ctrl+C to stop) |
| `pathscope.c`, `pshost.c`, `pshostw.c`, `pathscope.def` | source |
| `CMakeLists.txt`, `deploy.ps1` | build and deploy |

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

# remove autostart
Unregister-ScheduledTask -TaskName 'pathscope (x64)' -Confirm:$false
Unregister-ScheduledTask -TaskName 'pathscope (x86)' -Confirm:$false
```

Diagnostic log: `%TEMP%\pathscope.log`.

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
