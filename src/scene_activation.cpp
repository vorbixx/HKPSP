#include "scene_activation.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace hk3ds {
bool SceneActivationBank::load(const char* path)
{
    auto* file=std::fopen(path,"rb");
    if(!file){if(errno!=ENOENT)return false;nodes.clear();targets.clear();return true;}
    SceneActivationBank next;char magic[4];unsigned n=0,t=0;
    bool ok=std::fread(magic,4,1,file)==1&&!std::memcmp(magic,"HKA1",4)&&
        std::fread(&n,4,1,file)==1&&std::fread(&t,4,1,file)==1&&n<=2048&&t<=8192;
    if(ok){next.nodes.resize(n);next.targets.resize(t);}
    for(unsigned i=0;ok&&i<n;++i){auto& node=next.nodes[i];
        ok=std::fread(&node,sizeof(node),1,file)==1&&node.op<=SceneActivationNode::SceneBool;
        if(node.op==SceneActivationNode::Constant)ok=ok&&node.a<=1&&node.b==0;
        else if(node.op<=SceneActivationNode::GreaterEqual)ok=ok&&node.a!=0;
        else if(node.op==SceneActivationNode::Not)ok=ok&&node.a<i&&node.b==0;
        else if(node.op==SceneActivationNode::SceneBool)ok=ok&&node.a!=0&&node.b!=0;
        else ok=ok&&node.a<i&&node.b<i;
    }
    for(auto& target:next.targets){
        ok=ok&&std::fread(&target,sizeof(target),1,file)==1&&target.kind<=SceneActivationTarget::AcidWater&&target.node<n&&
            (target.kind==SceneActivationTarget::Gate?target.id<32:target.id!=0);
    }
    ok=ok&&std::fgetc(file)==EOF;std::fclose(file);
    if(!ok)return false;
    std::sort(next.targets.begin(),next.targets.end(),[](const auto& a,const auto& b){return a.kind<b.kind||(a.kind==b.kind&&a.id<b.id);});
    for(unsigned i=1;i<next.targets.size();++i)if(next.targets[i].kind==next.targets[i-1].kind&&next.targets[i].id==next.targets[i-1].id)return false;
    *this=std::move(next);return true;
}
void SceneActivationState::reset(const SceneActivationBank& bank,const SavedPlayerData& save,
                                 const SavedPlayerData::Entry* overrides,std::size_t overrideCount,
                                 const SceneBoolSave* sceneBools)
{
    for(auto& ids:disabled_)ids.clear();
    std::vector<unsigned char> values;values.reserve(bank.nodes.size());
    for(const auto& n:bank.nodes){
        auto value=save.get(n.a);
        for(std::size_t i=0;overrides&&i<overrideCount;++i)
            if(overrides[i].key==n.a){value=overrides[i].value;break;}
        const auto rhs=static_cast<std::int32_t>(n.b);bool result=false;
        switch(n.op){
        case SceneActivationNode::Constant:result=n.a!=0;break;
        case SceneActivationNode::Equal:result=value==rhs;break;
        case SceneActivationNode::NotEqual:result=value!=rhs;break;
        case SceneActivationNode::Less:result=value<rhs;break;
        case SceneActivationNode::LessEqual:result=value<=rhs;break;
        case SceneActivationNode::Greater:result=value>rhs;break;
        case SceneActivationNode::GreaterEqual:result=value>=rhs;break;
        case SceneActivationNode::And:result=n.a<values.size()&&n.b<values.size()&&values[n.a]&&values[n.b];break;
        case SceneActivationNode::Or:result=n.a<values.size()&&n.b<values.size()&&(values[n.a]||values[n.b]);break;
        case SceneActivationNode::Not:result=n.a<values.size()&&!values[n.a];break;
        case SceneActivationNode::SceneBool:result=sceneBools&&sceneBools->get({n.a,n.b});break;
        }
        values.push_back(result);
    }
    for(const auto& t:bank.targets)if(t.kind<disabled_.size()&&t.node<values.size()&&!values[t.node])disabled_[t.kind].push_back(t.id);
    for(auto& ids:disabled_){std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());}
}
bool SceneActivationState::enabled(unsigned kind,unsigned id) const
{
    return kind>=disabled_.size()||!std::binary_search(disabled_[kind].begin(),disabled_[kind].end(),id);
}
}
