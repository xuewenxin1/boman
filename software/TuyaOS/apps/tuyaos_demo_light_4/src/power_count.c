/**
 * @file kv_power_count.c
 * @brief 上电计数：KV 存储 + 阈值触发恢复出厂（light_4 无 AC 过零硬件）
 *
 * 与 1.0.19 一致：冷启动 test_gpio1 计次并出光；约 2s 后 power_check
 * 判断 cnt>=9 则重置进配网。无 GPIO 过零中断、无市电 FSM。
 */

#include "tuya_cloud_types.h"
#include "tuya_ws_db.h"
#include "tal_log.h"
#include "tal_sw_timer.h"
#include "power_count.h"
#include "dp_process.h"
#include "tal_thread.h"
#include "tkl_timer.h"
#include "tuya_iot_config.h"
#include "tuya_cloud_wifi_defs.h"
#include "tuya_iot_com_api.h"
#include "app_light_tm_rhythm.h"
#include "app_pwm.h"
#include <string.h>

STATIC TIMER_ID sg_rst_timer_id = NULL;
STATIC TIMER_ID sg_count1 = NULL;

/**********************************************************
 * @brief 定时器回调（超时自动清零）
 **********************************************************/
STATIC VOID kv_rst_timeout_cb(VOID *arg)
{
    extern DEMO_INFO_T sg_demo_info;
    sg_demo_info.cnt = 0;
    sg_demo_info.cnt1 = 0;
    device_config_save1();
    tal_sw_timer_stop(sg_rst_timer_id);
}

BOOL_T kv_power_on_count(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    return (sg_demo_info.cnt >= 9) ? TRUE : FALSE;
}

STATIC VOID power_check_timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;

    tkl_log_output("duandian,cnt = %d,cnt1 = %d\r\n",
                   sg_demo_info.cnt, sg_demo_info.cnt1);
    device_config_save1();

    if (sg_sleep_is_timing) {
        if (sg_demo_info.sleep_init[0] == 0x01) {
            sg_demo_info.sleep_init[0] = 0x0;
            app_light_stop_sleep_timer();
            dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                                   sg_demo_info.sleep_init, 14, 5);
        }
    }
    if (sg_wake_is_timing) {
        if (sg_demo_info.wake_init[0] == 0x01) {
            sg_demo_info.wake_init[0] = 0x0;
            app_light_stop_wake_timer();
            dev_report_dp_raw_sync(NULL, WAKEUP_MODE,
                                   sg_demo_info.wake_init, 11, 5);
        }
    }

    tal_sw_timer_start(sg_rst_timer_id, 5000, TAL_TIMER_ONCE);
    if (sg_demo_info.rhythm_switch) {
        app_light_stop_today_rhythm_timer();
    }
    __execute_power_loss_recovery();
}

VOID __execute_power_loss_recovery(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;

    if (kv_power_on_count() != TRUE) {
        return;
    }

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

    extern uint8_t custom_status[6];
    extern uint8_t night[6];
    extern uint8_t sleep_init[14];
    extern uint8_t wake_init[11];
    extern uint8_t switch_change_gear[21];
    extern uint8_t collect[7];
    extern uint8_t rhythm_sunlight1[66];

    memcpy(sg_demo_info.custom_status, custom_status, 6);
    memcpy(sg_demo_info.sleep_init, sleep_init, 14);
    memcpy(sg_demo_info.wake_init, wake_init, 11);
    memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);
    memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
    memcpy(sg_demo_info.night, night, 6);
    memcpy(sg_demo_info.collect, collect, 7);
    device_config_save1_force();
    tuya_iot_wf_gw_reset();
}

/* light_4 无过零脚：保留空实现，兼容头文件声明 */
VOID test_gpio(VOID)
{
}

VOID test_gpio1(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(kv_rst_timeout_cb, NULL, &sg_rst_timer_id),
                       __EXIT);
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb, NULL, &sg_count1),
                       __EXIT);
    tal_sw_timer_start(sg_count1, 2000, TAL_TIMER_ONCE);

    sg_demo_info.cnt++;
    sg_demo_info.cnt1++;
    app_pwm();
    device_config_save1_force();
__EXIT:
    return;
}

VOID example_gpio(VOID)
{
}
