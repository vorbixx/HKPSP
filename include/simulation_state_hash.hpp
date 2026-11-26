#pragma once
#include <algorithm>
// Diagnostic observation only. The shipped simulation has no hashing work.
#if defined(HKPSP_SIM_TRACE) || defined(HKPSP_PERF)
#include "tutorial.hpp"
#include <cstring>
#include <type_traits>
#include <stdexcept>

namespace hk3ds {
// Explicit field traversal: never read padding, vector capacity or addresses.
// The order is part of the diagnostic schema, shared by baseline and candidate.
struct SimulationStateHash {
    struct Result {std::uint64_t value;std::uint32_t leaves;};
    std::uint64_t value=14695981039346656037ull;
    std::uint32_t leaves=0;
    // Three allocation-free walks: collect bindings, locate owned targets in
    // field order, then hash. Addresses are used only for equality, never data.
    struct Binding {std::uintptr_t address=0;std::uint32_t node=0,ordinal=0;};
    std::array<Binding,1024> bindings{};
    std::size_t bindingCount=0;
    unsigned pass=0;
    std::uint32_t nodes=0;
    Binding* findBinding(std::uintptr_t address) {
        return std::lower_bound(bindings.data(),bindings.data()+bindingCount,address,
            [](const Binding& item,std::uintptr_t key){return item.address<key;});
    }
    void node(const void* address) {
        ++nodes;if(pass!=1)return;
        const auto key=reinterpret_cast<std::uintptr_t>(address);
        auto* found=findBinding(key);
        if(found!=bindings.data()+bindingCount&&found->address==key&&!found->node)found->node=nodes;
    }
    template<class T> void binding(T* pointer) {
        if(!pointer){add(0u);return;}
        const auto address=reinterpret_cast<std::uintptr_t>(pointer);
        auto* found=findBinding(address);
        if(found==bindings.data()+bindingCount||found->address!=address){
            if(pass!=0||bindingCount==bindings.size())throw std::runtime_error("Simulation hash binding overflow");
            std::move_backward(found,bindings.data()+bindingCount,bindings.data()+bindingCount+1);
            *found={address,0,static_cast<std::uint32_t>(bindingCount++)};
        }
        // Owned target: field traversal index. External immutable input or
        // callback: first-binding ordinal, preserving alias relationships.
        add(found->node?std::uint64_t(found->node):(1ull<<32)+found->ordinal+1);
    }
    void word(std::uint64_t bits) {value=(value^bits)*1099511628211ull;++leaves;}
    template<class T,std::enable_if_t<std::is_integral_v<T>||std::is_enum_v<T>,int> =0>
    void add(T v) {word(static_cast<std::uint64_t>(v));}
    void add(float v) {std::uint32_t bits;std::memcpy(&bits,&v,sizeof(bits));word(bits);}
    void add(double v) {std::uint64_t bits;std::memcpy(&bits,&v,sizeof(bits));word(bits);}
    template<class T,std::size_t N> void add(const T (&v)[N]) {
        node(&v);add(N);for(const auto& x:v)add(x);
    }
    template<std::size_t N> void add(const char (&v)[N]) {
        node(&v);
        std::size_t length=0;while(length<N&&v[length])++length;
        add(length);for(std::size_t i=0;i<length;++i)add(static_cast<unsigned char>(v[i]));
    }
    template<class T,std::size_t N> void add(const std::array<T,N>& v) {
        node(&v);add(N);for(const auto& x:v)add(x);
    }
    template<class T> void add(const std::vector<T>& v) {
        node(&v);add(v.size());for(const auto& x:v)add(x);
    }
    void add(const std::string& v) {node(&v);add(v.size());for(unsigned char x:v)add(x);}
    // Immutable assets are inputs, not simulation state. Mutable terrain is a
    // separate root. Back-references/callbacks contribute binding presence only.
    template<class T,std::enable_if_t<std::is_pointer_v<T>,int> =0>
    void add(const T& v) {
        if constexpr(std::is_same_v<std::remove_cv_t<std::remove_pointer_t<T>>,char>){
            add(v!=nullptr);if(v){add(std::strlen(v));for(auto p=v;*p;++p)add(static_cast<unsigned char>(*p));}
        }else binding(v);
    }
    template<class T> void add(const std::shared_ptr<T>& v) {binding(v.get());if(v)add(*v);}
    void add(const Clip* v) {binding(v);if(v)add(*v);}
    void add(const RoomGate* v) {binding(v);if(v)add(*v);}
    void add(const RoomBreakSoundChoice* v) {binding(v);if(v)add(*v);}
    void add(const AnimationBank& v) {node(&v);}
    void add(const RoomVisualBank& v) {node(&v);}
    void add(const LoreTabletBank& v) {node(&v);}
    void add(const RoomBreakableBank& v) {node(&v);}
    void add(const ContactEffectBank& v) {node(&v);}
#include "simulation_state_fields.inc"
};
// The injected RNG's storage belongs to the adapter, outside TutorialSession.
inline SimulationStateHash::Result simulationStateHash(const TutorialSession& session,
                                              const TerrainBank& terrain,std::uint32_t seed) {
    SimulationStateHash hash;
    for(hash.pass=0;hash.pass<3;++hash.pass){
        hash.value=14695981039346656037ull;hash.leaves=0;hash.nodes=0;
        hash.add(session);hash.add(terrain);hash.add(seed);
    }
    return {hash.value,hash.leaves};
}
}
#endif
