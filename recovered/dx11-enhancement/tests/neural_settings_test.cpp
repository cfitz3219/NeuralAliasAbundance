#include "core/neural_settings.hpp"
#include <limits>
#include <cstdio>
int main(){
    using dx11_enhancement::NeuralSettings;
    NeuralSettings s; s.validate();
    for(unsigned i=0;i<8;++i){
        s={};
        switch(i){case 0:s.model=4;break;case 1:s.style=3;break;case 2:s.auto_skin=2;break;
        case 3:s.intensity=std::numeric_limits<float>::quiet_NaN();break;case 4:s.structure=3;break;
        case 5:s.tone=-1;break;case 6:s.skin=-2;break;case 7:s.skin=std::numeric_limits<float>::infinity();break;}
        bool rejected=false;try{s.validate();}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)return 1;
    }
    s={};s.skin=-1;s.validate();s.skin=2;s.intensity=2;s.structure=2;s.tone=2;s.validate();
    if(s==NeuralSettings{})return 2;
    std::puts("PASS: neural invalid controls rejected; supported boundaries accepted; change comparison works.");
}
