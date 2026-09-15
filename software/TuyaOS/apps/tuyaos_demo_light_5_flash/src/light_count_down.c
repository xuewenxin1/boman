/**
 • @file app_light_countdown.c

 • @brief Light Countdown DP & timer logic (Minute-based)

 • @version 0.4

 • @date 2025-10-15

 */

#include "tal_sw_timer.h"
#include "light_count_down.h"
#include "dp_process.h"
#include "tal_log.h"
#include "tal_system.h"
#include "tal_memory.h"
#include "app_pwm.h"
#include "pwm_gradual.h"
#include "tuya_iot_com_api.h"
#include "light_pwm_mix.h"
#include "app_light_tm_schedule.h"
/***
 *variable define
 ***/
STATIC TM_COUNT_DOWN_INFORM_CB sg_countdown_over_cb = NULL;
STATIC TIMER_ID sg_countdown_timer = NULL;
STATIC UINT32_T sg_remain_min = 0; // 剩余分钟
STATIC BOOL_T sg_is_running = FALSE;

/************  回调函数 ***/
/* 五路灯：主灯 + 辅灯 + 夜灯。memory: 1=主辅, 2=主灯, 3=辅灯, 4=夜灯 */
STATIC VOID countdown_save_light_memory(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;

    if (sg_demo_info.night_switch == 1) {
        sg_demo_info.last_light_memory = 4;
    } else if (sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1) {
        sg_demo_info.last_light_memory = 1;
    } else if (sg_demo_info.white_switch == 1) {
        sg_demo_info.last_light_memory = 2;
    } else if (sg_demo_info.aux_switch == 1) {
        sg_demo_info.last_light_memory = 3;
    }
}

STATIC VOID countdown_restore_light_on(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    BOOL_T open_white = FALSE;
    BOOL_T open_aux = FALSE;
    BOOL_T open_night = FALSE;

    switch (sg_demo_info.last_light_memory) {
        case 1:
            open_white = TRUE;
            open_aux = TRUE;
            break;
        case 2:
            open_white = TRUE;
            break;
        case 3:
            open_aux = TRUE;
            break;
        case 4:
            open_night = TRUE;
            break;
        default:
            open_white = TRUE;
            open_aux = TRUE;
            break;
    }

    if (sg_demo_info.rhythm_switch == 1) {
        app_light_stop_today_rhythm_timer();
    }

    sg_demo_info.switch_status = 1;
    sg_demo_info.white_switch = open_white;
    sg_demo_info.aux_switch = open_aux;
    sg_demo_info.night_switch = open_night;

    uint16_t ww = 0, cw = 0, aux_ww = 0, aux_cw = 0, night_bright = 0;

    if (sg_demo_info.white_switch == 1) {
        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
        uint8_t white_temp1 = sg_demo_info.white_temp;
        ww = white_bright1 * white_temp1;
        cw = white_bright1 * 100 - ww;
        light_pwm_clamp_mix(&ww, &cw);
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
    } else {
        upload_device_bool_status(LIGHT_SWITCH, 0);
    }

    if (sg_demo_info.aux_switch == 1) {
        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
        uint8_t aux_temp1 = sg_demo_info.white_temp;
        aux_ww = aux_bright1 * aux_temp1;
        aux_cw = aux_bright1 * 100 - aux_ww;
        light_pwm_clamp_mix(&aux_ww, &aux_cw);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
    } else {
        upload_device_bool_status(AUX_SWITCH, 0);
    }

    if (sg_demo_info.night_switch == 1) {
        night_bright = sg_demo_info.night_bright * 100;
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
        upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
    } else {
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
    }

    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
    UINT32_T duties[] = {ww, cw, aux_ww, aux_cw, night_bright};
    pwm_gradual_duty_set_multi(5, channels, duties);
    upload_device_bool_status(DPID_SWITCH, 1);
    upload_device_enum_status(DPID_WORK_MODE, 16);
}

STATIC VOID my_countdown_over_cb(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern uint8_t work_mode;
    if (sg_demo_info.aux_switch == 0 && sg_demo_info.night_switch == 0 && sg_demo_info.white_switch == 0)
    {
        countdown_restore_light_on();
    }
    else
    {
        TUYA_PWM_NUM_E all_channels[5];
        UINT32_T all_duties[5];
        UINT8_T channel_count = 0;
        countdown_save_light_memory();
        if (sg_demo_info.white_switch == 1)
        {
            sg_demo_info.white_switch = 0;
            all_channels[channel_count] = BRIGHT_PWM;
            all_duties[channel_count++] = 0;
            
            all_channels[channel_count] = TEMP_PWM;
            all_duties[channel_count++] = 0;
            upload_device_bool_status(LIGHT_SWITCH, 0);
        }
        if (sg_demo_info.aux_switch == 1)
        {
            sg_demo_info.aux_switch = 0;
            all_channels[channel_count] = AUX_BRIGHT_PWM;
            all_duties[channel_count++] = 0;
            
            all_channels[channel_count] = AUX_TEMP_PWM;
            all_duties[channel_count++] = 0;
            upload_device_bool_status(AUX_SWITCH, 0);
        }
        if (sg_demo_info.night_switch == 1)
        {
            sg_demo_info.night_switch = 0;
            all_channels[channel_count] = NIGHT_BRIGHT_PWM;
            all_duties[channel_count++] = 0;
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        }
        if (sg_demo_info.aux_switch == 0 && sg_demo_info.white_switch == 0 && sg_demo_info.night_switch == 0)
        {
            sg_demo_info.switch_status = 0;
            upload_device_bool_status(DPID_SWITCH, 0);
        }
        if (channel_count > 0)
        {
            pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
        }
        if(sg_sleep_is_timing){
            if (sg_demo_info.sleep_init[0] == 0x01)
            {
                sg_demo_info.sleep_init[0] = 0x0;
                app_light_stop_sleep_timer();
                dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                                        sg_demo_info.sleep_init, 15, 5);
            }
        }
        if(sg_wake_is_timing){
            if (sg_demo_info.wake_init[0] == 0x01)
            {
                sg_demo_info.wake_init[0] = 0x0;
                app_light_stop_wake_timer();
                dev_report_dp_raw_sync(NULL, WAKEUP_MODE,
                                        sg_demo_info.wake_init, 11, 5);
            }
        }
        if (work_mode != 16) {
            work_mode = 16;
            app_light_schedule_on_stopped(SCHEDULE_EVT_SCENE);
        }
        upload_device_enum_status(DPID_WORK_MODE, 16);
    }
    // 倒计时结束后执行熄灯或其他逻辑
}

/************ 内部上报函数 ***/
STATIC OPERATE_RET __app_light_tm_upload_count_down(UINT_T minute)
{
    TY_OBJ_DP_S dp_obj_data = {0};
    dp_obj_data.dpid = COUNTDOWN;
    dp_obj_data.type = PROP_VALUE;
    dp_obj_data.value.dp_value = minute;

    OPERATE_RET rt = dev_report_dp_json_async(NULL, &dp_obj_data, 1);
    return rt;
}

/************ 定时器回调函数 ***/
STATIC VOID_T __app_light_countdown_timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    if (sg_remain_min == 0)
    {
        // 理论上不会到这里，因为启动前判断，但加保护
        tal_sw_timer_stop(sg_countdown_timer);
        sg_is_running = FALSE;
        __app_light_tm_upload_count_down(0);
        if (sg_countdown_over_cb)
            sg_countdown_over_cb();
        return;
    }

    // 减少 1 分钟
    sg_remain_min--;

    // 上报剩余分钟数
    __app_light_tm_upload_count_down(sg_remain_min);

    // 如果倒计时结束
    if (sg_remain_min == 0)
    {
        tal_sw_timer_stop(sg_countdown_timer);
        sg_is_running = FALSE;
        if (sg_countdown_over_cb)
            sg_countdown_over_cb();
    }
}

/************ 初始化倒计时模块 ***/
OPERATE_RET app_light_countdown_module_init(VOID_T)
{
    sg_countdown_over_cb = my_countdown_over_cb;
    sg_is_running = FALSE;
    sg_remain_min = 0;

    if (sg_countdown_timer == NULL)
    {
        OPERATE_RET rt = tal_sw_timer_create(__app_light_countdown_timer_cb, NULL, &sg_countdown_timer);
        if (OPRT_OK != rt)
        {
            return rt;
        }
    }
    return OPRT_OK;
}

/************ 启动倒计时 ***/
OPERATE_RET app_light_start_count_down_timer(UINT_T time_min)
{
    if (time_min == 0)
    {
        return OPRT_INVALID_PARM;
    }

    sg_remain_min = time_min;
    sg_is_running = TRUE;

    // 启动时立即上报一次初始值
    __app_light_tm_upload_count_down(time_min);

    // 启动定时器，每 1 分钟触发一次（60000ms）
    tal_sw_timer_start(sg_countdown_timer, 60000, TAL_TIMER_CYCLE);
    return OPRT_OK;
}

/************ 停止倒计时 ***/
OPERATE_RET app_light_stop_count_down_timer(VOID_T)
{
    if (!sg_is_running)
        return OPRT_OK;

    tal_sw_timer_stop(sg_countdown_timer);
    sg_is_running = FALSE;
    sg_remain_min = 0;

    __app_light_tm_upload_count_down(0);

    return OPRT_OK;
}

/************ 同步倒计时上报 ***/
OPERATE_RET app_light_tm_count_down_syn(VOID_T)
{
    if (!sg_is_running)
        __app_light_tm_upload_count_down(0);
    else
        __app_light_tm_upload_count_down(sg_remain_min);

    return OPRT_OK;
}
