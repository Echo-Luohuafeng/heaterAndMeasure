#pragma once
#include "heater_controller.h"
namespace heater {
// 新算法实现此父类；对象由注册表提供，生命周期覆盖运行，step 禁止阻塞/分配内存。
class Controller {
public:
    virtual ~Controller() = default;
    virtual bool configure(const controller_config_t&) noexcept = 0;
    virtual void reset() noexcept = 0;
    virtual controller_output_t step(const controller_input_t&) noexcept = 0;
    virtual void on_applied(const controller_applied_t&) noexcept {}
};
using ControllerFactory = Controller& (*)() noexcept;
struct Registration {
    controller_descriptor_t descriptor;
    ControllerFactory instance;
    bool (*validate)(const controller_config_t&) noexcept;
};
// 在注册表增加 descriptor、静态对象工厂和参数校验即可接入新算法。
const Registration* registrations(unsigned& count) noexcept;
}
