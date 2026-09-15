#ifndef __APP_LIGHT_TM_RHYTHM_H__
#define __APP_LIGHT_TM_RHYTHM_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RHYTHM_NODE_COUNT 8
#define RHYTHM_RAW_DATA_LEN 74

typedef enum {
    TM_RHYTHM_STATE_RUNNING = 0,
    TM_RHYTHM_STATE_STOP
} TM_RHYTHM_STATE_E;

typedef struct {
    uint8_t node_enable;     // 节点开关 0:关闭 1:开启
    uint8_t hour;            // 目标小时
    uint8_t minute;          // 目标分钟
    uint8_t per_bright;      // 亮度 1~100
    uint8_t per_temper;      // 色温 0~100
    uint8_t fade_type;       // 变光效果 0~9
    uint8_t auto_light_on;   // 开始调光时自动开灯 0~1
    uint8_t time_type;       // 时间类型 0=自定义,1=日出前,2=日出后,3=正午,4=日落前,5=日落后
} RHYTHM_NODE_DATA_T;


typedef struct {
    uint8_t mode;                          // 节律模式 0~3
    uint8_t active_day_mask;               // 生效日掩码，bit0=周日,...,bit6=周六
    RHYTHM_NODE_DATA_T nodes[RHYTHM_NODE_COUNT];
} LIG_TM_RHYTHM_CFG_T;


typedef void (*TM_RHYTHM_INFORM_CB)(RHYTHM_NODE_DATA_T *node, TM_RHYTHM_STATE_E state);
typedef struct {
    LIG_TM_RHYTHM_CFG_T cfg;
    bool is_timming;
    bool cfg_enable;               // 节律开关，单独DP控制
    TM_RHYTHM_INFORM_CB inform_cb;
} TM_RHYTHM_INFO_T;

// API
int app_light_tm_rhythm_init(TM_RHYTHM_INFORM_CB inform_cb);
void app_light_set_rhythm_timer(LIG_TM_RHYTHM_CFG_T *p_cfg);
void app_light_stop_today_rhythm_timer(void);
bool app_light_rhythm_is_in_time(void);
void app_light_tm_rhythm_syn(void);
void app_light_tm_rhythm_reset_data(void);
int app_light_tm_rhythm_dp_analysis(uint8_t *p_dp, uint32_t dp_len,
                                    LIG_TM_RHYTHM_CFG_T **p_rhythm_cfg);
void app_light_tm_rhythm_enable(void);
void app_light_tm_rhythm_disable(void);

void app_light_resume_today_rhythm_timer(void);

typedef enum {
    RHYTHM_USER_OP_LIGHT_ON = 0,  /* 手动/遥控开灯：仅期望亮度0时打断 */
    RHYTHM_USER_OP_DIM_TEMP = 1,  /* 调光/调色：开着灯且真变化才打断 */
    RHYTHM_USER_OP_LIGHT_OFF = 2, /* 手动/遥控关灯：只停渐变，不打断节律 */
} RHYTHM_USER_OP_E;

/// 当前节律时段期望亮度是否为 0（关灯节点生效中）
bool app_light_rhythm_expect_bright_zero_now(void);
/// 用户操作打断节律（开灯受期望亮度0限制；调光调色受；关灯只停渐变不打断）
void app_light_rhythm_interrupt_on_user_change(bool actually_changed, RHYTHM_USER_OP_E op);
/// 用户开夜灯：一律打断今日节律并上报（停渐变，不再跑调光/调色）
void app_light_rhythm_interrupt_on_night_open(void);
/// 节律正在跑（未打断）且当前时段非关灯：开灯应走主灯并立即同步节律，勿用夜灯记忆
bool app_light_rhythm_should_open_main_for_running(void);
void app_light_rhythm_status_update(bool interrupted);
void update_node_time(uint8_t node_index, int8_t sunrise_hour, int8_t sunrise_min,
                             int8_t sunset_hour, int8_t sunset_min);

void auto_light_on_action(RHYTHM_NODE_DATA_T *node, int node_pos, int enabled_nodes[], int enabled_count, int actual_fade_minutes);
int get_current_node_index();
void stop_fade(void);
bool check_and_perform_auto_light_on();
#ifdef __cplusplus
}
#endif
#endif
