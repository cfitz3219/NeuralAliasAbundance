#include "test_support.hpp"
#include "../../profiles/layout_cache.hpp"
using namespace na::profiles;
static Profile profile() {
    return {std::string(64,'a'),std::string(64,'b'),std::string(64,'c'),1,
        {{Role::Color,10,640,360,3,0,std::string(64,'d')},
         {Role::Depth,40,640,360,4,0,std::string(64,'e')}}};
}
struct Directory {
    std::filesystem::path path,root;
    Directory() {
        std::random_device random;
        root=std::filesystem::canonical(std::filesystem::current_path());
        path=root/("na-profile-test-"+std::to_string(random())+"-"+std::to_string(random()));
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("temporary directory collision");
    }
    ~Directory() {
        std::error_code e;
        const auto resolved=std::filesystem::weakly_canonical(path,e);
        if (!e && resolved.parent_path()==root && resolved.filename()==path.filename())
            std::filesystem::remove_all(resolved,e);
    }
};
int main() { return test::run({
    {"bounded versioned profile round-trip retains evidence not runtime IDs", [] {
        const auto p=profile(); Profile decoded;
        CHECK(Decode(Encode(p),decoded)); CHECK(decoded.bindings.size()==2);
        CHECK(decoded.evidence_sha256==p.evidence_sha256 && decoded.bindings[1].role==Role::Depth);
    }},
    {"malformed truncated oversized duplicate and unknown schema rejected", [] {
        auto p=profile(); Profile decoded;
        CHECK(!Decode(Encode(p)+"trailing",decoded)); CHECK(decoded.bindings.empty());
        CHECK(!Decode(Encode(p).substr(0,50),decoded));
        CHECK(!Decode(std::string(8193,'x'),decoded));
        p.bindings.push_back(p.bindings[0]); CHECK(Encode(p).empty());
        auto text=Encode(profile()); text.replace(text.find(" 1"),2," 2"); CHECK(!Decode(text,decoded));
    }},
    {"second fixture launch uses matching hints and invalidates structural changes", [] {
        Directory d; auto p=profile(); unsigned discoveries=0;
        auto launch=[&] {
            auto r=Load(d.path,p.executable_sha256,p.layout_sha256,p.classifier_version);
            if (r.result!=LoadResult::HintsAvailable) { ++discoveries; CHECK(Save(d.path,p)); }
            return r;
        };
        CHECK(launch().result==LoadResult::Missing); CHECK(discoveries==1);
        CHECK(launch().result==LoadResult::HintsAvailable); CHECK(discoveries==1);
        p.layout_sha256=std::string(64,'f');
        CHECK(launch().result==LoadResult::Mismatch); CHECK(discoveries==2);
        CHECK(launch().result==LoadResult::HintsAvailable);
        ++p.classifier_version;
        CHECK(launch().result==LoadResult::Mismatch); CHECK(discoveries==3);
        CHECK(launch().result==LoadResult::HintsAvailable);
    }},
    {"executable mismatch and filename traversal cannot produce hints", [] {
        Directory d; auto p=profile(); CHECK(Save(d.path,p));
        CHECK(Load(d.path,std::string(64,'f'),p.layout_sha256,1).result==LoadResult::Missing);
        CHECK(Load(d.path,"../bad",p.layout_sha256,1).result==LoadResult::Invalid);
        std::ofstream f(d.path/(p.executable_sha256+".profile"));
        p.executable_sha256=std::string(64,'f'); f<<Encode(p); f.close();
        CHECK(Load(d.path,std::string(64,'a'),p.layout_sha256,1).result==LoadResult::Mismatch);
    }},
    {"corrupt file and invalid save preserve fail-closed lookup", [] {
        Directory d; auto p=profile(); CHECK(Save(d.path,p));
        auto invalid=p; invalid.evidence_sha256.clear(); CHECK(!Save(d.path,invalid));
        CHECK(Load(d.path,p.executable_sha256,p.layout_sha256,1).result==LoadResult::HintsAvailable);
        std::ofstream f(d.path/(p.executable_sha256+".profile"),std::ios::trunc); f<<"partial write"; f.close();
        CHECK(Load(d.path,p.executable_sha256,p.layout_sha256,1).result==LoadResult::Invalid);
    }}
}); }
