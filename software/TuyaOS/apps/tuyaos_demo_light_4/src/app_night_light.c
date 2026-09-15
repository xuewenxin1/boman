/**
 * @file app_nightlight.c
 * @brief 智能夜灯（light_4：无夜灯 PWM，时段内按 1.0.19 走辅灯）
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
#include "light_pwm_mix.h"

TIMER_ID night_light_id = NULL;
uint8_t night1[6];
int night_light_mode = 0;

bool_t __in_night_time()
{
    extern POSIX_TM_S local_tm;

    int now_h = local_tm.tm_hour;
    int now_m = local_tm.tm_min;

    int sh = night1[2], sm = night1[3];
    int eh = night1[4], em = night1[5];

    int now_min = now_h * 60 + now_m;
    int start_min = sh * 60 + sm;
    int end_min = eh * 60 + em;

    if (start_min <= end_min) {
        return (now_min >= start_min && now_min <= end_min);
    }
    return (now_min >= start_min || now_min <= end_min);
}

bool_t app_nightlight_should_enter(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    extern bool time_synced;
    memset(night1, 0, 6);
    if (sg_demo_info.night[0] == 0) {
        return FALSE;
    }
    memcpy(night1, sg_demo_info.night, 6);
    if (!time_synced) {
        return FALSE;
    }
    return (night1[0] && __in_night_time());
}

/**
 * @brief 执行夜灯：与 1.0.19 一致，主关辅开（无独立夜灯通道）
 */
bool_t app_nightlight_apply(VOID)
{
    extern bool time_synced;
    extern DEMO_INFO_T sg_demo_info;

    if (!time_synced) {
        return FALSE;
    }
    if (!app_nightlight_should_enter()) {
        return FALSE;
    }

    if (sg_demo_info.rhythm_switch == 1) {
        app_light_rhythm_interrupt_on_night_open();
    }

    memset(night1, 0, 6);
    memcpy(night1, sg_demo_info.night, 6);

    if (!night1[0] || !__in_night_time()) {
        return FALSE;
    }

    night_light_mode = 1;
    sg_demo_info.night_switch = 1;
    sg_demo_info.night_bright = night1[1];
    sg_demo_info.aux_switch = 1;
    sg_demo_info.aux_bright = night1[1] ? night1[1] : 1;
    sg_demo_info.white_temp = 0;
    sg_demo_info.white_switch = 0;
    sg_demo_info.switch_status = 1;

    {
        uint16_t bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
        uint8_t temp1 = sg_demo_info.white_temp;
        uint16_t aux_ww = bright1 * temp1;
        uint16_t aux_cw = bright1 * 100 - aux_ww;
        light_pwm_clamp_mix(&aux_ww, &aux_cw);

        TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
        UINT32_T duties[] = {0, 0, aux_ww, aux_cw};
        pwm_gradual_duty_set_multi(4, channels, duties);
    }

    upload_device_bool_status(DPID_SWITCH, 1);
    upload_device_bool_status(LIGHT_SWITCH, 0);
    upload_device_bool_status(AUX_SWITCH, 1);
    upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);

    return TRUE;
}

VOID app_nightlight_update(UINT8_T *raw, UINT16_T len)
{
    extern DEMO_INFO_T sg_demo_info;
    if (!raw || len != 6) {
        return;
    }
    memcpy(sg_demo_info.night, raw, 6);
}

STATIC VOID_T __timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    extern bool time_synced;
    UINT32_T current_time = tal_system_get_millisecond();
    (void)timer_id;
    (void)arg;

    if (current_time < 10000) {
        return;
    }
    if (!time_synced) {
        return;
    }

    /* 与 light_31 / 1.0.19 一致：定时器仅做一次探测后停，不跑软夜灯状态机 */
    tal_sw_timer_stop(night_light_id);
}

VOID app_light_on(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    memset(night1, 0, 6);
    memcpy(night1, sg_demo_info.night, 6);

    if (night1[0]) {
        TUYA_CALL_ERR_GOTO(tal_sw_timer_create(__timer_cb, NULL, &night_light_id), __EXIT);
        TUYA_CALL_ERR_LOG(tal_sw_timer_start(night_light_id, 1000, TAL_TIMER_CYCLE));
    }

__EXIT:
    return;
}
