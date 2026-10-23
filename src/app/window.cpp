#include <windows.h>
#include <string>
#include <stdexcept>
#include <cstdio>
namespace {
HWND fg=nullptr, route=nullptr, description=nullptr;
HFONT font=nullptr;
HWND control(HWND parent,const wchar_t* kind,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id) {
    const auto result=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(result && font) SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    return result;
}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CREATE) {
        font=static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        control(window,L"STATIC",L"NeuralAliasAbundance - DX12 foundation",0,24,20,580,24,0);
        control(window,L"STATIC",L"Standalone project. No ReShade or Alien: Isolation dependency.",0,24,52,580,24,0);
        control(window,L"STATIC",L"Input route preference (preview only):",0,24,96,560,24,0);
        route=control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,24,124,560,160,100);
        for(const auto* label:{L"Automatic: validated upscaler, otherwise adapter",L"Existing upscaler integration",L"Engine/game adapter (games without an upscaler)"})
            SendMessageW(route,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
        SendMessageW(route,CB_SETCURSEL,0,0);
        description=control(window,L"STATIC",L"No game attached. DLL presence alone does not establish usable inputs.",0,24,166,560,50,101);
        const auto sr=control(window,L"BUTTON",L"DLAA / DLSS - backend connection pending",BS_AUTOCHECKBOX,24,230,560,28,102);
        const auto nr=control(window,L"BUTTON",L"Neural Rendering - experimental integration pending",BS_AUTOCHECKBOX,24,268,560,28,103);
        fg=control(window,L"BUTTON",L"Frame Generation - Not implemented yet",BS_AUTOCHECKBOX,24,306,560,28,104);
        EnableWindow(sr,FALSE);EnableWindow(nr,FALSE);EnableWindow(fg,FALSE);
        control(window,L"STATIC",L"This first milestone tests inputs and DX12 independently.\r\nIt does not install anything into a game or enable enhancement effects.",0,24,356,560,56,0);
        control(window,L"BUTTON",L"Close",BS_DEFPUSHBUTTON|WS_TABSTOP,464,422,120,30,IDCANCEL);
        return 0;
    }
    if(message==WM_COMMAND) {
        if(LOWORD(w)==IDCANCEL) { DestroyWindow(window);return 0; }
        if(LOWORD(w)==100 && HIWORD(w)==CBN_SELCHANGE) {
            const auto selection=SendMessageW(route,CB_GETCURSEL,0,0);
            const wchar_t* text=selection==1?L"Requires observed compatible upscaler evaluations and validated resources.":
                selection==2?L"Requires a validated engine/game adapter. Optical flow alone does not supply depth or camera data.":
                L"Prefer validated upscaler inputs; fall back to a validated adapter. Otherwise remain inactive.";
            SetWindowTextW(description,text);return 0;
        }
    }
    if(message==WM_DESTROY) { PostQuitMessage(0);return 0; }
    return DefWindowProcW(window,message,w,l);
}
}
int run_window(bool smoke) {
    WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"NeuralAliasAbundanceSettings";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
    if(!RegisterClassW(&wc)) return 1;
    const auto window=CreateWindowExW(0,wc.lpszClassName,L"NeuralAliasAbundance",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,640,510,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window) return 1;
    if(smoke) {
        bool ok=fg && !IsWindowEnabled(fg) && SendMessageW(fg,BM_GETCHECK,0,0)==BST_UNCHECKED;
        ok=ok && SendMessageW(route,CB_GETCOUNT,0,0)==3;
        for(int selection=0;selection<3;++selection) {
            SendMessageW(route,CB_SETCURSEL,static_cast<WPARAM>(selection),0);
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(100,CBN_SELCHANGE),reinterpret_cast<LPARAM>(route));
            ok=ok && GetWindowTextLengthW(description)>20;
        }
        DestroyWindow(window);std::puts(ok?"PASS: standalone settings, both routes, FG disabled":"FAIL: settings smoke");return ok?0:1;
    }
    ShowWindow(window,SW_SHOW);UpdateWindow(window);MSG message{};
    BOOL result;
    while((result=GetMessageW(&message,nullptr,0,0))>0) {
        if(!IsDialogMessageW(window,&message)) { TranslateMessage(&message);DispatchMessageW(&message); }
    }
    return result<0?1:0;
}
