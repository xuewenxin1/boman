/**
 * @file kv_power_count.c
 * @brief 上电计数：KV 存储 + 阈值触发恢复出厂 + AC 掉电/恢复切档
 *
 * AC 切档规则：
 * 1. 无下降沿持续超过 POWER_LOSS_DETECT_THRESHOLD_MS(50ms) → 判定 AC 掉电
 * 2. 掉电后再检测到 AC 正常：连续三个有效下降沿，相邻间隔 2~15ms → 切档
 *    （<2ms 重复沿忽略，>15ms 则从第 1 沿重新计）
 * 3. 冷启动 test_gpio1 已 cnt/cnt1++ 并出光，其后第一次 AC 恢复不切档
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
#include "app_pwm1.h"

#define GPIO_IRQ_PIN             TUYA_GPIO_NUM_15

STATIC TIMER_ID sg_rst_timer_id = NULL;
STATIC TIMER_ID sg_count1 = NULL;
STATIC TIMER_ID sg_count2 = NULL;
STATIC TIMER_ID count_timer = NULL;

uint32_t count_irq = 0;

/* AC 掉电：无下降沿超过该时间(ms)，与原先 POWER_LOSS_DETECT_THRESHOLD_MS 一致 */
#define POWER_LOSS_DETECT_THRESHOLD_MS  50
/* AC 正常恢复：相邻下降沿间隔 2~15ms，需连续三个沿 */
#define AC_NORMAL_EDGE_MIN_MS           2
#define AC_NORMAL_EDGE_MAX_MS          15
/* 两次切档最小间隔(ms)，防抖 */
#define AC_GEAR_SWITCH_DEBOUNCE_MS   500

typedef enum {
    AC_DETECT_IDLE = 0,            /* 监测中，AC 正常有波形 */
    AC_DETECT_LOSS_CONFIRMED,      /* 已确认掉电，等待恢复 */
    AC_DETECT_RECOVERY_EDGE1,        /* 已见第 1 个恢复沿，等第 2 个 */
    AC_DETECT_RECOVERY_EDGE2,        /* 已见第 2 个恢复沿，等第 3 个 */
} AC_DETECT_STATE_E;

STATIC AC_DETECT_STATE_E sg_ac_state = AC_DETECT_IDLE;
STATIC BOOL_T sg_ac_seen_edge = FALSE;
STATIC uint32_t sg_ac_last_edge_ms = 0;
STATIC uint32_t sg_ac_prev_recovery_edge_ms = 0;
STATIC uint32_t sg_ac_last_switch_ms = 0;
STATIC BOOL_T sg_skip_next_ac_gear = FALSE;

STATIC VOID ac_loss_poll(VOID);

STATIC CONST CHAR_T *ac_state_name(AC_DETECT_STATE_E st)
{
    switch (st) {
    case AC_DETECT_IDLE:             return "IDLE";
    case AC_DETECT_LOSS_CONFIRMED:   return "LOSS";
    case AC_DETECT_RECOVERY_EDGE1:   return "EDGE1";
    case AC_DETECT_RECOVERY_EDGE2:   return "EDGE2";
    default:                         return "?";
    }
}

STATIC VOID kv_rst_timeout_cb(VOID *arg)
{
    extern DEMO_INFO_T sg_demo_info;
    sg_demo_info.cnt = 0;
    sg_demo_info.cnt1 = 0;
    device_config_save1();
    tal_sw_timer_stop(sg_rst_timer_id);
}

STATIC VOID countirq_cb(VOID *arg)
{
    (void)arg;
    count_irq++;
    ac_loss_poll();
}

STATIC BOOL_T ac_edge_interval_valid(uint32_t dt_ms)
{
    return (dt_ms >= AC_NORMAL_EDGE_MIN_MS && dt_ms <= AC_NORMAL_EDGE_MAX_MS);
}

STATIC VOID ac_recovery_reset_first_edge(uint32_t now, CONST CHAR_T *reason)
{
    sg_ac_prev_recovery_edge_ms = now;
    sg_ac_state = AC_DETECT_RECOVERY_EDGE1;
    TAL_PR_NOTICE("[AC] ->EDGE1 reason=%s t=%u\r\n", reason, now);
}

STATIC VOID ac_on_gear_switch(VOID)
{
    extern DEMO_INFO_T sg_demo_info;

    if (sg_skip_next_ac_gear) {
        sg_skip_next_ac_gear = FALSE;
        TAL_PR_NOTICE("[AC] skip first AC gear after cold boot\r\n");
        return;
    }

    if (sg_ac_last_switch_ms != 0 &&
        (count_irq - sg_ac_last_switch_ms) < AC_GEAR_SWITCH_DEBOUNCE_MS) {
        TAL_PR_NOTICE("[AC] gear DEBOUNCE skip gap=%u\r\n",
                      count_irq - sg_ac_last_switch_ms);
        return;
    }
    sg_ac_last_switch_ms = count_irq;

    sg_demo_info.cnt++;
    sg_demo_info.cnt1++;
    TAL_PR_NOTICE("[AC] GEAR SWITCH ok cnt=%d cnt1=%d\r\n",
                  sg_demo_info.cnt, sg_demo_info.cnt1);
    tal_sw_timer_start(sg_count1, 100, TAL_TIMER_ONCE);
}

STATIC VOID ac_recovery_on_next_edge(uint32_t now, AC_DETECT_STATE_E cur_st)
{
    uint32_t dt = now - sg_ac_prev_recovery_edge_ms;

    if (dt < AC_NORMAL_EDGE_MIN_MS) {
        return;
    }

    if (dt > AC_NORMAL_EDGE_MAX_MS) {
        TAL_PR_NOTICE("[AC] dt=%u >%u restart\r\n", dt, AC_NORMAL_EDGE_MAX_MS);
        ac_recovery_reset_first_edge(now, "interval_too_long");
        return;
    }

    if (cur_st == AC_DETECT_RECOVERY_EDGE1) {
        sg_ac_prev_recovery_edge_ms = now;
        sg_ac_state = AC_DETECT_RECOVERY_EDGE2;
        TAL_PR_NOTICE("[AC] ->EDGE2 dt=%u OK\r\n", dt);
    } else {
        TAL_PR_NOTICE("[AC] ->EDGE3 dt=%u OK, trigger gear\r\n", dt);
        sg_ac_state = AC_DETECT_IDLE;
        ac_on_gear_switch();
    }
}

STATIC VOID ac_on_falling_edge(VOID)
{
    uint32_t now = count_irq;
    uint32_t gap = (sg_ac_last_edge_ms != 0) ? (now - sg_ac_last_edge_ms) : 0;
    AC_DETECT_STATE_E st = sg_ac_state;

    sg_ac_seen_edge = TRUE;
    sg_ac_last_edge_ms = now;

    if (st != AC_DETECT_IDLE || gap >= 40) {
        TAL_PR_NOTICE("[AC] fall st=%s t=%u gap=%u\r\n",
                      ac_state_name(st), now, gap);
    }

    switch (st) {
    case AC_DETECT_IDLE:
        break;

    case AC_DETECT_LOSS_CONFIRMED:
        ac_recovery_reset_first_edge(now, "loss_recover_e1");
        break;

    case AC_DETECT_RECOVERY_EDGE1:
        ac_recovery_on_next_edge(now, AC_DETECT_RECOVERY_EDGE1);
        break;

    case AC_DETECT_RECOVERY_EDGE2:
        ac_recovery_on_next_edge(now, AC_DETECT_RECOVERY_EDGE2);
        break;

    default:
        TAL_PR_NOTICE("[AC] bad state=%d reset IDLE\r\n", st);
        sg_ac_state = AC_DETECT_IDLE;
        break;
    }
}

STATIC VOID ac_loss_poll(VOID)
{
    uint32_t silent_ms;

    if (!sg_ac_seen_edge || sg_ac_last_edge_ms == 0) {
        return;
    }

    silent_ms = count_irq - sg_ac_last_edge_ms;
    if (silent_ms < POWER_LOSS_DETECT_THRESHOLD_MS) {
        return;
    }

    if (sg_ac_state == AC_DETECT_IDLE) {
        sg_ac_state = AC_DETECT_LOSS_CONFIRMED;
        TAL_PR_NOTICE("[AC] ->LOSS silent=%ums, wait 3 edges %u-%ums\r\n",
                      silent_ms, AC_NORMAL_EDGE_MIN_MS, AC_NORMAL_EDGE_MAX_MS);
    }
}

STATIC VOID power_check_timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;

    TAL_PR_NOTICE("[AC] power_check_timer_cb cnt=%d cnt1=%d\r\n",
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
    /* 短断电切档：打断今日节律；长断电冷启动不走此回调 */
    if (sg_demo_info.rhythm_switch) {
        app_light_stop_today_rhythm_timer();
    }
    tal_sw_timer_start(sg_rst_timer_id, 5000, TAL_TIMER_ONCE);
    app_pwm();
    __execute_power_loss_recovery();
}

STATIC VOID power_check_timer_cb1(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;

    tkl_log_output("duandian1,cnt = %d,cnt1 = %d\r\n", sg_demo_info.cnt, sg_demo_info.cnt1);
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
}

BOOL_T kv_power_on_count(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    return (sg_demo_info.cnt >= 9) ? TRUE : FALSE;
}

VOID __execute_power_loss_recovery(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    TAL_PR_NOTICE("duandian2,cnt = %d,cnt1 = %d\r\n", sg_demo_info.cnt, sg_demo_info.cnt1);
    if (kv_power_on_count() == TRUE) {
        sg_demo_info.cnt = 0;
        sg_demo_info.cnt1 = 0;
        sg_demo_info.checksum = 0xa5;
        sg_demo_info.switch_status = 1;
        sg_demo_info.default_state = 0;
        sg_demo_info.white_switch = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.last_light_memory = 2;
        sg_demo_info.white_temp = 55;
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
        device_config_save1();
        tuya_iot_wf_gw_reset();
    }
}

STATIC VOID_T __gpio_irq_callback(VOID_T *arg)
{
    (void)arg;
    ac_on_falling_edge();
}

VOID test_gpio(VOID)
{
    OPERATE_RET rt = OPRT_OK;

    sg_ac_state = AC_DETECT_IDLE;
    sg_ac_seen_edge = FALSE;
    sg_ac_last_edge_ms = 0;
    sg_ac_prev_recovery_edge_ms = 0;
    sg_ac_last_switch_ms = 0;
    sg_skip_next_ac_gear = FALSE;

    TAL_PR_NOTICE("[AC] init GPIO%d loss>%ums edge %u-%ums x3\r\n",
                  GPIO_IRQ_PIN, POWER_LOSS_DETECT_THRESHOLD_MS,
                  AC_NORMAL_EDGE_MIN_MS, AC_NORMAL_EDGE_MAX_MS);

    TUYA_GPIO_IRQ_T irq_cfg = {
        .cb = __gpio_irq_callback,
        .arg = NULL,
        .mode = TUYA_GPIO_IRQ_FALL,
    };
    TUYA_CALL_ERR_LOG(tkl_gpio_irq_init(GPIO_IRQ_PIN, &irq_cfg));
    TUYA_CALL_ERR_LOG(tkl_gpio_irq_enable(GPIO_IRQ_PIN));

    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(kv_rst_timeout_cb, NULL, &sg_rst_timer_id), __EXIT);
    tal_sw_timer_start(sg_rst_timer_id, 5000, TAL_TIMER_ONCE);

    tal_sw_timer_create(countirq_cb, NULL, &count_timer);
    tal_sw_timer_start(count_timer, 1, TAL_TIMER_CYCLE);

    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb, NULL, &sg_count1), __EXIT);
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb1, NULL, &sg_count2), __EXIT);
    tal_sw_timer_start(sg_count2, 100, TAL_TIMER_ONCE);
__EXIT:
    (void)rt;
    return;
}

VOID test_gpio1(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    /* 冷启动：cnt/cnt1++ 并出光；其后第一次 AC 恢复序列不切档 */
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb, NULL, &sg_count1), __EXIT);
    sg_skip_next_ac_gear = TRUE;
    sg_demo_info.cnt++;
    sg_demo_info.cnt1++;
    app_pwm();
__EXIT:
    (void)rt;
    return;
}
