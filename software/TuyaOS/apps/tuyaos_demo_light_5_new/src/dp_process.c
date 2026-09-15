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
#include "light_pwm_mix.h"

#include "tal_log.h"
#include "tal_memory.h"
#include "tal_workq_service.h"
#include "dp_process.h"
#include "app_light_tm_sleep.h"
#include "app_light_tm_wake.h"
#include "app_pwm.h"
#include "app_light_tm_rhythm.h"
#include "app_light_tm_schedule.h"
#include "app_mode.h"
#include "pwm_gradual.h"
#include "tal_sw_timer.h"
#include "tal_mutex.h"
#include "power_count.h"
/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

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
uint32_t white_off_time = 0;
uint32_t aux_off_time = 0;
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
bool_t sg_preview_active = false;
VOID dp_obj_process(CONST TY_OBJ_DP_S *dp_data_arr, UINT_T dp_cnt)
{
    UINT32_T index = 0;
    OPERATE_RET rt = OPRT_OK;
    TY_OBJ_DP_S *dp_data = NULL;
    extern uint16_t gradual_time_ms;
    extern bool sg_rhythm_interrupted;
    extern TM_RHYTHM_INFO_T sg_rhythm_info;
    extern int night_light_mode;
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;

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
            /* 伴眠/唤醒过程中会主动上报开关，回显值与当前一致时忽略，避免误打断 */
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_bool == sg_demo_info.switch_status) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();


            // 正常模式下的开关逻辑
            {
                bool sw_changed = (dp_data->value.dp_bool != sg_demo_info.switch_status);
                sg_demo_info.switch_status = dp_data->value.dp_bool;
                /* 关灯时先清主/辅标志再 LIGHT_OFF，才能正确 stop_fade；开灯在置标志后处理 */
                if (sg_demo_info.switch_status == 1) {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_ON);
                }
            }

            if (sg_demo_info.switch_status == 1)
            {
                // ==================== 开灯逻辑 ====================
                // 检查是否应该进入夜灯模式
                if ((app_nightlight_should_enter()) && (sg_demo_info.night_switch == 0))
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
                    bool open_night = false;
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
                        case 4:
                            open_night = true;
                            break;
                        default:
                            open_white = true;
                            open_aux = true;
                            break;
                    }
                    
                    // 设置灯的开关状态
                    sg_demo_info.white_switch = open_white;
                    sg_demo_info.aux_switch = open_aux;
                    sg_demo_info.night_switch = open_night;
                    if(sg_demo_info.white_switch == 1){
                        upload_device_bool_status(LIGHT_SWITCH, 1);
                    }
                    if(sg_demo_info.aux_switch == 1){
                        upload_device_bool_status(AUX_SWITCH, 1);
                    }
                    if(sg_demo_info.night_switch == 1){
                        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
                    }
                    uint16_t aux_ww,aux_cw,cw,ww,night_bright;
                    if(sg_demo_info.white_bright == 0 && sg_demo_info.white_switch == 1){
                        sg_demo_info.white_switch = 0;
                        upload_device_bool_status(LIGHT_SWITCH, 0);
                    }
                    if(sg_demo_info.white_switch == 1){
                        upload_device_bool_status(LIGHT_SWITCH, 1);
                        upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                        // 设置主灯和辅灯亮度
                        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                        uint8_t white_temp1 = sg_demo_info.white_temp;
                        ww = white_bright1 * white_temp1;
                        cw = white_bright1 * 100 - ww;
                        light_pwm_clamp_mix(&ww, &cw);
                    }else{
                        cw = 0;
                        ww = 0;     
                    }

                    if(sg_demo_info.aux_bright == 0 && sg_demo_info.aux_switch == 1){
                        sg_demo_info.aux_switch = 0;
                        upload_device_bool_status(AUX_SWITCH, 0);
                    }
                    if(sg_demo_info.aux_switch == 1){
                        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
                        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                        upload_device_bool_status(AUX_SWITCH, 1);
                        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                        uint8_t aux_temp1 = sg_demo_info.white_temp;
                        aux_ww = aux_bright1 * aux_temp1;
                        aux_cw = aux_bright1 * 100 - aux_ww;
                        light_pwm_clamp_mix(&aux_ww, &aux_cw);
                    }
                    else{
                        aux_cw = 0;
                        aux_ww = 0;     
                    }
                    if(sg_demo_info.night_switch == 1){
                        app_light_rhythm_interrupt_on_night_open();
                        night_bright = sg_demo_info.night_bright * 100;
                        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
                        upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
                    }else{
                        night_bright = 0;
                    }

                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                    UINT32_T duties[] = {ww, cw, aux_ww, aux_cw, night_bright};
                    pwm_gradual_duty_set_multi(5, channels, duties);

                    // 上报状态
                    upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
                    /* 开的是主/辅且节律未打断：立刻按当前节律亮色温同步 */
                    if (!open_night) {
                        app_light_tm_rhythm_syn();
                    }
                    
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
                else if(sg_demo_info.night_switch == 1)
                    sg_demo_info.last_light_memory = 4;
                
                // 关闭所有灯光状态
                sg_demo_info.white_switch = 0;
                sg_demo_info.aux_switch = 0;
                sg_demo_info.night_switch = 0;
                sg_demo_info.switch_status = 0;
                /* 主辅都灭：停渐变，不打断节律 */
                app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_LIGHT_OFF);

                // 关闭所有PWM输出
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {0, 0, 0, 0, 0};
                pwm_gradual_duty_set_multi(5, channels, duties);

                // 上报所有关闭状态
                upload_device_bool_status(DPID_SWITCH, 0);
                upload_device_bool_status(LIGHT_SWITCH, 0);
                upload_device_bool_status(AUX_SWITCH, 0);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                upload_device_enum_status(DPID_WORK_MODE, 16);
            }
            break;
        case DPID_WORK_MODE:
            work_mode = dp_data->value.dp_enum;
            if (work_mode != 16)
            {
                app_light_schedule_mark_configured(SCHEDULE_EVT_SCENE);
                app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_SCENE, TRUE);
                app_light_schedule_before_scene_start();
                if (sg_demo_info.rhythm_switch == 1) {
                    app_light_stop_today_rhythm_timer();
                }
            }
            if (work_mode == 0)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {5500, 4500, 5500, 4500, 0};
                pwm_gradual_duty_set_multi(5, channels, duties);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.white_temp = 55;
                sg_demo_info.night_switch = 0;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 55);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
            }
            else if (work_mode == 1)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
                pwm_gradual_duty_set_multi(5, channels, duties1);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.white_temp = 100;
                sg_demo_info.night_switch = 0;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 100);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
            }
            else if (work_mode == 2)
            {
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {0, 0, 0, 0, 50 * 100};
                pwm_gradual_duty_set_multi(5, channels, duties);

                sg_demo_info.white_switch = 0;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 0;
                sg_demo_info.night_switch = 1;
                sg_demo_info.night_bright = 50;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 0);
                upload_device_bool_status(AUX_SWITCH, 0);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
                upload_device_value_status(NIGHT_LIGHT_VALUE, 50);
            }
            else if (work_mode == 3)
            {
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;
                sg_demo_info.white_switch = sg_demo_info.collect[0];
                sg_demo_info.white_bright = sg_demo_info.collect[1];
                sg_demo_info.aux_switch = sg_demo_info.collect[2];
                sg_demo_info.aux_bright = sg_demo_info.collect[3];
                sg_demo_info.white_temp = sg_demo_info.collect[4];
                sg_demo_info.night_switch = sg_demo_info.collect[5];
                sg_demo_info.night_bright = sg_demo_info.collect[6];
                if ((sg_demo_info.white_switch == 1) || (sg_demo_info.aux_switch == 1)||(sg_demo_info.night_switch == 1))
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
                    all_channels[channel_count] = AUX_BRIGHT_PWM;
                    all_duties[channel_count++] = aux_ww;

                    all_channels[channel_count] = AUX_TEMP_PWM;
                    all_duties[channel_count++] = aux_cw;
                    upload_device_bool_status(AUX_SWITCH, 1);
                    upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                }
                if (sg_demo_info.night_switch == 1)
                {
                    sg_demo_info.white_switch = 0;
                    upload_device_bool_status(LIGHT_SWITCH, 0);
                    sg_demo_info.switch_status = 1;
                    upload_device_bool_status(DPID_SWITCH, 1);
                    sg_demo_info.aux_switch = 0;
                    upload_device_bool_status(AUX_SWITCH, 0);
                    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                    all_duties[channel_count++] = sg_demo_info.night_bright * 100;
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
                    upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
                }
                else
                {
                    sg_demo_info.night_switch = 0;
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                    all_duties[channel_count++] = 0;
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
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {ww, cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            break;
        case NIGHT_LIGHT_VALUE:
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_value == sg_demo_info.night_bright) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            {
                bool val_changed = (dp_data->value.dp_value != sg_demo_info.night_bright);
            sg_demo_info.night_bright = dp_data->value.dp_value;
                app_light_rhythm_interrupt_on_user_change(val_changed, RHYTHM_USER_OP_DIM_TEMP);
            }
            if (sg_demo_info.night_switch == 1)
            {
                upload_device_enum_status(DPID_WORK_MODE, 16);
                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, sg_demo_info.night_bright * 100);
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
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            break;
        case DPID_SWITCH_NIGHT_LIGHT:
            if ((sg_sleep_is_timing || sg_wake_is_timing) &&
                dp_data->value.dp_bool == sg_demo_info.night_switch) {
                break;
            }
            app_light_schedule_user_interrupt_sleep_wake();

            {
                bool sw_changed = (dp_data->value.dp_bool != sg_demo_info.night_switch);
                sg_demo_info.night_switch = dp_data->value.dp_bool;
                if (sg_demo_info.night_switch) {
                    app_light_rhythm_interrupt_on_night_open();
                }
                /* 关夜灯与节律不冲突 */
                (void)sw_changed;
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (dp_data->value.dp_bool == 1)
            {
                if(sg_demo_info.white_switch == 1&&sg_demo_info.aux_switch == 1)
                    sg_demo_info.last_light_memory = 1;
                else if(sg_demo_info.white_switch == 1)
                    sg_demo_info.last_light_memory = 2;
                else if(sg_demo_info.aux_switch == 1)
                    sg_demo_info.last_light_memory = 3;
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 0);
                upload_device_bool_status(AUX_SWITCH, 0);
                upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
                sg_demo_info.white_switch = 0;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 0;
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {0, 0, 0, 0, sg_demo_info.night_bright * 100};
                pwm_gradual_duty_set_multi(5, channels, duties);
            }
            else
            {
                sg_demo_info.last_light_memory = 4;
                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
            }
            if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) &&(sg_demo_info.night_switch == 0))
            {
                sg_demo_info.switch_status = 0;
                upload_device_bool_status(DPID_SWITCH, 0);
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
                /* 打开节律：打断正在运行的伴眠/唤醒 */
                app_light_schedule_before_rhythm_start();
                upload_device_bool_status(RHYTHM_STATUS, 0);

                // 检查是否有主灯或辅灯开启
                bool main_light_on = (sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch);

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
                        sg_rhythm_info.is_timming = false;
                        sg_rhythm_interrupted = false;
                    }
                }
            }
            else
            {
                upload_device_bool_status(RHYTHM_STATUS,0);
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
                if (sg_demo_info.white_switch) {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_ON);
                } else {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_OFF);
                }
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (dp_data->value.dp_bool == 1)
            {
                sg_demo_info.switch_status = 1;
                
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(DPID_SWITCH, 1);
                if (sg_demo_info.night_switch == 1)
                {
                    sg_demo_info.night_switch = 0;
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                }
                extern bool sg_rhythm_interrupted;
                if (sg_demo_info.rhythm_switch && !sg_rhythm_interrupted) {
                    app_light_tm_rhythm_syn();
                    upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                } else {
                    upload_device_value_status(DPID_WHITE_BRIGHT,sg_demo_info.white_bright);
                    upload_device_value_status(DPID_TEMP_VALUE,sg_demo_info.white_temp);
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                    light_pwm_clamp_mix(&ww, &cw);
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM,NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {ww, cw,0};
                pwm_gradual_duty_set_multi(3, channels, duties);
                }
            }
            else
            {
                sg_demo_info.last_light_memory = 2;
                white_off_time = tal_system_get_millisecond();
                if(white_off_time - aux_off_time <= 300)
                    sg_demo_info.last_light_memory = 1;
                /* 只关主灯：主路停到 0；辅灯若在渐变则继续 */
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {0, 0};
                if (sg_demo_info.aux_switch) {
                    pwm_gradual_close_channels_keep_rest(2, channels, duties);
                } else {
                pwm_gradual_duty_set_multi(2, channels, duties);
                }
                upload_device_bool_status(LIGHT_SWITCH, 0);
            }
            if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) &&(sg_demo_info.night_switch == 0))
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
                if (sg_demo_info.aux_switch) {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_ON);
                } else {
                    app_light_rhythm_interrupt_on_user_change(sw_changed, RHYTHM_USER_OP_LIGHT_OFF);
                }
            }
            upload_device_enum_status(DPID_WORK_MODE, 16);
            if (dp_data->value.dp_bool == 1)
            {
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_bool_status(DPID_SWITCH, 1);
                sg_demo_info.switch_status = 1;
                if (sg_demo_info.night_switch == 1)
                {
                    sg_demo_info.night_switch = 0;
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                }
                extern bool sg_rhythm_interrupted;
                if (sg_demo_info.rhythm_switch && !sg_rhythm_interrupted) {
                    app_light_tm_rhythm_syn();
                    upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
                    upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
                } else {
                    upload_device_value_status(DPID_AUX_BRIGHT_VALUE,sg_demo_info.aux_bright);
                    upload_device_value_status(DPID_TEMP_VALUE,sg_demo_info.white_temp);
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                    light_pwm_clamp_mix(&aux_ww, &aux_cw);
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM,NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw,0};
                pwm_gradual_duty_set_multi(3, channels, duties);
                }
            }
            else
            {
                sg_demo_info.last_light_memory = 3;
                aux_off_time = tal_system_get_millisecond();
                if(aux_off_time - white_off_time <= 300)
                    sg_demo_info.last_light_memory = 1;
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {0, 0};
                if (sg_demo_info.white_switch) {
                    pwm_gradual_close_channels_keep_rest(2, channels, duties);
                } else {
                pwm_gradual_duty_set_multi(2, channels, duties);
                }
                upload_device_bool_status(AUX_SWITCH, 0);
            }
            if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) &&(sg_demo_info.night_switch == 0))
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
                TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
                pwm_gradual_duty_set_multi(5, channels1, duties1);

                // 等待渐变完成
                pwm_gradual_wait_current_complete(1000); // 等待1秒

                // 第二阶段：设置为低亮度
                TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties2[] = {3, 3, 3, 3};
                pwm_gradual_duty_set_multi(4, channels2, duties2);

                pwm_gradual_wait_current_complete(1000); // 等待1秒
                // todo 重置
                extern uint8_t night[6];
                extern uint8_t custom_status[6];
                extern uint8_t sleep_init[14];
                extern uint8_t wake_init[11];
                extern uint8_t switch_change_gear[21];
                // extern
                sg_demo_info.cnt = 0;
                sg_demo_info.cnt1 = 0;
                sg_demo_info.checksum = 0xa5;
                sg_demo_info.last_light_memory = 1; /* 主+辅，重置后尚无操作记忆 */
                sg_demo_info.switch_status = 1;
                sg_demo_info.default_state = 0;
                sg_demo_info.white_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.white_temp = 55;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.night_switch = 0;
                sg_demo_info.night_bright = 50;
                sg_demo_info.change_light_status = 1;
                sg_demo_info.rhythm_switch = 0;
                sg_demo_info.first_network = 0;
                memset(sg_demo_info.custom_status, 0, 6);
                memset(sg_demo_info.sleep_init, 0, 14);
                memset(sg_demo_info.wake_init, 0, 11);
                memset(sg_demo_info.rhythm_sunlight, 0, 66);
                memset(sg_demo_info.work_mode_value, 0, 84);
                memset(sg_demo_info.switch_change_gear, 0, 21);
                memset(sg_demo_info.night, 0, 6);
                memcpy(sg_demo_info.night, night, 6);
                memcpy(sg_demo_info.custom_status, custom_status, 6);
                memcpy(sg_demo_info.sleep_init, sleep_init, 14);
                memcpy(sg_demo_info.wake_init, wake_init, 11);
                memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);
                memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
                if (sg_demo_info.change_light_status == 0)
                    gradual_time_ms = 20;
                else
                    gradual_time_ms = 1600;
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t  aux_temp1 = sg_demo_info.white_temp;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;

                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {ww, cw, aux_ww, aux_ww, 0};
                pwm_gradual_duty_set_multi(5, channels, duties);
                upload_device_all_status();
                upload_device_bool_status(DPID_SWITCH, 1);
                upload_device_bool_status(LIGHT_SWITCH, 1);
                upload_device_bool_status(AUX_SWITCH, 1);
                upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                upload_device_value_status(DPID_WHITE_BRIGHT, 100);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
                upload_device_value_status(DPID_TEMP_VALUE, 55);
                upload_device_value_status(NIGHT_LIGHT_VALUE, 50);

                sg_rhythm_info.is_timming = false;
                sg_rhythm_interrupted = true;
            }
            break;
        }

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
        uint16_t aux_ww = aux_bright1 * aux_temp1;
        uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
        light_pwm_clamp_mix(&aux_ww, &aux_cw);
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
    if(sg_demo_info.night_switch == 1){
        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
        all_duties[channel_count++] = sg_demo_info.night_bright * 100;
    }else{
        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
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
    case SWITCH_CHANGE_GEAR:
        memcpy(sg_demo_info.switch_change_gear, p_data, data_len);
        dev_report_dp_raw_sync(NULL, SWITCH_CHANGE_GEAR,
                               sg_demo_info.switch_change_gear, data_len, 5);
        break;
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
            bool main_light_on = (sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch);

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
                    sg_rhythm_info.is_timming = false;
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
    case SMART_NIGHT_LIGHT:
        memcpy(sg_demo_info.night, p_data, data_len);
        if (sg_preview_active)
        {
            if (sg_demo_info.night_switch)
                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, sg_demo_info.night_bright * 100);
            else
                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
            // 恢复PWM输出
            if (sg_demo_info.white_switch)
            {
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = (sg_demo_info.white_temp == 0) ? 1 : sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {ww, cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }

            if (sg_demo_info.aux_switch)
            {
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t white_temp1 = (sg_demo_info.white_temp == 0) ? 1 : sg_demo_info.white_temp;
                uint16_t aux_ww = aux_bright1 * white_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            sg_preview_active = false;
        }
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

        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
        all_duties[channel_count++] = p_data[0]*100;
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

    /* object type data */
    
    // bool type data
    p_obj_dp = p_all_obj_dp;
    
    /* 只上报当前运行态。初始/自定义初始仅在上电首次开灯(app_pwm cnt1==1)应用，
     * 切档后若此处再覆盖，会导致实灯已切夜灯而 App 仍显示 100%/55% 等初始值。 */
    sg_demo_info.switch_status =
        (sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch) ? 1 : 0;

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
    p_obj_dp->dpid = DPID_SWITCH_NIGHT_LIGHT;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.night_switch;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_AUX_BRIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.aux_bright;

    p_obj_dp++;
    p_obj_dp->dpid = NIGHT_LIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.night_bright;

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
    p_obj_dp->value.dp_bool = 0;

    p_obj_dp++;
    p_obj_dp->dpid = RHYTHM_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.rhythm_switch;

    TAL_PR_NOTICE("upload_device_all_status4\r\n");
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
    // p_obj_dp->dpid = DPID_SWITCH;
    // p_obj_dp->type = PROP_BOOL;
    // p_obj_dp->value.dp_bool = sg_demo_info.switch_status;

    // value type data
    // p_obj_dp++;
    p_obj_dp->dpid = DPID_WHITE_BRIGHT;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_bright;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_TEMP_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_temp;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_SWITCH_NIGHT_LIGHT;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.night_switch;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_AUX_BRIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.aux_bright;

    // p_obj_dp++;
    // p_obj_dp->dpid = LIGHT_SWITCH;
    // p_obj_dp->type = PROP_BOOL;
    // p_obj_dp->value.dp_bool = sg_demo_info.white_switch;

    // p_obj_dp++;
    // p_obj_dp->dpid = AUX_SWITCH;
    // p_obj_dp->type = PROP_BOOL;
    // p_obj_dp->value.dp_bool = sg_demo_info.aux_switch;

    TUYA_CALL_ERR_LOG(dev_query_dp_json_async(NULL, p_all_obj_dp, OBJ_DP_NUM_MAX));

    tal_free(p_all_obj_dp);
    p_all_obj_dp = NULL;
    p_obj_dp = NULL;

    /* put raw type data sync report int work queue*/
    tal_workq_schedule(WORKQ_SYSTEM, __upload_device_raw_dp, NULL);
}

/* 预留区 0x1EF000~0x1F4000=20KB：A/B 各 8KB，C 备份 4KB */
#define DEMO_PARTITION_SIZE   (20 * 1024)

static UINT_T sg_demo_start_addr = 0x1EF000;

typedef struct {
    UINT_T  write_addr;
    UINT_T  read_addr;
    UCHAR_T init_flag;
} DEMO_FLASH_OP_T;

static DEMO_FLASH_OP_T sg_demo_flash_op = {0};

/* ================== 三块定义 ================== */
#define BLOCK_AB_SIZE (4096)  /* A/B 各 8KB，减少换块擦除次数 */
#define BLOCK_C_SIZE  (4096)  /* C 保持 4KB */
#define BLOCK_A       (sg_demo_start_addr)
#define BLOCK_B       (sg_demo_start_addr + BLOCK_AB_SIZE)
#define BLOCK_C       (sg_demo_start_addr + 2 * BLOCK_AB_SIZE)

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

static UINT_T __block_size(UINT_T block_addr)
{
    return (block_addr == BLOCK_C) ? BLOCK_C_SIZE : BLOCK_AB_SIZE;
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

/* 本块内第一个可追加空位；满则返回 block+size */
static UINT_T __demo_find_first_blank(UINT_T block)
{
    UINT_T cur = block + sizeof(BLOCK_HDR);
    UINT_T end = block + __block_size(block);
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
static BOOL_T __scan_block(UINT_T addr, UINT_T block_size, DEMO_INFO_T *out, UINT_T *last_addr,
                          BOOL_T check_calibration, uint8_t cal_value)
{
    UINT_T cur = addr + sizeof(BLOCK_HDR);
    UINT_T end = addr + block_size;

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
    return __scan_block(block_addr, __block_size(block_addr), &tmp, &tmp_addr, FALSE, 0);
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
        TAL_PR_NOTICE("Backup: Current data invalid");
        return -2;
    }
    
    // 准备要备份的数据
    DEMO_INFO_T backup_data;
    memcpy(&backup_data, &sg_demo_info, sizeof(sg_demo_info));
    backup_data.time_stamp++;  // 增加时间戳
    backup_data.checksum = __demo_calc_checksum(&backup_data);
    
    // 擦除C块
    if (tkl_flash_erase(BLOCK_C, BLOCK_C_SIZE) != OPRT_OK) {
        TAL_PR_NOTICE("Backup: Failed to erase block C");
        return -3;
    }
    
    // 写入C块头部
    BLOCK_HDR hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
    if (tkl_flash_write(BLOCK_C, (UCHAR_T*)&hdr, sizeof(hdr)) != OPRT_OK) {
        TAL_PR_NOTICE("Backup: Failed to write C block header");
        return -4;
    }
    
    // 写入数据到C块
    UINT_T write_addr = BLOCK_C + sizeof(BLOCK_HDR);
    if (tkl_flash_write(write_addr, (UCHAR_T*)&backup_data, sizeof(backup_data)) != OPRT_OK) {
        TAL_PR_NOTICE("Backup: Failed to write data to C block");
        return -5;
    }
    
    // 验证写入
    DEMO_INFO_T verify;
    if (tkl_flash_read(write_addr, (UCHAR_T*)&verify, sizeof(verify)) != OPRT_OK) {
        TAL_PR_NOTICE("Backup: Failed to read back from C block");
        return -6;
    }
    
    if (!__demo_is_valid_data(&verify)) {
        TAL_PR_NOTICE("Backup: Data verification failed");
        return -7;
    }
    
    TAL_PR_NOTICE("Backup to C block successful, calibration=%d", backup_data.calibration);
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
        if (__scan_block(BLOCK_A, BLOCK_AB_SIZE, &da, &la, TRUE, 3)) {
            tkl_log_output("Init: Found calibration=3 in block A, ts=%lu", da.time_stamp);
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
        if (__scan_block(BLOCK_B, BLOCK_AB_SIZE, &db, &lb, TRUE, 3)) {
            tkl_log_output("Init: Found calibration=3 in block B, ts=%lu", db.time_stamp);
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
        tkl_log_output("Init: Using calibration=3 from block 0x%X (ts=%lu)", 
                     best_cal3_block, best_cal3_data.time_stamp);
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
            if (__scan_block(BLOCK_C, BLOCK_C_SIZE, &dc, &lc, TRUE, 3)) {
                tkl_log_output("Init: Found calibration=3 in block C, ts=%lu", dc.time_stamp);
                memcpy(&found_data, &dc, sizeof(dc));
                found_addr = lc;
                found_block = BLOCK_C;
                found_version = hc.version;
                found_timestamp = dc.time_stamp;
                found_valid = TRUE;
                
                // 从C块恢复，需要写到AB块
                tkl_log_output("Init: Recovering from C block to A block");
                
                // 选择A块作为恢复目标
                if (tkl_flash_erase(BLOCK_A, BLOCK_AB_SIZE) != OPRT_OK) {
                    tkl_log_output("Init: Failed to erase block A for recovery");
                    found_valid = FALSE;
                } else {
                    BLOCK_HDR new_hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
                    if (tkl_flash_write(BLOCK_A, (UCHAR_T*)&new_hdr, sizeof(new_hdr)) != OPRT_OK) {
                        tkl_log_output("Init: Failed to write block A header");
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
                                tkl_log_output("Init: Recovered from C to A block successfully");
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
            if (__scan_block(BLOCK_A, BLOCK_AB_SIZE, &tmp, &tmp_addr, FALSE, 0)) {
                tkl_log_output("Init: Found valid data in block A, ts=%lu, calibration=%d",
                            tmp.time_stamp, tmp.calibration);
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
            if (__scan_block(BLOCK_B, BLOCK_AB_SIZE, &tmp, &tmp_addr, FALSE, 0)) {
                tkl_log_output("Init: Found valid data in block B, ts=%lu, calibration=%d",
                            tmp.time_stamp, tmp.calibration);
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
            tkl_log_output("Init: Using normal data from block 0x%X, ts=%lu, calibration=%d",
                         found_block, found_timestamp, found_data.calibration);
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
        UINT_T block_end = g_active_block + BLOCK_AB_SIZE;
        if (g_write_addr + sizeof(DEMO_INFO_T) > block_end) {
            // 当前块已满，需要切换到另一块
            UINT_T other_block = (g_active_block == BLOCK_A) ? BLOCK_B : BLOCK_A;
            
            // 擦除另一块
            tkl_flash_erase(other_block, BLOCK_AB_SIZE);
            
            // 初始化另一块头部
            BLOCK_HDR other_hdr = {BLOCK_MAGIC, g_version + 1, BLOCK_VALID};
            tkl_flash_write(other_block, (UCHAR_T*)&other_hdr, sizeof(other_hdr));
            
            // 切换到另一块
            g_active_block = other_block;
            g_version++;
            g_write_addr = other_block + sizeof(BLOCK_HDR);
            
            tkl_log_output("Init: Active block full, switched to block 0x%X", g_active_block);
        }
        
        tkl_log_output("Init: Using valid data, calibration=%d, ts=%lu", 
                     sg_demo_info.calibration, sg_demo_info.time_stamp);

        /* 已校准设备若节律表全 0（旧版本默认初始化遗漏），补默认节律，避免上报全 0 */
        {
            BOOL_T rhythm_empty = TRUE;
            for (int i = 0; i < 66; i++) {
                if (sg_demo_info.rhythm_sunlight[i] != 0) {
                    rhythm_empty = FALSE;
                break;
            }
        }
            if (rhythm_empty) {
                extern uint8_t rhythm_sunlight1[66];
                memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);
                tkl_log_output("Init: rhythm empty in flash, restored rhythm_sunlight1");
            }
        }
    } else {
        // 5. 没有找到任何有效数据，使用默认数据
        // 5. 没有找到任何有效数据，使用默认数据
        tkl_log_output("Init: No valid data found, using default");
        
        // 擦除所有块，确保从干净状态开始
        tkl_log_output("Init: Erasing all blocks for clean start");
        
        // 擦除A块
        if (tkl_flash_erase(BLOCK_A, BLOCK_AB_SIZE) != OPRT_OK) {
            tkl_log_output("Init: Warning: Failed to erase block A");
        } else {
            tkl_log_output("Init: Block A erased successfully");
        }
        
        // 擦除B块
        if (tkl_flash_erase(BLOCK_B, BLOCK_AB_SIZE) != OPRT_OK) {
            tkl_log_output("Init: Warning: Failed to erase block B");
        } else {
            tkl_log_output("Init: Block B erased successfully");
        }
        
        // 擦除C块
        if (tkl_flash_erase(BLOCK_C, BLOCK_C_SIZE) != OPRT_OK) {
            tkl_log_output("Init: Warning: Failed to erase block C");
        } else {
            tkl_log_output("Init: Block C erased successfully");
        }
        
        // 初始化A块
        BLOCK_HDR hdr = {BLOCK_MAGIC, 1, BLOCK_VALID};
        if (tkl_flash_write(BLOCK_A, (UCHAR_T*)&hdr, sizeof(hdr)) != OPRT_OK) {
            tkl_log_output("Init: Failed to write block A header");
            // 写入失败，尝试使用内存中的默认值继续
        }
        
        // 设置默认数据
        memset(&sg_demo_info, 0, sizeof(sg_demo_info));
        sg_demo_info.cnt = 0;
        sg_demo_info.cnt1 = 0;
        sg_demo_info.gear_memory = 1;
        sg_demo_info.last_light_memory = 1; /* 主+辅，重置后尚无操作记忆 */
        sg_demo_info.switch_status = 1;
        sg_demo_info.default_state = 0;
        sg_demo_info.white_switch = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.white_temp = 55;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.aux_bright = 100;
        sg_demo_info.night_switch = 0;
        sg_demo_info.night_bright = 50;
        sg_demo_info.change_light_status = 1;
        sg_demo_info.rhythm_switch = 0;
        sg_demo_info.first_network = 0;
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
        extern uint8_t rhythm_sunlight1[66];

        if (custom_status) memcpy(sg_demo_info.custom_status, custom_status, 6);
        if (sleep_init) memcpy(sg_demo_info.sleep_init, sleep_init, 14);
        if (wake_init) memcpy(sg_demo_info.wake_init, wake_init, 11);
        if (switch_change_gear) memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
        if (night) memcpy(sg_demo_info.night, night, 6);
        if (collect) memcpy(sg_demo_info.collect, collect, 7);
        /* 与出厂复位/按键复位一致：必须写入默认节律，否则上报 RHYTHM_MODE 全 0 */
        memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);

        // 设置时间戳和校验和
        sg_demo_info.time_stamp = 1;
        
        // 重要：设置默认的calibration值
        sg_demo_info.calibration = 0;  // 默认校准值为1
        
        sg_demo_info.checksum = __demo_calc_checksum(&sg_demo_info);
        
        g_active_block = BLOCK_A;
        g_version = 1;
        g_write_addr = BLOCK_A + sizeof(BLOCK_HDR);

        TAL_PR_NOTICE("Init: Default data set, calibration=%d", sg_demo_info.calibration);
    }
    
    sg_demo_flash_op.init_flag = 1;
    tkl_log_output("Init complete: active_block=0x%X, calibration=%d, ts=%lu, write_addr=0x%X", 
             g_active_block, sg_demo_info.calibration, sg_demo_info.time_stamp, g_write_addr);
}

/* ================== 换到另一块并写入最新数据 ================== */
static int __save_switch_to_other_block(DEMO_INFO_T *data)
{
    UINT_T new_block = (g_active_block == BLOCK_A) ? BLOCK_B : BLOCK_A;
    UINT_T end = new_block + BLOCK_AB_SIZE;
    UINT_T addr;

    TAL_PR_NOTICE("Save: Block 0x%X full/failed, switching to 0x%X",
                  g_active_block, new_block);

    if (tkl_flash_erase(new_block, BLOCK_AB_SIZE) != OPRT_OK) {
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

/* ================== 保存函数 ================== */
int device_config_save1(void)
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

    end = g_active_block + BLOCK_AB_SIZE;

    /* 写指针可能落后，先对齐到第一个空白格 */
    if (g_write_addr < g_active_block + sizeof(BLOCK_HDR) ||
        g_write_addr + sizeof(DEMO_INFO_T) > end) {
        g_write_addr = __demo_find_first_blank(g_active_block);
    }

    /* 当前块已无空位：擦另一块，从头写最新数据 */
    if (g_write_addr + sizeof(DEMO_INFO_T) > end) {
        ret = __save_switch_to_other_block(&data);
        save_in_progress = 0;
        tal_mutex_unlock(sg_save_mutex);
        return ret;
    }

    /* 只写空白格；已占用/坏洞跳过，避免 NOR 半写把扫描截断 */
    while (g_write_addr + sizeof(DEMO_INFO_T) <= end) {
        if (tkl_flash_read(g_write_addr, (UCHAR_T *)&slot, sizeof(slot)) != OPRT_OK) {
            TAL_PR_NOTICE("Save: Read slot failed at 0x%X, skip", g_write_addr);
            g_write_addr += sizeof(DEMO_INFO_T);
            continue;
        }

        if (!__demo_is_blank_slot(&slot)) {
            TAL_PR_NOTICE("Save: Slot not blank at 0x%X, skip", g_write_addr);
            g_write_addr += sizeof(DEMO_INFO_T);
            continue;
        }

        TAL_PR_NOTICE("Save: Writing to 0x%X, ts=%lu, calibration=%d",
                      g_write_addr, data.time_stamp, data.calibration);

        if (tkl_flash_write(g_write_addr, (UCHAR_T*)&data, sizeof(data)) == OPRT_OK) {
            DEMO_INFO_T v;
            tkl_flash_read(g_write_addr, (UCHAR_T*)&v, sizeof(v));
            if (__demo_is_valid_data(&v)) {
                g_write_addr += sizeof(DEMO_INFO_T);
                memcpy(&sg_demo_info, &data, sizeof(data));
                TAL_PR_NOTICE("Save: Write successful, next write at 0x%X", g_write_addr);
                ret = (int)sizeof(DEMO_INFO_T);
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

    /* 本块试完都失败：换另一块 */
    ret = __save_switch_to_other_block(&data);
    save_in_progress = 0;
    tal_mutex_unlock(sg_save_mutex);
    return ret;
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
    BOOL_T valid = __scan_block(BLOCK_C, BLOCK_C_SIZE, &data, &addr, FALSE, 0);
    
    if (has_valid_data) *has_valid_data = valid;
    if (calibration && valid) *calibration = data.calibration;
    
    return valid ? 0 : -3;
}
