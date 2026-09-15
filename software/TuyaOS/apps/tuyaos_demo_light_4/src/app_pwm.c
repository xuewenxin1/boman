/**
 * @file examples_driver_pwm.c
 * @author www.tuya.com
 * @brief 一个简单的tkl pwm接口使用演示程序，可以通过命令行执行
 * @version 0.2
 * @date 2022-05-20
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include "tuya_cloud_types.h"
#include "tuya_svc_netmgr.h"
#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "tkl_pwm.h"
#include "app_pwm.h"
#include "pwm_gradual.h"
#include "tal_thread.h"
#include "dp_process.h"
#include "tal_sw_timer.h"
#include "pwm_gradual.h"
#include "power_count.h"
#include "light_pwm_mix.h"
/***********************************************************
*************************micro define***********************
***********************************************************/
#define PWM_DUTY 0 // 50% duty
#define PWM_FREQUENCY 3000
#define TIMER_DELAY_MS 10000 // 15秒延时

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/


/***********************************************************
***********************function define**********************
***********************************************************/

THREAD_HANDLE blink_task_handle = NULL;

/**
 * @brief 上电出光：按 custom_status 记忆/自定义（light_4 无墙壁开关切档）
 */
VOID app_pwm()
{
    extern DEMO_INFO_T sg_demo_info;
    extern uint16_t gradual_time_ms;

    if (sg_demo_info.calibration >= 3) {
        uint8_t type = sg_demo_info.custom_status[0];
        switch (type)
        {
        case 0:
            upload_device_bool_status(DPID_SWITCH, 1);
            upload_device_bool_status(LIGHT_SWITCH, 1);
            upload_device_bool_status(AUX_SWITCH, 1);
            upload_device_value_status(DPID_WHITE_BRIGHT, 100);
            upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
            upload_device_value_status(DPID_TEMP_VALUE, 55);
            sg_demo_info.switch_status = 1;
            sg_demo_info.aux_bright = 100;
            sg_demo_info.aux_switch = 1;
            sg_demo_info.white_temp = 55;
            sg_demo_info.white_bright = 100;
            sg_demo_info.white_switch = 1;
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {5500, 4500, 5500, 4500};
                pwm_gradual_duty_set_multi(4, channels, duties);
            }
            break;
        case 1: {
            TUYA_PWM_NUM_E all_channels[4];
            UINT32_T all_duties[4];
            UINT8_T channel_count = 0;
            /* 关灯或全灭时按记忆恢复；先清通道再赋值，避免残留导致 App 显示主辅都开 */
            if (sg_demo_info.white_switch == 0 && sg_demo_info.aux_switch == 0) {
                if (sg_demo_info.last_light_memory == 2) {
                    sg_demo_info.white_switch = 1;
                } else if (sg_demo_info.last_light_memory == 3) {
                    sg_demo_info.aux_switch = 1;
                } else {
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.aux_switch = 1;
                }
            }
            sg_demo_info.switch_status = 1;
            if (sg_demo_info.white_switch == 1)
            {
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                if (white_temp1 == 0)
                    white_temp1 = 1;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
            }
            else
            {
                upload_device_bool_status(LIGHT_SWITCH, 0);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = 0;
            }
            if (sg_demo_info.aux_switch == 1)
            {
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                if (aux_temp1 == 0)
                    aux_temp1 = 1;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            else
            {
                upload_device_bool_status(AUX_SWITCH, 0);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = 0;
            }
            if (channel_count > 0)
            {
                pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
            }
        }
            break;
        case 2: {
            TUYA_PWM_NUM_E all_channels[4];
            UINT32_T all_duties[4];
            UINT8_T channel_count = 0;
            if (sg_demo_info.custom_status[1] == 1)
            {
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.custom_status[2]);
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.custom_status[5]);
                sg_demo_info.white_switch = 1;
                sg_demo_info.white_temp = sg_demo_info.custom_status[5];
                sg_demo_info.white_bright = sg_demo_info.custom_status[2];
                sg_demo_info.switch_status = 1;
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                if (white_temp1 == 0)
                    white_temp1 = 1;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
            }
            else
            {
                sg_demo_info.white_switch = 0;
                upload_device_bool_status(LIGHT_SWITCH, 0);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = 0;
            }
            if (sg_demo_info.custom_status[3] == 1)
            {
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_bright = sg_demo_info.custom_status[4];
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_temp = sg_demo_info.custom_status[5];
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.custom_status[4]);
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.custom_status[5]);
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                if (aux_temp1 == 0)
                    aux_temp1 = 1;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            else
            {
                sg_demo_info.aux_switch = 0;
                upload_device_bool_status(AUX_SWITCH, 0);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = 0;
            }
            if ((sg_demo_info.custom_status[3] == 1) && (sg_demo_info.custom_status[1] == 1))
                sg_demo_info.last_light_memory = 1;
            else if (sg_demo_info.custom_status[1] == 1)
                sg_demo_info.last_light_memory = 2;
            else if (sg_demo_info.custom_status[3] == 1)
                sg_demo_info.last_light_memory = 3;
            sg_demo_info.switch_status =
                (sg_demo_info.white_switch || sg_demo_info.aux_switch) ? 1 : 0;
            upload_device_bool_status(DPID_SWITCH, sg_demo_info.switch_status);
            if (channel_count > 0)
            {
                pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
            }
        }
            break;
        default:
            break;
        }
        device_config_save1();
    }
    else {
        sg_demo_info.white_switch = 0;
        sg_demo_info.switch_status = 1;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.aux_bright = 100;
        sg_demo_info.white_temp = 100;
        {
            TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
            UINT32_T duties[] = {0, 0, 10000, 0};
            pwm_gradual_duty_set_multi(4, channels, duties);
        }
    }
    return;
}

/**
 * @brief 复位/配网后固定出光：主+辅亮度100、色温55
 */
VOID_T app_led_apply_memory_light(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    extern uint16_t gradual_time_ms;
    uint8_t bright = 100;
    uint8_t temp = 55;
    uint16_t white_bright1 = 5 + (bright - 1) * 95 / 99;
    uint16_t ww = white_bright1 * temp;
    uint16_t cw = white_bright1 * 100 - ww;
    light_pwm_clamp_mix(&ww, &cw);

    sg_demo_info.switch_status = 1;
    sg_demo_info.white_switch = 1;
    sg_demo_info.aux_switch = 1;
    sg_demo_info.night_switch = 0;
    sg_demo_info.white_bright = bright;
    sg_demo_info.aux_bright = bright;
    sg_demo_info.white_temp = temp;

    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties[] = {ww, cw, ww, cw};
    pwm_gradual_duty_set_multi(4, channels, duties);

    upload_device_bool_status(DPID_SWITCH, 1);
    upload_device_bool_status(LIGHT_SWITCH, 1);
    upload_device_bool_status(AUX_SWITCH, 1);
    upload_device_value_status(DPID_WHITE_BRIGHT, bright);
    upload_device_value_status(DPID_AUX_BRIGHT_VALUE, bright);
    upload_device_value_status(DPID_TEMP_VALUE, temp);

    if (sg_demo_info.change_light_status == 0)
        gradual_time_ms = 20;
    else
        gradual_time_ms = 1600;
}

/**
 * @brief led init — 配网呼吸三次后固定 100/55 出光
 */
VOID_T app_led_init()
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    extern uint16_t gradual_time_ms;
    extern int pwm_gradual;
    GW_WIFI_NW_STAT_E state;
    get_wf_gw_nw_status(state);
    gradual_time_ms = 1000;
    sg_demo_info.first_network++;
    device_config_save1();

    TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
    pwm_gradual_duty_set_multi(4, channels1, duties1);
    pwm_gradual_wait_current_complete(1000);

    TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties2[] = {1, 1, 1, 1};
    pwm_gradual_duty_set_multi(4, channels2, duties2);
    pwm_gradual_wait_current_complete(1000);

    TUYA_PWM_NUM_E channels3[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties3[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
    pwm_gradual_duty_set_multi(4, channels3, duties3);
    pwm_gradual_wait_current_complete(1000);

    TUYA_PWM_NUM_E channels4[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties4[] = {1, 1, 1, 1};
    pwm_gradual_duty_set_multi(4, channels4, duties4);
    pwm_gradual_wait_current_complete(1000);

    TUYA_PWM_NUM_E channels5[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties5[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
    pwm_gradual_duty_set_multi(4, channels5, duties5);
    pwm_gradual_wait_current_complete(1000);

    TUYA_PWM_NUM_E channels6[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
    UINT32_T duties6[] = {1, 1, 1, 1};
    pwm_gradual_duty_set_multi(4, channels6, duties6);
    pwm_gradual_wait_current_complete(1000);

    gradual_time_ms = 1000;
    app_led_apply_memory_light();
    pwm_gradual_wait_current_complete(1000);
}
