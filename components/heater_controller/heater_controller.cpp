#include "controller.hpp"
#include <cmath>
#include <algorithm>
namespace { heater::Controller* active; float max_duty; }
extern "C" {
const controller_descriptor_t* heater_controller_registry(unsigned* count) {
    struct Descriptors {
        controller_descriptor_t items[16]{};
        unsigned count{};
        Descriptors() {
            const auto* table=heater::registrations(count);
            if(count>16) count=16;
            for(unsigned i=0;i<count;++i) items[i]=table[i].descriptor;
        }
    };
    static const Descriptors descriptors;
    if(count) *count=descriptors.count;
    return descriptors.items;
}
bool heater_controller_validate(const controller_config_t* c) {
    if(!c || !std::isfinite(c->max_duty) || c->max_duty<0 || c->max_duty>95) return false;
    for(float t:c->target) if(!std::isfinite(t)) return false;
    unsigned n;const auto* table=heater::registrations(n);
    for(unsigned i=0;i<n;++i) if(table[i].descriptor.id==c->algorithm) {
        const auto& descriptor=table[i].descriptor;
        if(descriptor.parameter_count>16) return false;
        for(unsigned j=0;j<descriptor.parameter_count;++j) {
            const auto& p=descriptor.parameters[j];
            if(!p.count || p.offset+p.count>32) return false;
            for(unsigned k=0;k<p.count;++k) {
                const float v=c->parameters[p.offset+k];
                if(!std::isfinite(v) || v<p.minimum || v>p.maximum) return false;
            }
        }
        return table[i].validate(*c);
    }
    return false;
}
bool heater_controller_configure(const controller_config_t* c) {
    if(!heater_controller_validate(c)) return false;
    unsigned n;const auto* table=heater::registrations(n);
    for(unsigned i=0;i<n;++i) if(table[i].descriptor.id==c->algorithm) {
        auto& instance=table[i].instance();
        if(!instance.configure(*c)) return false;
        active=&instance; max_duty=c->max_duty; return true;
    }
    return false;
}
void heater_controller_reset(void) { if(active) active->reset(); }
void heater_controller_on_applied(const controller_applied_t* a) { if(active && a) active->on_applied(*a); }
controller_output_t heater_controller_step(const controller_input_t* in) {
    if(!active || !in) return {};
    auto out=active->step(*in);
    if(out.source_sequence!=in->sequence) out.valid=false;
    for(unsigned i=0;i<4;++i)
        if(!(in->valid_mask&(1U<<i)) || !std::isfinite(in->resistance[i])) out.requested[i]=0;
    for(float v:out.requested) if(!std::isfinite(v)) out.valid=false;
    for(auto& t:out.ticks) t=0;
    out.limited_mask=0;
    if(!out.valid) return out;
    // 通用执行变换：每个子类只需生成候选百分比，限幅和量化完全一致。
    const unsigned limit_ticks=static_cast<unsigned>(std::floor(max_duty*2));
    for(unsigned i=0;i<4;++i) {
        const float limited=std::clamp(out.requested[i],0.0f,max_duty);
        if(limited!=out.requested[i]) out.limited_mask|=1U<<i;
        out.ticks[i]=static_cast<uint8_t>(std::min(static_cast<unsigned>(std::lround(limited*2)),limit_ticks));
    }
    return out;
}
}
