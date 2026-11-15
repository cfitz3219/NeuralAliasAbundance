// NeuralAliasDlaa64.exe <channel name> <game process id>
// Started by the 32-bit in-game half; never by hand. No window, no console.
#include <windows.h>
#include <shellapi.h>
#include <cwchar>

namespace na::dlaa_remote { int RunCompanion(const wchar_t* name, DWORD game_pid); }

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 3) return 1;
    const DWORD pid = wcstoul(argv[2], nullptr, 10);
    if (!pid) return 1;
    return na::dlaa_remote::RunCompanion(argv[1], pid);
}
