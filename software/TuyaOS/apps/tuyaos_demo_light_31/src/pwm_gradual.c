#include "pwm_gradual.h"
#include "tal_sw_timer.h"
#include "tkl_system.h"
#include "tal_memory.h"
#include "tal_log.h"
#include <string.h>
#include "app_pwm1.h"
#include "tkl_pwm.h"
#include <math.h>
#include "dp_process.h"
#include "power_count.h"

// 模块全局变量
static PWM_CHANNEL_STATE_T channel_state[PWM_CH_MAX];
static GLOBAL_GRADUAL_CTRL_T global_ctrl = {0};
static BOOL_T timer_initialized = FALSE;
static BOOL_T timer_running = FALSE;
uint8_t default_start_duty = 0; // 默认起始占空比

#define TIMER_ID1        TUYA_TIMER_NUM_0
#define MAX_GRADUAL_TIME_MS 1600
#define MIN_GRADUAL_TIME_MS 500
#define PWM_FULL_RANGE 10000
// 外部变量
extern uint16_t gradual_time_ms;

// 启动定时器
static OPERATE_RET start_gradual_timer(VOID_T)
{
    if (!timer_running)
    {
        OPERATE_RET ret = tkl_timer_start(TIMER_ID1, 15 * 1000);
        if (ret == OPRT_OK)
        {
            timer_running = TRUE;
        }
        return ret;
    }
    return OPRT_OK;
}

// 停止定时器
static OPERATE_RET stop_gradual_timer(VOID_T)
{
    if (timer_running)
    {
        OPERATE_RET ret = tkl_timer_stop(TIMER_ID1);
        if (ret == OPRT_OK)
        {
            timer_running = FALSE;
        }
        return ret;
    }
    return OPRT_OK;
}

// 计算实际渐变时间（根据最大变化量调整）
static UINT32_T calculate_gradual_time(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[])
{
    extern uint16_t gradual_time_ms;
    UINT32_T configured_time = gradual_time_ms;
    
    if (configured_time > MAX_GRADUAL_TIME_MS)
    {
        configured_time = MAX_GRADUAL_TIME_MS;
    }
    else if (configured_time == 0)
    {
        configured_time = MAX_GRADUAL_TIME_MS;
    }
    
    // 计算最大变化量
    UINT32_T max_change = 0;
    
    for (int i = 0; i < channel_count; i++)
    {
        TUYA_PWM_NUM_E ch = channels[i];
        if (ch >= PWM_CH_MAX)
        {
            continue;
        }
        
        UINT32_T current_value = channel_state[ch].current_value;
        UINT32_T target_value = duties[i];
        
        UINT32_T change = 0;
        if (target_value > current_value)
        {
            change = target_value - current_value;
        }
        else
        {
            change = current_value - target_value;
        }
        
        if (change > max_change)
        {
            max_change = change;
        }
    }
    
    // 如果变化量为0，使用最小渐变时间
    if (max_change == 0)
    {
        return MIN_GRADUAL_TIME_MS;
    }
    
    // 计算变化量占总范围的比例
    float change_ratio = (float)max_change / (float)PWM_FULL_RANGE;
    
    // // 如果变化比例太小，使用最小渐变时间
    // if (change_ratio < 0.01f)  // 小于1%的变化
    // {
    //     return MIN_GRADUAL_TIME_MS;
    // }
    
    // 根据变化量比例计算实际渐变时间
    
    UINT32_T actual_time = (UINT32_T)(configured_time * change_ratio);
    
    // 确保渐变时间在合理范围内
    if (actual_time < MIN_GRADUAL_TIME_MS)
    {
        actual_time = MIN_GRADUAL_TIME_MS;
    }
    else if (actual_time > configured_time)
    {
        actual_time = configured_time;
    }
    if(configured_time == 20)
        actual_time = 20;
    return actual_time;
}

// 亮度渐变曲线（更平缓的曲线）
static float ease_bright_quadratic(float progress)
{
    if (progress < 0.0f) return 0.0f;
    if (progress > 1.0f) return 1.0f;
    
    // 对数曲线，更自然
    float raw_value = log10f(1.0f + 1.0f * progress);
    static const float max_value = log10f(2.0f);  // log10(10) = 1
    
    return raw_value / max_value;
}

// 色温渐变曲线（更快速的曲线）
static float ease_temp_quadratic(float progress)
{
    if (progress < 0.0f) return 0.0f;
    if (progress > 1.0f) return 1.0f;
    
    // 平方根曲线，响应更快
    return progress;
    // return progress * progress * (3.0f - 2.0f * progress);
}

// 根据曲线类型选择缓动函数
static float get_eased_progress(float progress, BOOL_T is_increasing, UINT8_T curve_type)
{
    if (is_increasing)
    {
        // 开灯：使用加速曲线
        if (curve_type == 0)  // 亮度曲线
        {
            return ease_bright_quadratic(progress);
        }
        else  // 色温曲线
        {
            return ease_temp_quadratic(progress);
        }
    }
    else
    {
        // 关灯：使用减速曲线
        if (curve_type == 0)  // 亮度曲线
        {
            // 亮度关灯曲线：更平滑
            return ease_bright_quadratic(progress);
        }
        else  // 色温曲线
        {
            // 色温关灯曲线：更快
            return ease_temp_quadratic(progress);
        }
    }
}

// 停止当前渐变并立即切换到目标值
static void stop_current_and_set_immediate(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[])
{
    extern DEMO_INFO_T sg_demo_info;
    if (global_ctrl.is_active)
    {
        // 停止当前渐变
        for (int i = 0; i < global_ctrl.channel_count; i++)
        {
            TUYA_PWM_NUM_E ch = global_ctrl.channels[i];
            if (ch < PWM_CH_MAX)
            {
                // 如果这个通道不在新请求中，则保持当前值
                BOOL_T found = FALSE;
                for (int j = 0; j < channel_count; j++)
                {
                    if (channels[j] == ch)
                    {
                        found = TRUE;
                        break;
                    }
                }
                if (!found)
                {
                    // 立即设置到当前值
                    uint16_t calibration_num = 10000;
                    if(ch == BRIGHT_PWM)
                        calibration_num = sg_demo_info.ww_calibration_coefficient;
                    if(ch == TEMP_PWM)
                        calibration_num = sg_demo_info.cw_calibration_coefficient;
                    tkl_pwm_duty_set(ch, calibration_num*channel_state[ch].current_value/10000);
                    tkl_pwm_start(ch);
                }
            }
        }
    }
    
    // 设置新的目标值
    for (int i = 0; i < channel_count; i++)
    {
        TUYA_PWM_NUM_E ch = channels[i];
        if (ch < PWM_CH_MAX)
        {
            // 立即更新通道状态
            channel_state[ch].current_value = duties[i];
            uint16_t calibration_num = 10000;
            if(channels[i] == BRIGHT_PWM)
                calibration_num = sg_demo_info.ww_calibration_coefficient;
            if(channels[i] == TEMP_PWM)
                calibration_num = sg_demo_info.cw_calibration_coefficient;
            
            tkl_pwm_duty_set(ch, calibration_num*duties[i]/10000);
            tkl_pwm_start(ch);
        }
    }
    
    global_ctrl.is_active = FALSE;
}

// 定时器回调函数
static VOID_T pwm_gradual_timer_cb(VOID_T *arg)
{
    static UINT32_T last_time = 0;
    extern DEMO_INFO_T sg_demo_info;
    UINT32_T now = tkl_system_get_millisecond();
    
    // 防止过于频繁执行
    if (now - last_time < 12)
    {
        return;
    }

    last_time = now;
    
    // 如果没有活跃渐变，停止定时器
    if (!global_ctrl.is_active)
    {
        stop_gradual_timer();
        return;
    }

    /* 渐变进行中市电掉了：停掉，避免掉电过程还改 PWM */
    if (!power_ac_ok_for_gradual()) {
        global_ctrl.is_active = FALSE;
        stop_gradual_timer();
        return;
    }
    
    // 处理当前渐变
    UINT32_T elapsed = now - global_ctrl.start_time;
    
    // 检查是否完成
    if (elapsed >= global_ctrl.gradual_time)
    {
        // 渐变完成，设置最终值
        for (int i = 0; i < global_ctrl.channel_count; i++)
        {
            TUYA_PWM_NUM_E ch = global_ctrl.channels[i];
            if (ch < PWM_CH_MAX)
            {
                channel_state[ch].current_value = global_ctrl.target_values[i];
                
                uint16_t calibration_num = 10000;
                if(ch == BRIGHT_PWM)
                    calibration_num = sg_demo_info.ww_calibration_coefficient;
                if(ch == TEMP_PWM)
                    calibration_num = sg_demo_info.cw_calibration_coefficient;
                uint16_t num = calibration_num*global_ctrl.target_values[i]/10000;
                if(num != 0){
                    tkl_pwm_duty_set(ch, num);
                    tkl_pwm_start(ch);
                }
                else
                    tkl_pwm_stop(ch);
            }
        }
        
        // 渐变完成，停止定时器
        global_ctrl.is_active = FALSE;
        stop_gradual_timer();
        return;
    }
    
    // 计算当前值并设置PWM
    float progress = (float)elapsed / (float)global_ctrl.gradual_time;
    if (progress > 1.0f) progress = 1.0f;
    
    for (int i = 0; i < global_ctrl.channel_count; i++)
    {
        TUYA_PWM_NUM_E ch = global_ctrl.channels[i];
        if (ch >= PWM_CH_MAX)
        {
            continue;
        }
        
        UINT32_T start_value = global_ctrl.start_values[i];
        UINT32_T target_value = global_ctrl.target_values[i];
        
        BOOL_T is_increasing = (target_value > start_value);
        float eased_progress = get_eased_progress(progress, is_increasing, global_ctrl.curve_type);
        
        UINT32_T current_value;
        if (is_increasing)
        {
            UINT32_T range = target_value - start_value;
            current_value = start_value + (UINT32_T)(range * eased_progress);
        }
        else
        {
            UINT32_T range = start_value - target_value;
            current_value = start_value - (UINT32_T)(range * eased_progress);
        }
        
        // 确保关灯时不会因为浮点计算误差导致不为0
        if (target_value == 0 && current_value < 10)
        {
            current_value = 0;
        }
        
        // 更新PWM值
        if (current_value != channel_state[ch].current_value)
        {
            uint16_t calibration_num = 10000;
            if(ch == BRIGHT_PWM)
                calibration_num = sg_demo_info.ww_calibration_coefficient;
            if(ch == TEMP_PWM)
                calibration_num = sg_demo_info.cw_calibration_coefficient;
            uint16_t num = calibration_num*current_value/10000;
            if(num != 0){
                tkl_pwm_duty_set(ch, num);
                tkl_pwm_start(ch);
            }
            else
                tkl_pwm_stop(ch);
            channel_state[ch].current_value = current_value;
        }
    }
}

OPERATE_RET pwm_gradual_init(VOID_T)
{
    if (timer_initialized)
    {
        return OPRT_OK;
    }
    
    // 初始化通道状态
    memset(channel_state, 0, sizeof(channel_state));
    memset(&global_ctrl, 0, sizeof(global_ctrl));
    
    for (int i = 0; i < PWM_CH_MAX; i++)
    {
        channel_state[i].is_initialized = TRUE;
        channel_state[i].current_value = default_start_duty;
    }
    
    // 创建渐变定时器
    TUYA_TIMER_BASE_CFG_T sg_timer_cfg = {
        .mode = TUYA_TIMER_MODE_PERIOD,
        .args = NULL,
        .cb = pwm_gradual_timer_cb
    };
    
    tkl_timer_init(TIMER_ID1, &sg_timer_cfg);
    
    timer_initialized = TRUE;
    timer_running = FALSE;
    return OPRT_OK;
}

// 修改后的pwm_gradual_duty_set_multi，增加曲线类型参数
static UINT32_T last_target_values[PWM_CH_MAX] = {0};
static BOOL_T has_stored_targets = FALSE;
OPERATE_RET pwm_gradual_duty_set_multi_ex(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[], UINT8_T curve_type)
{
    extern uint8_t preview_flag;
    extern DEMO_INFO_T sg_demo_info;
    uint16_t calibration_num = 10000;
    
    if(preview_flag == 1)
        return;

    /* 除上电渐变外：市电不在 / 掉电恢复中不启渐变（掉电灭灯走 bypass） */
    if (!power_ac_ok_for_gradual()) {
        TAL_PR_NOTICE("[PWM] skip gradual, ac not ok\r\n");
        return OPRT_COM_ERROR;
    }
        
    if (channel_count == 0 || channel_count > MAX_CHANNELS_PER_REQUEST)
    {
        return OPRT_INVALID_PARM;
    }
    
    if (!timer_initialized)
    {
        OPERATE_RET ret = pwm_gradual_init();
        if (ret != OPRT_OK)
        {
            return ret;
        }
    }
    
    // 验证通道参数
    for (int i = 0; i < channel_count; i++)
    {
        if (channels[i] >= PWM_CH_MAX)
        {
            return OPRT_INVALID_PARM;
        }
        
        if (!channel_state[channels[i]].is_initialized)
        {
            channel_state[channels[i]].current_value = default_start_duty;
            channel_state[channels[i]].is_initialized = TRUE;
            
            uint16_t calibration_num = 10000;
            if(channels[i] == BRIGHT_PWM)
                calibration_num = sg_demo_info.ww_calibration_coefficient;
            if(channels[i] == TEMP_PWM)
                calibration_num = sg_demo_info.cw_calibration_coefficient;
            tkl_pwm_duty_set(channels[i], calibration_num * default_start_duty/10000);
            tkl_pwm_start(channels[i]);
        }
    }
    
    // 保存这次的目标值
    for (int i = 0; i < channel_count; i++)
    {
        TUYA_PWM_NUM_E ch = channels[i];
        if (ch < PWM_CH_MAX)
        {
            last_target_values[ch] = duties[i];
        }
    }
    has_stored_targets = TRUE;
    
    // 如果有渐变在进行
    if (global_ctrl.is_active)
    {
        // 停止定时器
        stop_gradual_timer();
        
        // 让不在新请求中的通道立即跳变到当前目标值
        for (int i = 0; i < global_ctrl.channel_count; i++)
        {
            TUYA_PWM_NUM_E ch = global_ctrl.channels[i];
            if (ch < PWM_CH_MAX)
            {
                BOOL_T in_new_request = FALSE;
                
                // 检查是否在新请求中
                for (int j = 0; j < channel_count; j++)
                {
                    if (channels[j] == ch)
                    {
                        in_new_request = TRUE;
                        break;
                    }
                }
                
                // 如果不在新请求中，跳变到目标值
                if (!in_new_request)
                {
                    uint16_t calibration_num = 10000;
                    if(channels[i] == BRIGHT_PWM)
                        calibration_num = sg_demo_info.ww_calibration_coefficient;
                    if(channels[i] == TEMP_PWM)
                        calibration_num = sg_demo_info.cw_calibration_coefficient;
                    tkl_pwm_duty_set(ch, calibration_num*global_ctrl.target_values[i]/10000);
                    tkl_pwm_start(ch);
                    channel_state[ch].current_value = global_ctrl.target_values[i];
                }
            }
        }
        
        global_ctrl.is_active = FALSE;
    }
    
    // 设置新的渐变参数
    global_ctrl.channel_count = channel_count;
    global_ctrl.curve_type = curve_type;  // 设置曲线类型
    for (int i = 0; i < channel_count; i++)
    {
        TUYA_PWM_NUM_E ch = channels[i];
        global_ctrl.channels[i] = ch;
        global_ctrl.target_values[i] = duties[i];
        global_ctrl.start_values[i] = channel_state[ch].current_value;
    }
    
    // 计算渐变时间
    if(gradual_time_ms < 3010){
        global_ctrl.gradual_time = calculate_gradual_time(channel_count,channels,duties);
    }
    else
        global_ctrl.gradual_time = 3010;
    
    global_ctrl.start_time = tkl_system_get_millisecond();
    global_ctrl.is_active = TRUE;
    
    // 启动定时器
    return start_gradual_timer();
}

// 原函数保持不变，默认使用亮度曲线
OPERATE_RET pwm_gradual_duty_set_multi(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[])
{
    // 默认使用亮度曲线
    // return pwm_gradual_duty_set_multi_ex(channel_count, channels, duties, 0);
    return pwm_gradual_duty_set_multi_ex(channel_count, channels, duties, 0);
}

// 新增：调亮度专用函数
OPERATE_RET pwm_gradual_brightness_set(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[])
{
    // 使用亮度曲线
    return pwm_gradual_duty_set_multi_ex(channel_count, channels, duties, 0);
}

// 新增：调色温专用函数
OPERATE_RET pwm_gradual_colortemp_set(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[])
{
    // 使用色温曲线
    return pwm_gradual_duty_set_multi_ex(channel_count, channels, duties, 1);
}

// 单通道渐变函数
OPERATE_RET pwm_gradual_duty_set(TUYA_PWM_NUM_E ch, UINT32_T duty)
{
    TUYA_PWM_NUM_E channels[1] = {ch};
    UINT32_T duties[1] = {duty};
    
    return pwm_gradual_duty_set_multi(1, channels, duties);
}

// 单通道调亮度
OPERATE_RET pwm_gradual_brightness(TUYA_PWM_NUM_E ch, UINT32_T duty)
{
    TUYA_PWM_NUM_E channels[1] = {ch};
    UINT32_T duties[1] = {duty};
    
    return pwm_gradual_brightness_set(1, channels, duties);
}

// 单通道调色温
OPERATE_RET pwm_gradual_colortemp(TUYA_PWM_NUM_E ch, UINT32_T duty)
{
    TUYA_PWM_NUM_E channels[1] = {ch};
    UINT32_T duties[1] = {duty};
    
    return pwm_gradual_colortemp_set(1, channels, duties);
}

// 立即设置占空比（无渐变）
OPERATE_RET pwm_gradual_duty_set_immediate(TUYA_PWM_NUM_E ch_id, UINT32_T duty)
{
    extern DEMO_INFO_T sg_demo_info;
    if (ch_id >= PWM_CH_MAX)
    {
        return OPRT_INVALID_PARM;
    }
    
    if (!timer_initialized)
    {
        OPERATE_RET ret = pwm_gradual_init();
        if (ret != OPRT_OK)
        {
            return ret;
        }
    }
    
    // 如果有渐变正在进行，且包含此通道，停止该通道的渐变
    if (global_ctrl.is_active)
    {
        for (int i = 0; i < global_ctrl.channel_count; i++)
        {
            if (global_ctrl.channels[i] == ch_id)
            {
                // 从渐变中移除这个通道
                for (int j = i; j < global_ctrl.channel_count - 1; j++)
                {
                    global_ctrl.channels[j] = global_ctrl.channels[j+1];
                    global_ctrl.start_values[j] = global_ctrl.start_values[j+1];
                    global_ctrl.target_values[j] = global_ctrl.target_values[j+1];
                }
                global_ctrl.channel_count--;
                
                // 如果没有其他通道了，停止渐变
                if (global_ctrl.channel_count == 0)
                {
                    global_ctrl.is_active = FALSE;
                    stop_gradual_timer();
                }
                break;
            }
        }
    }
    
    // 立即设置
    channel_state[ch_id].current_value = duty;
    channel_state[ch_id].is_initialized = TRUE;
    uint16_t calibration_num = 10000;
    if(ch_id == BRIGHT_PWM)
        calibration_num = sg_demo_info.ww_calibration_coefficient;
    if(ch_id == TEMP_PWM)
        calibration_num = sg_demo_info.cw_calibration_coefficient;
    tkl_pwm_duty_set(ch_id, calibration_num*duty/10000);
    tkl_pwm_start(ch_id);
    
    // 关灯时停止PWM
    if (duty == 0)
    {
        tkl_pwm_stop(ch_id);
    }
    
    return OPRT_OK;
}

// 停止所有渐变
OPERATE_RET pwm_gradual_stop_all(VOID_T)
{
    if (!timer_initialized)
    {
        return OPRT_OK;
    }
    
    // 停止当前渐变
    global_ctrl.is_active = FALSE;
    
    // 停止定时器
    stop_gradual_timer();
    
    return OPRT_OK;
}

// 停止当前渐变
OPERATE_RET pwm_gradual_stop_current(VOID_T)
{
    if (!timer_initialized)
    {
        return OPRT_OK;
    }
    
    // 停止当前渐变
    global_ctrl.is_active = FALSE;
    stop_gradual_timer();
    
    return OPRT_OK;
}

// 获取当前占空比
UINT32_T pwm_gradual_get_current_duty(TUYA_PWM_NUM_E ch_id)
{
    if (ch_id >= PWM_CH_MAX || !timer_initialized)
    {
        return 0;
    }
    
    return channel_state[ch_id].current_value;
}

// 检查是否空闲
BOOL_T pwm_gradual_is_idle(VOID_T)
{
    if (!timer_initialized)
    {
        return TRUE;
    }
    
    return (!global_ctrl.is_active);
}

// 等待所有渐变完成
OPERATE_RET pwm_gradual_wait_idle(UINT32_T timeout_ms)
{
    if (!timer_initialized)
    {
        return OPRT_OK;
    }
    
    UINT32_T start_time = tkl_system_get_millisecond();
    
    while (!pwm_gradual_is_idle())
    {
        tkl_system_sleep(10);
        
        if (tkl_system_get_millisecond() - start_time > timeout_ms)
        {
            return OPRT_TIMEOUT;
        }
    }
    
    return OPRT_OK;
}

// 等待当前渐变完成
OPERATE_RET pwm_gradual_wait_current_complete(UINT32_T timeout_ms)
{
    if (!timer_initialized)
    {
        return OPRT_OK;
    }
    
    UINT32_T start_time = tkl_system_get_millisecond();
    
    while (global_ctrl.is_active)
    {
        tkl_system_sleep(10);
        
        if (tkl_system_get_millisecond() - start_time > timeout_ms)
        {
            return OPRT_TIMEOUT;
        }
    }
    
    return OPRT_OK;
}

// 立即停止渐变并跳转到目标值
OPERATE_RET pwm_gradual_stop_and_jump(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[])
{
    extern DEMO_INFO_T sg_demo_info;
    if (channel_count == 0 || channel_count > MAX_CHANNELS_PER_REQUEST)
    {
        return OPRT_INVALID_PARM;
    }
    
    if (!timer_initialized)
    {
        OPERATE_RET ret = pwm_gradual_init();
        if (ret != OPRT_OK)
        {
            return ret;
        }
    }
    
    // 停止当前渐变
    if (global_ctrl.is_active)
    {
        global_ctrl.is_active = FALSE;
        stop_gradual_timer();
    }
    
    // 立即跳转到目标值
    for (int i = 0; i < channel_count; i++)
    {
        TUYA_PWM_NUM_E ch = channels[i];
        if (ch < PWM_CH_MAX)
        {
            channel_state[ch].current_value = duties[i];
            channel_state[ch].is_initialized = TRUE;
            uint16_t calibration_num = 10000;
            if(channels[i] == BRIGHT_PWM)
                calibration_num = sg_demo_info.ww_calibration_coefficient;
            if(channels[i] == TEMP_PWM)
                calibration_num = sg_demo_info.cw_calibration_coefficient;
            tkl_pwm_duty_set(ch, calibration_num*duties[i]/10000);
            tkl_pwm_start(ch);
        }
    }
    
    return OPRT_OK;
}