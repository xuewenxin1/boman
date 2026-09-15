/**
 * @file app_light_tm_sleep.h
 * @brief Sleep light DP & gradual timer definitions
 * @version 0.4
 * @date 2025-10-14
 */

#ifndef __APP_LIGHT_TM_SLEEP_H__
#define __APP_LIGHT_TM_SLEEP_H__

#include "tuya_cloud_types.h"
#include "tuya_iot_com_api.h"
#ifdef __cplusplus
extern "C" {
#endif

/************************ 枚举定义 ****************************/
/// 睡眠定时回调状态
typedef enum {
    LIG_TM_SLEEP_STATE_START   = 0,  ///< 睡眠开始
    LIG_TM_SLEEP_STATE_RUNNING = 1,  ///< 渐变进行中
    LIG_TM_SLEEP_STATE_END     = 2,  ///< 渐变结束
} LIG_TM_SLEEP_STATE_E;

#define WEEK_SUNDAY     (1 << 0)
#define WEEK_MONDAY     (1 << 1)
#define WEEK_TUESDAY    (1 << 2)
#define WEEK_WEDNESDAY  (1 << 3)
#define WEEK_THURSDAY   (1 << 4)
#define WEEK_FRIDAY     (1 << 5)
#define WEEK_SATURDAY   (1 << 6)

#define WEEK_ONCE       0x00   // 执行一次
#define WEEK_EVERYDAY   0x80   // 每天执行

/************************ 结构体定义 ****************************/
/// 睡眠功能 DP 数据结构
typedef struct {
    uint8_t  enable;         ///< Byte0: 睡眠任务开关 (0=off,1=on)
    uint8_t duration;       ///< Byte1: 持续时长索引 (1~6 → 10~60分钟)
    uint8_t start_state;    ///< Byte2: 起始状态模式 (0=当前状态, 1=自定义起始)
    uint8_t  main_on;        ///< Byte3: 主灯开关
    uint8_t main_percent;   ///< Byte4: 主灯亮度百分比 (0~100)
    uint8_t  aux_on;         ///< Byte5: 辅灯开关
    uint8_t aux_percent;    ///< Byte6: 辅灯亮度百分比 (0~100)
    uint8_t temper;         ///< Byte7: 色温百分比 (0~100)
    uint8_t  night_on;       ///< Byte8: 夜灯开关
    uint8_t night_percent;  ///< Byte9: 夜灯亮度百分比 (0~100)
    uint8_t start_mode;     ///< Byte10: 启动模式 (0=立即启动,1=定时启动)
    uint8_t hour;           ///< Byte11: 启动小时 (0~23)
    uint8_t minute;         ///< Byte12: 启动分钟 (0~59)
    uint8_t week;
} SLEEP_INFO_T;

/************************ API 函数声明 ****************************/
/// 初始化睡眠定时器逻辑
OPERATE_RET app_light_tm_sleep_init(VOID);

/// DP 数据解析 → 结构体
OPERATE_RET app_light_tm_sleep_dp_to_info(IN UCHAR_T *p_dp, IN UINT_T dp_len);

/// 结构体 → DP 数据
OPERATE_RET app_light_tm_sleep_info_to_dp(OUT UCHAR_T **p_dp, OUT UINT_T *p_dp_len);

/// 启动睡眠渐变（立即或定时启动）
OPERATE_RET app_light_start_sleep_timer();

/// 停止睡眠渐变
VOID app_light_stop_sleep_timer(VOID_T);

/// 被抢占时完全停止当天伴眠，周期任务下一 scheduled 日再执行
VOID app_light_preempt_sleep_today(VOID_T);

/// 查询是否正在执行渐变
BOOL_T app_light_tm_sleep_is_timing(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_LIGHT_TM_SLEEP_H__ */
