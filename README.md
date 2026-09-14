# pathscope

Restores shell **path auto-completion** in Windows file dialogs, and suppresses
the text-suggestion popup over the filename box.

Built for Windows 11 **10.0.29648.1000** (Canary), 2026-09-14.

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

That guard exists so autocomplete gets out of the way of a **CJK IME candidate
list**. Windows 11 delivers ordinary text suggestions through the *same*
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
suppress the predictor on this build. Whatever protects the Run dialog, it is
not (only) the input scope.

Also note: `msctf!SetInputScope` refuses cross-thread callers —

```c
if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()) { ... }
return -2147024809;   // 0x80070057 E_INVALIDARG
```

so no external utility can set it on another app's window. That is why this is
a hook and not a standalone program.

## What this does

A global `WH_CBT` hook. On `HCBT_SETFOCUS` (which fires *before* focus is
assigned), if the window is a file dialog filename edit
(`Edit < ComboBox < ComboBoxEx32 < #32770`):

1. **Subclass it** and drop `WM_IME_NOTIFY`/`IMN_OPENCANDIDATE`, so bit `0x40`
   is never set → **the dropdown survives**.
2. **`ImmAssociateContext(hwnd, NULL)`** → no candidate session can attach →
   **no suggestion popup**.

Documented APIs only. No binary patching, no hardcoded offsets, so a Windows
update will not break it the way a `shell32` patch would.

### Trade-off

CJK IME composition is disabled **in the filename box only**. Every other
control on the system is untouched.

### Detection note

Do **not** identify the dialog by probing for `DUIViewWndClassName` or
`SHELLDLL_DefView`. At `HCBT_SETFOCUS` time the shell view does not exist yet,
so the probe always fails. The control shape alone is the reliable signal.

---

## Files

| File | Purpose |
|---|---|
| `pathscope_x64.dll` / `pathscope_x86.dll` | the hook |
| `pshostw_x64.exe` / `pshostw_x86.exe` | windowless host (autostart) |
| `pshost_x64.exe` / `pshost_x86.exe` | console host (testing; Ctrl+C to stop) |
| `pathscope.c`, `pshost.c`, `pshostw.c` | source |
| `arch.cmd`, `buildall.cmd` | build scripts (needs VS 2026 + Win SDK) |

Both architectures are needed: a global hook only injects into processes of
matching bitness.

## Control

```powershell
# status
Get-Process pshostw_x64, pshostw_x86 -ErrorAction SilentlyContinue

# stop (removes the hook from every process immediately)
taskkill /IM pshostw_x64.exe /IM pshostw_x86.exe

# start
C:\Tools\pathscope\pshostw_x64.exe pathscope_x64.dll
C:\Tools\pathscope\pshostw_x86.exe pathscope_x86.dll

# remove autostart entirely
Unregister-ScheduledTask -TaskName 'pathscope (x64)' -Confirm:$false
Unregister-ScheduledTask -TaskName 'pathscope (x86)' -Confirm:$false
```

Diagnostic log: `%TEMP%\pathscope.log` (one line per control it acts on).

## Limits

- Does not reach **elevated** apps unless the host itself runs elevated.
- Only while running — no hook, no fix.
- Detection targets the shell file dialog shape; custom dialogs that merely
  look similar are not affected.

## Rebuilding

```cmd
buildall.cmd
```

Requires Visual Studio 2026 (`vcvars64.bat` / `vcvarsamd64_x86.bat`) and the
Windows SDK. Adjust `VSROOT` in `arch.cmd` if VS moves.
