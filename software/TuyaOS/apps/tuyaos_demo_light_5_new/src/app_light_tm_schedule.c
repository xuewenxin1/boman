#include "app_light_tm_schedule.h"
#include "app_light_tm_sleep.h"
#include "app_light_tm_wake.h"
#include "app_light_tm_rhythm.h"
#include "dp_process.h"
#include "tal_log.h"
#include "tal_time_service.h"
#include "tal_system.h"

typedef struct {
    INT_T year;
    INT_T mon;
    INT_T mday;
} SCHEDULE_DAY_T;

static SCHEDULE_DAY_T sg_sleep_preempt_day = {-1, -1, -1};
static SCHEDULE_DAY_T sg_wake_preempt_day = {-1, -1, -1};

static UINT32_T sg_sleep_set_seq = 0;
static UINT32_T sg_wake_set_seq = 0;
static UINT32_T sg_scene_set_seq = 0;
static UINT32_T sg_rhythm_set_seq = 0;
static SCHEDULE_EVT_E sg_running_evt = SCHEDULE_EVT_SCENE;
static BOOL_T sg_has_running_evt = FALSE;

#define SCHEDULE_EVT_NONE 0xFF

extern uint8_t work_mode;
extern int sleep_state;
extern BOOL_T sg_sleep_is_timing;
extern BOOL_T sg_wake_is_timing;
extern TM_RHYTHM_INFO_T sg_rhythm_info;
extern bool sg_rhythm_interrupted;

static BOOL_T __is_same_calendar_day(SCHEDULE_DAY_T *day)
{
    extern POSIX_TM_S local_tm;
    extern bool time_synced;

    if (!time_synced || day->year < 0) {
        return FALSE;
    }

    return (day->year == local_tm.tm_year &&
            day->mon == local_tm.tm_mon &&
            day->mday == local_tm.tm_mday);
}

static VOID __mark_calendar_today(SCHEDULE_DAY_T *day)
{
    extern POSIX_TM_S local_tm;

    day->year = local_tm.tm_year;
    day->mon = local_tm.tm_mon;
    day->mday = local_tm.tm_mday;
}

static BOOL_T __week_match(UCHAR_T week)
{
    extern POSIX_TM_S local_tm;

    if (week == WEEK_ONCE || week == WEEK_EVERYDAY) {
        return TRUE;
    }

    UCHAR_T current_week_bit = 0;
    switch (local_tm.tm_wday) {
    case 0: current_week_bit = WEEK_SUNDAY; break;
    case 1: current_week_bit = WEEK_MONDAY; break;
    case 2: current_week_bit = WEEK_TUESDAY; break;
    case 3: current_week_bit = WEEK_WEDNESDAY; break;
    case 4: current_week_bit = WEEK_THURSDAY; break;
    case 5: current_week_bit = WEEK_FRIDAY; break;
    case 6: current_week_bit = WEEK_SATURDAY; break;
    default: break;
    }
    return (week & current_week_bit) != 0;
}

static BOOL_T __week_overlap(UCHAR_T week_a, UCHAR_T week_b)
{
    if (week_a == WEEK_ONCE || week_b == WEEK_ONCE) {
        return TRUE;
    }
    if (week_a == WEEK_EVERYDAY || week_b == WEEK_EVERYDAY) {
        return TRUE;
    }
    return (week_a & week_b) != 0;
}

static BOOL_T __same_timed_schedule(UCHAR_T hour_a, UCHAR_T minute_a, UCHAR_T week_a,
                                    UCHAR_T hour_b, UCHAR_T minute_b, UCHAR_T week_b)
{
    if (hour_a != hour_b || minute_a != minute_b) {
        return FALSE;
    }
    return __week_overlap(week_a, week_b);
}

static BOOL_T __minute_match(UCHAR_T hour, UCHAR_T minute)
{
    return app_light_schedule_preset_minute_due(hour, minute);
}

BOOL_T app_light_schedule_preset_minute_due(UCHAR_T hour, UCHAR_T minute)
{
    extern POSIX_TM_S local_tm;
    extern bool time_synced;

    if (!time_synced) {
        return FALSE;
    }
    if (local_tm.tm_hour != hour || local_tm.tm_min != minute) {
        return FALSE;
    }
    if (local_tm.tm_sec >= APP_LIGHT_SCHEDULE_TRIGGER_SEC_WIN) {
        return FALSE;
    }
    return TRUE;
}

static BOOL_T __sleep_timed_enabled(UCHAR_T *sleep_dp)
{
    return (sleep_dp[0] == 0x01) && (sleep_dp[10] != 0);
}

static BOOL_T __wake_timed_enabled(UCHAR_T *wake_dp)
{
    return (wake_dp[0] == 0x01) && (wake_dp[7] != 0);
}

static BOOL_T __sleep_scheduled_at(UCHAR_T hour, UCHAR_T minute)
{
    extern DEMO_INFO_T sg_demo_info;
    UCHAR_T *sleep_dp = sg_demo_info.sleep_init;

    if (!__sleep_timed_enabled(sleep_dp)) {
        return FALSE;
    }
    return sleep_dp[11] == hour && sleep_dp[12] == minute && __week_match(sleep_dp[13]);
}

static BOOL_T __wake_scheduled_at(UCHAR_T hour, UCHAR_T minute)
{
    extern DEMO_INFO_T sg_demo_info;
    UCHAR_T *wake_dp = sg_demo_info.wake_init;

    if (!__wake_timed_enabled(wake_dp)) {
        return FALSE;
    }
    return wake_dp[8] == hour && wake_dp[9] == minute && __week_match(wake_dp[10]);
}

/* 与 sleep/wake 定时器一致：精确分钟 + 秒级窗口 */
static BOOL_T __sleep_triggers_now(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    UCHAR_T *sleep_dp = sg_demo_info.sleep_init;

    if (!__sleep_timed_enabled(sleep_dp)) {
        return FALSE;
    }
    if (!__minute_match(sleep_dp[11], sleep_dp[12])) {
        return FALSE;
    }
    return __week_match(sleep_dp[13]);
}

static BOOL_T __wake_triggers_now(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    UCHAR_T *wake_dp = sg_demo_info.wake_init;

    if (!__wake_timed_enabled(wake_dp)) {
        return FALSE;
    }
    if (!__minute_match(wake_dp[8], wake_dp[9])) {
        return FALSE;
    }
    return __week_match(wake_dp[10]);
}

static SCHEDULE_EVT_E __winner_at_trigger_window(VOID_T)
{
    if (__sleep_triggers_now()) {
        return SCHEDULE_EVT_SLEEP;
    }
    if (__wake_triggers_now()) {
        return SCHEDULE_EVT_WAKE;
    }
    return SCHEDULE_EVT_NONE;
}

static UINT32_T __seq_of(SCHEDULE_EVT_E evt)
{
    switch (evt) {
    case SCHEDULE_EVT_SLEEP: return sg_sleep_set_seq;
    case SCHEDULE_EVT_WAKE: return sg_wake_set_seq;
    case SCHEDULE_EVT_SCENE: return sg_scene_set_seq;
    case SCHEDULE_EVT_RHYTHM: return sg_rhythm_set_seq;
    default: return 0;
    }
}

static VOID __preempt_running_tasks(SCHEDULE_EVT_E new_evt, BOOL_T include_same_priority)
{
    extern DEMO_INFO_T sg_demo_info;

    if (new_evt != SCHEDULE_EVT_SLEEP &&
        (sg_sleep_is_timing || sleep_state)) {
        app_light_preempt_sleep_today();
    }
    if (new_evt != SCHEDULE_EVT_WAKE && sg_wake_is_timing) {
        app_light_preempt_wake_today();
    }
    if (new_evt != SCHEDULE_EVT_SCENE && work_mode != 16) {
        work_mode = 16;
        upload_device_enum_status(DPID_WORK_MODE, 16);
    }
    if (new_evt != SCHEDULE_EVT_RHYTHM &&
        sg_demo_info.rhythm_switch == 1 && !sg_rhythm_interrupted) {
        app_light_stop_today_rhythm_timer();
    }
    (VOID)include_same_priority;
}

VOID app_light_schedule_mark_configured(SCHEDULE_EVT_E evt)
{
    UINT32_T seq = tal_system_get_millisecond();
    switch (evt) {
    case SCHEDULE_EVT_SLEEP: sg_sleep_set_seq = seq; break;
    case SCHEDULE_EVT_WAKE: sg_wake_set_seq = seq; break;
    case SCHEDULE_EVT_SCENE: sg_scene_set_seq = seq; break;
    case SCHEDULE_EVT_RHYTHM: sg_rhythm_set_seq = seq; break;
    default: break;
    }
}

VOID app_light_schedule_on_started(SCHEDULE_EVT_E evt)
{
    sg_running_evt = evt;
    sg_has_running_evt = TRUE;
}

VOID app_light_schedule_on_stopped(SCHEDULE_EVT_E evt)
{
    if (sg_has_running_evt && sg_running_evt == evt) {
        sg_has_running_evt = FALSE;
    }
}

VOID app_light_schedule_preempt_for_new_event(SCHEDULE_EVT_E evt, BOOL_T immediate)
{
    extern DEMO_INFO_T sg_demo_info;

    if (!immediate) {
        return;
    }

    if (sg_has_running_evt && __seq_of(evt) >= __seq_of(sg_running_evt)) {
        __preempt_running_tasks(evt, TRUE);
        return;
    }

    if ((sg_sleep_is_timing || sleep_state || sg_wake_is_timing || work_mode != 16) &&
        __seq_of(evt) > 0) {
        __preempt_running_tasks(evt, TRUE);
    }

    if (evt == SCHEDULE_EVT_WAKE && (sg_sleep_is_timing || sleep_state)) {
        app_light_preempt_sleep_today();
    }
    if (evt == SCHEDULE_EVT_SLEEP && sg_wake_is_timing) {
        app_light_preempt_wake_today();
    }
    (VOID)sg_demo_info;
}

BOOL_T app_light_schedule_should_skip_rhythm_at_minute(UCHAR_T hour, UCHAR_T minute)
{
    /* 节律到新节点优先：不再因伴眠/唤醒同窗口而跳过，由 before_rhythm_start 打断伴眠/唤醒 */
    (VOID)hour;
    (VOID)minute;
    return FALSE;
}

BOOL_T app_light_schedule_should_skip_scene_at_trigger(VOID_T)
{
    /* 情景与伴眠/唤醒互抢：未开始时情景可先跑，到点伴眠/唤醒再抢占；已开始时情景可抢占 */
    return FALSE;
}

VOID app_light_schedule_before_scene_start(VOID_T)
{
    __preempt_running_tasks(SCHEDULE_EVT_SCENE, FALSE);
    app_light_schedule_on_started(SCHEDULE_EVT_SCENE);
}

VOID app_light_schedule_before_sleep_start(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;

    if (work_mode != 16) {
        work_mode = 16;
        upload_device_enum_status(DPID_WORK_MODE, 16);
    }
    if (sg_demo_info.rhythm_switch == 1 && !sg_rhythm_interrupted) {
        app_light_stop_today_rhythm_timer();
    }
    if (sg_wake_is_timing) {
        app_light_preempt_wake_today();
    }
    app_light_schedule_on_started(SCHEDULE_EVT_SLEEP);
}

VOID app_light_schedule_before_wake_start(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;

    if (work_mode != 16) {
        work_mode = 16;
        upload_device_enum_status(DPID_WORK_MODE, 16);
    }
    if (sg_demo_info.rhythm_switch == 1 && !sg_rhythm_interrupted) {
        app_light_stop_today_rhythm_timer();
    }
    if (sg_sleep_is_timing || sleep_state) {
        app_light_preempt_sleep_today();
    }
    app_light_schedule_on_started(SCHEDULE_EVT_WAKE);
}

VOID app_light_schedule_before_rhythm_start(VOID_T)
{
    if (sg_sleep_is_timing || sleep_state) {
        TAL_PR_NOTICE("节律启动/到新节点，完全停止运行中的伴眠");
        app_light_preempt_sleep_today();
    }
    if (sg_wake_is_timing) {
        TAL_PR_NOTICE("节律启动/到新节点，完全停止运行中的唤醒");
        app_light_preempt_wake_today();
    }
    app_light_schedule_on_started(SCHEDULE_EVT_RHYTHM);
}

VOID app_light_schedule_user_interrupt_sleep_wake(VOID_T)
{
    if (sg_sleep_is_timing || sleep_state) {
        app_light_preempt_sleep_today();
    }
    if (sg_wake_is_timing) {
        app_light_preempt_wake_today();
    }
}

static BOOL_T __sleep_wake_same_time(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    UCHAR_T *sleep_dp = sg_demo_info.sleep_init;
    UCHAR_T *wake_dp = sg_demo_info.wake_init;

    if (!__sleep_timed_enabled(sleep_dp) || !__wake_timed_enabled(wake_dp)) {
        return FALSE;
    }

    return __same_timed_schedule(sleep_dp[11], sleep_dp[12], sleep_dp[13],
                                 wake_dp[8], wake_dp[9], wake_dp[10]);
}

BOOL_T app_light_schedule_reject_sleep_on_enable(VOID_T)
{
    /* 同时间允许伴眠与唤醒同时 armed，到点按优先级执行 */
    return FALSE;
}

BOOL_T app_light_schedule_reject_wake_on_enable(VOID_T)
{
    return FALSE;
}

VOID app_light_schedule_on_sleep_disable(VOID_T)
{
    app_light_schedule_on_stopped(SCHEDULE_EVT_SLEEP);
    app_light_schedule_clear_sleep_preempt();
}

VOID app_light_schedule_on_wake_disable(VOID_T)
{
    app_light_schedule_on_stopped(SCHEDULE_EVT_WAKE);
    app_light_schedule_clear_wake_preempt();
}

VOID app_light_schedule_revalidate(VOID_T)
{
}

VOID app_light_schedule_boot_finish(VOID_T)
{
    /* 同时间伴眠/唤醒可同时开启，到点由 should_skip_* 按伴眠>唤醒仲裁 */
}

BOOL_T app_light_schedule_should_skip_sleep_at_trigger(VOID_T)
{
    /* 伴眠优先级高于唤醒，到点不因唤醒而跳过伴眠 */
    return FALSE;
}

BOOL_T app_light_schedule_should_skip_wake_at_trigger(VOID_T)
{
    /* 伴眠优先：同触发窗口内跳过唤醒；伴眠已在跑时也跳过 */
    if (__sleep_triggers_now()) {
        return TRUE;
    }
    if (__sleep_wake_same_time() && (sg_sleep_is_timing || sleep_state)) {
        return TRUE;
    }
    return FALSE;
}

VOID app_light_schedule_mark_sleep_preempted_today(VOID_T)
{
    __mark_calendar_today(&sg_sleep_preempt_day);
}

VOID app_light_schedule_mark_wake_preempted_today(VOID_T)
{
    __mark_calendar_today(&sg_wake_preempt_day);
}

BOOL_T app_light_schedule_is_sleep_preempted_today(VOID_T)
{
    return __is_same_calendar_day(&sg_sleep_preempt_day);
}

BOOL_T app_light_schedule_is_wake_preempted_today(VOID_T)
{
    return __is_same_calendar_day(&sg_wake_preempt_day);
}

VOID app_light_schedule_clear_sleep_preempt(VOID_T)
{
    sg_sleep_preempt_day.year = -1;
    sg_sleep_preempt_day.mon = -1;
    sg_sleep_preempt_day.mday = -1;
}

VOID app_light_schedule_clear_wake_preempt(VOID_T)
{
    sg_wake_preempt_day.year = -1;
    sg_wake_preempt_day.mon = -1;
    sg_wake_preempt_day.mday = -1;
}
