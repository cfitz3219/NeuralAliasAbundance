#pragma once
#include <cmath>
#include <fstream>
#include <filesystem>
#include <stdexcept>
namespace dx11_enhancement {
struct NeuralSettings {
    unsigned model=0,style=0,auto_skin=1;
    float intensity=1,structure=1,tone=0,skin=1;
    bool operator==(const NeuralSettings& b)const{
        return model==b.model && style==b.style && auto_skin==b.auto_skin && intensity==b.intensity &&
            structure==b.structure && tone==b.tone && skin==b.skin;
    }
    void validate()const{
        const auto range=[](float f,float lo,float hi){return std::isfinite(f) && f>=lo && f<=hi;};
        if(model>3 || style>2 || auto_skin>1 || !range(intensity,0,2) || !range(structure,0,2) ||
            !range(tone,0,2) || !range(skin,-1,2))throw std::runtime_error("invalid neural settings");
    }
    void save(const std::filesystem::path& path)const{
        validate();std::ofstream out(path);out.precision(9);
        out<<1<<' '<<model<<' '<<style<<' '<<auto_skin<<' '<<intensity<<' '<<structure<<' '<<tone<<' '<<skin<<'\n';
        out.flush();if(!out)throw std::runtime_error("write neural settings");
    }
    static NeuralSettings read(const std::filesystem::path& path){
        NeuralSettings s;unsigned version=0;std::ifstream in(path);
        if(!(in>>version>>s.model>>s.style>>s.auto_skin>>s.intensity>>s.structure>>s.tone>>s.skin) || version!=1)
            throw std::runtime_error("read neural settings/version");
        in>>std::ws;if(!in.eof())throw std::runtime_error("extra neural settings data");
        s.validate();return s;
    }
};
}
