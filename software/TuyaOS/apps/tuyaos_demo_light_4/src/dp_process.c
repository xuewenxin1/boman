/**
 * @file dp_process.c
 * @author www.tuya.com
 * @version 0.1
 * @date 2022-09-22
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include "tuya_cloud_types.h"
#include "tuya_iot_com_api.h"

#include "tal_log.h"
#include "tal_memory.h"
#include "tal_workq_service.h"
#include "tal_system.h"
#include "tal_watchdog.h"
#include "tkl_flash.h"
#include "dp_process.h" 
#include "app_light_tm_sleep.h"
#include "app_light_tm_wake.h"
#include "app_pwm.h"
#include "app_light_tm_rhythm.h"
#include "app_light_tm_schedule.h"
#include "pwm_gradual.h"
#include "tal_sw_timer.h"
#include "tal_mutex.h"
#include "power_count.h"
#include "app_power_disturb.h"
#include "light_pwm_mix.h"
#include "app_night_light.h"
/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/
uint32_t white_off_time = 0;
uint32_t aux_off_time = 0;
DEMO_INFO_T sg_demo_info = {0};
uint8_t rhythm_sunlight1[66] = { // 节律开关 开启
    0x00,
    0x7F, // 周一到周日生效 (01111111b)
    // 节点1 日出前 (05:50, 20%, 0%, 30分钟渐变)
    0x01, 0x05, 0x32, 0x14, 0x00, 0x03, 0x00, 0x01,
    // 节点2 日出后 (07:00, 80%, 50%, 全程渐变)
    0x01, 0x07, 0x00, 0x50, 0x32, 0x01, 0x00, 0x02,
    // 节点3 正午 (12:00, 100%, 100%, 全程渐变)
    0x01, 0x0C, 0x00, 0x64, 0x64, 0x01, 0x00, 0x03,
    // 节点4 日落前 (16:00, 90%, 50%, 全程渐变)
    0x01, 0x10, 0x00, 0x5A, 0x32, 0x01, 0x00, 0x04,
    // 节点5 日落后 (18:10, 80%, 40%, 全程渐变)
    0x01, 0x12, 0x0A, 0x50, 0x28, 0x01, 0x00, 0x05,
    // 节点6 睡觉 (23:00, 1%, 0%, 全程渐变)
    0x01, 0x17, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00};
/***********************************************************
***********************variable define**********************
***********************************************************/

/***********************************************************
***********************function define**********************
***********************************************************/
/**
 * @brief Output the received data and reply to the cloud
 *
 * @param[in] dp_data_arr: the array of recevie dp
 * @param[in] dp_cnt: the number of dp
 *
 * @return none
 */
uint8_t time_s = 0;
uint8_t work_mode = 16;
VOID dp_obj_process(CONST TY_OBJ_DP_S *dp_data_arr, UINT_T dp_cnt)
{
    UINT32_T index = 0;
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S *dp_data = NULL;
    extern uint16_t gradual_time_ms;
    extern bool sg_rhythm_interrupted;
    extern TM_RHYTHM_INFO_T sg_rhythm_info;
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern int night_light_mode;

    if (NULL == dp_data_arr || 0 == dp_cnt)
    {
        return;
    }

    for (index = 0; index < dp_cnt; index++)
    {
        dp_data = (TY_OBJ_DP_S *)&dp_data_arr[index];
        switch (dp_data->dpid)
        {
        case DPID_SWITCH:
            /* 伴眠/唤醒主动上报回显忽略 */
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_bool == sg_demo_info.switch_status) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();


            // 正常模式下的开关逻辑
            {
                bool sw_changed = (dp_data->value.dp_bool != sg_demo_info.switch_status);
                sg_demo_info.switch_status = dp_data->value.dp_bool;
                if (sg_demo_info.switch_status == 0) {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_OFF);
                } else {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_ON);
                }
            }

            if (sg_demo_info.switch_status == 1)
            {
                // ==================== 开灯逻辑 ====================
                // 检查是否应该进入夜灯模式
                if ((app_nightlight_should_enter()) && (sg_demo_info.aux_switch == 0))
                {
                    app_nightlight_apply();
                    if (night_light_mode == 1)
                    {
                        night_light_mode = 0;
                        sg_demo_info.switch_status = 1;
                        return;
                    }
                }
                else
                {
                    bool open_white = false;
                    bool open_aux = false;
                    // bool open_night = false;
                    switch (sg_demo_info.last_light_memory) {
                        case 1: // 主辅灯都关闭
                            open_white = true;
                            open_aux = true;
                            break;
                        case 2: // 关闭的是主灯
                            open_white = true;
                            open_aux = false;
                            break;
                        case 3: // 关闭的是辅灯
                            open_white = false;
                            open_aux = true;
                            break;
                    }
                    
                    // 设置灯的开关状态
                    sg_demo_info.white_switch = open_white;
                    sg_demo_info.aux_switch = open_aux;
                    uint16_t aux_ww,aux_cw,cw,ww;
                    if(sg_demo_info.white_switch == 1){
                        upload_device_bool_status(LIGHT_SWITCH, 1);
                    }
                    if(sg_demo_info.aux_switch == 1){
                        upload_device_bool_status(AUX_SWITCH, 1);
                    }
                    if(sg_demo_info.white_bright == 0 && sg_demo_info.white_switch == 1){
                        sg_demo_info.white_switch = 0;
                        upload_device_bool_status(LIGHT_SWITCH, 0);
                    }
                    if(sg_demo_info.white_switch == 1){
                        upload_device_bool_status(LIGHT_SWITCH, 1);
                        // 设置主灯和辅灯亮度
                        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                        uint8_t white_temp1 = sg_demo_info.white_temp;
                                                ww = white_bright1 * white_temp1;
                        cw = white_bright1 * 100 - ww;
                       light_pwm_clamp_mix(&ww, &cw);
                        // if (ww >= 9999)
                        // {
                        //     ww = 9999;
                        //     cw = 1;
                        // }
                        // if (cw >= 9999)
                        // {
                        //     cw = 9999;
                        //     ww = 1;
                        // }
                    }else{
                        cw = 0;
                        ww = 0;     
                    }
                    if(sg_demo_info.aux_bright == 0 && sg_demo_info.aux_switch == 1){
                        sg_demo_info.aux_switch = 0;
                        upload_device_bool_status(AUX_SWITCH, 0);
                    }
                    if(sg_demo_info.aux_switch == 1){
                        // upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
                        // upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                        upload_device_bool_status(AUX_SWITCH, 1);
                        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                        uint8_t aux_temp1 = sg_demo_info.white_temp;
                                                aux_ww = aux_bright1 * aux_temp1;
                        aux_cw = aux_bright1 * 100 - aux_ww;
                       light_pwm_clamp_mix(&aux_ww, &aux_cw);
                        // if (aux_ww >= 9999)
                        // {
                        //     aux_ww = 9999;
                        //     aux_cw = 1;
                        // }
                        // if (aux_cw >= 9999)
                        // {
                        //     aux_cw = 9999;
                        //     aux_ww = 1000;
                        // }
                    }
                    else{
                        aux_cw = 0;
                        aux_ww = 0;     
                    }

                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                    UINT32_T duties[] = {ww, cw, aux_ww, aux_cw};
                    pwm_gradual_duty_set_multi(4, channels, duties);

                    // 上报状态
                    upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
                    
                }
                // 总开关状态上报
                upload_device_bool_status(DPID_SWITCH, 1);
            }
            else
            {
                // ==================== 关灯逻辑 ====================
                if(sg_demo_info.white_switch == 1 &&sg_demo_info.aux_switch == 1)
                    sg_demo_info.last_light_memory = 1;
                else if(sg_demo_info.white_switch == 1)
                    sg_demo_info.last_light_memory = 2;
                else if(sg_demo_info.aux_switch == 1)
                    sg_demo_info.last_light_memory = 3;
                
                // 关闭所有灯光状态
                sg_demo_info.white_switch = 0;
                sg_demo_info.aux_switch = 0;
                sg_demo_info.switch_status = 0;

                // 关闭所有PWM输出
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {0, 0, 0, 0};
                pwm_gradual_duty_set_multi(4, channels, duties);

                // 上报所有关闭状态
                upload_device_bool_status(DPID_SWITCH, 0);
                upload_device_bool_status(LIGHT_SWITCH, 0);
                upload_device_bool_status(AUX_SWITCH, 0);
                // upload_device_value_status(DPID_WHITE_BRIGHT, 0);
                // upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 0);
                // upload_device_value_status(DPID_TEMP_VALUE, 0);
                upload_device_enum_status(DPID_WORK_MODE, 16);
            }
            break;
        case DPID_WORK_MODE:
            work_mode = dp_data->value.dp_enum;
            if (work_mode != 16)
            {
                if (app_light_schedule_should_skip_scene_at_trigger()) {
                    work_mode = 16;
                    break;
                }
                app_light_schedule_mark_configured(SCHEDULE_EVT_SCENE);
                app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_SCENE, TRUE);
                if (sg_demo_info.rhythm_switch == 1) {
                    app_light_stop_today_rhythm_timer();
                }
            }
            if (work_mode == 0)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {5500, 4500, 5500, 4500};
                pwm_gradual_duty_set_multi(4, channels, duties);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.white_temp = 55;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 55);
            }
            else if (work_mode == 1)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {10000, 0, 10000, 0};
                pwm_gradual_duty_set_multi(4, channels, duties);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.white_temp = 100;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 100);
            }
            else if (work_mode == 2)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {100*35, 100*65, 30*35, 30*65};
                pwm_gradual_duty_set_multi(4, channels, duties);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.aux_bright = 30;
                sg_demo_info.white_temp = 35;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 30);
                upload_device_value_status(DPID_TEMP_VALUE, 35);
            }
            else if (work_mode == 3)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {30*1, 30*99, 70*1, 70*99};
                pwm_gradual_duty_set_multi(4, channels, duties);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 30;
                sg_demo_info.aux_bright = 70;
                sg_demo_info.white_temp = 0;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 30);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 70);
                upload_device_value_status(DPID_TEMP_VALUE, 0);
            }
            
            else if (work_mode == 4)
            {
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                sg_demo_info.white_switch = sg_demo_info.collect[0];
                sg_demo_info.white_bright = sg_demo_info.collect[1];
                sg_demo_info.aux_switch = sg_demo_info.collect[2];
                sg_demo_info.aux_bright = sg_demo_info.collect[3];
                sg_demo_info.white_temp = sg_demo_info.collect[4];
                if ((sg_demo_info.white_switch == 1) || (sg_demo_info.aux_switch == 1))
                {
                    sg_demo_info.switch_status = 1;
                    upload_device_bool_status(DPID_SWITCH, 1);
                }
                if (sg_demo_info.white_switch == 1)
                {
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                                        uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                    light_pwm_clamp_mix(&ww, &cw);
                    // if (ww >= 9999)
                    // {
                    //     ww = 9999;
                    //     cw = 1;
                    // }
                    // if (cw >= 9999)
                    // {
                    //     cw = 9999;
                    //     ww = 1;
                    // }
                    all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = ww;

                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = cw;
                    upload_device_bool_status(LIGHT_SWITCH, 1);
                    upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                }
                if (sg_demo_info.aux_switch == 1)
                {
                    uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                    uint8_t aux_temp1 = sg_demo_info.white_temp;
                                        uint16_t aux_ww = aux_bright1 * aux_temp1;
                    uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                    light_pwm_clamp_mix(&aux_ww, &aux_cw);
                    // if (aux_ww >= 9999)
                    // {
                    //     aux_ww = 9999;
                    //     aux_cw = 1;
                    // }
                    // if (aux_cw >= 9999)
                    // {
                    //     aux_cw = 9999;
                    //     aux_ww = 1;
                    // }
                    all_channels[channel_count] = AUX_BRIGHT_PWM;
                    all_duties[channel_count++] = aux_ww;

                    all_channels[channel_count] = AUX_TEMP_PWM;
                    all_duties[channel_count++] = aux_cw;
                    upload_device_bool_status(AUX_SWITCH, 1);
                    upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                }
                if (channel_count > 0)
                {
                    pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                }
            }
            else if (work_mode == 16)
                ;
            else
            {
                app_mode_apply_group(work_mode);
            }

            break;
        case CHANGE_LIGHT_STATUS:
            sg_demo_info.change_light_status = dp_data->value.dp_enum;
            if (sg_demo_info.change_light_status == 0)
                gradual_time_ms = 20;
            else
                gradual_time_ms = 1600;
            break;
        case DPID_WHITE_BRIGHT:
            /* 同值=伴眠回显忽略；真调节则打断伴眠/唤醒后生效 */
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_value == sg_demo_info.white_bright) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            {
                bool val_changed = (dp_data->value.dp_value != sg_demo_info.white_bright);
                sg_demo_info.white_bright = dp_data->value.dp_value;
                app_light_rhythm_interrupt_on_user_change(val_changed, RHYTHM_USER_OP_DIM_TEMP);
            }
            if (sg_demo_info.white_switch == 1)
            {
                upload_device_enum_status(DPID_WORK_MODE, 16);
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                // if (ww >= 9999)
                // {
                //     ww = 9999;
                //     cw = 1;
                // }
                // if (cw >= 9999)
                // {
                //     cw = 9999;
                //     ww = 1;
                // }

                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {ww, cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            break;
        case DPID_TEMP_VALUE:
        {
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_value == sg_demo_info.white_temp) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            TUYA_PWM_NUM_E all_channels[5];
            UINT32_T all_duties[5];
            UINT8_T channel_count = 0;
            {
                bool val_changed = (dp_data->value.dp_value != sg_demo_info.white_temp);
                sg_demo_info.white_temp = dp_data->value.dp_value;
                app_light_rhythm_interrupt_on_user_change(val_changed, RHYTHM_USER_OP_DIM_TEMP);
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (sg_demo_info.white_switch == 1)
            {
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                // if (ww >= 9999)
                // {
                //     ww = 9999;
                //     cw = 1;
                // }
                // if (cw >= 9999)
                // {
                //     cw = 9999;
                //     ww = 1;
                // }
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;

                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
            }
            if (sg_demo_info.aux_switch == 1)
            {
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                // if (aux_ww >= 9999)
                // {
                //     aux_ww = 9999;
                //     aux_cw = 1;
                // }
                // if (aux_cw >= 9999)
                // {
                //     aux_cw = 9999;
                //     aux_ww = 1;
                // }
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;

                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            if (channel_count > 0)
            {
                pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
            }
        }
        break;
        case DPID_AUX_BRIGHT_VALUE:
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_value == sg_demo_info.aux_bright) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            {
                bool val_changed = (dp_data->value.dp_value != sg_demo_info.aux_bright);
                sg_demo_info.aux_bright = dp_data->value.dp_value;
                app_light_rhythm_interrupt_on_user_change(val_changed, RHYTHM_USER_OP_DIM_TEMP);
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (sg_demo_info.aux_switch == 1)
            {
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                                uint16_t aux_ww = aux_bright1 * white_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                // if (aux_ww >= 9999)
                // {
                //     aux_ww = 9999;
                //     aux_cw = 1;
                // }
                // if (aux_cw >= 9999)
                // {
                //     aux_cw = 9999;
                //     aux_ww = 1;
                // }
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            break;
        case COUNTDOWN:
            time_s = dp_data->value.dp_value; // 从 DP 获取倒计时时间（秒）
            if (time_s == 0)
            {
                app_light_stop_count_down_timer();
            }
            else
                app_light_start_count_down_timer(time_s);
            break;
        case RHYTHM_SWITCH:
        {
            if (sg_demo_info.change_light_status == 0)
                gradual_time_ms = 20;
            else
                gradual_time_ms = 1600;
            extern bool sg_rhythm_interrupted;
            extern TM_RHYTHM_INFO_T sg_rhythm_info;
            uint8_t sw = dp_data->value.dp_bool;
            sg_demo_info.rhythm_switch = sw;

            if (sw)
            {
                /* 开启节律时退出情景模式 */
                if (work_mode != 16) {
                    work_mode = 16;
                    app_light_schedule_on_stopped(SCHEDULE_EVT_SCENE);
                    upload_device_enum_status(DPID_WORK_MODE, 16);
                }
                app_light_schedule_mark_configured(SCHEDULE_EVT_RHYTHM);
                /* 打开节律：可打断伴眠；唤醒进行中则等待（不打断） */
                app_light_schedule_before_rhythm_start();
                upload_device_bool_status(RHYTHM_STATUS, 0);

                // 检查是否有主灯或辅灯开启
                bool main_light_on = (sg_demo_info.white_switch || sg_demo_info.aux_switch);

                if (main_light_on)
                {
                    app_light_resume_today_rhythm_timer(); // 重新启动节律
                }
                else
                {
                    // 灯没开，检查是否需要自动开灯（DP触发，只检查节点时间）
                    if (check_and_perform_auto_light_on()) {
                        // 自动开灯成功，启动节律
                        app_light_tm_rhythm_syn();
                        upload_device_bool_status(RHYTHM_STATUS, 0);
                        app_light_resume_today_rhythm_timer();
                    } else {
                        // 灯没开，且不在自动开灯节点，节律处于等待状态
                        sg_rhythm_interrupted = false;
                    }
                }
            }
            else
            {
                // upload_device_bool_status(RHYTHM_STATUS,0);
                app_light_stop_today_rhythm_timer(); // 停止节律
            }
        }
        break;
        case RHYTHM_STATUS:
        {
            extern int sg_interrupted_node;
            uint8_t status = dp_data->value.dp_bool;
            sg_rhythm_interrupted = status;
            sg_interrupted_node = get_current_node_index();
        }
        break;

        case LIGHT_SWITCH:
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_bool == sg_demo_info.white_switch) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            {
                bool sw_changed = (dp_data->value.dp_bool != sg_demo_info.white_switch);
                sg_demo_info.white_switch = dp_data->value.dp_bool;
                app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_ON);
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (dp_data->value.dp_bool == 1)
            {
                sg_demo_info.switch_status = 1;
                
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT,sg_demo_info.white_bright);
                upload_device_value_status(DPID_TEMP_VALUE,sg_demo_info.white_temp);
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                // if (ww >= 9999)
                // {
                //     ww = 9999;
                //     cw = 1;
                // }
                // if (cw >= 9999)
                // {
                //     cw = 9999;
                //     ww = 1;
                // }
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {ww, cw,0};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            else
            {
                sg_demo_info.last_light_memory = 2;
                white_off_time = tal_system_get_millisecond();
                if(white_off_time - aux_off_time <= 5000)
                    sg_demo_info.last_light_memory = 1;
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {0, 0};
                pwm_gradual_duty_set_multi(2, channels, duties);
                upload_device_bool_status(LIGHT_SWITCH, 0);
            }
            if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0))
            {
                sg_demo_info.switch_status = 0;
                upload_device_bool_status(DPID_SWITCH, 0);
            }
            break;
        case AUX_SWITCH:
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_bool == sg_demo_info.aux_switch) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            {
                bool sw_changed = (dp_data->value.dp_bool != sg_demo_info.aux_switch);
                sg_demo_info.aux_switch = dp_data->value.dp_bool;
                app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_ON);
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (dp_data->value.dp_bool == 1)
            {
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE,sg_demo_info.aux_bright);
                upload_device_value_status(DPID_TEMP_VALUE,sg_demo_info.white_temp);
                upload_device_bool_status(DPID_SWITCH, 1);
                sg_demo_info.switch_status = 1;
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                // if (aux_ww >= 9999)
                // {
                //     aux_ww = 9999;
                //     aux_cw = 1;
                // }
                // if (aux_cw >= 9999)
                // {
                //     aux_cw = 9999;
                //     aux_ww = 1;
                // }
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            else
            {
                sg_demo_info.last_light_memory = 3;
                aux_off_time = tal_system_get_millisecond();
                if(aux_off_time - white_off_time <= 5000)
                    sg_demo_info.last_light_memory = 1;
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {0, 0};
                pwm_gradual_duty_set_multi(2, channels, duties);
                upload_device_bool_status(AUX_SWITCH, 0);
            }
            if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0))
            {
                sg_demo_info.switch_status = 0;
                upload_device_bool_status(DPID_SWITCH, 0);
            }
            break;
        case DPID_RESET:
        {
            uint8_t night[6] = {0x00, 0x32, 0x17, 0x32, 0x06, 0x00};
            if (dp_data->value.dp_bool == 1)
            {
                gradual_time_ms = 1000;
                // 第一阶段：设置高亮度
                TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
                pwm_gradual_duty_set_multi(4, channels1, duties1);

                // 等待渐变完成
                pwm_gradual_wait_current_complete(1000); // 等待1秒

                // 第二阶段：设置为低亮度
                TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties2[] = {1, 1, 1, 1};
                pwm_gradual_duty_set_multi(4, channels2, duties2);

                pwm_gradual_wait_current_complete(1000); // 等待1秒
                // todo 重置
                extern uint8_t night[6];
                extern uint8_t custom_status[6];
                extern uint8_t sleep_init[14];
                extern uint8_t wake_init[11];
                extern uint8_t switch_change_gear[21];
                extern uint8_t power_outage[10];
                // extern
                sg_demo_info.cnt = 0;
                sg_demo_info.cnt1 = 0;
                sg_demo_info.checksum = 0xa5;
                sg_demo_info.switch_status = 1;
                sg_demo_info.default_state = 0;
                sg_demo_info.white_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.last_light_memory = 1; /* 重置后默认主+辅记忆 */
                sg_demo_info.white_temp = 55;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.change_light_status = 1;
                sg_demo_info.rhythm_switch = 0;
                sg_demo_info.first_network = 0;
                sg_demo_info.beep_switch = 1;
                sg_demo_info.hand_sweep_switch = 1;
                // sg_demo_info.calibration = 0;
                memset(sg_demo_info.custom_status, 0, 6);
                memset(sg_demo_info.sleep_init, 0, 14);
                memset(sg_demo_info.wake_init, 0, 11);
                memset(sg_demo_info.rhythm_sunlight, 0, 66);
                memset(sg_demo_info.work_mode_value, 0, 84);
                memset(sg_demo_info.switch_change_gear, 0, 21);
                memset(sg_demo_info.night, 0, 6);
                memset(sg_demo_info.power_outage, 0, 10);
                memcpy(sg_demo_info.night, night, 6);
                memcpy(sg_demo_info.custom_status, custom_status, 6);
                memcpy(sg_demo_info.sleep_init, sleep_init, 14);
                memcpy(sg_demo_info.wake_init, wake_init, 11);
                memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);
                memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
                memcpy(sg_demo_info.power_outage, power_outage, 10);
                if (sg_demo_info.change_light_status == 0)
                    gradual_time_ms = 20;
                else
                    gradual_time_ms = 1600;
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t  aux_temp1 = sg_demo_info.white_temp;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {ww, cw, aux_ww, aux_ww};
                pwm_gradual_duty_set_multi(4, channels, duties);
                upload_device_all_status();
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 55);

                sg_rhythm_interrupted = true;
            }
            break;
        }
        break;
        case DPID_BEEP_SWITCH:
        {
            uint8_t sw = dp_data->value.dp_bool;
            if (sw)
            {
                sg_demo_info.beep_switch = 1;
            }
            else
            {
                sg_demo_info.beep_switch = 0;
            }
        }
        break;
        case HAND_SWEEP_SWITCH:
        {
            uint8_t sw = dp_data->value.dp_bool;
            if (sw)
            {
                sg_demo_info.hand_sweep_switch = 1;
            }
            else
            {
                sg_demo_info.hand_sweep_switch = 0;
            }
        }
        break;

        default:
            break;
        }
    }
    device_config_save();
    TUYA_CALL_ERR_LOG(dev_report_dp_json_async(NULL, dp_data_arr, dp_cnt));

    return;
}

/**
 * @brief Output the received raw type data and reply to the cloud
 *
 * @param[in] dpid: received raw dp id
 * @param[in] p_data: raw dp data
 * @param[in] data_len: the length of data
 *
 * @return none
 */
TIMER_ID sg_preview_timer = 0;
uint8_t preview_flag = 0;//预览标志位
static VOID __sg_previce_cb(PVOID_T pTimerArg)
{
    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;
    preview_flag = 0;
    if(sg_demo_info.white_switch == 1){
        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
        uint8_t white_temp1 = sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
        uint16_t cw = white_bright1 * 100 - ww;
        light_pwm_clamp_mix(&ww, &cw);
        // if (ww >= 9999)
        // {
        //     ww = 9999;
        //     cw = 1;
        // }
        // if (cw >= 9999)
        // {
        //     cw = 9999;
        //     ww = 1;
        // }
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = ww;
        
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = cw;
    }else{
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = 0;
        
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = 0;
    }
    if(sg_demo_info.aux_switch == 1){
        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
        uint8_t aux_temp1 = sg_demo_info.white_temp;
        if (aux_bright1 == 0)
            aux_bright1 = 1;
        uint16_t aux_ww = aux_bright1 * aux_temp1;
        uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
        light_pwm_clamp_mix(&aux_ww, &aux_cw);
        // if (aux_ww >= 9999)
        // {
        //     aux_ww = 9999;
        //     aux_cw = 1;
        // }
        // if (aux_cw >= 9999)
        // {
        //     aux_cw = 9999;
        //     aux_ww = 1;
        // }
        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = aux_ww;
        
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = aux_cw;
    }else{
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

VOID dp_raw_process(UINT8_T dpid, CONST UINT8_T *p_data, UINT_T data_len)
{
    OPERATE_RET rt = OPRT_OK;
    UINT_T i = 0;
    extern bool sg_rhythm_interrupted;
    extern TM_RHYTHM_INFO_T sg_rhythm_info;
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    switch (dpid)
    {
    case SLEEP_MODE:
        if (p_data && data_len >= 14)
        {
            memcpy(sg_demo_info.sleep_init, p_data, data_len);
            if (app_light_tm_sleep_dp_to_info((UCHAR_T *)p_data, data_len) == OPRT_OK)
            {
                if (sg_demo_info.sleep_init[0] == 0x01)
                {
                    app_light_schedule_mark_configured(SCHEDULE_EVT_SLEEP);
                    app_light_start_sleep_timer();
                    if (sg_demo_info.sleep_init[10] == 0) {
                        upload_device_enum_status(DPID_WORK_MODE, 16);
                    }
                }
                else
                {
                    app_light_stop_sleep_timer();
                }
            }
        }
        dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                               sg_demo_info.sleep_init, data_len, 5);
        break;
    case WAKEUP_MODE:
    {
        if (p_data && data_len >= 11)
        {
            memcpy(sg_demo_info.wake_init, p_data, data_len);
            if (app_light_tm_wake_dp_to_info((UCHAR_T *)p_data, data_len) == OPRT_OK)
            {
                if (sg_demo_info.wake_init[0] == 0x01)
                {
                    app_light_schedule_mark_configured(SCHEDULE_EVT_WAKE);
                    if (sg_demo_info.wake_init[7] == 0) {
                        upload_device_enum_status(DPID_WORK_MODE, 16);
                    }
                    app_light_start_wake_timer();
                }
                else
                    app_light_stop_wake_timer();
            }
        }
        dev_report_dp_raw_sync(NULL, WAKEUP_MODE,
                               sg_demo_info.wake_init, data_len, 5);
    }

    break;
    // case SWITCH_CHANGE_GEAR:
    //     memcpy(sg_demo_info.switch_change_gear, p_data, data_len);
    //     dev_report_dp_raw_sync(NULL, SWITCH_CHANGE_GEAR,
    //                            sg_demo_info.switch_change_gear, data_len, 5);
    //     break;
    case RHYTHM_MODE:
    {
        extern TM_RHYTHM_INFO_T sg_rhythm_info;
        extern bool sg_rhythm_interrupted;
        
        if (!p_data || data_len < 66)
            break; // 至少 66 字节

        memcpy(sg_demo_info.rhythm_sunlight, p_data, data_len);

        uint8_t rhythm_mode = p_data[0];     // 0x01 节律模式
        uint8_t active_day_mask = p_data[1]; // 0x02 生效日掩码

        memset(&sg_rhythm_info.cfg, 0, sizeof(sg_rhythm_info.cfg));
        sg_rhythm_info.cfg.mode = rhythm_mode;
        sg_rhythm_info.cfg.active_day_mask = active_day_mask;

        for (int i = 0; i < RHYTHM_NODE_COUNT; i++)
        {
            uint8_t *node_data = &p_data[2 + i * 8];
            RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[i];
            node->node_enable = node_data[0];   // 节点开关
            node->hour = node_data[1];          // 目标小时
            node->minute = node_data[2];        // 目标分钟
            node->per_bright = node_data[3];    // 亮度
            node->per_temper = node_data[4];    // 色温
            node->fade_type = node_data[5];     // 变光效果
            node->auto_light_on = node_data[6]; // 自动开灯
            node->time_type = node_data[7];     // 时间类型
        }

        // 更新节点时间（基于日出日落）
        extern uint8_t sunrise_hour, sunrise_min, sunset_hour, sunset_min;
        if (sunrise_hour > 0)
        {
            for (int i = 0; i < RHYTHM_NODE_COUNT; i++)
            {
                update_node_time(i, sunrise_hour, sunrise_min, sunset_hour, sunset_min);
            }
        }

        if (sg_demo_info.rhythm_switch == 1)
        {
            upload_device_bool_status(RHYTHM_STATUS, 0);
            app_light_stop_today_rhythm_timer(); // 停止节律
            // 设置节律定时器
            app_light_set_rhythm_timer(&sg_rhythm_info.cfg);

            // 检查是否有主灯或辅灯开启
            bool main_light_on = (sg_demo_info.white_switch || sg_demo_info.aux_switch);

            if (main_light_on)
            {
                // 同步到当前节律状态
                app_light_tm_rhythm_syn();
                app_light_resume_today_rhythm_timer(); // 重新启动节律
            }
            else
            {
                // 灯没开，检查是否需要自动开灯（DP触发，只检查节点时间）
                if (check_and_perform_auto_light_on()) {
                    // 自动开灯成功，启动节律
                    upload_device_bool_status(RHYTHM_STATUS, 0);
                    app_light_tm_rhythm_syn();
                    app_light_resume_today_rhythm_timer();
                } else {
                    // 灯没开，且不在自动开灯节点，节律处于等待状态
                    sg_rhythm_interrupted = false;
                }
            }
        }

        dev_report_dp_raw_sync(NULL, RHYTHM_MODE,
                            sg_demo_info.rhythm_sunlight, data_len, 5);
    }
    break;
    case DPID_SCENE_DATA:
        if (p_data && data_len > 0) {
            UINT_T copy_len = (data_len > 84) ? 84 : data_len;
            memset(sg_demo_info.work_mode_value, 0, 84);
            memcpy(sg_demo_info.work_mode_value, p_data, copy_len);
            app_mode_save_raw(sg_demo_info.work_mode_value, 84);
            dev_report_dp_raw_sync(NULL, DPID_SCENE_DATA,
                                   sg_demo_info.work_mode_value, 84, 5);
        }
        break;
    // case POWER_OUTAGE:
    //     memcpy(sg_demo_info.power_outage,p_data,data_len);
    //     app_power_disturb_update(p_data,data_len);
    //     dev_report_dp_raw_sync(NULL,POWER_OUTAGE,sg_demo_info.power_outage,data_len,5);
    // break;
    case SMART_NIGHT_LIGHT:
        memcpy(sg_demo_info.night, p_data, data_len);
        app_nightlight_update(p_data, data_len);
        dev_report_dp_raw_sync(NULL, SMART_NIGHT_LIGHT,
                               sg_demo_info.night, data_len, 5);
        break;
    case CUSTOM_STATUS:
        sg_demo_info.default_state = p_data[0];
        memcpy(sg_demo_info.custom_status, p_data, data_len);
        dev_report_dp_raw_sync(NULL, CUSTOM_STATUS,
                               sg_demo_info.custom_status, data_len, 5);
        break;
    case NIGHT_PREVIEW:{
        TUYA_PWM_NUM_E all_channels[5];
        UINT32_T all_duties[5];
        UINT8_T channel_count = 0;
        uint16_t ww;
        uint16_t cw;
        uint16_t aux_cw;
        uint16_t aux_ww;
        if(p_data[1] != 0){
            uint16_t white_bright1 = 5 + (p_data[1] - 1) * 95 / 99;
            uint8_t white_temp1 = p_data[3];
                        ww = white_bright1 * white_temp1;
            cw = white_bright1 * 100 - ww;
           light_pwm_clamp_mix(&ww, &cw);
            // if (ww >= 9999)
            // {
            //     ww = 9999;
            //     cw = 1;
            // }
            // if (cw >= 9999)
            // {
            //     cw = 9999;
            //     ww = 1;
            // }
        }
        else
        {
            cw = 0;
            ww = 0;
        }
        if(p_data[2] != 0){
            uint16_t aux_bright1 = 5 + (p_data[2] - 1) * 95 / 99;
            uint8_t aux_temp1 = p_data[3];
                        aux_ww = aux_bright1 * aux_temp1;
            aux_cw = aux_bright1 * 100 - aux_ww;
           light_pwm_clamp_mix(&aux_ww, &aux_cw);
            // if (aux_ww >= 9999)
            // {
            //     aux_ww = 9999;
            //     aux_cw = 1;
            // }
            // if (aux_cw >= 9999)
            // {
            //     aux_cw = 9999;
            //     aux_ww = 1;
            // }
        }
        else
        {
            aux_cw = 0;
            aux_ww = 0;
        }
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = ww;
        
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = cw;

        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = aux_ww;
        
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = aux_cw;
        preview_flag = 0;
        if (channel_count > 0)
        {
            pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
        }
        preview_flag = 1;
        tal_sw_timer_start(sg_preview_timer, 5000, TAL_TIMER_ONCE);
    }
    break;
    default:
        break;
    }
    device_config_save();
    return;
}

/**
 * @brief   upload swtich status
 *
 * @param[in] : state   the state of switch
 *
 * @return none
 */
VOID upload_device_bool_status(BYTE_T dpid, BOOL_T state)
{
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S obj_dp = {0};

    memset((UCHAR_T *)&obj_dp, 0, SIZEOF(obj_dp));

    obj_dp.dpid = dpid;
    obj_dp.type = PROP_BOOL;
    obj_dp.value.dp_bool = state;

    TUYA_CALL_ERR_LOG(dev_report_dp_json_async(NULL, &obj_dp, 1));
    device_config_save();
    return;
}

VOID upload_device_value_status(BYTE_T dpid, INT_T state)
{
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S obj_dp = {0};

    memset((UCHAR_T *)&obj_dp, 0, SIZEOF(obj_dp));

    obj_dp.dpid = dpid;
    obj_dp.type = PROP_VALUE;
    obj_dp.value.dp_value = state;

    TUYA_CALL_ERR_LOG(dev_report_dp_json_async(NULL, &obj_dp, 1));
    device_config_save();
    return;
}

VOID upload_device_enum_status(BYTE_T dpid, UINT_T state)
{
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S obj_dp = {0};

    memset((UCHAR_T *)&obj_dp, 0, SIZEOF(obj_dp));

    obj_dp.dpid = dpid;
    obj_dp.type = PROP_ENUM;
    obj_dp.value.dp_enum = state;

    TUYA_CALL_ERR_LOG(dev_report_dp_json_async(NULL, &obj_dp, 1));
    device_config_save();
    return;
}

// 修正时间，保证 hour 在 0~23，min 在 0~59
static void fix_time(int8_t *hour, int8_t *min)
{
    while (*min < 0)
    {
        *min += 60;
        (*hour)--;
    }
    while (*min >= 60)
    {
        *min -= 60;
        (*hour)++;
    }
    while (*hour < 0)
    {
        *hour += 24;
    }
    while (*hour >= 24)
    {
        *hour -= 24;
    }
}
typedef struct
{
    int8_t hour;
    int8_t min;
} time_point_t;

static void calc_rhythm_nodes(uint8_t sunrise_hour, uint8_t sunrise_min,
                              uint8_t sunset_hour, uint8_t sunset_min,
                              time_point_t nodes[8])
{
    // 日出时间
    nodes[0].hour = sunrise_hour;
    nodes[0].min = sunrise_min; // 日出前
    fix_time(&nodes[0].hour, &nodes[0].min);

    nodes[1].hour = sunrise_hour + 1; // 日出后
    nodes[1].min = sunrise_min;
    fix_time(&nodes[1].hour, &nodes[1].min);

    // 正午 = 日出 + (日落 - 日出) / 2
    int sunrise_total = sunrise_hour * 60 + sunrise_min;
    int sunset_total = sunset_hour * 60 + sunset_min;
    int noon_total = sunrise_total + (sunset_total - sunrise_total) / 2;
    nodes[2].hour = noon_total / 60;
    nodes[2].min = noon_total % 60;

    nodes[3].hour = sunset_hour - 2; // 日落前
    nodes[3].min = sunset_min;
    fix_time(&nodes[3].hour, &nodes[3].min);

    nodes[4].hour = sunset_hour;
    nodes[4].min = sunset_min; // 日落后
    fix_time(&nodes[4].hour, &nodes[4].min);

    nodes[5].hour = 23; // 睡觉固定 23:00
    nodes[5].min = 0;
}

static void build_rhythm_payload(uint8_t *buf,
                                 const time_point_t nodes[8])
{
    uint8_t idx = 0;
    buf[idx++] = 0x7F; // 周一到周日生效 (01111111b)
    buf[idx++] = 0x06; // 节点数 = 6

    // 节点1 日出前 (亮度20%，色温0%，30分钟渐变)
    buf[idx++] = 0x01;
    buf[idx++] = nodes[0].hour;
    buf[idx++] = nodes[0].min;
    buf[idx++] = 0x14; // 20%
    buf[idx++] = 0x00; // 0%
    buf[idx++] = 0x06; // 30分钟渐变
    buf[idx++] = 0x00; // 开始状态
    buf[idx++] = 0x00; // 结束状态

    // 节点2 日出后 (80%，50%，1分钟渐变)
    buf[idx++] = 0x01;
    buf[idx++] = nodes[1].hour;
    buf[idx++] = nodes[1].min;
    buf[idx++] = 0x50; // 80%
    buf[idx++] = 0x32; // 50%
    buf[idx++] = 0x02; // 1分钟渐变
    buf[idx++] = 0x00;
    buf[idx++] = 0x00;

    // 节点3 正午 (100%，100%，1分钟渐变)
    buf[idx++] = 0x01;
    buf[idx++] = nodes[2].hour;
    buf[idx++] = nodes[2].min;
    buf[idx++] = 0x64; // 100%
    buf[idx++] = 0x64; // 100%
    buf[idx++] = 0x02;
    buf[idx++] = 0x00;
    buf[idx++] = 0x00;

    // 节点4 日落前 (90%，50%，1分钟渐变)
    buf[idx++] = 0x01;
    buf[idx++] = nodes[3].hour;
    buf[idx++] = nodes[3].min;
    buf[idx++] = 0x5A; // 90%
    buf[idx++] = 0x32; // 50%
    buf[idx++] = 0x02;
    buf[idx++] = 0x00;
    buf[idx++] = 0x00;

    // 节点5 日落后 (80%，40%，1分钟渐变)
    buf[idx++] = 0x01;
    buf[idx++] = nodes[4].hour;
    buf[idx++] = nodes[4].min;
    buf[idx++] = 0x50; // 80%
    buf[idx++] = 0x28; // 40%
    buf[idx++] = 0x02;
    buf[idx++] = 0x00;
    buf[idx++] = 0x00;

    // 节点6 睡觉 (23:00, 1%，0%，1分钟渐变)
    buf[idx++] = 0x01;
    buf[idx++] = nodes[5].hour;
    buf[idx++] = nodes[5].min;
    buf[idx++] = 0x01; // 1%
    buf[idx++] = 0x00; // 0%
    buf[idx++] = 0x02;
    buf[idx++] = 0x00;
    buf[idx++] = 0x00;
}

/**
 * @brief report all dp to the cloud
 *
 * @param[in] none:
 *
 * @return none
 */
VOID upload_device_all_status(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    extern uint8_t sunrise_hour;
    extern uint8_t sunrise_min;
    extern uint8_t sunset_hour;
    extern uint8_t sunset_min;
    TY_OBJ_DP_S *p_all_obj_dp = NULL;
    TY_OBJ_DP_S *p_obj_dp = NULL;

    p_all_obj_dp = (TY_OBJ_DP_S *)tal_malloc(OBJ_DP_NUM_MAX * SIZEOF(TY_OBJ_DP_S));
    if (NULL == p_all_obj_dp)
    {
        return;
    }
    memset((UCHAR_T *)p_all_obj_dp, 0, OBJ_DP_NUM_MAX * SIZEOF(TY_OBJ_DP_S));

    // TAL_PR_NOTICE("upload_device_all_status\r\n");

    /* 只上报当前运行态。初始/自定义初始仅在上电首次开灯应用，切档后不可再覆盖。 */
    sg_demo_info.switch_status =
        (sg_demo_info.white_switch || sg_demo_info.aux_switch) ? 1 : 0;

    p_obj_dp = p_all_obj_dp;
    
    p_obj_dp->dpid = DPID_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.switch_status;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_WORK_MODE;
    p_obj_dp->type = PROP_ENUM;
    p_obj_dp->value.dp_enum = 16;

    // value type data
    p_obj_dp++;
    p_obj_dp->dpid = DPID_WHITE_BRIGHT;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_bright;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_TEMP_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_temp;

    p_obj_dp++;
    p_obj_dp->dpid = COUNTDOWN;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = 0;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_AUX_BRIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.aux_bright;

    p_obj_dp++;
    p_obj_dp->dpid = LIGHT_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.white_switch;

    p_obj_dp++;
    p_obj_dp->dpid = AUX_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.aux_switch;

    p_obj_dp++;
    p_obj_dp->dpid = CHANGE_LIGHT_STATUS;
    p_obj_dp->type = PROP_ENUM;
    p_obj_dp->value.dp_enum = sg_demo_info.change_light_status;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_RESET;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = 0;

    p_obj_dp++;
    p_obj_dp->dpid = RHYTHM_STATUS;
    p_obj_dp->type = PROP_BOOL;
    {
        extern bool sg_rhythm_interrupted;
        p_obj_dp->value.dp_bool = sg_rhythm_interrupted ? 1 : 0;
    }

    p_obj_dp++;
    p_obj_dp->dpid = RHYTHM_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.rhythm_switch;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_BEEP_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.beep_switch;

    p_obj_dp++;
    p_obj_dp->dpid = HAND_SWEEP_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.hand_sweep_switch;

    TUYA_CALL_ERR_LOG(dev_report_dp_json_async(NULL, p_all_obj_dp, OBJ_DP_NUM_MAX));

    tal_free(p_all_obj_dp);
    p_all_obj_dp = NULL;
    p_obj_dp = NULL;

    dev_report_dp_raw_sync(NULL, CUSTOM_STATUS, sg_demo_info.custom_status, 6, 5);

    dev_report_dp_raw_sync(NULL, SMART_NIGHT_LIGHT, sg_demo_info.night, 6, 5);

    dev_report_dp_raw_sync(NULL, DPID_SCENE_DATA, sg_demo_info.work_mode_value, 84, 5);

    dev_report_dp_raw_sync(NULL, RHYTHM_MODE, sg_demo_info.rhythm_sunlight, 66, 5);

    dev_report_dp_raw_sync(NULL, SWITCH_CHANGE_GEAR, sg_demo_info.switch_change_gear, 21, 5);

    dev_report_dp_raw_sync(NULL, WAKEUP_MODE, sg_demo_info.wake_init, 11, 5);

    dev_report_dp_raw_sync(NULL, SLEEP_MODE, sg_demo_info.sleep_init, 14, 5);

    dev_report_dp_raw_sync(NULL,POWER_OUTAGE,sg_demo_info.power_outage,10,5);

    return;
}

STATIC VOID_T __upload_device_raw_dp(VOID_T *data)
{
    OPERATE_RET rt = OPRT_OK;
}

/**
 * @brief respone all dp status when receive query from cloud or app
 *
 * @param[in] none:
 *
 * @return none
 */
VOID_T respone_device_all_status(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S *p_all_obj_dp = NULL;
    TY_OBJ_DP_S *p_obj_dp = NULL;

    p_all_obj_dp = (TY_OBJ_DP_S *)tal_malloc(OBJ_DP_NUM_MAX * SIZEOF(TY_OBJ_DP_S));
    if (NULL == p_all_obj_dp)
    {
        return;
    }
    memset((UCHAR_T *)p_all_obj_dp, 0, OBJ_DP_NUM_MAX * SIZEOF(TY_OBJ_DP_S));

    /* object type data */
    // bool type data
    p_obj_dp = p_all_obj_dp;
    p_obj_dp->dpid = DPID_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.switch_status;

    // value type data
    p_obj_dp++;
    p_obj_dp->dpid = DPID_WHITE_BRIGHT;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_bright;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_TEMP_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_temp;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_AUX_BRIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.aux_bright;

    p_obj_dp++;
    p_obj_dp->dpid = LIGHT_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.white_switch;

    p_obj_dp++;
    p_obj_dp->dpid = AUX_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.aux_switch;

    TUYA_CALL_ERR_LOG(dev_query_dp_json_async(NULL, p_all_obj_dp, OBJ_DP_NUM_MAX));

    tal_free(p_all_obj_dp);
    p_all_obj_dp = NULL;
    p_obj_dp = NULL;

    /* put raw type data sync report int work queue*/
    tal_workq_schedule(WORKQ_SYSTEM, __upload_device_raw_dp, NULL);
}

// #define COUNT_PARTINTION_SIZE   (2 * 1024)  // 2KB分区

// static UINT_T sg_demo_start_addr = 0x1EF000;  // 默认起始地址

// typedef struct {
//     UINT_T  write_addr;
//     UINT_T  read_addr;
//     UCHAR_T init_flag;
// } DEMO_FLASH_OP_T;

// static DEMO_FLASH_OP_T sg_demo_flash_op = {0};
// #define DEMO_PARTITION_SIZE    (2 * 1024)  // 2KB分区
// /**
//  * @brief 计算结构体校验和（不包含时间戳）
//  */
// static uint8_t __demo_calc_checksum(DEMO_INFO_T *p_info)
// {
//     if (p_info == NULL) {
//         return 0;
//     }
    
//     uint8_t *p_data = (uint8_t *)p_info;
//     uint8_t checksum = 0;
//     size_t timestamp_offset = offsetof(DEMO_INFO_T, time_stamp);
//     size_t checksum_offset = offsetof(DEMO_INFO_T, checksum);
    
//     // 计算除了checksum和时间戳字段外的所有字节
//     for (size_t i = 0; i < sizeof(DEMO_INFO_T); i++) {
//         // 跳过checksum字段
//         if (i == checksum_offset) {
//             continue;
//         }
//         // 跳过time_stamp字段（uint32_t，4个字节）
//         if (i >= timestamp_offset && i < timestamp_offset + sizeof(uint32_t)) {
//             continue;
//         }
//         checksum += p_data[i];
//     }
    
//     return checksum;
// }

// /**
//  * @brief 验证数据有效性
//  */
// static BOOL_T __demo_is_valid_data(DEMO_INFO_T *p_info)
// {
//     if (p_info == NULL) {
//         return FALSE;
//     }
    
//     // 校验和验证（不包含时间戳）
//     uint8_t calc_checksum = __demo_calc_checksum(p_info);
//     if (p_info->checksum != calc_checksum) {
//         return FALSE;
//     }
    
//     // 时间戳必须大于0且小于最大值
//     if (p_info->time_stamp == 0 || p_info->time_stamp == 0xFFFFFFFF) {
//         return FALSE;
//     }
    
//     return TRUE;
// }

// /**
//  * @brief 获取下一个递增时间戳
//  */
// static uint32_t __demo_get_next_timestamp(void)
// {
//     static uint32_t current_timestamp = 0;
    
//     if (current_timestamp == 0) {
//         // 第一次获取，从已保存的数据中读取
//         if (sg_demo_info.time_stamp > 0 && sg_demo_info.time_stamp != 0xFFFFFFFF) {
//             current_timestamp = sg_demo_info.time_stamp;
//         } else {
//             current_timestamp = 1;
//         }
//     }
    
//     // 递增
//     current_timestamp++;
    
//     // 防止溢出，超过0xFFFFFFF0后重置为1
//     if (current_timestamp > 0xFFFFFFF0) {
//         current_timestamp = 1;
//     }
    
//     return current_timestamp;
// }

// /**
//  * @brief 初始化flash存储
//  */
// static void __demo_flash_init(void)
// {
//     UINT_T start_addr = sg_demo_start_addr;
//     DEMO_INFO_T demo_info_tmp = {0};
//     UINT_T find_count = 0;
//     UCHAR_T error_count = 0;
//     UCHAR_T block_num = 0;
//     UINT_T block_start_addr = 0;
//     UINT_T latest_valid_addr = sg_demo_start_addr;
//     uint32_t latest_timestamp = 0;
//     BOOL_T found_any_valid = FALSE;

//     tal_sw_timer_create(__sg_previce_cb, NULL, &sg_preview_timer);
//     if (sg_demo_flash_op.init_flag) {
//         return;
//     }

//     if (0 == sg_demo_start_addr) {
//         return;
//     }
    
//     // 搜索两个块
//     for (block_num = 0; block_num < 2; block_num++) {
//         find_count = 0;
//         block_start_addr = sg_demo_start_addr + (DEMO_PARTITION_SIZE / 2) * block_num;
//         start_addr = block_start_addr;
        
//         for (;;) {
//             // 检查是否超出当前块范围
//             if (start_addr >= (block_start_addr + (DEMO_PARTITION_SIZE / 2))) {
//                 break;
//             }

//             tkl_flash_read(start_addr, (UCHAR_T *)&demo_info_tmp, sizeof(DEMO_INFO_T));

//             // 通过校验和验证数据有效性
//             if (__demo_is_valid_data(&demo_info_tmp)) {
//                 found_any_valid = TRUE;
                
//                 // 记录时间戳最大的有效数据
//                 if (demo_info_tmp.time_stamp > latest_timestamp) {
//                     latest_timestamp = demo_info_tmp.time_stamp;
//                     latest_valid_addr = start_addr;
//                 }
                
//                 start_addr += sizeof(DEMO_INFO_T);
//                 find_count++;

//                 // 查找超时处理
//                 if (find_count >= ((DEMO_PARTITION_SIZE / 2) / sizeof(DEMO_INFO_T))) {
//                     error_count++;
//                     break;
//                 }
//             } else {
//                 // 找到第一个无效数据的位置
//                 break;
//             }
//         }
//     }

//     if (found_any_valid && latest_timestamp > 0) {
//         // 找到有效数据
//         tkl_flash_read(latest_valid_addr, (UCHAR_T *)&sg_demo_info, sizeof(DEMO_INFO_T));
//         start_addr = latest_valid_addr;
//     } else {
//         // 如果没有找到有效数据，擦除整个分区
//         tkl_flash_erase(sg_demo_start_addr, DEMO_PARTITION_SIZE);
//         start_addr = sg_demo_start_addr;
        
//         // 设置默认值
//         memset(&sg_demo_info, 0, sizeof(DEMO_INFO_T));
//         sg_demo_info.time_stamp = 1;
//         sg_demo_info.checksum = __demo_calc_checksum(&sg_demo_info);
//     }

//     sg_demo_flash_op.init_flag = 1;
//     sg_demo_flash_op.write_addr = start_addr;
//     sg_demo_flash_op.read_addr = start_addr;
// }

// // 添加互斥保护
// static UCHAR_T sg_flash_saving = 0;

// /**
//  * @brief 原子操作：检查并准备写入地址
//  */
// static OPERATE_RET __demo_prepare_write_addr_atomic(UINT_T write_addr)
// {
//     UINT_T start_addr = sg_demo_start_addr;
    
//     // 确保地址对齐
//     if (write_addr < start_addr || write_addr >= start_addr + DEMO_PARTITION_SIZE) {
//         return OPRT_INVALID_PARM;
//     }
    
//     // 检查是否需要擦除
//     BOOL_T need_erase = FALSE;
//     uint8_t test_data[4] = {0};
    
//     // 读取前4个字节检查是否需要擦除
//     for (int i = 0; i < 4; i++) {
//         uint8_t test_byte = 0;
//         OPERATE_RET ret = tkl_flash_read(write_addr + i, &test_byte, 1);
//         if (ret != OPRT_OK) {
//             return ret;
//         }
//         if (test_byte != 0xFF) {
//             need_erase = TRUE;
//             break;
//         }
//     }
    
//     if (need_erase) {
//         // 确定要擦除的块
//         UINT_T block_start = 0;
//         if (write_addr < start_addr + DEMO_PARTITION_SIZE / 2) {
//             block_start = start_addr;  // 块A
//         } else {
//             block_start = start_addr + DEMO_PARTITION_SIZE / 2;  // 块B
//         }
        
//         // TAL_PR_NOTICE("Erasing block at 0x%x (for write to 0x%x)", block_start, write_addr);
        
//         // 擦除整个块
//         OPERATE_RET ret = tkl_flash_erase(block_start, DEMO_PARTITION_SIZE / 2);
//         if (ret != OPRT_OK) {
//             // TAL_PR_NOTICE("Flash erase failed at 0x%x: %d", block_start, ret);
//             return ret;
//         }
        
//         // 验证擦除结果
//         for (int i = 0; i < 4; i++) {
//             uint8_t test_byte = 0;
//             ret = tkl_flash_read(block_start + i, &test_byte, 1);
//             if (ret != OPRT_OK || test_byte != 0xFF) {
//                 // TAL_PR_NOTICE("Verify erase failed at 0x%x: read=0x%x", block_start + i, test_byte);
//                 return OPRT_COM_ERROR;
//             }
//         }
        
//         // TAL_PR_NOTICE("Block erased successfully at 0x%x", block_start);
//     }
    
//     return OPRT_OK;
// }
// /**
//  * @brief 原子操作：写入并验证
//  */
// static OPERATE_RET __demo_write_and_verify_atomic(UINT_T addr, DEMO_INFO_T *p_info)
// {
//     if (p_info == NULL) {
//         return OPRT_INVALID_PARM;
//     }
    
//     // 检查地址对齐 - 需要4字节对齐
//     if ((addr % 4) != 0) {
//         // 对齐地址到4字节边界
//         addr = (addr / 4) * 4;
//         // TAL_PR_NOTICE("Address realigned from 0x%x to 0x%x", sg_demo_flash_op.write_addr, addr);
//     }
    
//     // 确保地址不越界
//     if (addr < sg_demo_start_addr || addr >= sg_demo_start_addr + DEMO_PARTITION_SIZE) {
//         // TAL_PR_NOTICE("Address out of range: 0x%x", addr);
//         addr = sg_demo_start_addr;
//     }
    
//     // 1. 保存原始校验和
//     uint8_t original_checksum = p_info->checksum;
//     p_info->checksum = 0;
    
//     // 2. 计算新的校验和
//     p_info->checksum = __demo_calc_checksum(p_info);
    
//     // TAL_PR_NOTICE("Writing to 0x%x, checksum=0x%x, timestamp=%u, struct_size=%d", addr, p_info->checksum, p_info->time_stamp, sizeof(DEMO_INFO_T));
    
//     // 3. 写入数据
//     OPERATE_RET ret = tkl_flash_write(addr, (UCHAR_T *)p_info, sizeof(DEMO_INFO_T));
//     if (ret != OPRT_OK) {
//         // TAL_PR_NOTICE("Flash write failed at 0x%x: %d", addr, ret);
//         p_info->checksum = original_checksum;
//         return ret;
//     }
    
//     // 4. 等待写入完成
//     tkl_system_sleep(2);
    
//     // 5. 验证写入
//     DEMO_INFO_T verify_info = {0};
//     ret = tkl_flash_read(addr, (UCHAR_T *)&verify_info, sizeof(DEMO_INFO_T));
//     if (ret != OPRT_OK) {
//         // TAL_PR_NOTICE("Flash read failed for verify at 0x%x: %d", addr, ret);
//         p_info->checksum = original_checksum;
//         return ret;
//     }
    
//     // 验证校验和
//     if (!__demo_is_valid_data(&verify_info)) {
//         // TAL_PR_NOTICE("Checksum verify failed: stored=0x%x, calc=0x%x", verify_info.checksum, __demo_calc_checksum(&verify_info));
//         p_info->checksum = original_checksum;
//         return OPRT_COM_ERROR;
//     }
    
//     // 验证时间戳
//     if (verify_info.time_stamp != p_info->time_stamp) {
//         // TAL_PR_NOTICE("Timestamp mismatch: stored=%u, expected=%u", verify_info.time_stamp, p_info->time_stamp);
//         p_info->checksum = original_checksum;
//         return OPRT_COM_ERROR;
//     }
    
//     // TAL_PR_NOTICE("Write verify success at 0x%x", addr);
//     return OPRT_OK;
// }
// UCHAR_T save_in_progress = 0;
// STATIC TIMER_ID save_data = NULL; // 保存数据定时器
// STATIC VOID save_data_cb(TIMER_ID timer_id, VOID_T *arg)
// {
//     // 设置保存标志
//     save_in_progress = 1;
//     int result = 0;
//     // TAL_PR_NOTICE("save1\r\n");
//     do {
//         if (!sg_demo_flash_op.init_flag) {
//             __demo_flash_init();
//         }

//         // 更新时间戳（使用递增机制）
//         uint32_t new_timestamp = __demo_get_next_timestamp();
//         sg_demo_info.time_stamp = new_timestamp;
        
//         // TAL_PR_NOTICE("Saving config: time=%u", sg_demo_info.time_stamp);

//         UINT_T write_addr = sg_demo_flash_op.write_addr;
//         UINT_T start_addr = sg_demo_start_addr;
//         UINT_T end_addr = start_addr + DEMO_PARTITION_SIZE;
        
//         // TAL_PR_NOTICE("Current write addr: 0x%x", write_addr);
        
//         // 检查地址有效性
//         if (write_addr < start_addr || write_addr >= end_addr) {
//             // TAL_PR_NOTICE("Invalid write addr: 0x%x, reset to start 0x%x", write_addr, start_addr);
//             write_addr = start_addr;
//             sg_demo_flash_op.write_addr = start_addr;
//         }
        
//         // 确保地址是4字节对齐的
//         if ((write_addr % 4) != 0) {
//             // 对齐到4字节边界
//             write_addr = ((write_addr + 3) / 4) * 4;
//             // TAL_PR_NOTICE("Align write addr to: 0x%x", write_addr);
            
//             // 检查对齐后是否越界
//             if (write_addr + sizeof(DEMO_INFO_T) > end_addr) {
//                 write_addr = start_addr;
//                 // TAL_PR_NOTICE("Aligned addr out of range, reset to start: 0x%x", write_addr);
//             }
//         }
        
//         // 计算下一个写入地址
//         UINT_T next_write_addr = write_addr + sizeof(DEMO_INFO_T);
        
//         // 确保下一个地址也是4字节对齐
//         if ((next_write_addr % 4) != 0) {
//             next_write_addr = ((next_write_addr + 3) / 4) * 4;
//         }
        
//         // 检查是否需要跨块
//         UINT_T block_a_end = start_addr + DEMO_PARTITION_SIZE / 2;
//         BOOL_T will_cross_block = FALSE;
        
//         if (write_addr < block_a_end && next_write_addr >= block_a_end) {
//             will_cross_block = TRUE;
//             //TAL_PR_NOTICE("Will cross from block A to block B: 0x%x -> 0x%x", 
//                         //  write_addr, next_write_addr);
//         } else if (next_write_addr >= end_addr) {
//             //TAL_PR_NOTICE("Will wrap to start address: 0x%x -> 0x%x", 
//                         //  next_write_addr, start_addr);
//         }
        
//         // 如果即将跨块，检查下一个块是否需要擦除
//         if (will_cross_block) {
//             UINT_T block_b_start = block_a_end;
//             BOOL_T need_erase = FALSE;
            
//             // 检查块B的第一个位置
//             for (int i = 0; i < 4; i++) {
//                 uint8_t test_byte = 0;
//                 OPERATE_RET ret = tkl_flash_read(block_b_start + i, &test_byte, 1);
//                 if (ret != OPRT_OK) {
//                     //TAL_PR_NOTICE("Flash read error at 0x%x: %d", block_b_start + i, ret);
//                     need_erase = TRUE;
//                     break;
//                 }
//                 if (test_byte != 0xFF) {
//                     //TAL_PR_NOTICE("Block B not empty at 0x%x: 0x%x", 
//                                 //  block_b_start + i, test_byte);
//                     need_erase = TRUE;
//                     break;
//                 }
//             }
            
//             if (need_erase) {
//                 // TAL_PR_NOTICE("Pre-erasing block B at 0x%x, size=%d", 
//                 //              block_b_start, DEMO_PARTITION_SIZE / 2);
//                 OPERATE_RET ret = tkl_flash_erase(block_b_start, DEMO_PARTITION_SIZE / 2);
//                 if (ret != OPRT_OK) {
//                     //TAL_PR_NOTICE("Pre-erase failed: %d", ret);
//                 } else {
//                     //TAL_PR_NOTICE("Pre-erase successful");
//                 }
//             }
//         }
        
//         // 准备当前写入地址
//         OPERATE_RET ret = __demo_prepare_write_addr_atomic(write_addr);
//         if (ret != OPRT_OK) {
//             //TAL_PR_NOTICE("Prepare write addr failed: %d", ret);
            
//             // 擦除整个区域并重试
//             //TAL_PR_NOTICE("Erasing entire partition (0x%x, %d) and retrying...", 
//                         //  start_addr, DEMO_PARTITION_SIZE);
//             tkl_flash_erase(start_addr, DEMO_PARTITION_SIZE);
            
//             // 验证擦除
//             BOOL_T erase_ok = TRUE;
//             for (int i = 0; i < 4; i++) {
//                 uint8_t test_byte = 0;
//                 if (tkl_flash_read(start_addr + i, &test_byte, 1) != OPRT_OK || test_byte != 0xFF) {
//                     erase_ok = FALSE;
//                     break;
//                 }
//             }
            
//             if (!erase_ok) {
//                 //TAL_PR_NOTICE("Erase verification failed!");
//                 break;
//             }
            
//             write_addr = start_addr;
//             sg_demo_flash_op.write_addr = start_addr;
//             sg_demo_flash_op.read_addr = start_addr;
            
//             //TAL_PR_NOTICE("Retry with write addr: 0x%x", write_addr);
//         }
        
//         // 保存一个副本，避免在写入过程中被修改
//         DEMO_INFO_T save_copy;
//         memcpy(&save_copy, &sg_demo_info, sizeof(DEMO_INFO_T));
        
//         // 写入并验证
//         ret = __demo_write_and_verify_atomic(write_addr, &save_copy);
//         if (ret != OPRT_OK) {
//             //TAL_PR_NOTICE("Write and verify failed: %d", ret);
            
//             // 重试一次
//             //TAL_PR_NOTICE("Retry write...");
            
//             // 先擦除当前块
//             UINT_T current_block_start = 0;
//             if (write_addr < start_addr + DEMO_PARTITION_SIZE / 2) {
//                 current_block_start = start_addr;  // 块A
//             } else {
//                 current_block_start = start_addr + DEMO_PARTITION_SIZE / 2;  // 块B
//             }
            
//             // TAL_PR_NOTICE("Erasing current block at 0x%x", current_block_start);
//             tkl_flash_erase(current_block_start, DEMO_PARTITION_SIZE / 2);
            
//             // 重新写入
//             ret = __demo_write_and_verify_atomic(write_addr, &save_copy);
//             if (ret != OPRT_OK) {
//                 //TAL_PR_NOTICE("Retry also failed: %d", ret);
//                 break;
//             }
//         }
        
//         // 更新到全局变量
//         memcpy(&sg_demo_info, &save_copy, sizeof(DEMO_INFO_T));
        
//         // 更新地址
//         sg_demo_flash_op.read_addr = write_addr;
//         sg_demo_flash_op.write_addr = next_write_addr;
        
//         // 处理回绕
//         if (sg_demo_flash_op.write_addr >= end_addr) {
//             sg_demo_flash_op.write_addr = start_addr;
//         }
        
//         result = sizeof(DEMO_INFO_T);
//         //TAL_PR_NOTICE("Save success, next write addr: 0x%x", sg_demo_flash_op.write_addr);
        
//     } while(0);
    
//     // 清除保存标志
//     save_in_progress = 0;
// }

// /**
//  * @brief 保存配置（线程安全版）
//  */
// int device_config_save(void)
// {
//     if (0 == sg_demo_start_addr) {
//         //TAL_PR_NOTICE("Flash address not set!");
//         return 0;
//     }

//     // 检查是否已经在保存中
//     if (save_in_progress) {
//         //TAL_PR_NOTICE("Save already in progress, skip this save");
//         return 0;
//     }
    
//     tal_sw_timer_start(save_data,5000,TAL_TIMER_ONCE);
    
//     return 1;
// }
// int device_config_save1(void)
// {
//     if (0 == sg_demo_start_addr) {
//         //TAL_PR_NOTICE("Flash address not set!");
//         return 0;
//     }

//     // 检查是否已经在保存中
//     if (save_in_progress) {
//         //TAL_PR_NOTICE("Save already in progress, skip this save");
//         return 0;
//     }
    
//     // 设置保存标志
//     save_in_progress = 1;
//     // tkl_log_output("save\r\n");
//     int result = 0;
    
//     do {
//         if (!sg_demo_flash_op.init_flag) {
//             __demo_flash_init();
//         }

//         // 更新时间戳（使用递增机制）
//         uint32_t new_timestamp = __demo_get_next_timestamp();
//         sg_demo_info.time_stamp = new_timestamp;
        
//         //TAL_PR_NOTICE("Saving config: time=%u", sg_demo_info.time_stamp);

//         UINT_T write_addr = sg_demo_flash_op.write_addr;
//         UINT_T start_addr = sg_demo_start_addr;
//         UINT_T end_addr = start_addr + DEMO_PARTITION_SIZE;
        
//         //TAL_PR_NOTICE("Current write addr: 0x%x", write_addr);
        
//         // 检查地址有效性
//         if (write_addr < start_addr || write_addr >= end_addr) {
//             //TAL_PR_NOTICE("Invalid write addr: 0x%x, reset to start 0x%x", write_addr, start_addr);
//             write_addr = start_addr;
//             sg_demo_flash_op.write_addr = start_addr;
//         }
        
//         // 确保地址是4字节对齐的
//         if ((write_addr % 4) != 0) {
//             // 对齐到4字节边界
//             write_addr = ((write_addr + 3) / 4) * 4;
//             //TAL_PR_NOTICE("Align write addr to: 0x%x", write_addr);
            
//             // 检查对齐后是否越界
//             if (write_addr + sizeof(DEMO_INFO_T) > end_addr) {
//                 write_addr = start_addr;
//                 //TAL_PR_NOTICE("Aligned addr out of range, reset to start: 0x%x", write_addr);
//             }
//         }
        
//         // 计算下一个写入地址
//         UINT_T next_write_addr = write_addr + sizeof(DEMO_INFO_T);
        
//         // 确保下一个地址也是4字节对齐
//         if ((next_write_addr % 4) != 0) {
//             next_write_addr = ((next_write_addr + 3) / 4) * 4;
//         }
        
//         // 检查是否需要跨块
//         UINT_T block_a_end = start_addr + DEMO_PARTITION_SIZE / 2;
//         BOOL_T will_cross_block = FALSE;
        
//         if (write_addr < block_a_end && next_write_addr >= block_a_end) {
//             will_cross_block = TRUE;
//             //TAL_PR_NOTICE("Will cross from block A to block B: 0x%x -> 0x%x", 
//                         //  write_addr, next_write_addr);
//         } else if (next_write_addr >= end_addr) {
//             //TAL_PR_NOTICE("Will wrap to start address: 0x%x -> 0x%x", 
//                         //  next_write_addr, start_addr);
//         }
        
//         // 如果即将跨块，检查下一个块是否需要擦除
//         if (will_cross_block) {
//             UINT_T block_b_start = block_a_end;
//             BOOL_T need_erase = FALSE;
            
//             // 检查块B的第一个位置
//             for (int i = 0; i < 4; i++) {
//                 uint8_t test_byte = 0;
//                 OPERATE_RET ret = tkl_flash_read(block_b_start + i, &test_byte, 1);
//                 if (ret != OPRT_OK) {
//                     //TAL_PR_NOTICE("Flash read error at 0x%x: %d", block_b_start + i, ret);
//                     need_erase = TRUE;
//                     break;
//                 }
//                 if (test_byte != 0xFF) {
//                     //TAL_PR_NOTICE("Block B not empty at 0x%x: 0x%x", 
//                                 //  block_b_start + i, test_byte);
//                     need_erase = TRUE;
//                     break;
//                 }
//             }
            
//             if (need_erase) {
//                 // TAL_PR_NOTICE("Pre-erasing block B at 0x%x, size=%d", 
//                 //              block_b_start, DEMO_PARTITION_SIZE / 2);
//                 OPERATE_RET ret = tkl_flash_erase(block_b_start, DEMO_PARTITION_SIZE / 2);
//                 if (ret != OPRT_OK) {
//                     //TAL_PR_NOTICE("Pre-erase failed: %d", ret);
//                 } else {
//                     //TAL_PR_NOTICE("Pre-erase successful");
//                 }
//             }
//         }
        
//         // 准备当前写入地址
//         OPERATE_RET ret = __demo_prepare_write_addr_atomic(write_addr);
//         if (ret != OPRT_OK) {
//             //TAL_PR_NOTICE("Prepare write addr failed: %d", ret);
            
//             // 擦除整个区域并重试
//             //TAL_PR_NOTICE("Erasing entire partition (0x%x, %d) and retrying...", 
//                         //  start_addr, DEMO_PARTITION_SIZE);
//             tkl_flash_erase(start_addr, DEMO_PARTITION_SIZE);
            
//             // 验证擦除
//             BOOL_T erase_ok = TRUE;
//             for (int i = 0; i < 4; i++) {
//                 uint8_t test_byte = 0;
//                 if (tkl_flash_read(start_addr + i, &test_byte, 1) != OPRT_OK || test_byte != 0xFF) {
//                     erase_ok = FALSE;
//                     break;
//                 }
//             }
            
//             if (!erase_ok) {
//                 //TAL_PR_NOTICE("Erase verification failed!");
//                 break;
//             }
            
//             write_addr = start_addr;
//             sg_demo_flash_op.write_addr = start_addr;
//             sg_demo_flash_op.read_addr = start_addr;
            
//             //TAL_PR_NOTICE("Retry with write addr: 0x%x", write_addr);
//         }
        
//         // 保存一个副本，避免在写入过程中被修改
//         DEMO_INFO_T save_copy;
//         memcpy(&save_copy, &sg_demo_info, sizeof(DEMO_INFO_T));
        
//         // 写入并验证
//         ret = __demo_write_and_verify_atomic(write_addr, &save_copy);
//         if (ret != OPRT_OK) {
//             //TAL_PR_NOTICE("Write and verify failed: %d", ret);
            
//             // 重试一次
//             //TAL_PR_NOTICE("Retry write...");
            
//             // 先擦除当前块
//             UINT_T current_block_start = 0;
//             if (write_addr < start_addr + DEMO_PARTITION_SIZE / 2) {
//                 current_block_start = start_addr;  // 块A
//             } else {
//                 current_block_start = start_addr + DEMO_PARTITION_SIZE / 2;  // 块B
//             }
            
//             // TAL_PR_NOTICE("Erasing current block at 0x%x", current_block_start);
//             tkl_flash_erase(current_block_start, DEMO_PARTITION_SIZE / 2);
            
//             // 重新写入
//             ret = __demo_write_and_verify_atomic(write_addr, &save_copy);
//             if (ret != OPRT_OK) {
//                 //TAL_PR_NOTICE("Retry also failed: %d", ret);
//                 break;
//             }
//         }
        
//         // 更新到全局变量
//         memcpy(&sg_demo_info, &save_copy, sizeof(DEMO_INFO_T));
        
//         // 更新地址
//         sg_demo_flash_op.read_addr = write_addr;
//         sg_demo_flash_op.write_addr = next_write_addr;
        
//         // 处理回绕
//         if (sg_demo_flash_op.write_addr >= end_addr) {
//             sg_demo_flash_op.write_addr = start_addr;
//         }
        
//         result = sizeof(DEMO_INFO_T);
//         //TAL_PR_NOTICE("Save success, next write addr: 0x%x", sg_demo_flash_op.write_addr);
        
//     } while(0);
    
//     // 清除保存标志
//     save_in_progress = 0;
//     return result;
// }
// /**
//  * @brief 加载配置
//  */
// int device_config_load(void)
// {
//     extern uint8_t night[6];
//     extern uint8_t custom_status[6];
//     extern uint8_t sleep_init[14];
//     extern uint8_t wake_init[11];
//     extern uint8_t switch_change_gear[21];
//     extern uint8_t collect[7];

//     if (0 == sg_demo_start_addr) {
//         return 0;
//     }

//     // 强制重新初始化
//     sg_demo_flash_op.init_flag = 0;
//     __demo_flash_init();

//     DEMO_INFO_T tmp_info = {0};
//     UINT_T latest_addr = 0;
//     uint32_t latest_time = 0;
//     UINT_T read_addr = sg_demo_start_addr;
//     UINT_T end_addr = sg_demo_start_addr + DEMO_PARTITION_SIZE;
//     tal_sw_timer_create(save_data_cb, NULL,&save_data);
//     // 遍历整个分区，找到时间戳最新的有效数据
//     while (read_addr < end_addr) {
//         memset(&tmp_info, 0, sizeof(DEMO_INFO_T));
        
//         if (tkl_flash_read(read_addr, (UCHAR_T *)&tmp_info, sizeof(DEMO_INFO_T)) != OPRT_OK) {
//             read_addr += sizeof(DEMO_INFO_T);
//             continue;
//         }
        
//         // 验证数据有效性
//         if (__demo_is_valid_data(&tmp_info)) {
//             if (tmp_info.time_stamp > latest_time) {
//                 TAL_PR_NOTICE("latest_time = %d,tmp_info.time_stamp = %d\r\n",latest_time,tmp_info.time_stamp);
//                 latest_time = tmp_info.time_stamp;
//                 latest_addr = read_addr;
//                 memcpy(&sg_demo_info, &tmp_info, sizeof(DEMO_INFO_T));
//             }
//         }
        
//         read_addr += sizeof(DEMO_INFO_T);
//     }

//     if (latest_time > 0) {
//         // 找到最新数据
//         sg_demo_flash_op.read_addr = latest_addr;
//         sg_demo_flash_op.write_addr = latest_addr + sizeof(DEMO_INFO_T);
        
//         // 检查写地址是否需要回绕
//         if (sg_demo_flash_op.write_addr >= end_addr) {
//             sg_demo_flash_op.write_addr = sg_demo_start_addr;
//         }
//         TAL_PR_NOTICE("device_config_load,%ld\r\n",sg_demo_flash_op.read_addr);
//         //               latest_time, sg_demo_flash_op.write_addr);
//         return sizeof(DEMO_INFO_T);
//     }

//     TAL_PR_NOTICE("No valid config found, use default");
    
//     // 设置默认值
//     memset(&sg_demo_info, 0, sizeof(DEMO_INFO_T));
//     sg_demo_info.cnt = 0;
//     sg_demo_info.cnt1 = 0;
//     sg_demo_info.gear_memory = 1;
//     sg_demo_info.switch_status = 1;
//     sg_demo_info.default_state = 0;
//     sg_demo_info.white_switch = 1;
//     sg_demo_info.white_bright = 100;
//     sg_demo_info.white_temp = 55;
//     sg_demo_info.aux_switch = 1;
//     sg_demo_info.aux_bright = 100;
//     sg_demo_info.change_light_status = 1;
//     sg_demo_info.rhythm_switch = 0;
//     sg_demo_info.first_network = 0;
//     sg_demo_info.calibration = 0;
//     memset(sg_demo_info.custom_status, 0, 6);
//     memset(sg_demo_info.sleep_init, 0, 14);
//     memset(sg_demo_info.wake_init, 0, 11);
//     memset(sg_demo_info.rhythm_sunlight, 0, 66);
//     memset(sg_demo_info.work_mode_value, 0, 84);
//     memset(sg_demo_info.switch_change_gear, 0, 21);
//     memset(sg_demo_info.night, 0, 6);
//     memset(sg_demo_info.collect, 0, 7);

//     // 从外部数组复制数据
//     extern uint8_t custom_status[6];
//     extern uint8_t sleep_init[14];
//     extern uint8_t wake_init[11];
//     extern uint8_t switch_change_gear[21];
//     extern uint8_t collect[7];
//     extern uint8_t night[6];

//     if (custom_status) memcpy(sg_demo_info.custom_status, custom_status, 6);
//     if (sleep_init) memcpy(sg_demo_info.sleep_init, sleep_init, 14);
//     if (wake_init) memcpy(sg_demo_info.wake_init, wake_init, 11);
//     if (switch_change_gear) memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
//     if (night) memcpy(sg_demo_info.night, night, 6);
//     if (collect) memcpy(sg_demo_info.collect, collect, 7);

//     // 设置时间戳和校验和
//     sg_demo_info.time_stamp = 1;
//     sg_demo_info.checksum = 0;
//     sg_demo_info.checksum = __demo_calc_checksum(&sg_demo_info);

//     sg_demo_flash_op.read_addr = sg_demo_start_addr;
//     sg_demo_flash_op.write_addr = sg_demo_start_addr;
//     device_config_save1();
//     return 1;
// }
#define DEMO_PARTITION_SIZE   (12 * 1024)  // 3KB分区

static UINT_T sg_demo_start_addr = 0x1EF000;

typedef struct {
    UINT_T  write_addr;
    UINT_T  read_addr;
    UCHAR_T init_flag;
} DEMO_FLASH_OP_T;

static DEMO_FLASH_OP_T sg_demo_flash_op = {0};

/* ================== 三块定义 ================== */
#define BLOCK_SIZE (4096)  // 每个块2KB
#define BLOCK_A    (sg_demo_start_addr)
#define BLOCK_B    (sg_demo_start_addr + BLOCK_SIZE)
#define BLOCK_C    (sg_demo_start_addr + 2 * BLOCK_SIZE)  // 第三块作为备份

#define BLOCK_MAGIC   0xA5A5A5A5
#define BLOCK_VALID   0x00000000
#define BLOCK_INVALID 0xFFFFFFFF

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t valid;
} BLOCK_HDR;

static UINT_T g_active_block = 0;  // 当前活跃块（A或B）
static UINT_T g_write_addr = 0;
static uint32_t g_version = 0;
static MUTEX_HANDLE sg_save_mutex = NULL;
UCHAR_T save_in_progress = 0;

/* 切档风暴防护：5 秒内最多真正写 Flash 1 次；冷却期内合并为一次补写 */
#define SAVE1_COOLDOWN_MS  5000

static SYS_TIME_T sg_last_save1_ms = 0;
static TIMER_ID sg_save1_cooldown_timer = NULL;
static BOOL_T sg_save1_pending = FALSE;

static VOID __save_feed_wdt(VOID)
{
    (VOID)tal_watchdog_refresh();
}

/* C 块仅校准备份用，日常/自愈只擦写 A/B，绝不碰 C */
static VOID __flash_erase_ab_blocks(VOID)
{
    __save_feed_wdt();
    (VOID)tkl_flash_erase(BLOCK_A, BLOCK_SIZE);
    __save_feed_wdt();
    (VOID)tkl_flash_erase(BLOCK_B, BLOCK_SIZE);
    TAL_PR_NOTICE("Save: force erased blocks A/B (C kept for calibration)");
}

/* ================== 校验函数 ================== */
static uint8_t __demo_calc_checksum(DEMO_INFO_T *p_info)
{
    uint8_t *p_data = (uint8_t *)p_info;
    uint8_t checksum = 0;

    size_t ts_off = offsetof(DEMO_INFO_T, time_stamp);
    size_t cs_off = offsetof(DEMO_INFO_T, checksum);

    for (size_t i = 0; i < sizeof(DEMO_INFO_T); i++) {
        if (i == cs_off) continue;
        if (i >= ts_off && i < ts_off + 4) continue;
        checksum += p_data[i];
    }
    return checksum;
}

static BOOL_T __demo_is_valid_data(DEMO_INFO_T *p)
{
    if (!p) return FALSE;
    if (p->time_stamp == 0 || p->time_stamp == 0xFFFFFFFF) return FALSE;
    return (p->checksum == __demo_calc_checksum(p));
}

/* NOR 擦除后为全 0xFF；与校验失败的“半写坏洞”区分开 */
static BOOL_T __demo_is_blank_slot(const DEMO_INFO_T *p)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;

    if (!p) return FALSE;
    for (i = 0; i < sizeof(DEMO_INFO_T); i++) {
        if (b[i] != 0xFF) {
            return FALSE;
        }
    }
    return TRUE;
}

/* 本块内第一个可追加空位；满则返回 block+BLOCK_SIZE */
static UINT_T __demo_find_first_blank(UINT_T block)
{
    UINT_T cur = block + sizeof(BLOCK_HDR);
    UINT_T end = block + BLOCK_SIZE;
    DEMO_INFO_T tmp;

    while (cur + sizeof(DEMO_INFO_T) <= end) {
        if (tkl_flash_read(cur, (UCHAR_T *)&tmp, sizeof(tmp)) != OPRT_OK) {
            return end;
        }
        if (__demo_is_blank_slot(&tmp)) {
            return cur;
        }
        cur += sizeof(DEMO_INFO_T);
    }
    return end;
}

/* 检查是否为特定calibration值的数据 */
static BOOL_T __is_calibration_data(DEMO_INFO_T *p, uint8_t cal_value)
{
    if (!__demo_is_valid_data(p)) return FALSE;
    if (p->calibration == cal_value) return TRUE;
    return FALSE;
}

/* ================== 读取块头 ================== */
static BOOL_T __read_hdr(UINT_T addr, BLOCK_HDR *hdr)
{
    if (tkl_flash_read(addr, (UCHAR_T*)hdr, sizeof(BLOCK_HDR)) != OPRT_OK)
        return FALSE;

    if (hdr->magic != BLOCK_MAGIC) return FALSE;
    if (hdr->valid != BLOCK_VALID) return FALSE;

    return TRUE;
}

/* ================== 扫描块获取最新数据 ================== */
static BOOL_T __scan_block(UINT_T addr, DEMO_INFO_T *out, UINT_T *last_addr, 
                          BOOL_T check_calibration, uint8_t cal_value)
{
    UINT_T cur = addr + sizeof(BLOCK_HDR);
    UINT_T end = addr + BLOCK_SIZE;

    DEMO_INFO_T tmp;
    uint32_t max_ts = 0;
    UINT_T max_addr = 0;
    DEMO_INFO_T max_data = {0};
    BOOL_T found = FALSE;

    while (cur + sizeof(DEMO_INFO_T) <= end) {
        if (tkl_flash_read(cur, (UCHAR_T*)&tmp, sizeof(tmp)) != OPRT_OK)
            break;

        /* 全 0xFF：追加区结束。坏洞（非空且校验失败）：跳过继续扫 */
        if (__demo_is_blank_slot(&tmp)) {
            break;
        }
        if (!__demo_is_valid_data(&tmp)) {
            cur += sizeof(DEMO_INFO_T);
            continue;
        }

        if (check_calibration) {
            if (tmp.calibration != cal_value) {
                cur += sizeof(DEMO_INFO_T);
                continue;
            }
        }

        if (tmp.time_stamp > max_ts) {
            max_ts = tmp.time_stamp;
            memcpy(&max_data, &tmp, sizeof(tmp));
            max_addr = cur;
            found = TRUE;
        }

        cur += sizeof(DEMO_INFO_T);
    }

    if (found) {
        if (out) memcpy(out, &max_data, sizeof(max_data));
        if (last_addr) *last_addr = max_addr;
        return TRUE;
    }

    return FALSE;
}

/* ================== 检查块是否有效 ================== */
static BOOL_T __is_block_valid(UINT_T block_addr)
{
    BLOCK_HDR hdr = {0};
    if (!__read_hdr(block_addr, &hdr)) {
        return FALSE;
    }
    
    // 检查块中是否有有效数据
    DEMO_INFO_T tmp = {0};
    UINT_T tmp_addr = 0;
    return __scan_block(block_addr, &tmp, &tmp_addr, FALSE, 0);
}

/* ================== 备份到C块 ================== */
int device_config_backup_to_c(void)
{
    // 确保已初始化
    if (!sg_demo_flash_op.init_flag) {
        __demo_flash_init();
        if (!sg_demo_flash_op.init_flag) {
            return -1;
        }
    }
    
    // 检查当前数据是否有效
    if (!__demo_is_valid_data(&sg_demo_info)) {
        // TAL_PR_NOTICE("Backup: Current data invalid");
        return -2;
    }
    
    // 准备要备份的数据
    DEMO_INFO_T backup_data;
    memcpy(&backup_data, &sg_demo_info, sizeof(sg_demo_info));
    backup_data.time_stamp++;  // 增加时间戳
    backup_data.checksum = __demo_calc_checksum(&backup_data);
    
    // 擦除C块
    if (tkl_flash_erase(BLOCK_C, BLOCK_SIZE) != OPRT_OK) {
        // TAL_PR_NOTICE("Backup: Failed to erase block C");
        return -3;
    }
    
    // 写入C块头部
    BLOCK_HDR hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
    if (tkl_flash_write(BLOCK_C, (UCHAR_T*)&hdr, sizeof(hdr)) != OPRT_OK) {
        // TAL_PR_NOTICE("Backup: Failed to write C block header");
        return -4;
    }
    
    // 写入数据到C块
    UINT_T write_addr = BLOCK_C + sizeof(BLOCK_HDR);
    if (tkl_flash_write(write_addr, (UCHAR_T*)&backup_data, sizeof(backup_data)) != OPRT_OK) {
        // TAL_PR_NOTICE("Backup: Failed to write data to C block");
        return -5;
    }
    
    // 验证写入
    DEMO_INFO_T verify;
    if (tkl_flash_read(write_addr, (UCHAR_T*)&verify, sizeof(verify)) != OPRT_OK) {
        // TAL_PR_NOTICE("Backup: Failed to read back from C block");
        return -6;
    }
    
    if (!__demo_is_valid_data(&verify)) {
        // TAL_PR_NOTICE("Backup: Data verification failed");
        return -7;
    }
    
    // TAL_PR_NOTICE("Backup to C block successful, calibration=%d", backup_data.calibration);
    return 0;
}

/* ================== 初始化函数 ================== */
void __demo_flash_init(void)
{
    BLOCK_HDR ha = {0}, hb = {0};
    DEMO_INFO_T da = {0}, db = {0};
    UINT_T la = 0, lb = 0;
    BOOL_T found_valid = FALSE;
    DEMO_INFO_T found_data = {0};
    UINT_T found_addr = 0;
    UINT_T found_block = 0;
    uint32_t found_version = 0;
    uint32_t found_timestamp = 0;  // 添加时间戳跟踪

    // tkl_log_output("=== Flash Initialization Start ===");
    tal_sw_timer_create(__sg_previce_cb, NULL, &sg_preview_timer);
    // 1. 首先查找所有块中的 calibration==3 数据，选择时间戳最新的
    uint32_t max_cal3_ts = 0;
    DEMO_INFO_T best_cal3_data = {0};
    UINT_T best_cal3_addr = 0;
    UINT_T best_cal3_block = 0;
    uint32_t best_cal3_version = 0;
    BOOL_T found_calibration_3 = FALSE;
    
    // 检查A块中是否有calibration==3的数据
    if (__read_hdr(BLOCK_A, &ha)) {
        if (__scan_block(BLOCK_A, &da, &la, TRUE, 3)) {
            // tkl_log_output("Init: Found calibration=3 in block A, ts=%lu", da.time_stamp);
            if (da.time_stamp > max_cal3_ts) {
                max_cal3_ts = da.time_stamp;
                memcpy(&best_cal3_data, &da, sizeof(da));
                best_cal3_addr = la;
                best_cal3_block = BLOCK_A;
                best_cal3_version = ha.version;
                found_calibration_3 = TRUE;
            }
        }
    }
    
    // 检查B块中是否有calibration==3的数据
    if (__read_hdr(BLOCK_B, &hb)) {
        if (__scan_block(BLOCK_B, &db, &lb, TRUE, 3)) {
            // tkl_log_output("Init: Found calibration=3 in block B, ts=%lu", db.time_stamp);
            if (db.time_stamp > max_cal3_ts) {
                max_cal3_ts = db.time_stamp;
                memcpy(&best_cal3_data, &db, sizeof(db));
                best_cal3_addr = lb;
                best_cal3_block = BLOCK_B;
                best_cal3_version = hb.version;
                found_calibration_3 = TRUE;
            }
        }
    }
    
    // 如果找到 calibration==3 的数据，使用最新的那个
    if (found_calibration_3) {
        // tkl_log_output("Init: Using calibration=3 from block 0x%X (ts=%lu)", 
        //              best_cal3_block, best_cal3_data.time_stamp);
        memcpy(&found_data, &best_cal3_data, sizeof(best_cal3_data));
        found_addr = best_cal3_addr;
        found_block = best_cal3_block;
        found_version = best_cal3_version;
        found_timestamp = best_cal3_data.time_stamp;
        found_valid = TRUE;
    } else {
        // 2. 如果AB块都没有calibration==3，检查C块
        BLOCK_HDR hc = {0};
        DEMO_INFO_T dc = {0};
        UINT_T lc = 0;
        
        if (__read_hdr(BLOCK_C, &hc)) {
            if (__scan_block(BLOCK_C, &dc, &lc, TRUE, 3)) {
                // tkl_log_output("Init: Found calibration=3 in block C, ts=%lu", dc.time_stamp);
                memcpy(&found_data, &dc, sizeof(dc));
                found_addr = lc;
                found_block = BLOCK_C;
                found_version = hc.version;
                found_timestamp = dc.time_stamp;
                found_valid = TRUE;
                
                // 从C块恢复，需要写到AB块
                // tkl_log_output("Init: Recovering from C block to A block");
                
                // 选择A块作为恢复目标
                if (tkl_flash_erase(BLOCK_A, BLOCK_SIZE) != OPRT_OK) {
                    // tkl_log_output("Init: Failed to erase block A for recovery");
                    found_valid = FALSE;
                } else {
                    BLOCK_HDR new_hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
                    if (tkl_flash_write(BLOCK_A, (UCHAR_T*)&new_hdr, sizeof(new_hdr)) != OPRT_OK) {
                        // tkl_log_output("Init: Failed to write block A header");
                        found_valid = FALSE;
                    } else {
                        // 更新数据时间戳
                        found_data.time_stamp++;
                        found_data.checksum = __demo_calc_checksum(&found_data);
                        
                        UINT_T write_addr = BLOCK_A + sizeof(BLOCK_HDR);
                        if (tkl_flash_write(write_addr, (UCHAR_T*)&found_data, sizeof(found_data)) == OPRT_OK) {
                            DEMO_INFO_T verify;
                            tkl_flash_read(write_addr, (UCHAR_T*)&verify, sizeof(verify));
                            if (__demo_is_valid_data(&verify)) {
                                found_addr = write_addr;
                                found_block = BLOCK_A;
                                found_version = 1;
                                found_timestamp = verify.time_stamp;
                                // tkl_log_output("Init: Recovered from C to A block successfully");
                            } else {
                                found_valid = FALSE;
                            }
                        } else {
                            found_valid = FALSE;
                        }
                    }
                }
            }
        }
    }
    
    // 3. 如果都没有calibration==3的数据，找AB块中的其他有效数据（A/B 一起比 time_stamp）
    if (!found_valid) {
        uint32_t max_any_ts = 0;
        DEMO_INFO_T best_any_data = {0};
        UINT_T best_any_addr = 0;
        UINT_T best_any_block = 0;
        uint32_t best_any_version = 0;
        BOOL_T found_any = FALSE;

        /* A/B 都扫描，取 time_stamp 更大的一条 */
        if (__read_hdr(BLOCK_A, &ha)) {
            DEMO_INFO_T tmp = {0};
            UINT_T tmp_addr = 0;
            if (__scan_block(BLOCK_A, &tmp, &tmp_addr, FALSE, 0)) {
                /* 仅接受 cal==0/3，排除中间态 1/2（与 light_3 普通数据加载一致） */
                if ((tmp.calibration == 0 || tmp.calibration == 3) &&
                    tmp.time_stamp > max_any_ts) {
                    max_any_ts = tmp.time_stamp;
                    memcpy(&best_any_data, &tmp, sizeof(tmp));
                    best_any_addr = tmp_addr;
                    best_any_block = BLOCK_A;
                    best_any_version = ha.version;
                    found_any = TRUE;
                }
            }
        }

        if (__read_hdr(BLOCK_B, &hb)) {
            DEMO_INFO_T tmp = {0};
            UINT_T tmp_addr = 0;
            if (__scan_block(BLOCK_B, &tmp, &tmp_addr, FALSE, 0)) {
                if ((tmp.calibration == 0 || tmp.calibration == 3) &&
                    tmp.time_stamp > max_any_ts) {
                    max_any_ts = tmp.time_stamp;
                    memcpy(&best_any_data, &tmp, sizeof(tmp));
                    best_any_addr = tmp_addr;
                    best_any_block = BLOCK_B;
                    best_any_version = hb.version;
                    found_any = TRUE;
                }
            }
        }

        if (found_any) {
            memcpy(&found_data, &best_any_data, sizeof(best_any_data));
            found_addr = best_any_addr;
            found_block = best_any_block;
            found_version = best_any_version;
            found_timestamp = best_any_data.time_stamp;
            found_valid = TRUE;
        }
    }
    
    // 4. 如果找到有效数据
    if (found_valid) {
        // 设置活跃块（如果是C块找到的，现在应该已经在A块）
        g_active_block = (found_block == BLOCK_C) ? BLOCK_A : found_block;
        g_version = found_version;
        memcpy(&sg_demo_info, &found_data, sizeof(found_data));
        
        /* 写指针落到本块第一个空白格，勿用 found_addr+size（后面可能有洞/已写格） */
        g_write_addr = __demo_find_first_blank(g_active_block);
        
        // 检查写地址是否超出块边界
        UINT_T block_end = g_active_block + BLOCK_SIZE;
        if (g_write_addr + sizeof(DEMO_INFO_T) > block_end) {
            // 当前块已满，需要切换到另一块
            UINT_T other_block = (g_active_block == BLOCK_A) ? BLOCK_B : BLOCK_A;
            
            // 擦除另一块
            tkl_flash_erase(other_block, BLOCK_SIZE);
            
            // 初始化另一块头部
            BLOCK_HDR other_hdr = {BLOCK_MAGIC, g_version + 1, BLOCK_VALID};
            tkl_flash_write(other_block, (UCHAR_T*)&other_hdr, sizeof(other_hdr));
            
            // 切换到另一块
            g_active_block = other_block;
            g_version++;
            g_write_addr = other_block + sizeof(BLOCK_HDR);
            
            // tkl_log_output("Init: Active block full, switched to block 0x%X", g_active_block);
        }
        
        // tkl_log_output("Init: Using valid data, calibration=%d, ts=%lu", 
        //              sg_demo_info.calibration, sg_demo_info.time_stamp);
    } else {
        // 5. 没有找到任何有效数据，使用默认数据
        // 5. 没有找到任何有效数据，使用默认数据
        // tkl_log_output("Init: No valid data found, using default");
        
        // 擦除所有块，确保从干净状态开始
        // tkl_log_output("Init: Erasing all blocks for clean start");
        
        // 擦除A块
        if (tkl_flash_erase(BLOCK_A, BLOCK_SIZE) != OPRT_OK) {
            // tkl_log_output("Init: Warning: Failed to erase block A");
        }
        
        // 擦除B块
        if (tkl_flash_erase(BLOCK_B, BLOCK_SIZE) != OPRT_OK) {
            // tkl_log_output("Init: Warning: Failed to erase block B");
        }
        
        // 擦除C块
        if (tkl_flash_erase(BLOCK_C, BLOCK_SIZE) != OPRT_OK) {
            // tkl_log_output("Init: Warning: Failed to erase block C");
        }
        
        // 初始化A块
        BLOCK_HDR hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
        if (tkl_flash_write(BLOCK_A, (UCHAR_T*)&hdr, sizeof(hdr)) != OPRT_OK) {
            // tkl_log_output("Init: Failed to write block A header");
            // 写入失败，尝试使用内存中的默认值继续
        }
        
        // 设置默认数据
        memset(&sg_demo_info, 0, sizeof(sg_demo_info));
        sg_demo_info.cnt = 0;
        sg_demo_info.cnt1 = 0;
        sg_demo_info.gear_memory = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.default_state = 0;
        sg_demo_info.white_switch = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.last_light_memory = 1; /* 重置后默认主+辅记忆 */
        sg_demo_info.white_temp = 55;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.aux_bright = 100;
        sg_demo_info.change_light_status = 1;
        sg_demo_info.rhythm_switch = 0;
        sg_demo_info.first_network = 0;
        sg_demo_info.beep_switch = 1;
        sg_demo_info.hand_sweep_switch = 1;
        memset(sg_demo_info.custom_status, 0, 6);
        memset(sg_demo_info.sleep_init, 0, 14);
        memset(sg_demo_info.wake_init, 0, 11);
        memset(sg_demo_info.rhythm_sunlight, 0, 66);
        memset(sg_demo_info.work_mode_value, 0, 84);
        memset(sg_demo_info.switch_change_gear, 0, 21);
        memset(sg_demo_info.night, 0, 6);
        memset(sg_demo_info.collect, 0, 7);

        // 从外部数组复制数据
        extern uint8_t custom_status[6];
        extern uint8_t sleep_init[14];
        extern uint8_t wake_init[11];
        extern uint8_t switch_change_gear[21];
        extern uint8_t collect[7];
        extern uint8_t night[6];
        extern uint8_t power_outage[10];

        if (custom_status) memcpy(sg_demo_info.custom_status, custom_status, 6);
        if (sleep_init) memcpy(sg_demo_info.sleep_init, sleep_init, 14);
        if (wake_init) memcpy(sg_demo_info.wake_init, wake_init, 11);
        if (switch_change_gear) memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
        if (night) memcpy(sg_demo_info.night, night, 6);
        if (collect) memcpy(sg_demo_info.collect, collect, 7);
        if(power_outage) memcpy(sg_demo_info.power_outage, power_outage, 10);

        // 设置时间戳和校验和
        sg_demo_info.time_stamp = 1;
        
        // 重要：设置默认的calibration值
        sg_demo_info.calibration = 0;  // 默认校准值为1
        
        sg_demo_info.checksum = __demo_calc_checksum(&sg_demo_info);
        
        g_active_block = BLOCK_A;
        g_version = 1;
        g_write_addr = BLOCK_A + sizeof(BLOCK_HDR);

        // TAL_PR_NOTICE("Init: Default data set, calibration=%d", sg_demo_info.calibration);
    }
    
    sg_demo_flash_op.init_flag = 1;
    // tkl_log_output("Init complete: active_block=0x%X, calibration=%d, ts=%lu, write_addr=0x%X", 
    //          g_active_block, sg_demo_info.calibration, sg_demo_info.time_stamp, g_write_addr);
}

/* ================== 换到另一块并写入最新数据 ================== */
static int __save_switch_to_other_block(DEMO_INFO_T *data)
{
    UINT_T new_block = (g_active_block == BLOCK_A) ? BLOCK_B : BLOCK_A;
    UINT_T end = new_block + BLOCK_SIZE;
    UINT_T addr;

    TAL_PR_NOTICE("Save: Block 0x%X full/failed, switching to 0x%X",
                  g_active_block, new_block);

    if (tkl_flash_erase(new_block, BLOCK_SIZE) != OPRT_OK) {
        TAL_PR_NOTICE("Save: Failed to erase new block 0x%X", new_block);
        return 0;
    }

    BLOCK_HDR hdr = {BLOCK_MAGIC, g_version + 1, BLOCK_VALID};
    if (tkl_flash_write(new_block, (UCHAR_T*)&hdr, sizeof(hdr)) != OPRT_OK) {
        TAL_PR_NOTICE("Save: Failed to write new block header");
        return 0;
    }

    addr = new_block + sizeof(BLOCK_HDR);
    while (addr + sizeof(DEMO_INFO_T) <= end) {
        if (tkl_flash_write(addr, (UCHAR_T*)data, sizeof(*data)) == OPRT_OK) {
            DEMO_INFO_T v;
            tkl_flash_read(addr, (UCHAR_T*)&v, sizeof(v));
            if (__demo_is_valid_data(&v)) {
                g_active_block = new_block;
                g_version++;
                g_write_addr = addr + sizeof(DEMO_INFO_T);
                memcpy(&sg_demo_info, data, sizeof(*data));
                TAL_PR_NOTICE("Save: Switched to block 0x%X, ts=%lu, calibration=%d",
                              g_active_block, data->time_stamp, data->calibration);
                return (int)sizeof(DEMO_INFO_T);
            }
        }
        TAL_PR_NOTICE("Save: New-block slot fail at 0x%X, try next", addr);
        addr += sizeof(DEMO_INFO_T);
    }

    TAL_PR_NOTICE("Save: No writable slot in other block 0x%X", new_block);
    return 0;
}

static VOID __save_mutex_ensure(VOID)
{
    if (sg_save_mutex == NULL) {
        tal_mutex_create_init(&sg_save_mutex);
    }
}

/* 双块都写挂：擦除后重建 A 块并写入最新数据（兜底自愈） */
static int __save_force_recover_write(DEMO_INFO_T *data)
{
    BLOCK_HDR hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
    UINT_T addr;

    TAL_PR_NOTICE("Save: force recover erase A/B + rewrite (C untouched)");
    __flash_erase_ab_blocks();
    __save_feed_wdt();
    if (tkl_flash_write(BLOCK_A, (UCHAR_T *)&hdr, sizeof(hdr)) != OPRT_OK) {
        TAL_PR_NOTICE("Save: force recover header fail");
        return 0;
    }
    addr = BLOCK_A + sizeof(BLOCK_HDR);
    __save_feed_wdt();
    if (tkl_flash_write(addr, (UCHAR_T *)data, sizeof(*data)) != OPRT_OK) {
        TAL_PR_NOTICE("Save: force recover data fail");
        return 0;
    }
    g_active_block = BLOCK_A;
    g_version = 1;
    g_write_addr = addr + sizeof(DEMO_INFO_T);
    memcpy(&sg_demo_info, data, sizeof(*data));
    return (int)sizeof(DEMO_INFO_T);
}

static VOID save1_cooldown_cb(TIMER_ID timer_id, VOID_T *arg)
{
    (void)timer_id;
    (void)arg;
    if (sg_save1_pending) {
        sg_save1_pending = FALSE;
        (VOID)device_config_save1_force();
    }
}

/* ================== 保存函数（立即写，可绕过冷却） ================== */
int device_config_save1_force(void)
{
    DEMO_INFO_T data;
    DEMO_INFO_T slot;
    UINT_T end;
    int ret = 0;

    __save_mutex_ensure();
    tal_mutex_lock(sg_save_mutex);

    if (save_in_progress) {
        tal_mutex_unlock(sg_save_mutex);
        return 0;
    }
    save_in_progress = 1;

    if (!sg_demo_flash_op.init_flag) {
        __demo_flash_init();
        if (!sg_demo_flash_op.init_flag) {
            save_in_progress = 0;
            tal_mutex_unlock(sg_save_mutex);
            return 0;
        }
    }

    memcpy(&data, &sg_demo_info, sizeof(sg_demo_info));
    data.time_stamp++;
    data.checksum = __demo_calc_checksum(&data);

    end = g_active_block + BLOCK_SIZE;
    __save_feed_wdt();

    if (g_write_addr < g_active_block + sizeof(BLOCK_HDR) ||
        g_write_addr + sizeof(DEMO_INFO_T) > end) {
        g_write_addr = __demo_find_first_blank(g_active_block);
    }

    if (g_write_addr + sizeof(DEMO_INFO_T) > end) {
        ret = __save_switch_to_other_block(&data);
        if (ret <= 0) {
            ret = __save_force_recover_write(&data);
        }
        if (ret > 0) {
            sg_last_save1_ms = tal_system_get_millisecond();
        }
        save_in_progress = 0;
        tal_mutex_unlock(sg_save_mutex);
        return ret;
    }

    while (g_write_addr + sizeof(DEMO_INFO_T) <= end) {
        __save_feed_wdt();
        if (tkl_flash_read(g_write_addr, (UCHAR_T *)&slot, sizeof(slot)) != OPRT_OK) {
            g_write_addr += sizeof(DEMO_INFO_T);
            continue;
        }

        if (!__demo_is_blank_slot(&slot)) {
            g_write_addr += sizeof(DEMO_INFO_T);
            continue;
        }

        if (tkl_flash_write(g_write_addr, (UCHAR_T*)&data, sizeof(data)) == OPRT_OK) {
            DEMO_INFO_T v;
            tkl_flash_read(g_write_addr, (UCHAR_T*)&v, sizeof(v));
            if (__demo_is_valid_data(&v)) {
                g_write_addr += sizeof(DEMO_INFO_T);
                memcpy(&sg_demo_info, &data, sizeof(data));
                ret = (int)sizeof(DEMO_INFO_T);
                sg_last_save1_ms = tal_system_get_millisecond();
                save_in_progress = 0;
                tal_mutex_unlock(sg_save_mutex);
                return ret;
            }
            TAL_PR_NOTICE("Save: Verification failed at 0x%X, skip to next", g_write_addr);
        } else {
            TAL_PR_NOTICE("Save: Write failed at 0x%X, skip to next", g_write_addr);
        }

        g_write_addr += sizeof(DEMO_INFO_T);
    }

    ret = __save_switch_to_other_block(&data);
    if (ret <= 0) {
        ret = __save_force_recover_write(&data);
    }
    if (ret > 0) {
        sg_last_save1_ms = tal_system_get_millisecond();
    }
    save_in_progress = 0;
    tal_mutex_unlock(sg_save_mutex);
    return ret;
}

/* 普通立即保存：5s 冷却，风暴期丢掉多余写入 */
int device_config_save1(void)
{
    SYS_TIME_T now = tal_system_get_millisecond();
    SYS_TIME_T elapsed;

    if (sg_last_save1_ms != 0) {
        elapsed = now - sg_last_save1_ms;
        if (elapsed < SAVE1_COOLDOWN_MS) {
            SYS_TIME_T remain = SAVE1_COOLDOWN_MS - elapsed;
            sg_save1_pending = TRUE;
            if (sg_save1_cooldown_timer == NULL) {
                tal_sw_timer_create(save1_cooldown_cb, NULL, &sg_save1_cooldown_timer);
            }
            if (FALSE == tal_sw_timer_is_running(sg_save1_cooldown_timer)) {
                tal_sw_timer_start(sg_save1_cooldown_timer, (TIME_MS)remain, TAL_TIMER_ONCE);
            }
            return 0;
        }
    }

    sg_save1_pending = FALSE;
    return device_config_save1_force();
}

/* ================== 原有接口保持不变 ================== */
STATIC TIMER_ID save_data = NULL;

STATIC VOID save_data_cb(TIMER_ID timer_id, VOID_T *arg)
{
    device_config_save1();
}

int device_config_save(void)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    UINT_T delay_ms = 5000;

    if (0 == sg_demo_start_addr) {
        return 0;
    }

    if (save_in_progress) {
        return 0;
    }

    /* 伴眠/唤醒中仍落盘，但拉长间隔；定时器已在跑则不重置，避免步进 upload 一直推迟 */
    if (sg_sleep_is_timing || sg_wake_is_timing) {
        delay_ms = 30000;
        if (save_data != NULL && TRUE == tal_sw_timer_is_running(save_data)) {
            return 1;
        }
    }

    if (save_data == NULL) {
        tal_sw_timer_create(save_data_cb, NULL, &save_data);
    }

    tal_sw_timer_start(save_data, delay_ms, TAL_TIMER_ONCE);
    return 1;
}

int device_config_load(void)
{
    __demo_flash_init();
    return sizeof(DEMO_INFO_T);
}

/* ================== 检查C块状态 ================== */
int device_config_check_block_c(BOOL_T *has_valid_data, uint8_t *calibration)
{
    BLOCK_HDR hdr = {0};
    
    if (tkl_flash_read(BLOCK_C, (UCHAR_T*)&hdr, sizeof(hdr)) != OPRT_OK) {
        if (has_valid_data) *has_valid_data = FALSE;
        return -1;
    }
    
    if (hdr.magic != BLOCK_MAGIC || hdr.valid != BLOCK_VALID) {
        if (has_valid_data) *has_valid_data = FALSE;
        return -2;
    }
    
    // 检查是否有有效数据
    DEMO_INFO_T data = {0};
    UINT_T addr = 0;
    BOOL_T valid = __scan_block(BLOCK_C, &data, &addr, FALSE, 0);
    
    if (has_valid_data) *has_valid_data = valid;
    if (calibration && valid) *calibration = data.calibration;
    
    return valid ? 0 : -3;
}
