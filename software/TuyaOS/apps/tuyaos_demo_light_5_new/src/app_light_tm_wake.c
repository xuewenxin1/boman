/**
 * @file app_light_tm_wake.c
 * @brief Wake light DP & timer logic
 * @version 0.2
 * @date 2025-10-15
 */

#include "app_light_tm_wake.h"
#include "tal_sw_timer.h"
#include "dp_process.h"
#include "tal_memory.h"
#include "tal_log.h"
#include "tuya_ws_db.h"
#include <string.h>
#include "app_light_tm_sleep.h"
#include "app_light_tm_schedule.h"
#include "tal_time_service.h"
#include "app_pwm.h"
#include "pwm_gradual.h"
#include "light_pwm_mix.h"

#define WAKE_GRADUAL_PERIOD_S 60u
#define WAKE_FLASH_KEY "wake_cfg"
static const UINT_T sg_wake_time_map[6] = {600, 1200, 1800, 2400, 3000, 3600};

// 定时器公用伴眠的单实例
TIMER_ID sg_wake_timer_id = 0;
static UCHAR_T sg_wake_timer_gradual = 0;
extern UINT_T remain_cnt;
BOOL_T sg_wake_is_timing = false;
WAKE_INFO_T info = {0};
extern int sleep_state;

static VOID __wake_1percent_gradual_cb(PVOID_T pTimerArg);
static VOID __wake_start_timer_cb(PVOID_T pTimerArg);

static VOID __wake_timer_dispatch(PVOID_T pTimerArg)
{
    if (sg_wake_timer_gradual) {
        __wake_1percent_gradual_cb(pTimerArg);
    } else {
        __wake_start_timer_cb(pTimerArg);
    }
}

static VOID __wake_timer_restart(UCHAR_T gradual, UINT_T interval_ms)
{
    sg_wake_timer_gradual = gradual;
    if (sg_wake_timer_id == 0) {
        tal_sw_timer_create(__wake_timer_dispatch, NULL, &sg_wake_timer_id);
    } else {
        tal_sw_timer_stop(sg_wake_timer_id);
    }
    tal_sw_timer_start(sg_wake_timer_id, interval_ms, TAL_TIMER_CYCLE);
}

static VOID __wake_timer_stop_only(VOID_T)
{
    if (sg_wake_timer_id) {
        tal_sw_timer_stop(sg_wake_timer_id);
    }
}

// 1%渐变相关变量
static uint8_t wake_main_bright = 0;
static uint8_t wake_aux_bright = 0;
static uint8_t wake_temp = 0;

// 独立间隔渐变相关变量（类似睡眠模式）
static UINT_T sg_wake_timer_start_time = 0;
static UINT_T sg_wake_main_interval_ms = 0;
static UINT_T sg_wake_aux_interval_ms = 0;
static UINT_T sg_wake_temp_interval_ms = 0;
static UINT_T sg_wake_timer_interval_ms = 1000;

// 记录每个参数的上次渐变时间
static UINT_T sg_wake_last_main_gradual_time = 0;
static UINT_T sg_wake_last_aux_gradual_time = 0;
static UINT_T sg_wake_last_temp_gradual_time = 0;

/************************ 内部函数 ****************************/
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
 * @param target_value 目标值
 * @return 渐变间隔时间(毫秒)
 */
static UINT_T __calc_wake_param_interval(UINT_T total_seconds, USHORT_T start_value, USHORT_T target_value)
{
    if (start_value == target_value) return 0xFFFFFFFF; // 不需要渐变
    
    // 计算需要多少次1%变化
    UINT_T steps_needed = (start_value < target_value) ? (target_value - start_value) : (start_value - target_value);
    if (steps_needed == 0) return 0xFFFFFFFF;
    
    // 计算每次1%变化的时间间隔
    UINT_T interval_ms = (total_seconds * 1000) / steps_needed;
    
    // 限制最小间隔为100ms
    if (interval_ms < 100) interval_ms = 100;
    
    return interval_ms;
}

/**
 * @brief 按照1%的步长计算下一个值
 */
static USHORT_T __calc_step_value_1percent(USHORT_T current_val, USHORT_T target_val)
{
    if (current_val == target_val)
        return target_val;
    
    // 按照1%的步长渐变
    if (current_val < target_val) {
        return (current_val + 1 > target_val) ? target_val : (current_val + 1);
    } else {
        return (current_val - 1 < target_val) ? target_val : (current_val - 1);
    }
}

/**
 * @brief 计算多个间隔的最大公约数作为定时器间隔
 */
static UINT_T __calculate_wake_gcd_of_intervals(VOID)
{
    UINT_T gcd_value = 0;
    
    // 收集所有需要渐变的参数的间隔
    UINT_T intervals[3] = {0};
    UINT_T count = 0;
    
    if (sg_wake_main_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_wake_main_interval_ms;
    }
    if (sg_wake_aux_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_wake_aux_interval_ms;
    }
    if (sg_wake_temp_interval_ms != 0xFFFFFFFF) {
        intervals[count++] = sg_wake_temp_interval_ms;
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

/**
 * @brief 检查是否到达渐变时间点
 */
static BOOL_T __is_wake_time_to_gradual(UINT_T elapsed_time, UINT_T interval, UINT_T timer_interval, UINT_T *last_gradual_time)
{
    if (interval == 0xFFFFFFFF) return FALSE; // 不需要渐变
    
    // 检查是否到达间隔时间的整数倍附近
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
 * @brief 初始化唤醒渐变间隔
 */
static VOID __init_wake_gradual_intervals(UINT_T total_seconds)
{
    USHORT_T target_main = info.main_percent;
    USHORT_T target_aux = info.aux_percent;
    USHORT_T target_temp = info.temper;
    
    // 计算每个参数的渐变间隔
    sg_wake_main_interval_ms = __calc_wake_param_interval(total_seconds, wake_main_bright, target_main);
    sg_wake_aux_interval_ms = __calc_wake_param_interval(total_seconds, wake_aux_bright, target_aux);
    sg_wake_temp_interval_ms = __calc_wake_param_interval(total_seconds, wake_temp, target_temp);
    
    // 计算所有间隔的最大公约数作为定时器间隔
    sg_wake_timer_interval_ms = __calculate_wake_gcd_of_intervals();
    
    // 限制定时器间隔在100ms到1000ms之间
    if (sg_wake_timer_interval_ms < 1000) sg_wake_timer_interval_ms = 1000;
}

// 1%渐变回调（使用独立间隔）
static VOID __wake_1percent_gradual_cb(PVOID_T pTimerArg)
{
    extern DEMO_INFO_T sg_demo_info;
    extern UINT32_T elapsed_time;
    if (!info.enable || !sg_wake_is_timing)
        return;
    if(sg_demo_info.rhythm_switch == 1){
        app_light_stop_today_rhythm_timer();
    }
    BOOL_T need_update = FALSE;
    BOOL_T all_finished = TRUE;

    USHORT_T target_main = info.main_percent;
    USHORT_T target_aux = info.aux_percent;
    USHORT_T target_temp = info.temper;

    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;

    elapsed_time += sg_wake_timer_interval_ms;
    // 主灯1%渐变处理
    if (info.main_switch && sg_demo_info.white_switch && wake_main_bright != target_main)
    {
        if (elapsed_time > 0 && __is_wake_time_to_gradual(elapsed_time, sg_wake_main_interval_ms, sg_wake_timer_interval_ms, &sg_wake_last_main_gradual_time))
        {
            uint8_t new_bright = __calc_step_value_1percent(wake_main_bright, target_main);
            if (new_bright != wake_main_bright) {
                wake_main_bright = new_bright;
                sg_demo_info.white_bright = wake_main_bright;
                need_update = TRUE;
                
                // 计算PWM值
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = wake_temp; // 使用当前渐变色温
                if (white_temp1 == 0) white_temp1 = 1;
                
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                
                light_pwm_clamp_mix(&ww, &cw);
all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;
                
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
                
                upload_device_value_status(DPID_WHITE_BRIGHT, wake_main_bright);
            }
        }
        if (wake_main_bright != target_main) all_finished = FALSE;
    }

    // 辅灯1%渐变处理
    if (info.aux_switch && sg_demo_info.aux_switch && wake_aux_bright != target_aux)
    {
        if (elapsed_time > 0 && __is_wake_time_to_gradual(elapsed_time, sg_wake_aux_interval_ms, sg_wake_timer_interval_ms, &sg_wake_last_aux_gradual_time))
        {
            uint8_t new_aux_bright = __calc_step_value_1percent(wake_aux_bright, target_aux);
            if (new_aux_bright != wake_aux_bright) {
                wake_aux_bright = new_aux_bright;
                sg_demo_info.aux_bright = wake_aux_bright;
                need_update = TRUE;
                
                // 计算PWM值
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t white_temp1 = wake_temp; // 使用当前渐变色温
                if (white_temp1 == 0) white_temp1 = 1;
                
                uint16_t aux_ww = aux_bright1 * white_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;
                
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
                
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, wake_aux_bright);
            }
        }
        if (wake_aux_bright != target_aux) all_finished = FALSE;
    }

    // 色温1%渐变处理（主灯和辅灯共享）
    if ((info.main_switch || info.aux_switch) && wake_temp != target_temp)
    {
        if (elapsed_time > 0 && __is_wake_time_to_gradual(elapsed_time, sg_wake_temp_interval_ms, sg_wake_timer_interval_ms, &sg_wake_last_temp_gradual_time))
        {
            uint8_t new_temp = __calc_step_value_1percent(wake_temp, target_temp);
            if (new_temp != wake_temp) {
                wake_temp = new_temp;
                need_update = TRUE;
                
                // 更新主灯和辅灯的色温
                if (info.main_switch && sg_demo_info.white_switch) {
                    sg_demo_info.white_temp = wake_temp;
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                    uint8_t white_temp1 = sg_demo_info.white_temp; // 使用当前渐变色温
                    if (white_temp1 == 0) white_temp1 = 1;
                    
                    uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                    
                    light_pwm_clamp_mix(&ww, &cw);
all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = ww;
                    
                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = cw;
                }
                if (info.aux_switch && sg_demo_info.aux_switch) {
                    sg_demo_info.white_temp = wake_temp;
                    uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                    uint8_t aux_temp1 = sg_demo_info.white_temp; // 使用当前渐变色温
                    if (aux_temp1 == 0) aux_temp1 = 1;
                    
                    uint16_t aux_ww = aux_bright1 * aux_temp1;
                    uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                    
                    light_pwm_clamp_mix(&aux_ww, &aux_cw);
all_channels[channel_count] = AUX_BRIGHT_PWM;
                    all_duties[channel_count++] = aux_ww;
                    
                    all_channels[channel_count] = AUX_TEMP_PWM;
                    all_duties[channel_count++] = aux_cw;
                }
            }
        }
        if (wake_temp != target_temp) all_finished = FALSE;
    }

    // 上报色温变化
    if (need_update && (info.main_switch || info.aux_switch)) {
        upload_device_value_status(DPID_TEMP_VALUE, wake_temp);
    }
    if (channel_count > 0)
    {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }
    // 检查是否所有参数都已达到目标值
    if (all_finished)
    {
        TUYA_PWM_NUM_E all_channels[5];
        UINT32_T all_duties[5];
        UINT8_T channel_count = 0;
        // 设置最终值
        if (info.main_switch)
        {
            sg_demo_info.white_bright = target_main;
            sg_demo_info.white_temp = target_temp;
            upload_device_value_status(DPID_WHITE_BRIGHT, target_main);
            
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
        }
        if (info.aux_switch)
        {
            sg_demo_info.aux_bright = target_aux;
            sg_demo_info.white_temp = target_temp;
            upload_device_value_status(DPID_AUX_BRIGHT_VALUE, target_aux);
            
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
        }
        
        // 上报最终色温值
        upload_device_value_status(DPID_TEMP_VALUE, target_temp);
        if (channel_count > 0)
        {
            pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
        }
        sg_wake_is_timing = FALSE;
        __wake_timer_stop_only();

        if (info.week == WEEK_ONCE)
        {
            info.enable = 0;
            sg_demo_info.wake_init[0] = 0x0;
            app_light_stop_wake_timer();
            dev_report_dp_raw_sync(NULL, WAKEUP_MODE, sg_demo_info.wake_init, 11, 5);
        }
        return;
    }
}

static VOID __wake_apply_minimum_start(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;

    wake_main_bright = 1;
    wake_aux_bright = 1;
    wake_temp = 0;

    TAL_PR_NOTICE("唤醒从最低亮度1%%开始");

    if (info.main_switch) {
        sg_demo_info.white_bright = 1;
        sg_demo_info.white_temp = 0;
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_switch = 1;
        if (sg_demo_info.night_switch == 1) {
            sg_demo_info.night_switch = 0;
            sg_demo_info.last_light_memory = 4;
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        }
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, 1);
        upload_device_value_status(DPID_TEMP_VALUE, 0);
        pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);

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
    } else {
        if (sg_demo_info.white_switch == 1) {
            sg_demo_info.white_switch = 0;
            upload_device_bool_status(LIGHT_SWITCH, 0);
            sg_demo_info.last_light_memory = 2;
            all_channels[channel_count] = BRIGHT_PWM;
            all_duties[channel_count++] = 0;

            all_channels[channel_count] = TEMP_PWM;
            all_duties[channel_count++] = 0;
        }
    }

    if (info.aux_switch) {
        sg_demo_info.aux_bright = 1;
        sg_demo_info.white_temp = 0;
        sg_demo_info.switch_status = 1;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.night_switch = 0;
        if (sg_demo_info.night_switch == 1) {
            sg_demo_info.night_switch = 0;
            sg_demo_info.last_light_memory = 4;
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        }
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 1);
        upload_device_value_status(DPID_TEMP_VALUE, 0);
        pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);

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
    } else {
        if (sg_demo_info.aux_switch == 1) {
            sg_demo_info.aux_switch = 0;
            upload_device_bool_status(AUX_SWITCH, 0);
            all_channels[channel_count] = AUX_BRIGHT_PWM;
            all_duties[channel_count++] = 0;

            all_channels[channel_count] = AUX_TEMP_PWM;
            all_duties[channel_count++] = 0;
            sg_demo_info.last_light_memory = 3;
        }
    }

    if (channel_count > 0) {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }
}

static VOID __wake_begin_gradual(VOID_T)
{
    extern UINT32_T elapsed_time;

    app_light_schedule_before_wake_start();
    upload_device_enum_status(DPID_WORK_MODE, 16);

    elapsed_time = 0;
    sg_wake_last_main_gradual_time = 0;
    sg_wake_last_aux_gradual_time = 0;
    sg_wake_last_temp_gradual_time = 0;

    UINT_T total_seconds = sg_wake_time_map[(info.duration > 0 && info.duration <= 6) ? (info.duration - 1) : 2];
    __init_wake_gradual_intervals(total_seconds);

    __wake_timer_restart(1, sg_wake_timer_interval_ms);
    sg_wake_is_timing = TRUE;
}

// 定时启动回调，每分钟检查一次
static VOID __wake_start_timer_cb(PVOID_T pTimerArg)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;
    extern bool sg_rhythm_interrupted;
    if (!info.enable)
        return;

    extern POSIX_TM_S local_tm;

    extern bool time_synced;
    if (!time_synced) return;

    if (app_light_schedule_is_wake_preempted_today()) {
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
    if (info.week == WEEK_ONCE) {
        week_match = TRUE;
    } else if (info.week == WEEK_EVERYDAY) {
        week_match = TRUE;
    } else {
        week_match = (info.week & current_week_bit) != 0;
    }

    // 精确到设定分钟，15s 轮询内首次命中触发
    if (!app_light_schedule_preset_minute_due(info.start_hour, info.start_min) ||
        !week_match)
    {
        return;
    }

    if (sg_wake_is_timing) {
        return;
    }

    if (app_light_schedule_should_skip_wake_at_trigger()) {
        TAL_PR_NOTICE("唤醒到点但与伴眠同时间，伴眠优先，跳过唤醒");
        if (info.week == WEEK_ONCE) {
            info.enable = 0;
            sg_demo_info.wake_init[0] = 0x0;
            app_light_stop_wake_timer();
            dev_report_dp_raw_sync(NULL, WAKEUP_MODE, sg_demo_info.wake_init, 11, 5);
        }
        return;
    }

    if (app_light_tm_sleep_is_timing() || sleep_state) {
        TAL_PR_NOTICE("唤醒到点，完全停止运行中的伴眠");
        app_light_preempt_sleep_today();
    }

    app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_WAKE, TRUE);
    __wake_apply_minimum_start();
    __wake_begin_gradual();
}

/************************ API 函数 ****************************/
OPERATE_RET app_light_wake_init(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    memset(&info, 0, sizeof(WAKE_INFO_T));
    memcpy(&info, sg_demo_info.wake_init, 11);

    if (app_light_tm_wake_dp_to_info(sg_demo_info.wake_init, 11) == OPRT_OK)
    {
        // app_light_start_wake_timer();
        if (info.start_time_en == 0)
        {
            info.enable = 0;
            sg_demo_info.wake_init[0] = 0x0;
            app_light_stop_wake_timer();
            dev_report_dp_raw_sync(NULL, WAKEUP_MODE, sg_demo_info.wake_init, 11, 5);
        }
        else
        {
            __wake_timer_restart(0, APP_LIGHT_SCHEDULE_POLL_MS);
            sg_wake_is_timing = false;
        }
    }

    app_light_schedule_boot_finish();
    return OPRT_OK;
}

OPERATE_RET app_light_tm_wake_dp_to_info(UCHAR_T *p_dp, UINT_T dp_len)
{
    if (!p_dp || dp_len < 10)
        return OPRT_INVALID_PARM;

    info.enable = p_dp[0];
    info.duration = p_dp[1];
    info.main_switch = p_dp[2];
    info.main_percent = p_dp[3];
    info.aux_switch = p_dp[4];
    info.aux_percent = p_dp[5];
    info.temper = p_dp[6];
    info.start_time_en = p_dp[7];
    info.start_hour = p_dp[8];
    info.start_min = p_dp[9];
    info.week = p_dp[10];

    return OPRT_OK;
}

OPERATE_RET app_light_wake_info_to_dp(UCHAR_T **p_dp, UINT_T *p_dp_len)
{
    if (!p_dp || !p_dp_len)
        return OPRT_INVALID_PARM;
    UCHAR_T *buf = (UCHAR_T *)tal_malloc(11);
    if (!buf)
        return OPRT_MALLOC_FAILED;
    memset(buf, 0, 11);

    buf[0] = info.enable;
    buf[1] = info.duration;
    buf[2] = info.main_switch;
    buf[3] = info.main_percent;
    buf[4] = info.aux_switch;
    buf[5] = info.aux_percent;
    buf[6] = info.temper;
    buf[7] = info.start_time_en;
    buf[8] = info.start_hour;
    buf[9] = info.start_min;
    buf[10] = info.week;

    *p_dp = buf;
    *p_dp_len = 11;
    return OPRT_OK;
}

OPERATE_RET app_light_start_wake_timer()
{
    extern bool sg_rhythm_interrupted;
    extern DEMO_INFO_T sg_demo_info;
    if (!info.enable)
        return OPRT_INVALID_PARM;

    if (app_light_schedule_reject_wake_on_enable()) {
        info.enable = 0;
        sg_demo_info.wake_init[0] = 0x0;
        app_light_stop_wake_timer();
        return OPRT_OK;
    }
    app_light_schedule_revalidate();
    app_light_schedule_clear_wake_preempt();

    if (info.start_time_en == 0)
    {
        app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_WAKE, TRUE);
        if (app_light_tm_sleep_is_timing() || sleep_state) {
            TAL_PR_NOTICE("唤醒立即启动，完全停止运行中的伴眠");
            app_light_preempt_sleep_today();
        }

        if (!sg_wake_is_timing)
        {
            __wake_apply_minimum_start();
            __wake_begin_gradual();
        }
    }
    else
    {
        // 定时启动，每分钟检查
        __wake_timer_restart(0, APP_LIGHT_SCHEDULE_POLL_MS);
        sg_wake_is_timing = false;
    }

    return OPRT_OK;
}

VOID app_light_preempt_wake_today(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    extern UINT32_T elapsed_time;

    if (!sg_wake_is_timing) {
        return;
    }

    __wake_timer_stop_only();

    sg_wake_is_timing = FALSE;
    elapsed_time = 0;
    wake_main_bright = 0;
    wake_aux_bright = 0;
    wake_temp = 0;
    sg_wake_last_main_gradual_time = 0;
    sg_wake_last_aux_gradual_time = 0;
    sg_wake_last_temp_gradual_time = 0;
    app_light_schedule_on_stopped(SCHEDULE_EVT_WAKE);

    if (info.start_time_en == 0 || info.week == WEEK_ONCE) {
        info.enable = 0;
        sg_demo_info.wake_init[0] = 0x0;
        app_light_schedule_clear_wake_preempt();
        dev_report_dp_raw_sync(NULL, WAKEUP_MODE, sg_demo_info.wake_init, 11, 5);
        TAL_PR_NOTICE("唤醒被抢占，当天完全停止");
        return;
    }

    app_light_schedule_mark_wake_preempted_today();
    TAL_PR_NOTICE("唤醒被抢占，当天完全停止，下一周期日再执行");

    if (info.enable && info.start_time_en == 1) {
        __wake_timer_restart(0, APP_LIGHT_SCHEDULE_POLL_MS);
    }
}

VOID app_light_stop_wake_timer(VOID_T)
{
    __wake_timer_stop_only();
    info.enable = 0;
    sg_wake_is_timing = FALSE;
    app_light_schedule_on_wake_disable();
}

BOOL_T app_light_wake_is_timing(VOID_T)
{
    return sg_wake_is_timing;
}