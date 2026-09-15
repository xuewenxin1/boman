/**
 * @file examples_driver_pwm.c
 * @author www.tuya.com
 * @brief 一个简单的tkl pwm接口使用演示程序，可以通过命令行执行
 * @version 0.1
 * @date 2022-05-20
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include "tuya_cloud_types.h"
#include "light_pwm_mix.h"

#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "app_mode.h"
#include "app_pwm.h"
#include "dp_process.h"
#include "pwm_gradual.h"
/***********************************************************
*************************micro define***********************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/

/***********************************************************
***********************function define**********************
***********************************************************/
MODE_DATA_T g_mode_data = {0};

/**
 * @brief 初始化
 */
VOID app_mode_init(VOID)
{
    extern DEMO_INFO_T sg_demo_info;

    memset(&g_mode_data, 0, sizeof(g_mode_data));
    /* 上电从 Flash 中的情景 RAW 恢复到运行态，避免重启后情景为空 */
    app_mode_save_raw(sg_demo_info.work_mode_value, MODE_GROUP_NUM * MODE_ITEM_NUM);
}

/**
 * @brief 保存 RAW 数据 (dp102 下发时调用)
 * @param raw: 下发的原始数据
 * @param len: 数据长度
 */
VOID app_mode_save_raw(UCHAR_T *raw, UINT_T len)
{
    if (raw == NULL || len < (MODE_GROUP_NUM * MODE_ITEM_NUM))
    {
        return;
    }

    for (UINT8_T i = 0; i < MODE_GROUP_NUM; i++)
    {
        g_mode_data.groups[i].main_sw = raw[i * MODE_ITEM_NUM + MODE_IDX_MAIN_SW];
        g_mode_data.groups[i].main_bright = raw[i * MODE_ITEM_NUM + MODE_IDX_MAIN_BRIGHT];
        g_mode_data.groups[i].aux_sw = raw[i * MODE_ITEM_NUM + MODE_IDX_AUX_SW];
        g_mode_data.groups[i].aux_bright = raw[i * MODE_ITEM_NUM + MODE_IDX_AUX_BRIGHT];
        g_mode_data.groups[i].color_temp = raw[i * MODE_ITEM_NUM + MODE_IDX_CT];
        g_mode_data.groups[i].night_sw = raw[i * MODE_ITEM_NUM + MODE_IDX_NIGHT_SW];
        g_mode_data.groups[i].night_bright = raw[i * MODE_ITEM_NUM + MODE_IDX_NIGHT_BRIGHT];
    }
}

/**
 * @brief 执行某个模式组 (dp21 下发时调用)
 * @param group_idx: 枚举值 (1~13) 对应 group[0~12]
 */
VOID app_mode_apply_group(UINT8_T group_idx)
{
    extern DEMO_INFO_T sg_demo_info;
    if (group_idx < 3 || group_idx > 15)
    {
        return;
    }
    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;
    MODE_GROUP_T *g = &g_mode_data.groups[group_idx - 4];

    // TODO: 根据具体硬件执行
    if (g->main_sw == 1)
    {
        sg_demo_info.white_switch = 1;
        sg_demo_info.white_bright = g->main_bright;
        sg_demo_info.white_temp = g->color_temp;
        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
        uint8_t white_temp1 = sg_demo_info.white_temp;
        // if (white_temp1 == 0)
        //     white_temp1 = 1;
        uint16_t ww = white_bright1 * white_temp1;
        uint16_t cw = white_bright1 * 100 - ww;
        light_pwm_clamp_mix(&ww, &cw);
all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = ww;
        
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = cw;
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, g->main_bright);
        upload_device_value_status(DPID_TEMP_VALUE, g->color_temp);
    }
    else
    {
        sg_demo_info.white_switch = 0;
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = 0;
        
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = 0;
        upload_device_bool_status(LIGHT_SWITCH, 0);
    }
    if (g->aux_sw == 1)
    {
        sg_demo_info.aux_switch = 1;
        sg_demo_info.aux_bright = g->aux_bright;
        sg_demo_info.white_temp = g->color_temp;
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
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, g->aux_bright);
        upload_device_value_status(DPID_TEMP_VALUE, g->color_temp);
    }
    else
    {
        sg_demo_info.aux_switch = 0;
        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = 0;
        
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = 0;
        upload_device_bool_status(AUX_SWITCH, 0);
    }
    if (g->night_sw == 1)
    {
        sg_demo_info.night_switch = 1;
        sg_demo_info.night_bright = g->night_bright;
        sg_demo_info.white_switch = 0;
        sg_demo_info.aux_switch = 0;
        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
        all_duties[channel_count++] = g->night_bright * 100;
        upload_device_bool_status(LIGHT_SWITCH, 0);
        upload_device_bool_status(AUX_SWITCH, 0);
        upload_device_value_status(DPID_WHITE_BRIGHT, 0);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 0);
        upload_device_value_status(DPID_TEMP_VALUE, 0);
        
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 1);
        upload_device_value_status(NIGHT_LIGHT_VALUE, g->night_bright);
    }
    else
    {
        sg_demo_info.night_switch = 0;
        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
        all_duties[channel_count++] = 0;
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
    }
    if(sg_demo_info.white_switch == 0 && sg_demo_info.night_switch == 0 && sg_demo_info.aux_switch == 0){
        sg_demo_info.switch_status = 0;
        upload_device_bool_status(DPID_SWITCH, 0);
    }else if(sg_demo_info.white_switch == 1 || sg_demo_info.night_switch == 1 || sg_demo_info.aux_switch == 1){
        sg_demo_info.switch_status = 1;
        upload_device_bool_status(DPID_SWITCH, 1);
    }
    if (channel_count > 0)
    {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }
}

VOID app_scene_apply_builtin(UINT8_T mode_idx)
{
    extern DEMO_INFO_T sg_demo_info;
    TUYA_PWM_NUM_E all_channels[5];
    UINT32_T all_duties[5];
    UINT8_T channel_count = 0;

    sg_demo_info.night_switch = 0;
    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
    all_duties[channel_count++] = 0;

    switch (mode_idx) {
    case 0: /* 阅读 */
        sg_demo_info.white_switch = 1;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.aux_bright = 100;
        sg_demo_info.white_temp = 55;
        {
            uint16_t white_bright1 = 5 + (100 - 1) * 95 / 99;
            uint16_t ww = white_bright1 * 55;
            uint16_t cw = white_bright1 * 100 - ww;
            light_pwm_clamp_mix(&ww, &cw);
            all_channels[channel_count] = BRIGHT_PWM;
            all_duties[channel_count++] = ww;
            all_channels[channel_count] = TEMP_PWM;
            all_duties[channel_count++] = cw;
            all_channels[channel_count] = AUX_BRIGHT_PWM;
            all_duties[channel_count++] = ww;
            all_channels[channel_count] = AUX_TEMP_PWM;
            all_duties[channel_count++] = cw;
        }
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, 100);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
        upload_device_value_status(DPID_TEMP_VALUE, 55);
        break;
    case 1: /* 日光 */
        sg_demo_info.white_switch = 1;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.aux_bright = 100;
        sg_demo_info.white_temp = 100;
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = 9997;
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = 3;
        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = 9997;
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = 3;
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, 100);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 100);
        upload_device_value_status(DPID_TEMP_VALUE, 100);
        break;
    case 2: /* 就餐 */
        sg_demo_info.white_switch = 1;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.aux_bright = 30;
        sg_demo_info.white_temp = 35;
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = 100 * 35;
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = 100 * 65;
        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = 30 * 35;
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = 30 * 65;
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, 100);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 30);
        upload_device_value_status(DPID_TEMP_VALUE, 35);
        break;
    case 3: /* 温馨 */
    default:
        sg_demo_info.white_switch = 1;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_bright = 30;
        sg_demo_info.aux_bright = 70;
        sg_demo_info.white_temp = 0;
        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = 30;
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = 30 * 99;
        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = 70;
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = 70 * 99;
        upload_device_bool_status(DPID_SWITCH, 1);
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_value_status(DPID_WHITE_BRIGHT, 30);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, 70);
        upload_device_value_status(DPID_TEMP_VALUE, 0);
        break;
    }

    if (channel_count > 0) {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }
}

/**
 * @brief 上报某组模式数据
 * @param group_idx: 枚举值 (1~13)
 */
VOID app_mode_report_group(UINT8_T group_idx)
{
    if (group_idx == 0 || group_idx > MODE_GROUP_NUM)
    {
        return;
    }

    MODE_GROUP_T *g = &g_mode_data.groups[group_idx - 1];
}
