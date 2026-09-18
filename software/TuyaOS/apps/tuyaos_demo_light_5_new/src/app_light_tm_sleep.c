/**
 * @file app_light_tm_sleep.c
 * @brief Sleep light DP parsing & timer logic
 * @version 0.6
 * @date 2025-10-15
 */

#include "tal_sw_timer.h"
#include "tal_memory.h"
#include "tal_log.h"
#include "dp_process.h"
#include "app_light_tm_sleep.h"
#include "app_light_tm_wake.h"
#include "tuya_ws_db.h"
#include "app_light_tm_schedule.h"
#include <string.h>
#include "app_pwm.h"
#include "tal_time_service.h"
#include "pwm_gradual.h"
#include "light_pwm_mix.h"

#define LIG_TM_SLEEP_FLASH_KEY "sleep_cfg"
#define SLEEP_BRIGHT_MAX 1000

// 睡眠时间映射表（单位秒）
static const UINT_T sg_sleep_time_map[6] = {600, 1200, 1800, 2400, 3000, 3600};

static UCHAR_T gradual_time = 0; ///< sg_sleep_time_map 索引 (0~5)
BOOL_T sg_sleep_is_timing = FALSE;
TIMER_ID sg_sleep_timer_id = 0;
static UCHAR_T sg_sleep_timer_gradual = 0;

static VOID __sleep_gradual_cb(PVOID_T pTimerArg);
static VOID __sleep_start_timer_cb(PVOID_T pTimerArg);

static VOID __sleep_timer_dispatch(PVOID_T pTimerArg)
{
    if (sg_sleep_timer_gradual) {
        __sleep_gradual_cb(pTimerArg);
    } else {
        __sleep_start_timer_cb(pTimerArg);
    }
}

static VOID __sleep_timer_restart(UCHAR_T gradual, UINT_T interval_ms)
{
    sg_sleep_timer_gradual = gradual;
    if (sg_sleep_timer_id == 0) {
        tal_sw_timer_create(__sleep_timer_dispatch, NULL, &sg_sleep_timer_id);
    } else {
        tal_sw_timer_stop(sg_sleep_timer_id);
    }
    tal_sw_timer_start(sg_sleep_timer_id, interval_ms, TAL_TIMER_CYCLE);
}

static VOID __sleep_timer_stop_only(VOID_T)
{
    if (sg_sleep_timer_id) {
        tal_sw_timer_stop(sg_sleep_timer_id);
    }
}

uint8_t main_bright = 0;
uint8_t aux_bright = 0;
uint8_t night_bright = 0;
uint8_t current_temp = 0;        // 当前色温值
uint8_t target_temp = 0;         // 目标色温值（通常为0）
int sleep_state = 0;

static SLEEP_INFO_T sg_sleep_run_info = {0}; ///< 当前睡眠DP配置

// 全局变量记录每个参数的上次渐变时间
static UINT_T sg_timer_start_time = 0;

// 每个参数独立的渐变间隔（毫秒）
static UINT_T sg_main_interval_ms = 0;
static UINT_T sg_aux_interval_ms = 0;
static UINT_T sg_night_interval_ms = 0;
static UINT_T sg_temp_interval_ms = 0;

// 定时器间隔
static UINT_T sg_timer_interval_ms = 1000; // 默认100ms

// ---------------- 内部函数 ----------------
/**
 * @brief 计算两个数的最大公约数
 */
static UINT_T __gcd(UINT_T a, UINT_T b)
{
    while (b != 0) {
        UINT_T temp = b;
        b = a % b;
        a = temp;
    }
    return a;
}

/**
 * @brief 计算1%变化所需的时间间隔
 * @param total_seconds 总渐变时间(秒)
 * @param start_value 起始值
 * @return 渐变间隔时间(毫秒)
 */
static UINT_T __calc_param_interval(UINT_T total_seconds, USHORT_T start_value)
{
    if (start_value == 0) return 0xFFFFFFFF; // 不需要渐变
    
    UINT_T interval_ms = (total_seconds * 1000) / start_value;
    
    // 限制最小间隔为100ms
    if (interval_ms < 100) interval_ms = 100;
    
    return interval_ms;
}

/**
 * @brief 按照1%的步长计算下一个值
 * @param current_val 当前值
 * @param target_val 目标值
 * @return 下一个渐变值
 */
static USHORT_T __calc_step_value_1percent(USHORT_T current_val, USHORT_T target_val)
{
    if (current_val == target_val)
        return target_val;
    
    // 按照1%的步长渐变
    if (current_val > target_val) {
        return (current_val - 1 < target_val) ? target_val : (current_val - 1);
    } else {
        return (current_val + 1 > target_val) ? target_val : (current_val + 1);
    }
}

/**
 * @brief 计算多个间隔的最大公约数作为定时器间隔
 */
static UINT_T __calculate_gcd_of_intervals(VOID)
{
    UINT_T gcd_value = 0;
    
    // 收集所有需要渐变的参数的间隔
    UINT_T intervals[4] = {0};
    UINT_T count = 0;
    
    if (sg_main_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_main_interval_ms;
    }
    if (sg_aux_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_aux_interval_ms;
    }
    if (sg_night_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_night_interval_ms;
    }
    if (sg_temp_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_temp_interval_ms;
    }
    
    if (count == 0) {
        return 1000; // 默认1秒
    }
    
    // 计算所有间隔的最大公约数
    gcd_value = intervals[0];
    for (UINT_T i = 1; i < count; i++) {
        gcd_value = __gcd(gcd_value, intervals[i]);
    }
    
    return gcd_value;
}

UINT32_T sg_last_main_gradual_time = 0;
UINT32_T sg_last_aux_gradual_time = 0;
UINT32_T sg_last_night_gradual_time = 0;
UINT32_T sg_last_temp_gradual_time = 0;
/**
 * @brief 检查是否到达渐变时间点
 * @param elapsed_time 经过的时间(毫秒)
 * @param interval 渐变间隔(毫秒)
 * @param timer_interval 定时器间隔(毫秒)
 * @return TRUE: 需要渐变, FALSE: 不需要渐变
 */
BOOL_T __is_time_to_gradual_v2(UINT_T elapsed_time, UINT_T interval, UINT_T timer_interval, UINT_T *last_gradual_time)
{
    if (interval == 0xFFFFFFFF) return FALSE; // 不需要渐变
    
    // 检查是否到达间隔时间的整数倍附近（与 light_3 一致）
    UINT_T remainder = elapsed_time % interval;
    BOOL_T should_gradual = (remainder < timer_interval) || (remainder > interval - timer_interval);
    
    if (should_gradual) {
        // 检查是否已经在这个时间点渐变过了
        UINT_T expected_gradual_time = (elapsed_time / interval) * interval;
        if (*last_gradual_time == expected_gradual_time) {
            return FALSE; // 已经在这个时间点渐变过了
        }
        *last_gradual_time = expected_gradual_time;
        return TRUE;
    }
    
    return FALSE;
}

/**
 * @brief 初始化所有参数的渐变间隔，并计算最小公约数作为定时器间隔
 */
static VOID __init_gradual_intervals(UINT_T total_seconds)
{
    // 计算每个参数的渐变间隔
    sg_main_interval_ms = __calc_param_interval(total_seconds, main_bright);
    sg_aux_interval_ms = __calc_param_interval(total_seconds, aux_bright);
    sg_night_interval_ms = __calc_param_interval(total_seconds, night_bright);
    sg_temp_interval_ms = __calc_param_interval(total_seconds, current_temp);
    
    // 计算所有间隔的最大公约数作为定时器间隔
    sg_timer_interval_ms = __calculate_gcd_of_intervals();
    
    // 限制定时器间隔在100ms到1000ms之间
    if (sg_timer_interval_ms < 1000) sg_timer_interval_ms = 1000;
    // if (sg_timer_interval_ms > 1000) sg_timer_interval_ms = 1000;
}

// 统一的定时器回调，检查每个参数是否到了渐变时间（逻辑对齐 light_3，保留辅灯）
UINT32_T elapsed_time = 0;
static VOID __sleep_gradual_cb(PVOID_T pTimerArg)
{
    extern DEMO_INFO_T sg_demo_info;

    if (!sg_sleep_run_info.enable)
        return;
    if (!sg_sleep_is_timing && !sleep_state)
        return;
    if(sg_demo_info.rhythm_switch == 1){
        app_light_stop_today_rhythm_timer();
    }
    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;

    BOOL_T need_update = FALSE;
    BOOL_T all_finished = TRUE;

    elapsed_time += sg_timer_interval_ms;
    // 检查主灯亮度是否需要渐变（终点亮度1，不是关灯）
    if (sg_sleep_run_info.main_on && sg_demo_info.white_switch && main_bright > 1)
    {
        if (elapsed_time > 0 &&  __is_time_to_gradual_v2(elapsed_time, sg_main_interval_ms, sg_timer_interval_ms, &sg_last_main_gradual_time))
        {
            uint8_t new_bright = __calc_step_value_1percent(main_bright, 1);
            if (new_bright != main_bright) {
                main_bright = new_bright;
                sg_demo_info.white_bright = main_bright;
                need_update = TRUE;

                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * (100 - 5) / (100 - 1);
                uint8_t white_temp1 = current_temp ? current_temp : 1;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);

                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;

                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;

                upload_device_value_status(DPID_WHITE_BRIGHT, main_bright);
            }
        }
        if (main_bright > 1) all_finished = FALSE;
    }

    // 检查辅灯亮度是否需要渐变
    if (sg_sleep_run_info.aux_on && sg_demo_info.aux_switch && aux_bright > 1)
    {
        if (elapsed_time > 0 &&  __is_time_to_gradual_v2(elapsed_time, sg_aux_interval_ms, sg_timer_interval_ms, &sg_last_aux_gradual_time))
        {
            uint8_t new_aux_bright = __calc_step_value_1percent(aux_bright, 1);
            if (new_aux_bright != aux_bright) {
                aux_bright = new_aux_bright;
                sg_demo_info.aux_bright = aux_bright;
                need_update = TRUE;

                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * (100 - 5) / (100 - 1);
                uint8_t white_temp1 = current_temp ? current_temp : 1;
                uint16_t aux_ww = aux_bright1 * white_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);

                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;

                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;

                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, aux_bright);
            }
        }
        if (aux_bright > 1) all_finished = FALSE;
    }

    // 检查夜灯亮度是否需要渐变
    if (sg_sleep_run_info.night_on && sg_demo_info.night_switch && night_bright > 1)
    {
        if (elapsed_time > 0 && __is_time_to_gradual_v2(elapsed_time, sg_night_interval_ms, sg_timer_interval_ms, &sg_last_night_gradual_time))
        {
            uint8_t new_night_bright = __calc_step_value_1percent(night_bright, 1);
            if (new_night_bright != night_bright) {
                night_bright = new_night_bright;
                sg_demo_info.night_bright = night_bright;
                need_update = TRUE;

                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = night_bright * 100;
                upload_device_value_status(NIGHT_LIGHT_VALUE, night_bright);
            }
        }
        if (night_bright > 1) all_finished = FALSE;
    }

    // 检查色温是否需要渐变（与 light_3 一致：只改状态/上报，亮度步进里已带色温 PWM）
    if ((sg_sleep_run_info.main_on || sg_sleep_run_info.aux_on) && current_temp > 0)
    {
        if (elapsed_time > 0 && __is_time_to_gradual_v2(elapsed_time, sg_temp_interval_ms, sg_timer_interval_ms, &sg_last_temp_gradual_time))
        {
            uint8_t new_temp = __calc_step_value_1percent(current_temp, target_temp);
            if (new_temp != current_temp) {
                current_temp = new_temp;
                need_update = TRUE;

                if ((sg_sleep_run_info.main_on && sg_demo_info.white_switch) ||
                    (sg_sleep_run_info.aux_on && sg_demo_info.aux_switch)) {
                    sg_demo_info.white_temp = current_temp;
                }

                upload_device_value_status(DPID_TEMP_VALUE, current_temp);
            }
        }
        if (current_temp > 0) all_finished = FALSE;
    }

    // 上报色温变化（只在有变化时再报一次，对齐 light_3）
    if (need_update && (sg_sleep_run_info.main_on || sg_sleep_run_info.aux_on)) {
        upload_device_value_status(DPID_TEMP_VALUE, current_temp);
    }
    if (channel_count > 0)
    {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }

    // 伴眠结束：亮度保持 1（不写 0、不上报亮度/色温），关实灯并上报开关关
    if (all_finished)
    {
        elapsed_time = 0;

        if (sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1) {
            sg_demo_info.last_light_memory = 1;
        } else if (sg_demo_info.white_switch == 1) {
            sg_demo_info.last_light_memory = 2;
        } else if (sg_demo_info.aux_switch == 1) {
            sg_demo_info.last_light_memory = 3;
        } else if (sg_demo_info.night_switch == 1) {
            sg_demo_info.last_light_memory = 4;
        }

        if (sg_sleep_run_info.main_on) {
            main_bright = 1;
            current_temp = 0;
            sg_demo_info.white_bright = 1;
            sg_demo_info.white_temp = 0;
        }
        if (sg_sleep_run_info.aux_on) {
            aux_bright = 1;
            current_temp = 0;
            sg_demo_info.aux_bright = 1;
            sg_demo_info.white_temp = 0;
        }
        if (sg_sleep_run_info.night_on) {
            night_bright = 1;
            sg_demo_info.night_bright = 1;
        }

        sg_demo_info.white_switch = 0;
        sg_demo_info.aux_switch = 0;
        sg_demo_info.night_switch = 0;
        sg_demo_info.switch_status = 0;

        {
            TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
            UINT32_T duties[] = {0, 0, 0, 0, 0};
            pwm_gradual_duty_set_multi(5, channels, duties);
        }

        sg_sleep_is_timing = FALSE;
        sleep_state = 0;
        __sleep_timer_stop_only();

        upload_device_bool_status(DPID_SWITCH, 0);
        upload_device_bool_status(LIGHT_SWITCH, 0);
        upload_device_bool_status(AUX_SWITCH, 0);
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);

        if (sg_sleep_run_info.week == WEEK_ONCE)
        {
            sg_sleep_run_info.enable = 0;
            sg_demo_info.sleep_init[0] = 0x0;
            app_light_schedule_on_sleep_disable();
            dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                                   sg_demo_info.sleep_init, 15, 5);
        }
        device_config_save();
        return;
    }
}

static VOID __sleep_apply_custom_start_pwm(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;

    main_bright = sg_sleep_run_info.main_percent;
    aux_bright = sg_sleep_run_info.aux_percent;
    night_bright = sg_sleep_run_info.night_percent;
    current_temp = sg_sleep_run_info.temper;
    target_temp = 0;

    if (sg_sleep_run_info.main_on == 0 && sg_sleep_run_info.aux_on == 0) {
        sg_demo_info.last_light_memory = 1;
    } else if (sg_sleep_run_info.main_on == 0) {
        sg_demo_info.last_light_memory = 2;
    } else if (sg_sleep_run_info.aux_on == 0) {
        sg_demo_info.last_light_memory = 3;
    }

    if (sg_sleep_run_info.main_on) {
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_switch = 1;
        sg_demo_info.white_bright = main_bright;
        sg_demo_info.white_temp = current_temp;
        if (sg_demo_info.night_switch == 1) {
            sg_demo_info.night_switch = 0;
            sg_demo_info.last_light_memory = 4;
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        }
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(LIGHT_SWITCH, 1);

        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
        uint8_t white_temp1 = sg_demo_info.white_temp;
        if (white_temp1 == 0) white_temp1 = 1;

        uint16_t ww = white_bright1 * white_temp1;
        uint16_t cw = white_bright1 * 100 - ww;

        light_pwm_clamp_mix(&ww, &cw);
all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = ww;

        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = cw;
        upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
        pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
    } else {
        sg_demo_info.white_switch = 0;
        upload_device_bool_status(LIGHT_SWITCH, 0);
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = 0;

        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = 0;
    }

    if (sg_sleep_run_info.aux_on) {
        sg_demo_info.switch_status = 1;
        sg_demo_info.aux_switch = 1;
        if (sg_demo_info.night_switch == 1) {
            sg_demo_info.night_switch = 0;
            sg_demo_info.last_light_memory = 4;
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        }
        sg_demo_info.aux_bright = aux_bright;
        sg_demo_info.white_temp = current_temp;
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);

        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
        uint8_t aux_temp1 = sg_demo_info.white_temp;
        if (aux_temp1 == 0) aux_temp1 = 1;

        uint16_t aux_ww = aux_bright1 * aux_temp1;
        uint16_t aux_cw = aux_bright1 * 100 - aux_ww;

        light_pwm_clamp_mix(&aux_ww, &aux_cw);
all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = aux_ww;

        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = aux_cw;
        pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
    } else {
        sg_demo_info.aux_switch = 0;
        upload_device_bool_status(AUX_SWITCH, 0);
        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = 0;

        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = 0;
    }

    if (sg_sleep_run_info.night_on) {
        sg_demo_info.night_switch = 1;
        if (sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1) {
            sg_demo_info.last_light_memory = 1;
        } else if (sg_demo_info.white_switch == 1) {
            sg_demo_info.last_light_memory = 2;
        } else if (sg_demo_info.aux_switch == 1) {
            sg_demo_info.last_light_memory = 3;
        }
        sg_demo_info.switch_status = 1;
        sg_demo_info.aux_switch = 0;
        sg_demo_info.white_switch = 0;
        sg_demo_info.night_bright = night_bright;
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 0);
        upload_device_bool_status(LIGHT_SWITCH, 0);
        upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);

        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
        all_duties[channel_count++] = sg_demo_info.night_bright * 100;
    } else {
        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
        all_duties[channel_count++] = 0;
    }

    if (channel_count > 0) {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }
}

static VOID __sleep_prepare_start_brightness(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;

    if (sg_sleep_run_info.start_state == 0) {
        main_bright = sg_demo_info.white_bright;
        aux_bright = sg_demo_info.aux_bright;
        night_bright = sg_demo_info.night_bright;
        current_temp = (sg_demo_info.white_switch == 1) ? sg_demo_info.white_temp :
                       (sg_demo_info.aux_switch == 1) ? sg_demo_info.white_temp : 1;
        target_temp = 0;

        sg_sleep_run_info.main_on = sg_demo_info.white_switch;
        sg_sleep_run_info.aux_on = sg_demo_info.aux_switch;
        sg_sleep_run_info.night_on = sg_demo_info.night_switch;

        // TAL_PR_NOTICE("伴眠从当前亮度开始: 主%u 辅%u 夜%u 色温%u",
        //               main_bright, aux_bright, night_bright, current_temp);
    } else {
        // TAL_PR_NOTICE("伴眠从自定义亮度开始: 主%u%% 辅%u%% 夜%u%% 色温%u",
        //               sg_sleep_run_info.main_percent, sg_sleep_run_info.aux_percent,
        //               sg_sleep_run_info.night_percent, sg_sleep_run_info.temper);
        __sleep_apply_custom_start_pwm();
    }
}

static VOID __sleep_begin_gradual(VOID_T)
{
    app_light_schedule_before_sleep_start();
    upload_device_enum_status(DPID_WORK_MODE, 16);

    gradual_time = (sg_sleep_run_info.duration >= 1 && sg_sleep_run_info.duration <= 6) ?
                   (sg_sleep_run_info.duration - 1) : 2;
    sleep_state = 1;
    elapsed_time = 0;
    sg_last_main_gradual_time = 0;
    sg_last_aux_gradual_time = 0;
    sg_last_night_gradual_time = 0;
    sg_last_temp_gradual_time = 0;

    UINT_T total_seconds = (gradual_time < 6) ? sg_sleep_time_map[gradual_time] : sg_sleep_time_map[2];
    __init_gradual_intervals(total_seconds);

    __sleep_timer_restart(1, sg_timer_interval_ms);
    sg_sleep_is_timing = TRUE;
}

// 定时启动回调，每分钟检查一次
static VOID __sleep_start_timer_cb(PVOID_T pTimerArg)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;
    extern bool sg_rhythm_interrupted;
    if (!sg_sleep_run_info.enable)
        return;

    extern POSIX_TM_S local_tm;
    extern bool time_synced;
    if (!time_synced) return;

    if (app_light_schedule_is_sleep_preempted_today()) {
        return;
    }

    // 检查当前星期是否符合设置
    BOOL_T week_match = FALSE;
    UCHAR_T current_week_bit = 0;

    switch (local_tm.tm_wday)
    {
    case 0: current_week_bit = WEEK_SUNDAY; break;
    case 1: current_week_bit = WEEK_MONDAY; break;
    case 2: current_week_bit = WEEK_TUESDAY; break;
    case 3: current_week_bit = WEEK_WEDNESDAY; break;
    case 4: current_week_bit = WEEK_THURSDAY; break;
    case 5: current_week_bit = WEEK_FRIDAY; break;
    case 6: current_week_bit = WEEK_SATURDAY; break;
    }

    // 检查星期设置
    if (sg_sleep_run_info.week == WEEK_ONCE)
        week_match = TRUE;// 执行一次模式，只要时间匹配就执行
    else if (sg_sleep_run_info.week == WEEK_EVERYDAY)
        week_match = TRUE;// 每天执行
    else
        week_match = (sg_sleep_run_info.week & current_week_bit) != 0;// 检查特定星期

    // 精确到设定分钟，15s 轮询内首次命中触发
    if (!app_light_schedule_preset_minute_due(sg_sleep_run_info.hour,
                                            sg_sleep_run_info.minute) ||
        !week_match)
    {
        return;
    }

    if (sg_sleep_is_timing || sleep_state) {
        return;
    }

    if (app_light_schedule_should_skip_sleep_at_trigger()) {
        return;
    }

    if (app_light_wake_is_timing()) {
        // TAL_PR_NOTICE("伴眠到点，完全停止运行中的唤醒");
        app_light_preempt_wake_today();
    }

    app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_SLEEP, TRUE);
    __sleep_prepare_start_brightness();
    __sleep_begin_gradual();
}

// ---------------- API 函数 ----------------
OPERATE_RET app_light_tm_sleep_init(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    memset(&sg_sleep_run_info, 0, 14);
    memcpy(&sg_sleep_run_info, sg_demo_info.sleep_init, 14);

    if (app_light_tm_sleep_dp_to_info(sg_demo_info.sleep_init, 14) == OPRT_OK)
    {
        if (sg_sleep_run_info.start_mode == 0)
        {
            sg_sleep_run_info.enable = 0;
            sg_demo_info.sleep_init[0] = 0x0;
            app_light_stop_sleep_timer();
            dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                                    sg_demo_info.sleep_init, 15, 5);
        }
        else
        {
            __sleep_timer_restart(0, APP_LIGHT_SCHEDULE_POLL_MS);
            sg_sleep_is_timing = false;
        }
    }

    return OPRT_OK;
}

OPERATE_RET app_light_tm_sleep_dp_to_info(UCHAR_T *p_dp, UINT_T dp_len)
{
    if (!p_dp || dp_len < 13)
        return OPRT_INVALID_PARM;

    sg_sleep_run_info.enable = p_dp[0];
    sg_sleep_run_info.duration = p_dp[1];
    sg_sleep_run_info.start_state = p_dp[2];
    sg_sleep_run_info.main_on = p_dp[3];
    sg_sleep_run_info.main_percent = p_dp[4];
    sg_sleep_run_info.aux_on = p_dp[5];
    sg_sleep_run_info.aux_percent = p_dp[6];
    sg_sleep_run_info.temper = p_dp[7];
    sg_sleep_run_info.night_on = p_dp[8];
    sg_sleep_run_info.night_percent = p_dp[9];
    sg_sleep_run_info.start_mode = p_dp[10];
    sg_sleep_run_info.hour = p_dp[11];
    sg_sleep_run_info.minute = p_dp[12];
    sg_sleep_run_info.week = p_dp[13];

    return OPRT_OK;
}

OPERATE_RET app_light_tm_sleep_info_to_dp(UCHAR_T **p_dp, UINT_T *p_dp_len)
{
    if (!p_dp || !p_dp_len)
        return OPRT_INVALID_PARM;

    UCHAR_T *buf = (UCHAR_T *)tal_malloc(14);
    if (!buf)
        return OPRT_MALLOC_FAILED;
    memset(buf, 0, 14);

    buf[0] = sg_sleep_run_info.enable;
    buf[1] = sg_sleep_run_info.duration;
    buf[2] = sg_sleep_run_info.start_state;
    buf[3] = sg_sleep_run_info.main_on;
    buf[4] = sg_sleep_run_info.main_percent;
    buf[5] = sg_sleep_run_info.aux_on;
    buf[6] = sg_sleep_run_info.aux_percent;
    buf[7] = sg_sleep_run_info.temper;
    buf[8] = sg_sleep_run_info.night_on;
    buf[9] = sg_sleep_run_info.night_percent;
    buf[10] = sg_sleep_run_info.start_mode;
    buf[11] = sg_sleep_run_info.hour;
    buf[12] = sg_sleep_run_info.minute;
    buf[13] = sg_sleep_run_info.week;

    *p_dp = buf;
    *p_dp_len = 14;

    return OPRT_OK;
}

OPERATE_RET app_light_start_sleep_timer()
{
    extern DEMO_INFO_T sg_demo_info;
    extern bool sg_rhythm_interrupted;
    if (!sg_sleep_run_info.enable)
        return OPRT_INVALID_PARM;

    if (app_light_schedule_reject_sleep_on_enable()) {
        sg_sleep_run_info.enable = 0;
        sg_demo_info.sleep_init[0] = 0x0;
        app_light_stop_sleep_timer();
        return OPRT_OK;
    }
    app_light_schedule_revalidate();
    app_light_schedule_clear_sleep_preempt();

    gradual_time = (sg_sleep_run_info.duration >= 1 && sg_sleep_run_info.duration <= 6) ?
                   (sg_sleep_run_info.duration - 1) : 2;

    if (sg_sleep_run_info.start_mode == 0)
    {
        app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_SLEEP, TRUE);
        if (app_light_wake_is_timing()) {
            // TAL_PR_NOTICE("伴眠立即启动，完全停止运行中的唤醒");
            app_light_preempt_wake_today();
        }

        if (!sg_sleep_is_timing)
        {
            __sleep_prepare_start_brightness();
            __sleep_begin_gradual();
        }
    }
    else
    {
        // 定时启动，每分钟检查
        __sleep_timer_restart(0, APP_LIGHT_SCHEDULE_POLL_MS);
        sg_sleep_is_timing = false;
    }

    return OPRT_OK;
}

VOID app_light_preempt_sleep_today(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;

    if (!sg_sleep_is_timing && !sleep_state) {
        return;
    }

    __sleep_timer_stop_only();

    sg_sleep_is_timing = FALSE;
    sleep_state = 0;
    elapsed_time = 0;
    main_bright = 0;
    aux_bright = 0;
    night_bright = 0;
    current_temp = 0;
    target_temp = 0;
    sg_last_main_gradual_time = 0;
    sg_last_aux_gradual_time = 0;
    sg_last_night_gradual_time = 0;
    sg_last_temp_gradual_time = 0;
    app_light_schedule_on_stopped(SCHEDULE_EVT_SLEEP);

    if (sg_sleep_run_info.start_mode == 0 || sg_sleep_run_info.week == WEEK_ONCE) {
        sg_sleep_run_info.enable = 0;
        sg_demo_info.sleep_init[0] = 0x0;
        app_light_schedule_clear_sleep_preempt();
        dev_report_dp_raw_sync(NULL, SLEEP_MODE, sg_demo_info.sleep_init, 15, 5);
        // TAL_PR_NOTICE("伴眠被抢占，当天完全停止");
        return;
    }

    app_light_schedule_mark_sleep_preempted_today();
    // TAL_PR_NOTICE("伴眠被抢占，当天完全停止，下一周期日再执行");

    if (sg_sleep_run_info.enable && sg_sleep_run_info.start_mode == 1) {
        __sleep_timer_restart(0, APP_LIGHT_SCHEDULE_POLL_MS);
    }
}

VOID app_light_stop_sleep_timer(VOID_T)
{
    __sleep_timer_stop_only();
    sg_sleep_run_info.enable = 0;
    elapsed_time = 0;
    sg_sleep_is_timing = FALSE;
    main_bright = 0;
    aux_bright = 0;
    night_bright = 0;
    current_temp = 0;
    target_temp = 0;
    sleep_state = 0;
    app_light_schedule_on_sleep_disable();
}

BOOL_T app_light_tm_sleep_is_timing(VOID_T)
{
    return sg_sleep_is_timing;
}
