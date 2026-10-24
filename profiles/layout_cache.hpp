// A8 portable profile persistence. Profiles are hints, never resource leases.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace na::profiles {
enum class Role : uint32_t { Color=1, Depth=2, Motion=3, Projection=4, View=5 };
struct Binding {
    Role role{};
    uint32_t format{}, width{}, height{}, slot{};
    uint64_t byte_offset{};
    std::string shader_sha256;
};
struct Profile {
    std::string executable_sha256, layout_sha256, evidence_sha256;
    uint32_t classifier_version{1};
    std::vector<Binding> bindings;
};
inline bool Digest(const std::string& s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    });
}
inline bool Valid(const Profile& p) {
    if (!Digest(p.executable_sha256) || !Digest(p.layout_sha256) || !Digest(p.evidence_sha256) ||
        !p.classifier_version || p.classifier_version>65535 || p.bindings.empty() || p.bindings.size()>5) return false;
    uint32_t roles=0;
    for (const auto& b:p.bindings) {
        const auto role=static_cast<uint32_t>(b.role);
        if (role<1 || role>5 || (roles&(1u<<role)) || !b.format || b.format>2048 ||
            !b.width || !b.height || b.width>16384 || b.height>16384 || b.slot>1048576 ||
            b.byte_offset>(uint64_t(1)<<40) || !Digest(b.shader_sha256)) return false;
        roles|=1u<<role;
    }
    return true;
}
inline std::string Encode(const Profile& p) {
    if (!Valid(p)) return {};
    std::ostringstream out;
    out<<"NA_LAYOUT_PROFILE 1\n"<<p.executable_sha256<<'\n'<<p.layout_sha256<<'\n'
       <<p.evidence_sha256<<'\n'<<p.classifier_version<<' '<<p.bindings.size()<<'\n';
    for (const auto& b:p.bindings)
        out<<static_cast<uint32_t>(b.role)<<' '<<b.format<<' '<<b.width<<' '<<b.height<<' '
           <<b.slot<<' '<<b.byte_offset<<' '<<b.shader_sha256<<'\n';
    return out.str();
}
inline bool Decode(const std::string& bytes,Profile& output) {
    output={};
    if (bytes.empty() || bytes.size()>8192) return false;
    std::istringstream in(bytes);
    Profile p{}; std::string magic; uint32_t schema{},count{};
    in>>magic>>schema>>p.executable_sha256>>p.layout_sha256>>p.evidence_sha256>>p.classifier_version>>count;
    if (!in || magic!="NA_LAYOUT_PROFILE" || schema!=1 || count<1 || count>5) return false;
    for (uint32_t i=0;i<count;++i) {
        Binding b{}; uint32_t role{};
        in>>role>>b.format>>b.width>>b.height>>b.slot>>b.byte_offset>>b.shader_sha256;
        if (!in) return false;
        b.role=static_cast<Role>(role); p.bindings.push_back(b);
    }
    in>>std::ws;
    if (!in.eof() || !Valid(p)) return false;
    output=std::move(p); return true;
}
enum class LoadResult { Missing, Invalid, Mismatch, HintsAvailable };
struct Lookup { LoadResult result{LoadResult::Missing}; Profile hints{}; };
inline Lookup Load(const std::filesystem::path& root,const std::string& executable,
    const std::string& observed_layout,uint32_t classifier_version) {
    Lookup result{};
    if (!Digest(executable) || !Digest(observed_layout) || !classifier_version || classifier_version>65535) {
        result.result=LoadResult::Invalid; return result;
    }
    // Only a validated hex key contributes to the filename; no stored paths or IDs.
    std::ifstream file(root/(executable+".profile"),std::ios::binary);
    if (!file) return result;
    std::string bytes(8193,'\0');
    file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    const auto count=file.gcount();
    if (file.bad() || count>8192) { result.result=LoadResult::Invalid; return result; }
    bytes.resize(static_cast<size_t>(count));
    Profile p{};
    if (!Decode(bytes,p)) { result.result=LoadResult::Invalid; return result; }
    if (p.executable_sha256!=executable || p.layout_sha256!=observed_layout || p.classifier_version!=classifier_version) {
        result.result=LoadResult::Mismatch; return result;
    }
    result.result=LoadResult::HintsAvailable; result.hints=std::move(p); return result;
}
inline bool Save(const std::filesystem::path& root,const Profile& profile) {
    const auto bytes=Encode(profile); if (bytes.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(root,error); if (error) return false;
    static std::atomic<uint64_t> sequence{0};
    // Random suffix avoids cross-process clobber; a temporary is never a load key.
    std::string suffix;
    try {
        std::random_device random;
        suffix=std::to_string(random())+"-"+std::to_string(random())+"-"+std::to_string(++sequence);
    } catch (...) { return false; }
    const auto final=root/(profile.executable_sha256+".profile");
    const auto temporary=root/(profile.executable_sha256+"."+suffix+".tmp");
    {
        std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
        if (!file) return false;
        file.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) { file.close(); std::filesystem::remove(temporary,error); return false; }
        file.close();
        if (!file) { std::filesystem::remove(temporary,error); return false; }
    }
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(),final.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return true;
#else
    std::filesystem::rename(temporary,final,error); if (!error) return true;
#endif
    std::filesystem::remove(temporary,error); return false;
}
} // namespace na::profiles
