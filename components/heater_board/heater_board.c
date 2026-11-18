#include "heater_board.h"

#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "esp_attr.h"
#include "esp_check.h"

#define PWM_RESOLUTION_HZ 2000000U
#define PWM_PERIOD_TICKS (PWM_RESOLUTION_HZ / HEATER_PWM_FREQUENCY_HZ)
_Static_assert(PWM_PERIOD_TICKS == 200, "PWM ticks must represent 0.5 percent");

const int heater_pwm_gpio[HEATER_CHANNEL_COUNT] = {4, 5, 6, 7};
const int heater_adc_cs_gpio[HEATER_CHANNEL_COUNT] = {8, 9, 10, 14};
const int heater_adc_drdy_gpio[HEATER_CHANNEL_COUNT] = {17, 18, 21, 38};

static const char *TAG = "heater_board";
static mcpwm_cmpr_handle_t s_comparator[HEATER_CHANNEL_COUNT];
static mcpwm_gen_handle_t s_generator[HEATER_CHANNEL_COUNT];
static mcpwm_sync_handle_t s_sync;

// 直接拉低两个请求 GPIO，供启动失败、停热和中断异常路径快速关断。
void IRAM_ATTR heater_board_requests_off(void)
{
    gpio_set_level(HEATER_HEAT_REQ_GPIO, 0);
    gpio_set_level(HEATER_MEAS_REQ_GPIO, 0);
}

// 直接设置 HEAT_REQ 电平，由阶段状态机安排互锁及预充电时序。
void IRAM_ATTR heater_board_heat_request(bool enabled)
{
    gpio_set_level(HEATER_HEAT_REQ_GPIO, enabled);
}

// 直接设置 MEAS_REQ 电平，由阶段中断保证测量截止时刻。
void IRAM_ATTR heater_board_measure_request(bool enabled)
{
    gpio_set_level(HEATER_MEAS_REQ_GPIO, enabled);
}

// 逐路设置持续强制低电平，即使一路失败也继续处理其余输出并汇总结果。
bool IRAM_ATTR heater_board_pwm_low(void)
{
    bool success = true;
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        if (mcpwm_generator_set_force_level(s_generator[i], 0, true) != ESP_OK) {
            success = false;
        }
    }
    return success;
}

// 装载四路比较值后触发软件同步，仅解除非零占空比通道的强制低电平。
// 任一步失败立即返回 false，由控制层执行请求关断与故障锁定。
bool IRAM_ATTR heater_board_pwm_apply(const uint8_t duty[HEATER_CHANNEL_COUNT])
{
    uint8_t ticks[4];
    for (size_t i=0;i<4;++i) {
        if (duty[i] > CONFIG_HEATER_OUTPUT_DUTY_LIMIT) {
            heater_board_requests_off(); heater_board_pwm_low(); return false;
        }
        ticks[i]=duty[i]*2U;
    }
    return heater_board_pwm_apply_ticks(ticks);
}

bool IRAM_ATTR heater_board_pwm_apply_ticks(const uint8_t ticks[4])
{
    for (size_t i=0;i<4;++i) {
        if (ticks[i]>CONFIG_HEATER_OUTPUT_DUTY_LIMIT*2U) {
            heater_board_requests_off(); heater_board_pwm_low(); return false;
        }
    }
    for (size_t i=0;i<4;++i) {
        if (mcpwm_comparator_set_compare_value(s_comparator[i], ticks[i] ? ticks[i] : 1U)!=ESP_OK)
            return false;
    }
    if (mcpwm_soft_sync_activate(s_sync)!=ESP_OK) return false;
    for (size_t i=0;i<4;++i) {
        if (ticks[i] && mcpwm_generator_set_force_level(s_generator[i],-1,true)!=ESP_OK) return false;
    }
    return true;
}

// 先建立安全低电平，再配置共用计时器、软件同步及两组四路 PWM 输出。
// 比较值在计数归零或同步时更新，初始化结束后载波运行但输出仍强制为低。
esp_err_t heater_board_init(void)
{
    const uint64_t requests = (1ULL << HEATER_HEAT_REQ_GPIO) | (1ULL << HEATER_MEAS_REQ_GPIO);
    heater_board_requests_off();
    const gpio_config_t outputs = {
        .pin_bit_mask = requests | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) | (1ULL << 7),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "GPIO initialization failed");
    heater_board_requests_off();
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        gpio_set_level(heater_pwm_gpio[i], 0);
    }
    mcpwm_timer_handle_t timer;
    const mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = PWM_RESOLUTION_HZ,
        .period_ticks = PWM_PERIOD_TICKS,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_config, &timer), TAG, "PWM timer failed");
    const mcpwm_soft_sync_config_t sync_config = {};
    ESP_RETURN_ON_ERROR(mcpwm_new_soft_sync_src(&sync_config, &s_sync), TAG, "PWM sync failed");
    const mcpwm_timer_sync_phase_config_t phase = {
        .sync_src = s_sync,
        .count_value = 0,
        .direction = MCPWM_TIMER_DIRECTION_UP,
    };
    ESP_RETURN_ON_ERROR(mcpwm_timer_set_phase_on_sync(timer, &phase), TAG, "PWM phase failed");
    for (size_t pair = 0; pair < HEATER_CHANNEL_COUNT / 2; ++pair) {
        mcpwm_oper_handle_t oper;
        const mcpwm_operator_config_t oper_config = {.group_id = 0};
        ESP_RETURN_ON_ERROR(mcpwm_new_operator(&oper_config, &oper), TAG, "PWM operator failed");
        ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(oper, timer), TAG, "PWM connection failed");
        for (size_t side = 0; side < 2; ++side) {
            const size_t i = pair * 2 + side;
            const mcpwm_comparator_config_t compare_config = {
                .flags.update_cmp_on_tez = true,
                .flags.update_cmp_on_sync = true,
            };
            ESP_RETURN_ON_ERROR(mcpwm_new_comparator(oper, &compare_config, &s_comparator[i]),
                                TAG, "PWM comparator failed");
            const mcpwm_generator_config_t gen_config = {.gen_gpio_num = heater_pwm_gpio[i]};
            ESP_RETURN_ON_ERROR(mcpwm_new_generator(oper, &gen_config, &s_generator[i]),
                                TAG, "PWM generator failed");
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(s_generator[i], 0, true),
                                TAG, "PWM hold failed");
            ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(s_comparator[i], 1),
                                TAG, "PWM compare failed");
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(s_generator[i],
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY,
                                             MCPWM_GEN_ACTION_HIGH)), TAG, "PWM rise failed");
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(s_generator[i],
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, s_comparator[i],
                                               MCPWM_GEN_ACTION_LOW)), TAG, "PWM fall failed");
        }
    }
    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(timer), TAG, "PWM enable failed");
    return mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);
}
