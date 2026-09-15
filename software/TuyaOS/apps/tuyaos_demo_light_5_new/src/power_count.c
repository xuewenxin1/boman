/**
 * @file kv_power_count.c
 * @brief 上电计数：KV 存储 + 断电触发恢复出厂 + AC 掉电/恢复切档
 *
 * cnt / Flash 保存：与原逻辑一致（恢复确认后 cnt++，冷启动跳过首次；
 *                   落盘走 power_check/cb1/rst 的 device_config_save1，及 app_pwm→save）
 *
 * AC：超过 200ms 无过零 → 20ms 立变灭灯；交流恢复从 0 切到下一档。
 * 恢复需连续 5 个有效过零。cnt>=9 时再连续确认 5 次市电仍在，才重置进配网。
 * 恢复后 200ms 内不再二次判 LOSS。
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
#include "pwm_gradual.h"
#include "tal_system.h"
#include "tkl_gpio.h"
#include "product_hw.h"
#include <string.h>

#define GPIO_IRQ_PIN             TUYA_GPIO_NUM_21

/* 负载检测：GPIO14 主灯 / GPIO16 辅灯，供 pwm 校准系数乘算 */
int test_main_load = LIGHT_MAIN_LOAD_HIGH;
int test_auxiliary_load = LIGHT_AUX_LOAD_HIGH;

STATIC TIMER_ID sg_rst_timer_id = NULL;
STATIC TIMER_ID sg_count1 = NULL;
STATIC TIMER_ID sg_count2 = NULL;
STATIC TIMER_ID count_timer = NULL;
STATIC TIMER_ID sg_ac_reset_confirm_tm = NULL;

uint32_t count_irq = 0;

/* AC 掉电：无下降沿超过该时间(ms) */
#define POWER_LOSS_DETECT_THRESHOLD_MS  200
/* AC 正常：50Hz 全波约 10ms、半波约 20ms；1ms 软件定时会抖到 11~12ms */
#define AC_NORMAL_EDGE_MIN_MS           2
#define AC_NORMAL_EDGE_MAX_MS          22
/* 恢复确认所需连续有效沿个数（多判几次，减少残压毛刺当恢复） */
#define AC_RECOVERY_EDGE_NEED           5
/* 连续有效沿后才认为 AC 已锁定，避免 GPIO 上电毛刺当掉电 */
#define AC_LOCK_EDGE_NEED               3
/* 开中断后宽限期：漏沿/WiFi 初始化不准关灯 */
#define AC_BOOT_GRACE_MS                300
/* 短断电恢复出光后，短暂忽略 LOSS，防止漏沿把灯再灭掉 */
#define AC_RECOVERY_HOLDOFF_MS          200
/* cnt>=9 后：每隔 20ms 再看市电，连续 5 次都在才重置 */
#define AC_RESET_CONFIRM_NEED           5
#define AC_RESET_CONFIRM_MS             20

typedef enum {
    AC_DETECT_IDLE = 0,            /* 监测中，AC 正常有波形 */
    AC_DETECT_LOSS_CONFIRMED,      /* 已确认掉电，等待恢复 */
    AC_DETECT_RECOVERY,            /* 恢复计数中，等满 AC_RECOVERY_EDGE_NEED */
} AC_DETECT_STATE_E;

STATIC AC_DETECT_STATE_E sg_ac_state = AC_DETECT_IDLE;
STATIC BOOL_T sg_ac_seen_edge = FALSE;
STATIC uint32_t sg_ac_last_edge_ms = 0;
STATIC uint32_t sg_ac_prev_recovery_edge_ms = 0;
STATIC uint8_t sg_ac_recovery_edge_cnt = 0;
STATIC BOOL_T sg_skip_next_ac_gear = FALSE;
/* 本供电周期内已判定过 AC 断电：仅禁止 gw_reset / 残压出光，不影响 cnt */
STATIC BOOL_T sg_ac_saw_loss = FALSE;
STATIC BOOL_T sg_ac_locked = FALSE;
STATIC uint8_t sg_ac_lock_edge_cnt = 0;
STATIC uint32_t sg_ac_lock_prev_ms = 0;
STATIC uint32_t sg_ac_boot_ms = 0;
STATIC BOOL_T sg_ac_boot_ready = FALSE;
STATIC BOOL_T sg_ac_did_fade = FALSE;
STATIC uint32_t sg_ac_holdoff_until = 0;
STATIC BOOL_T sg_ac_need_light = FALSE;
STATIC uint8_t sg_ac_reset_confirm_cnt = 0;
STATIC BOOL_T sg_boot_9x_checked = FALSE;
/* 掉电 20ms 灭等特例：允许无市电也走渐变 */
STATIC BOOL_T sg_ac_pwm_bypass = FALSE;
/* 冷启动出光/切档保护窗：AC 未就绪也不能掐渐变 */
STATIC uint32_t sg_ac_cold_light_until_ms = 0;
#define AC_COLD_LIGHT_GUARD_MS          2000

STATIC VOID ac_loss_poll(VOID);
STATIC VOID ac_stop_9x_confirm(VOID);
STATIC VOID ac_start_9x_confirm(VOID);
STATIC VOID ac_pwm_hold_black(VOID);

STATIC BOOL_T ac_in_boot_grace(VOID)
{
    if (!sg_ac_boot_ready) {
        return TRUE;
    }
    return ((count_irq - sg_ac_boot_ms) < AC_BOOT_GRACE_MS) ? TRUE : FALSE;
}

/* IDLE 下用规律过零沿锁定真实 AC，并在宽限期结束后丢掉冷启动 skip */
STATIC VOID ac_try_lock(uint32_t now)
{
    uint32_t dt;

    if (sg_ac_locked) {
        if (sg_skip_next_ac_gear && !ac_in_boot_grace()) {
            sg_skip_next_ac_gear = FALSE;
        }
        return;
    }

    if (sg_ac_lock_prev_ms == 0) {
        sg_ac_lock_prev_ms = now;
        sg_ac_lock_edge_cnt = 1;
        return;
    }

    dt = now - sg_ac_lock_prev_ms;
    sg_ac_lock_prev_ms = now;
    if (dt < AC_NORMAL_EDGE_MIN_MS) {
        return;
    }
    if (dt > AC_NORMAL_EDGE_MAX_MS) {
        sg_ac_lock_edge_cnt = 1;
        return;
    }

    sg_ac_lock_edge_cnt++;
    if (sg_ac_lock_edge_cnt >= AC_LOCK_EDGE_NEED) {
        sg_ac_locked = TRUE;
        TAL_PR_NOTICE("[AC] locked, skip=%d grace=%d\r\n",
                      sg_skip_next_ac_gear, ac_in_boot_grace());
        /* 宽限期内不重置；宽限期结束后由 1ms 定时器再看 9 次 */
    }
}


/* 近 POWER_LOSS_DETECT_THRESHOLD_MS 内仍有过零沿 → 认为 AC 还在 */
STATIC BOOL_T ac_is_present(VOID)
{
    if (!sg_ac_seen_edge || sg_ac_last_edge_ms == 0) {
        return FALSE;
    }
    return ((count_irq - sg_ac_last_edge_ms) < POWER_LOSS_DETECT_THRESHOLD_MS)
           ? TRUE : FALSE;
}

/* 掉电后尚未完全恢复：禁止切档 / 出光 / 软件重启 */
STATIC BOOL_T ac_awaiting_recovery(VOID)
{
    return (sg_ac_state == AC_DETECT_LOSS_CONFIRMED ||
            sg_ac_state == AC_DETECT_RECOVERY) ? TRUE : FALSE;
}

VOID power_ac_pwm_bypass_begin(VOID)
{
    sg_ac_pwm_bypass = TRUE;
}

VOID power_ac_pwm_bypass_end(VOID)
{
    sg_ac_pwm_bypass = FALSE;
}

/*
 * 初上电 test_gpio1 那次 PWM：完全不管 AC（bypass 挂到保护窗结束）。
 * 保护窗过后：须市电在、且不在掉电恢复中。
 * 说明：cnt>=9 仅 AC 恢复路径不出光；冷启动/重启仍要亮，方便和 AC 路径区分。
 */
BOOL_T power_ac_ok_for_gradual(VOID)
{
    uint32_t now_ms;

    if (sg_ac_pwm_bypass) {
        return TRUE;
    }
    /* 初上电保护窗到期：关掉 bypass，之后才恢复 AC 门控 */
    if (sg_ac_cold_light_until_ms != 0) {
        now_ms = tal_system_get_millisecond();
        if (now_ms < sg_ac_cold_light_until_ms) {
            return TRUE;
        }
        sg_ac_cold_light_until_ms = 0;
        sg_ac_pwm_bypass = FALSE;
    }
    /* 上电阶段：检测未开、未锁住、或仍在宽限 */
    if (!sg_ac_boot_ready || !sg_ac_locked || ac_in_boot_grace()) {
        return TRUE;
    }
    if (!ac_is_present() || ac_awaiting_recovery()) {
        return FALSE;
    }
    return TRUE;
}

/* 初始化未完成 / 市电未锁住 / 宽限期内 / 当前没过零：不允许重置 */
STATIC BOOL_T ac_reset_allowed(VOID)
{
    if (kv_power_on_count() != TRUE) {
        return FALSE;
    }
    if (!sg_ac_boot_ready || ac_in_boot_grace() || !sg_ac_locked) {
        return FALSE;
    }
    if (!ac_is_present() || ac_awaiting_recovery()) {
        return FALSE;
    }
    return TRUE;
}

STATIC VOID ac_cancel_pending_gear_apply(CONST CHAR_T *reason)
{
    (void)reason;
    /* 短断电已确认恢复、正在出光：不要把出光定时器停掉 */
    if (sg_ac_need_light) {
        return;
    }
    if (sg_ac_holdoff_until != 0 && count_irq < sg_ac_holdoff_until) {
        return;
    }
    if (sg_count1 != NULL && tal_sw_timer_is_running(sg_count1)) {
        tal_sw_timer_stop(sg_count1);
    }
}

/* 0：掉电不灭。1：掉电 20ms 立变灭，来电从 0 切下一档 */
#ifndef AC_LOSS_OFF_ENABLE
#define AC_LOSS_OFF_ENABLE  1
#endif

/* 掉电灭灯用 20ms 立变；切档跟 change_light_status：0=立变20ms，其它=渐变1600ms */
#define AC_LOSS_OFF_TIME_MS      20

STATIC VOID ac_apply_change_light_time(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    extern uint16_t gradual_time_ms;

    if (sg_demo_info.change_light_status == 0) {
        gradual_time_ms = 20;
    } else {
        gradual_time_ms = 1600;
    }
}

/* AC 掉电：立变拉到 0（不走渐变门控，避免无市电时被拦住） */
STATIC VOID ac_pwm_gradual_off(VOID)
{
    extern uint8_t preview_flag;

    preview_flag = 0;
    ac_pwm_hold_black();
}

STATIC VOID ac_pwm_fade_stop(VOID)
{
    /* 恢复切档出光前打断掉电渐灭 */
    (VOID)pwm_gradual_stop_current();
}

/* 切下一档必须从全灭起步，避免残留上一档亮度和色温 */
STATIC VOID ac_pwm_hold_black(VOID)
{
    ac_pwm_fade_stop();
    (VOID)pwm_gradual_duty_set_immediate(BRIGHT_PWM, 0);
    (VOID)pwm_gradual_duty_set_immediate(TEMP_PWM, 0);
    (VOID)pwm_gradual_duty_set_immediate(AUX_BRIGHT_PWM, 0);
    (VOID)pwm_gradual_duty_set_immediate(AUX_TEMP_PWM, 0);
    (VOID)pwm_gradual_duty_set_immediate(NIGHT_BRIGHT_PWM, 0);
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
    if (sg_ac_locked && !ac_in_boot_grace()) {
        if (sg_skip_next_ac_gear) {
            sg_skip_next_ac_gear = FALSE;
        }
        /* cnt>=9：保持不亮，并持续尝试重置确认（含 >9） */
        if (kv_power_on_count()) {
            sg_ac_need_light = FALSE;
            if (sg_count1 != NULL && tal_sw_timer_is_running(sg_count1)) {
                tal_sw_timer_stop(sg_count1);
            }
            if (!sg_boot_9x_checked ||
                (sg_ac_reset_confirm_tm != NULL &&
                 !tal_sw_timer_is_running(sg_ac_reset_confirm_tm))) {
                sg_boot_9x_checked = TRUE;
                ac_start_9x_confirm();
            }
        } else if (!sg_boot_9x_checked) {
            sg_boot_9x_checked = TRUE;
        }
    }
    ac_loss_poll();
}

STATIC VOID ac_recovery_reset_first_edge(uint32_t now, CONST CHAR_T *reason)
{
    (void)reason;
    sg_ac_prev_recovery_edge_ms = now;
    sg_ac_recovery_edge_cnt = 1;
    sg_ac_state = AC_DETECT_RECOVERY;
}

STATIC VOID ac_apply_light_after_recovery(BOOL_T do_gear_inc)
{
    extern DEMO_INFO_T sg_demo_info;

    if (do_gear_inc) {
        sg_demo_info.cnt++;
        sg_demo_info.cnt1++;
        TAL_PR_NOTICE("[AC] recover -> next gear from_0 cnt=%d cnt1=%d\r\n",
                      sg_demo_info.cnt, sg_demo_info.cnt1);
    } else {
        TAL_PR_NOTICE("[AC] recover -> light on cnt=%d cnt1=%d\r\n",
                      sg_demo_info.cnt, sg_demo_info.cnt1);
    }

    /* cnt>=9（含大于 9）：仅 AC 恢复不出光；重启走 test_gpio1 仍会亮 */
    if (kv_power_on_count()) {
        sg_ac_need_light = FALSE;
        TAL_PR_NOTICE("[AC] cnt>=9 skip pwm until reset (AC path) cnt=%d\r\n",
                      sg_demo_info.cnt);
        ac_start_9x_confirm();
        return;
    }

    /* 先拉到 0，再从灭灯切到下一档 */
    ac_pwm_hold_black();
    sg_ac_did_fade = FALSE;
    sg_ac_need_light = TRUE;
    sg_ac_holdoff_until = count_irq + AC_RECOVERY_HOLDOFF_MS;

    /* 切档：跟 App 渐变/立变设置 */
    ac_apply_change_light_time();

    /* 不在 GPIO 中断里出光：10ms 后定时器执行，且不允许被二次 LOSS 取消 */
    if (sg_count1 != NULL) {
        tal_sw_timer_start(sg_count1, 10, TAL_TIMER_ONCE);
    }
}

STATIC BOOL_T ac_light_is_off(VOID)
{
    return ((pwm_gradual_get_current_duty(BRIGHT_PWM) == 0) &&
            (pwm_gradual_get_current_duty(TEMP_PWM) == 0) &&
            (pwm_gradual_get_current_duty(AUX_BRIGHT_PWM) == 0) &&
            (pwm_gradual_get_current_duty(AUX_TEMP_PWM) == 0) &&
            (pwm_gradual_get_current_duty(NIGHT_BRIGHT_PWM) == 0))
           ? TRUE : FALSE;
}

STATIC VOID ac_on_gear_switch(VOID)
{
    BOOL_T light_off = (sg_ac_did_fade || ac_light_is_off()) ? TRUE : FALSE;

    if (sg_skip_next_ac_gear) {
        sg_skip_next_ac_gear = FALSE;
        if (!light_off) {
            TAL_PR_NOTICE("[AC] skip extra gear after cold boot, light on\r\n");
            return;
        }
    }

    /* 交流已恢复：切下一档，渐变出光 */
    ac_apply_light_after_recovery(TRUE);
}

STATIC VOID ac_recovery_on_next_edge(uint32_t now)
{
    uint32_t dt = now - sg_ac_prev_recovery_edge_ms;

    if (dt < AC_NORMAL_EDGE_MIN_MS) {
        return;
    }

    if (dt > AC_NORMAL_EDGE_MAX_MS) {
        ac_recovery_reset_first_edge(now, "interval_too_long");
        return;
    }

    sg_ac_prev_recovery_edge_ms = now;
    sg_ac_recovery_edge_cnt++;
    if (sg_ac_recovery_edge_cnt >= AC_RECOVERY_EDGE_NEED) {
        sg_ac_state = AC_DETECT_IDLE;
        sg_ac_recovery_edge_cnt = 0;
        ac_on_gear_switch();
    }
}

STATIC VOID ac_on_falling_edge(VOID)
{
    uint32_t now = count_irq;
    AC_DETECT_STATE_E st = sg_ac_state;

    sg_ac_seen_edge = TRUE;
    sg_ac_last_edge_ms = now;

    switch (st) {
    case AC_DETECT_IDLE:
        ac_try_lock(now);
        break;

    case AC_DETECT_LOSS_CONFIRMED:
        ac_recovery_reset_first_edge(now, "loss_recover_e1");
        break;

    case AC_DETECT_RECOVERY:
        ac_recovery_on_next_edge(now);
        break;

    default:
        sg_ac_state = AC_DETECT_IDLE;
        sg_ac_recovery_edge_cnt = 0;
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

    /* 上电未锁定 / 宽限期内：漏沿当毛刺，不能把 test_gpio1 刚点的灯灭掉 */
    if (!sg_ac_locked || ac_in_boot_grace()) {
        return;
    }

    /* 短断电刚恢复出光：漏几个沿不当成又掉电 */
    if (sg_ac_holdoff_until != 0 && count_irq < sg_ac_holdoff_until) {
        return;
    }

    /* IDLE：新的掉电，渐灭。RECOVERY 中又静默：恢复没凑满，回到 LOSS，不要再从上一档亮起。 */
    if (sg_ac_state == AC_DETECT_IDLE || sg_ac_state == AC_DETECT_RECOVERY) {
        BOOL_T from_recovery = (sg_ac_state == AC_DETECT_RECOVERY) ? TRUE : FALSE;

        sg_ac_state = AC_DETECT_LOSS_CONFIRMED;
        sg_ac_recovery_edge_cnt = 0;
        sg_ac_saw_loss = TRUE;
        ac_cancel_pending_gear_apply("ac_loss");
        ac_stop_9x_confirm();
        if (from_recovery) {
            TAL_PR_NOTICE("[AC] recover abort silent=%ums, stay LOSS\r\n", silent_ms);
            sg_ac_did_fade = TRUE;
#if AC_LOSS_OFF_ENABLE
            ac_pwm_gradual_off();
#endif
            return;
        }
        TAL_PR_NOTICE("[AC] ->LOSS silent=%ums, off=%d\r\n",
                      silent_ms, AC_LOSS_OFF_ENABLE);
        sg_ac_did_fade = TRUE;
#if AC_LOSS_OFF_ENABLE
        ac_pwm_gradual_off();
#endif
    }
}

STATIC VOID ac_stop_9x_confirm(VOID)
{
    sg_ac_reset_confirm_cnt = 0;
    if (sg_ac_reset_confirm_tm != NULL && tal_sw_timer_is_running(sg_ac_reset_confirm_tm)) {
        tal_sw_timer_stop(sg_ac_reset_confirm_tm);
    }
}

/* 市电已恢复且 cnt>=9：再连续看几次过零，都在才重置 */
STATIC VOID ac_reset_confirm_cb(TIMER_ID timer_id, VOID_T *arg)
{
    extern DEMO_INFO_T sg_demo_info;

    (void)timer_id;
    (void)arg;

    if (kv_power_on_count() != TRUE) {
        ac_stop_9x_confirm();
        return;
    }
    if (!ac_reset_allowed()) {
        TAL_PR_NOTICE("[AC] 9x confirm wait %u/%u, ac not ready, retry\r\n",
                      sg_ac_reset_confirm_cnt, AC_RESET_CONFIRM_NEED);
        sg_ac_reset_confirm_cnt = 0;
        if (sg_ac_reset_confirm_tm != NULL) {
            tal_sw_timer_start(sg_ac_reset_confirm_tm, AC_RESET_CONFIRM_MS * 5,
                               TAL_TIMER_ONCE);
        }
        return;
    }

    sg_ac_reset_confirm_cnt++;
    TAL_PR_NOTICE("[AC] 9x confirm %u/%u cnt=%d ac=1\r\n",
                  sg_ac_reset_confirm_cnt, AC_RESET_CONFIRM_NEED, sg_demo_info.cnt);
    if (sg_ac_reset_confirm_cnt >= AC_RESET_CONFIRM_NEED) {
        ac_stop_9x_confirm();
        __execute_power_loss_recovery();
        return;
    }
    if (sg_ac_reset_confirm_tm != NULL) {
        tal_sw_timer_start(sg_ac_reset_confirm_tm, AC_RESET_CONFIRM_MS, TAL_TIMER_ONCE);
    }
}

STATIC VOID ac_start_9x_confirm(VOID)
{
    if (!ac_reset_allowed()) {
        TAL_PR_NOTICE("[AC] 9x skip, ac not ready lock=%d grace=%d ac=%d\r\n",
                      sg_ac_locked, ac_in_boot_grace(), ac_is_present());
        return;
    }
    if (sg_ac_reset_confirm_tm == NULL) {
        return;
    }
    if (tal_sw_timer_is_running(sg_ac_reset_confirm_tm)) {
        return;
    }
    sg_ac_reset_confirm_cnt = 0;
    TAL_PR_NOTICE("[AC] 9x confirm start, need %u checks\r\n", AC_RESET_CONFIRM_NEED);
    tal_sw_timer_start(sg_ac_reset_confirm_tm, AC_RESET_CONFIRM_MS, TAL_TIMER_ONCE);
}

STATIC VOID power_check_timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;

    TAL_PR_NOTICE("[AC] power_check_timer_cb cnt=%d cnt1=%d ac=%d saw_loss=%d\r\n",
                  sg_demo_info.cnt, sg_demo_info.cnt1,
                  ac_is_present(), sg_ac_saw_loss);

    /* 短断电切档不写 Flash：电压不稳时擦块会卡死。cnt 先放 RAM，5 秒定时器和 9 次重置再落盘 */

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
    if (sg_demo_info.rhythm_switch) {
        app_light_stop_today_rhythm_timer();
    }
    tal_sw_timer_start(sg_rst_timer_id, 5000, TAL_TIMER_ONCE);

    /* 市电已恢复：先出光；cnt>=9 不出光，只走重置确认 */
    if (kv_power_on_count()) {
        sg_ac_need_light = FALSE;
        if (sg_count1 != NULL && tal_sw_timer_is_running(sg_count1)) {
            tal_sw_timer_stop(sg_count1);
        }
        if (ac_is_present() && !ac_awaiting_recovery()) {
            ac_start_9x_confirm();
        }
    } else if (sg_ac_need_light) {
        sg_ac_need_light = FALSE;
        ac_pwm_hold_black();
        ac_apply_change_light_time();
        app_pwm();
    }
}

STATIC VOID power_check_timer_cb1(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;

    tkl_log_output("duandian1,cnt = %d,cnt1 = %d\r\n", sg_demo_info.cnt, sg_demo_info.cnt1);
    device_config_save1();
    /* 100ms 仍在上电宽限期内，这里不重置 */
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

    TAL_PR_NOTICE("duandian2,cnt=%d cnt1=%d saw_loss=%d ac=%d seen=%d lock=%d grace=%d\r\n",
                  sg_demo_info.cnt, sg_demo_info.cnt1,
                  sg_ac_saw_loss, ac_is_present(), sg_ac_seen_edge,
                  sg_ac_locked, ac_in_boot_grace());

    /* 初始化中 / 市电未锁住 / 当前没过零：不重置 */
    if (!ac_reset_allowed()) {
        TAL_PR_NOTICE("[AC] skip reset, ac not ready\r\n");
        return;
    }

    TAL_PR_NOTICE("[AC] 9x wall reset, cnt=%d\r\n", sg_demo_info.cnt);

    sg_demo_info.cnt = 0;
    sg_demo_info.cnt1 = 0;
    sg_demo_info.checksum = 0xa5;
    sg_demo_info.switch_status = 1;
    sg_demo_info.default_state = 0;
    sg_demo_info.white_switch = 1;
    sg_demo_info.white_bright = 100;
    sg_demo_info.last_light_memory = 1; /* 5_new 主+辅默认记忆 */
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
    ac_pwm_fade_stop();
    device_config_save1_force();
    tuya_iot_wf_gw_reset();
}

STATIC VOID_T __gpio_irq_callback(VOID_T *arg)
{
    (void)arg;
    ac_on_falling_edge();
}

VOID test_gpio(VOID)
{
    OPERATE_RET rt = OPRT_OK;

    TAL_PR_NOTICE("[BOOT] app: test_gpio enter ms=%u\r\n",
                  (unsigned)tal_system_get_millisecond());

    sg_ac_state = AC_DETECT_IDLE;
    sg_ac_seen_edge = FALSE;
    sg_ac_last_edge_ms = 0;
    sg_ac_prev_recovery_edge_ms = 0;
    sg_ac_recovery_edge_cnt = 0;
    sg_ac_locked = FALSE;
    sg_ac_lock_edge_cnt = 0;
    sg_ac_lock_prev_ms = 0;
    sg_ac_did_fade = FALSE;
    sg_ac_holdoff_until = 0;
    sg_ac_need_light = FALSE;
    sg_ac_reset_confirm_cnt = 0;
    sg_boot_9x_checked = FALSE;
    sg_ac_boot_ms = count_irq;
    sg_ac_boot_ready = TRUE;
    /* 保留 test_gpio1 设置的 sg_skip_next_ac_gear，勿清掉 */
    /* sg_ac_saw_loss 由冷启动 test_gpio1 清零 */
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
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(ac_reset_confirm_cb, NULL, &sg_ac_reset_confirm_tm), __EXIT);
    tal_sw_timer_start(sg_count2, 100, TAL_TIMER_ONCE);
    TAL_PR_NOTICE("[BOOT] app: test_gpio exit ms=%u\r\n",
                  (unsigned)tal_system_get_millisecond());
__EXIT:
    (void)rt;
    return;
}

VOID test_gpio1(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    TAL_PR_NOTICE("[BOOT] app: test_gpio1 enter ms=%u\r\n",
                  (unsigned)tal_system_get_millisecond());
    /* 冷启动：cnt++/cnt1++ 后立刻切光/出光，不等人 AC；其后第一次 AC 恢复不切档 */
    sg_ac_did_fade = FALSE;
    sg_ac_boot_ready = FALSE;
    sg_ac_saw_loss = FALSE;
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb, NULL, &sg_count1), __EXIT);
    sg_skip_next_ac_gear = TRUE;
    sg_demo_info.cnt++;
    sg_demo_info.cnt1++;
    device_config_save1();
    TAL_PR_NOTICE("duandian3,cnt=%d cnt1=%d\r\n",
                  sg_demo_info.cnt, sg_demo_info.cnt1);
    /* 重启/冷启动一律出光（含 cnt>=9）；只有 AC 恢复路径才保持不亮 */
    if (kv_power_on_count()) {
        TAL_PR_NOTICE("[BOOT] app: test_gpio1 cnt>=9 still light (reboot) ms=%u cnt=%d\r\n",
                      (unsigned)tal_system_get_millisecond(),
                      sg_demo_info.cnt);
    }
    /* 初上电那次 PWM 不管 AC：bypass 保持到保护窗结束，渐变中途也不掐 */
    sg_ac_cold_light_until_ms =
        tal_system_get_millisecond() + AC_COLD_LIGHT_GUARD_MS;
    power_ac_pwm_bypass_begin();
    app_pwm();
    /* 切档后的 gear_memory 必须立刻落盘：前面 save1 还是旧档，5s 延时保存来不及断电 */
    device_config_save1_force();
    TAL_PR_NOTICE("[BOOT] app: test_gpio1 light on ms=%u cnt1=%d gear=%d\r\n",
                  (unsigned)tal_system_get_millisecond(),
                  sg_demo_info.cnt1, sg_demo_info.gear_memory);
__EXIT:
    (void)rt;
    return;
}

VOID Load_calibration(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    TUYA_GPIO_LEVEL_E read_level = 0;
    TUYA_GPIO_BASE_CFG_T gpio_cfg;

    memset(&gpio_cfg, 0, sizeof(gpio_cfg));
    gpio_cfg.direct = TUYA_GPIO_INPUT;

    TUYA_CALL_ERR_LOG(tkl_gpio_init(TUYA_GPIO_NUM_14, &gpio_cfg));
    TUYA_CALL_ERR_LOG(tkl_gpio_init(TUYA_GPIO_NUM_16, &gpio_cfg));

    tkl_gpio_read(TUYA_GPIO_NUM_14, &read_level);
    test_main_load = (read_level == TUYA_GPIO_LEVEL_HIGH)
                     ? LIGHT_MAIN_LOAD_HIGH : LIGHT_MAIN_LOAD_LOW;
    TAL_PR_NOTICE("read_level14=%d main_load=%d%% (%s)\r\n",
                  read_level, test_main_load, LIGHT_HW_NAME);

    tkl_gpio_read(TUYA_GPIO_NUM_16, &read_level);
    test_auxiliary_load = (read_level == TUYA_GPIO_LEVEL_HIGH)
                          ? LIGHT_AUX_LOAD_HIGH : LIGHT_AUX_LOAD_LOW;
    TAL_PR_NOTICE("read_level16=%d aux_load=%d%% (%s)\r\n",
                  read_level, test_auxiliary_load, LIGHT_HW_NAME);

    (void)rt;
}
