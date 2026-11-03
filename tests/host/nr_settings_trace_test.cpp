#include "test_support.hpp"
#include "host_dx12/settings.hpp"
int main() {
    using namespace na::settings;
    SetNrEnabled(true,NrWriteSource::DefaultIni);
    SetNrEnabled(false,NrWriteSource::UserIni);
    SetNrEnabled(true,NrWriteSource::Overlay,1);
    auto h=GetNrWrites();
    CHECK(h.count==3);
    CHECK(h.entries[0].after==1 && h.entries[0].source==NrWriteSource::DefaultIni);
    CHECK(h.entries[1].before==1 && h.entries[1].after==0 && h.entries[1].source==NrWriteSource::UserIni);
    CHECK(h.entries[2].before==0 && h.entries[2].after==1 && h.entries[2].input_flags==1);
    CHECK(h.entries[2].thread!=0 && Get().nr_enabled.load()==1);
    for(uint32_t i=0;i<40;++i) SetNrEnabled((i%2)!=0,NrWriteSource::Overlay,2);
    h=GetNrWrites(); CHECK(h.count==16);
    CHECK(h.entries[0].sequence==28 && h.entries[15].sequence==43);
    for(uint32_t i=1;i<h.count;++i) {
        CHECK(h.entries[i].sequence==h.entries[i-1].sequence+1);
        CHECK(h.entries[i].before==h.entries[i-1].after);
        CHECK(h.entries[i].input_flags==2);
    }
    return test::finish("nr_settings_trace_test");
}
