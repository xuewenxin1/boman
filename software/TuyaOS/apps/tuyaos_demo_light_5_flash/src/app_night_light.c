/**
 * @file app_nightlight.c
 * @brief 智能夜灯功能模块
 */

#include "app_night_light.h"
#include "tal_log.h"
#include "dp_process.h"
#include "tal_sw_timer.h"
#include "pwm_gradual.h"
#include "tkl_pwm.h"
#include "app_pwm.h"
#include <string.h>
#include "tuya_error_code.h"
#include "power_count.h"
#include "app_light_tm_rhythm.h"
// 使用简单变量代替结构体
TIMER_ID night_light_id = NULL;
uint8_t night1[6];

/**
 * @brief 判断当前是否在夜灯时段内
 */
bool_t __in_night_time()
{
    OPERATE_RET rt = OPRT_OK;
    extern POSIX_TM_S local_tm;

    int now_h = local_tm.tm_hour;
    int now_m = local_tm.tm_min;

    int sh = night1[2], sm = night1[3]; // start
    int eh = night1[4], em = night1[5]; // end

    // 转换为分钟方便比较
    int now_min = now_h * 60 + now_m;
    int start_min = sh * 60 + sm;
    int end_min = eh * 60 + em;

    if (start_min <= end_min)
    {
        // 当天时间段
        return (now_min >= start_min && now_min <= end_min);
    }
    else
    {
        // 跨天时间段 (比如 22:00 ~ 06:00)
        return (now_min >= start_min || now_min <= end_min);
    }
}

/**
 * @brief 判断是否应该进入夜灯模式
 */
bool_t app_nightlight_should_enter(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    extern bool time_synced;
    memset(night1, 0, 6);
    if (sg_demo_info.night[0] == 0)
    {
        return FALSE; // 夜灯功能未开启
    }
    memcpy(night1, sg_demo_info.night, 6);
    if (!time_synced) return FALSE;

    // 夜灯功能开启且在夜灯时段内
    return (night1[0] && __in_night_time(night1));
}

int night_light_mode = 0;
/**
 * @brief 执行夜灯逻辑
 */
bool_t app_nightlight_apply(VOID)
{
    extern bool time_synced;
    extern DEMO_INFO_T sg_demo_info;
    if (!time_synced) return;
    
    // 如果不应该进入夜灯模式，直接返回
    if (!app_nightlight_should_enter())
    {
        return FALSE;
    }
    if(sg_demo_info.rhythm_switch == 1){
        app_light_rhythm_interrupt_on_night_open();
    }
    memset(night1, 0, 6);
    memcpy(night1, sg_demo_info.night, 6);

    if (night1[0])
    { // 夜灯功能开启
        if (__in_night_time(night1))
        {
            night_light_mode = 1;

            // 亮度限制 1~50
            UINT8_T percent = night1[1];

            // 转换为 PWM (0~1000)
            UINT16_T duty = percent;

            // 设置夜灯状态
            sg_demo_info.night_switch = 1;
            sg_demo_info.night_bright = duty;

            // 关闭主灯和辅灯

            TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
            UINT32_T duties[] = {0, 0, 0, 0, duty*100};
            pwm_gradual_duty_set_multi(5, channels, duties);

            // 更新设备状态
            upload_device_bool_status(DPID_SWITCH, 1);
            upload_device_bool_status(LIGHT_SWITCH, 0);
            upload_device_bool_status(AUX_SWITCH, 0);
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
            upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);

            sg_demo_info.white_switch = 0;
            sg_demo_info.switch_status = 1;
            sg_demo_info.aux_switch = 0;

            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @brief 处理来自 DP 的夜灯 raw 数据
 */
VOID app_nightlight_update(UINT8_T *raw, UINT16_T len)
{
    extern DEMO_INFO_T sg_demo_info;
    if (!raw || len != 6)
    {
        return;
    }

    memcpy(sg_demo_info.night, raw, 6);
}

STATIC VOID_T __timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    OPERATE_RET rt = OPRT_OK;
    extern bool time_synced;
    UINT32_T current_time = tal_system_get_millisecond();
    if (current_time < 10000)
    {
        tal_system_sleep(1000);
        return;
    }
    if (!time_synced) return;

    // app_nightlight_apply();
    tal_sw_timer_stop(night_light_id);
}

/**
 * @brief 在灯开启时调用
 */
VOID app_light_on(VOID)
{
    OPERATE_RET rt = OPRT_OK;

    // 只在夜灯可能开启的情况下创建定时器
    extern DEMO_INFO_T sg_demo_info;
    memset(night1, 0, 6);
    memcpy(night1, sg_demo_info.night, 6);
    
    if (night1[0])
    {
        TUYA_CALL_ERR_GOTO(tal_sw_timer_create(__timer_cb, NULL, &night_light_id), __EXIT);
        TUYA_CALL_ERR_LOG(tal_sw_timer_start(night_light_id, 1000, TAL_TIMER_CYCLE));
    }

__EXIT:
    return;
}
