/**
 * @file app_mode.c
 * @brief 情景模式（light_4：主+辅；与 1.0.19 一致，不处理夜灯 PWM）
 */

#include "tuya_cloud_types.h"

#include "tal_log.h"
#include "tal_thread.h"
#include "tal_system.h"
#include "app_mode.h"
#include "app_pwm.h"
#include "dp_process.h"
#include "pwm_gradual.h"
#include "light_pwm_mix.h"
#include <string.h>

MODE_DATA_T g_mode_data = {0};

VOID app_mode_init(VOID)
{
    extern DEMO_INFO_T sg_demo_info;

    memset(&g_mode_data, 0, sizeof(g_mode_data));
    app_mode_save_raw(sg_demo_info.work_mode_value, MODE_GROUP_NUM * MODE_ITEM_NUM);
}

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
    }
}

/**
 * @param group_idx: 情景枚举，与 light_5 / 原 light_4 源码一致：groups[group_idx-4]
 * @note 仅出主/辅；night 字段只存不驱动
 */
VOID app_mode_apply_group(UINT8_T group_idx)
{
    extern DEMO_INFO_T sg_demo_info;
    if (group_idx < 4 || group_idx > 15)
    {
        return;
    }

    TUYA_PWM_NUM_E all_channels[4];
    UINT32_T all_duties[4];
    UINT8_T channel_count = 0;
    MODE_GROUP_T *g = &g_mode_data.groups[group_idx - 5];

    if (g->main_sw == 1)
    {
        sg_demo_info.white_switch = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.white_bright = g->main_bright;
        sg_demo_info.white_temp = g->color_temp;
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
        upload_device_bool_status(DPID_SWITCH, 1);
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
        sg_demo_info.switch_status = 1;
        sg_demo_info.aux_bright = g->aux_bright;
        sg_demo_info.white_temp = g->color_temp;
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
        upload_device_bool_status(DPID_SWITCH, 1);
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
    if(sg_demo_info.white_switch == 0 &&  sg_demo_info.aux_switch == 0){
        sg_demo_info.switch_status = 0;
        upload_device_bool_status(DPID_SWITCH, 0);
    }else if(sg_demo_info.white_switch == 1 ||  sg_demo_info.aux_switch == 1){
        sg_demo_info.switch_status = 1;
        upload_device_bool_status(DPID_SWITCH, 1);
    }

    if (channel_count > 0)
    {
        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
    }
}

VOID app_mode_report_group(UINT8_T group_idx)
{
    if (group_idx == 0 || group_idx > MODE_GROUP_NUM)
    {
        return;
    }

    MODE_GROUP_T *g = &g_mode_data.groups[group_idx - 1];
    (void)g;
}
