/* pickfolder - opens the shell folder picker (IFileOpenDialog + FOS_PICKFOLDERS),
   the same call Electron's dialog.showOpenDialog({properties:['openDirectory']})
   makes. Test harness for pathscope. "pickfolder file" opens the file picker. */
#define COBJMACROS
#include <windows.h>
#include <shobjidl.h>

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show)
{
    IFileOpenDialog *d = NULL;
    DWORD opts = 0;
    BOOL folder = !(cmd && lstrcmpiW(cmd, L"file") == 0);
    (void)hi; (void)hp; (void)show;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IFileOpenDialog, (void **)&d))) return 2;
    IFileOpenDialog_GetOptions(d, &opts);
    if (folder) IFileOpenDialog_SetOptions(d, opts | FOS_PICKFOLDERS);
    IFileOpenDialog_SetTitle(d, folder ? L"pickfolder test" : L"pickfile test");
    IFileOpenDialog_Show(d, NULL);
    IFileOpenDialog_Release(d);
    CoUninitialize();
    return 0;
}
