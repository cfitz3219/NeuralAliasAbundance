// DRED report (host_dx12/diag/dred.hpp): enabled before the device exists, nothing reported
// while the device is healthy, and exactly one report file after a real removal
// (ID3D12Device5::RemoveDevice, no TDR). Hardware or WARP; SKIP (77) without either.
#include "test_support.hpp"
#include "host_dx12/diag/dred.hpp"

#include <cstdio>
#include <string>

int main()
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    const size_t len = wcslen(dir);
    if (len && (dir[len - 1] == L'\\' || dir[len - 1] == L'/')) dir[len - 1] = 0;
    const std::string summary = na::diag::EnableDred(true, true, dir);
    std::printf("%s\n", summary.c_str());
    CHECK(summary.rfind("dred:", 0) == 0);

    test::Device d;
    if (!test::CreateDevice(d)) return test::SkipNoDevice("dred_test");

    CHECK(na::diag::DescribeRemoval(d.device.Get()).empty());
    na::diag::ReportIfRemoved(d.device.Get());
    CHECK(!na::diag::DredReported());
    na::diag::ReportIfRemoved(nullptr);
    CHECK(!na::diag::DredReported());

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\NeuralAlias-dred-%lu.txt", dir, GetCurrentProcessId());
    DeleteFileW(path);

    test::ComPtr<ID3D12Device5> d5;
    if (FAILED(d.device.As(&d5)) || !d5) {
        std::printf("ID3D12Device5 unavailable: removal part skipped\n");
        return test::finish("dred_test");
    }
    d5->RemoveDevice();
    CHECK(d.device->GetDeviceRemovedReason() != S_OK);
    const std::string text = na::diag::DescribeRemoval(d.device.Get());
    std::printf("%s", text.c_str());
    CHECK(text.find("device removed: reason=0x") == 0);

    na::diag::ReportIfRemoved(d.device.Get());
    CHECK(na::diag::DredReported());
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(f != INVALID_HANDLE_VALUE);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[64] = {};
        DWORD n = 0;
        ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
        CloseHandle(f);
        CHECK(std::string(buf).find("device removed") == 0);
    }
    // Once per process: a second removal sighting leaves the file alone.
    DeleteFileW(path);
    na::diag::ReportIfRemoved(d.device.Get());
    CHECK(GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES);
    return test::finish("dred_test");
}
