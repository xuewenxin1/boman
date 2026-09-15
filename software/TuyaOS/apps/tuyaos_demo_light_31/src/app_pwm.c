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
#include "app_pwm1.h"
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

/**
 * @brief 定时器回调函数
 *
 * @param[in] timer_id: 定时器ID
 * @param[in] p_args: 用户参数
 * @return none
 */
/**
 * @brief example_pwm_start_and_stop
 *
 * @return none
 */
THREAD_HANDLE blink_task_handle = NULL;
VOID app_pwm()
{
    uint32_t count = 0;
    extern uint16_t gradual_time_ms;
    uint8_t switch_change = 0;
    uint8_t enabled_modes[4] = {0};            // 存储开启的模式索引
    uint8_t current_mode_index = 0;            // 当前应该执行的分支索引

    TAL_PR_NOTICE("----------app_pwm--------------\r\n");
    if (sg_demo_info.switch_change_gear[0] == 1)
    {
        if (sg_demo_info.switch_change_gear[1] == 1)
        {
            enabled_modes[switch_change] = 1;
            switch_change++;
        }
        if (sg_demo_info.switch_change_gear[7] == 1)
        {
            enabled_modes[switch_change] = 2;
            switch_change++;
        }
        if (sg_demo_info.switch_change_gear[13] == 1)
        {
            enabled_modes[switch_change] = 3;
            switch_change++;
        }
        if (sg_demo_info.switch_change_gear[19] == 1)
        {
            enabled_modes[switch_change] = 4;
            switch_change++;
        }
    }
    else{
        sg_demo_info.cnt1 = 1;
    }
    
    // 计算当前应该执行哪个开启的模式
    
    if (switch_change > 0 && sg_demo_info.cnt1 >= 2)
    {
        // 每次切档都是在当前记忆档位基础上+1
        uint8_t mode_index = (sg_demo_info.gear_memory + 1) % switch_change;
        current_mode_index = enabled_modes[mode_index];
        TAL_PR_NOTICE("gear switch: cnt1=%d mem %d->%d mode=%d n=%d\r\n",
                      sg_demo_info.cnt1, sg_demo_info.gear_memory,
                      mode_index, current_mode_index, switch_change);
        sg_demo_info.gear_memory = mode_index;
    } else if (sg_demo_info.cnt1 >= 2) {
        /* 总开关开了但子档全关：无法切档，至少按记忆出光，避免 current_mode=0 空切 */
        TAL_PR_NOTICE("gear skip: cnt1=%d en0=%d n=%d, restore light\r\n",
                      sg_demo_info.cnt1, sg_demo_info.switch_change_gear[0],
                      switch_change);
        sg_demo_info.cnt1 = 1;
    }
    TAL_PR_NOTICE("sg_demo_info.calibration = %d\r\n", sg_demo_info.calibration);
    if(sg_demo_info.calibration == 3){
        if (sg_demo_info.cnt1 == 1)
        {
            uint8_t type = sg_demo_info.custom_status[0];
            switch (type)
            {
            case 0:
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 55);
                sg_demo_info.switch_status = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.white_switch = 1;
                sg_demo_info.white_temp = 55;
                sg_demo_info.night_switch = 0;
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {5500, 4500, 0};
                pwm_gradual_duty_set_multi(3, channels, duties);
                break;
            case 1:{
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                /* 与 5_new 一致：全灭或不一致时按 last_light_memory 恢复，先清再置位 */
                if (sg_demo_info.switch_status == 0 ||
                    (sg_demo_info.white_switch == 0 && sg_demo_info.night_switch == 0)) {
                    sg_demo_info.white_switch = 0;
                    sg_demo_info.night_switch = 0;
                    if (sg_demo_info.last_light_memory == 4) {
                        sg_demo_info.night_switch = 1;
                    } else { /* 2 或默认：主灯 */
                        sg_demo_info.white_switch = 1;
                    }
                }
                sg_demo_info.switch_status =
                    (sg_demo_info.white_switch || sg_demo_info.night_switch) ? 1 : 0;
                if (sg_demo_info.white_switch == 1)
                {
                    upload_device_bool_status(DPID_SWITCH, 1);
                    upload_device_bool_status(LIGHT_SWITCH, 1);
                    upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * (100 - 5) / (100 - 1);
                    uint8_t white_temp1 = sg_demo_info.white_temp;
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
                if (sg_demo_info.night_switch == 1)
                {
                    /* 夜灯独占：主灯开关必须关掉，避免 App 仍显示主灯开 */
                    sg_demo_info.white_switch = 0;
                    sg_demo_info.switch_status = 1;
                    upload_device_bool_status(DPID_SWITCH, 1);
                    upload_device_bool_status(LIGHT_SWITCH, 0);
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
                    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                    all_duties[channel_count++] = sg_demo_info.night_bright * 100;
                    upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
                }
                else{
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                    all_duties[channel_count++] = 0;
                }
                sg_demo_info.switch_status =
                    (sg_demo_info.white_switch || sg_demo_info.night_switch) ? 1 : 0;
                upload_device_bool_status(DPID_SWITCH, sg_demo_info.switch_status);
                if (channel_count > 0)
                {
                    pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                }
            }
                break;
            case 2:{
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                sg_demo_info.night_switch = 0;
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
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
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * (100 - 5) / (100 - 1);
                    uint8_t white_temp1 = sg_demo_info.white_temp;
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
                sg_demo_info.switch_status = sg_demo_info.white_switch ? 1 : 0;
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
        }
        else
        {
            switch (current_mode_index)
            {
            case 1:{
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                sg_demo_info.night_switch = 0;
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                if (sg_demo_info.switch_change_gear[2] == 1)
                {
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.last_light_memory = 2;
                    sg_demo_info.white_bright = sg_demo_info.switch_change_gear[3];
                    sg_demo_info.white_temp = sg_demo_info.switch_change_gear[6];
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * (100 - 5) / (100 - 1);
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                    uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                    all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = ww;
                    
                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = cw;
                    upload_device_bool_status(DPID_SWITCH, 1);
                    upload_device_bool_status(LIGHT_SWITCH, 1);
                    upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
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
                sg_demo_info.switch_status = sg_demo_info.white_switch ? 1 : 0;
                upload_device_bool_status(DPID_SWITCH, sg_demo_info.switch_status);
                if (channel_count > 0)
                {
                    pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                }
            }
                break;
            case 2:{
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                sg_demo_info.night_switch = 0;
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                if (sg_demo_info.switch_change_gear[8] == 1)
                {
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.last_light_memory = 2;
                    sg_demo_info.white_bright = sg_demo_info.switch_change_gear[9];
                    sg_demo_info.white_temp = sg_demo_info.switch_change_gear[12];
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * (100 - 5) / (100 - 1);
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                    uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                    light_pwm_clamp_mix(&ww, &cw);
                    all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = ww;
                    
                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = cw;
                    upload_device_bool_status(DPID_SWITCH, 1);
                    upload_device_bool_status(LIGHT_SWITCH, 1);
                    upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
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
                sg_demo_info.switch_status = sg_demo_info.white_switch ? 1 : 0;
                upload_device_bool_status(DPID_SWITCH, sg_demo_info.switch_status);
                if (channel_count > 0)
                {
                    pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                }
            }
                break;
            case 3:{
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                sg_demo_info.night_switch = 0;
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
                if (sg_demo_info.switch_change_gear[14] == 1)
                {
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.last_light_memory = 2;
                    sg_demo_info.white_bright = sg_demo_info.switch_change_gear[15];
                    sg_demo_info.white_temp = sg_demo_info.switch_change_gear[18];
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * (100 - 5) / (100 - 1);
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                    uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                    light_pwm_clamp_mix(&ww, &cw);
                    all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = ww;
                    
                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = cw;
                    upload_device_bool_status(DPID_SWITCH, 1);
                    upload_device_bool_status(LIGHT_SWITCH, 1);
                    upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
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
                sg_demo_info.switch_status = sg_demo_info.white_switch ? 1 : 0;
                upload_device_bool_status(DPID_SWITCH, sg_demo_info.switch_status);
                if (channel_count > 0)
                {
                    pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                }
            }
                break;
            case 4:
                sg_demo_info.white_switch = 0;
                sg_demo_info.switch_status = 1;
                sg_demo_info.night_switch = 1;
                sg_demo_info.last_light_memory = 4;
                sg_demo_info.night_bright = sg_demo_info.switch_change_gear[20];

                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {0, 0, sg_demo_info.night_bright*100};
                pwm_gradual_duty_set_multi(3, channels, duties);
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 0);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
                upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
                break;
            default:
                break;
            }
        }
        /* 仅在校准有效时落盘：走 5s 防抖，切档风暴不会每秒写爆 Flash */
        device_config_save();
    }
    else{
        sg_demo_info.white_switch = 0;
        sg_demo_info.switch_status = 1;
        sg_demo_info.night_switch = 1;
        sg_demo_info.night_bright = 100;
        TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM,  NIGHT_BRIGHT_PWM};
        UINT32_T duties[] = {0, 0,  100*100};
        pwm_gradual_duty_set_multi(3, channels, duties);
    }
    return;
}



/**
 * @brief 复位/配网后固定出光：亮度100、色温55（5700K），并与 APP 对齐
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
    sg_demo_info.night_switch = 0;
    sg_demo_info.white_bright = bright;
    sg_demo_info.white_temp = temp;

    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, NIGHT_BRIGHT_PWM};
    UINT32_T duties[] = {ww, cw, 0};
    pwm_gradual_duty_set_multi(3, channels, duties);

    upload_device_bool_status(DPID_SWITCH, 1);
    upload_device_bool_status(LIGHT_SWITCH, 1);
    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
    upload_device_value_status(DPID_WHITE_BRIGHT, bright);
    upload_device_value_status(DPID_TEMP_VALUE, temp);

    if (sg_demo_info.change_light_status == 0)
        gradual_time_ms = 20;
    else
        gradual_time_ms = 1600;
}

/**
 * @brief led init — 配网呼吸三次后按记忆出光（默认 5700K）
 */
VOID_T app_led_init()
{
    extern DEMO_INFO_T sg_demo_info;
    extern uint16_t gradual_time_ms;

    gradual_time_ms = 1000;
    sg_demo_info.first_network++;
    device_config_save1();
    // 第一阶段：设置高亮度
    TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM,NIGHT_BRIGHT_PWM};
    UINT32_T duties1[] = { LIGHT_PWM_BOUNDARY_MAX, LIGHT_PWM_BOUNDARY_MIN, 0 };
    pwm_gradual_duty_set_multi(3, channels1, duties1);

    // 等待渐变完成
    pwm_gradual_wait_current_complete(1000);  // 等待1秒

    // 第二阶段：设置为低亮度
    TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM};
    UINT32_T duties2[] = {3, 3};
    pwm_gradual_duty_set_multi(2, channels2, duties2);

    pwm_gradual_wait_current_complete(1000);  // 等待1秒

    // 第一阶段：设置高亮度
    TUYA_PWM_NUM_E channels3[] = {BRIGHT_PWM, TEMP_PWM, NIGHT_BRIGHT_PWM};
    UINT32_T duties3[] = { LIGHT_PWM_BOUNDARY_MAX, LIGHT_PWM_BOUNDARY_MIN, 0 };
    pwm_gradual_duty_set_multi(3, channels3, duties3);

    // 等待渐变完成
    pwm_gradual_wait_current_complete(1000);  // 等待1秒

    // 第二阶段：设置为低亮度
    TUYA_PWM_NUM_E channels4[] = {BRIGHT_PWM, TEMP_PWM};
    UINT32_T duties4[] = {3, 3};
    pwm_gradual_duty_set_multi(2, channels4, duties4);

    pwm_gradual_wait_current_complete(1000);  // 等待1秒
    TUYA_PWM_NUM_E channels5[] = {BRIGHT_PWM, TEMP_PWM, NIGHT_BRIGHT_PWM};
    UINT32_T duties5[] = { LIGHT_PWM_BOUNDARY_MAX, LIGHT_PWM_BOUNDARY_MIN, 0 };
    pwm_gradual_duty_set_multi(3, channels5, duties5);

    pwm_gradual_wait_current_complete(1000);  // 等待1秒
    TUYA_PWM_NUM_E channels6[] = {BRIGHT_PWM, TEMP_PWM};
    UINT32_T duties6[] = {3, 3};
    pwm_gradual_duty_set_multi(2, channels6, duties6);
    pwm_gradual_wait_current_complete(1000);  // 等待1秒

    /* 呼吸结束：按记忆打光并上报，避免灯/APP 色温不一致 */
    gradual_time_ms = 1000;
    app_led_apply_memory_light();
    pwm_gradual_wait_current_complete(1000);
}