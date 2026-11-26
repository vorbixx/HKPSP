#include "../include/platform.hpp"
#include "draw_list.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <cmath>

int main(int argc,char** argv){
    try{
        hkpsp::HostOptions options;
        for(int i=1;i<argc;++i){
            const std::string arg=argv[i];
            if(arg=="--self-test"){hkpsp::checkDrawList();std::cout<<"Projection contracts: flat, rotated, opacity, culling passed\n";return 0;}
            if(arg=="--check-asset"){
                if(i+2>=argc)throw std::runtime_error("Required: --check-asset room|animation|texture FILE");
                const std::string kind=argv[++i],path=argv[++i];
                if(kind=="room")hkpsp::readRoom(path);
                else if(kind=="animation")hkpsp::readAnimation(path);
                else if(kind=="texture")hkpsp::readTexture(path);
                else throw std::runtime_error("Unknown asset kind: "+kind);
                std::cout<<"Asset valid: "<<kind<<'\n';return 0;
            }
            if(arg=="--fast"){options.realtime=false;continue;}
            if(arg=="--menus"){options.menus=true;continue;}
            if(arg=="--room-ge"){options.roomGe=true;continue;}
            if(arg=="--no-raster"){options.raster=false;continue;}
            if(arg=="--captures-only"){options.capturesOnly=true;continue;}
            if(arg=="--pose"){
                if(i+4>=argc||std::string(argv[i+1])!="hero"||std::string(argv[i+3])!="camera")
                    throw std::runtime_error("Required: --pose hero X,Y camera X,Y");
                const auto pair=[](const std::string& text,float& x,float& y){
                    const auto comma=text.find(',');std::size_t a=0,b=0;
                    if(comma==std::string::npos)throw std::runtime_error("Pose requires X,Y");
                    x=std::stof(text.substr(0,comma),&a);y=std::stof(text.substr(comma+1),&b);
                    if(a!=comma||b!=text.size()-comma-1||!std::isfinite(x)||!std::isfinite(y))
                        throw std::runtime_error("Invalid pose coordinates");
                };
                pair(argv[i+2],options.pose.heroX,options.pose.heroY);
                pair(argv[i+4],options.pose.cameraX,options.pose.cameraY);
                options.fixedPose=true;i+=4;continue;
            }
            if(i+1>=argc)throw std::runtime_error("Missing value for "+arg);
            const std::string value=argv[++i];
            if(arg=="--assets")options.assets=value;
            else if(arg=="--data")options.data=value;
            else if(arg=="--source")options.source=value;
            else if(arg=="--output")options.output=value;
            else if(arg=="--replay")options.replay=value;
            else if(arg=="--ui-replay"){options.uiReplay=value;options.menus=true;}
            else if(arg=="--save")options.save=value;
            else if(arg=="--frames"||arg=="--every"){
                std::size_t end=0;const auto number=std::stoul(value,&end);
                if(end!=value.size()||!number||number>1000000)throw std::runtime_error("Invalid number: "+value);
                if(arg=="--frames")options.frames=static_cast<unsigned>(number);else options.every=static_cast<unsigned>(number);
            }else throw std::runtime_error("Unknown option: "+arg);
        }
        if(options.assets.empty()||options.source.empty()||options.output.empty())throw std::runtime_error("Required: --assets DIR --source HK3DS --output DIR [--frames N] [--every N] [--fast]");
        const auto platform=hkpsp::makeHost(options);hkpsp::runGame(*platform,options.frames);return 0;
    }catch(const std::exception& error){std::cerr<<"hkpsp-host: "<<error.what()<<'\n';return 1;}
}
