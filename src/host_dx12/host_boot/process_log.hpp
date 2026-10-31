#pragma once
#include <windows.h>
#include <string>
#include <algorithm>

namespace na::host {
// Worker-side path construction. Helpers and game processes never truncate or
// interleave each other's receipts. No file creation or mutation here.
inline bool ProcessLogPath(wchar_t* out,size_t capacity,const wchar_t* configured) {
    if(!out || !capacity)return false;
    out[0]=0;if(!configured || !configured[0])return false;
    std::wstring path(configured);
    const auto slash=path.find_last_of(L"/\\"),dot=path.find_last_of(L'.');
    const auto at=dot!=std::wstring::npos && (slash==std::wstring::npos || dot>slash)?dot:path.size();
    path.insert(at,L"-"+std::to_wstring(GetCurrentProcessId()));
    if(path.size()>=capacity)return false;
    std::copy(path.begin(),path.end(),out);out[path.size()]=0;return true;
}
}
