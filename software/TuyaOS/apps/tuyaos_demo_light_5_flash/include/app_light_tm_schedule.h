#ifndef __APP_LIGHT_TM_SCHEDULE_H__
#define __APP_LIGHT_TM_SCHEDULE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCHEDULE_EVT_SLEEP = 0,
    SCHEDULE_EVT_WAKE = 1,
    SCHEDULE_EVT_SCENE = 2,
    SCHEDULE_EVT_RHYTHM = 3,
} SCHEDULE_EVT_E;

/* 定时伴眠/唤醒轮询与到点窗口（缩小 ±1 分钟与 60s 轮询误差） */
#define APP_LIGHT_SCHEDULE_POLL_MS           15000
#define APP_LIGHT_SCHEDULE_TRIGGER_SEC_WIN   15

BOOL_T app_light_schedule_preset_minute_due(UCHAR_T hour, UCHAR_T minute);

BOOL_T app_light_schedule_reject_sleep_on_enable(VOID_T);
BOOL_T app_light_schedule_reject_wake_on_enable(VOID_T);
VOID app_light_schedule_on_sleep_disable(VOID_T);
VOID app_light_schedule_on_wake_disable(VOID_T);
VOID app_light_schedule_revalidate(VOID_T);
VOID app_light_schedule_boot_finish(VOID_T);
BOOL_T app_light_schedule_should_skip_sleep_at_trigger(VOID_T);
BOOL_T app_light_schedule_should_skip_wake_at_trigger(VOID_T);
VOID app_light_schedule_mark_sleep_preempted_today(VOID_T);
VOID app_light_schedule_mark_wake_preempted_today(VOID_T);
BOOL_T app_light_schedule_is_sleep_preempted_today(VOID_T);
BOOL_T app_light_schedule_is_wake_preempted_today(VOID_T);
VOID app_light_schedule_clear_sleep_preempt(VOID_T);
VOID app_light_schedule_clear_wake_preempt(VOID_T);

VOID app_light_schedule_mark_configured(SCHEDULE_EVT_E evt);
VOID app_light_schedule_on_started(SCHEDULE_EVT_E evt);
VOID app_light_schedule_on_stopped(SCHEDULE_EVT_E evt);
VOID app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_E evt, BOOL_T immediate);
BOOL_T app_light_schedule_should_skip_rhythm_at_minute(UCHAR_T hour, UCHAR_T minute);
BOOL_T app_light_schedule_should_skip_scene_at_trigger(VOID_T);
VOID app_light_schedule_before_scene_start(VOID_T);
VOID app_light_schedule_before_sleep_start(VOID_T);
VOID app_light_schedule_before_wake_start(VOID_T);
/// 打开节律 / 节律到新节点：完全停止当天伴眠与唤醒
VOID app_light_schedule_before_rhythm_start(VOID_T);
/// 用户调光/调色/开关等：打断当天伴眠/唤醒（周期任务下一 scheduled 日继续）
VOID app_light_schedule_user_interrupt_sleep_wake(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_LIGHT_TM_SCHEDULE_H__ */
