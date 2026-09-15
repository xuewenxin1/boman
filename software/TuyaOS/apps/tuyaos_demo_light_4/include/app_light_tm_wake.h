/**
 * @file app_light_tm_wake.h
 * @brief Wake light DP & timer logic
 * @version 0.1
 * @date 2025-10-14
 */

#ifndef __APP_LIGHT_TM_WAKE_H__
#define __APP_LIGHT_TM_WAKE_H__

#include "tuya_cloud_types.h"
#include "tuya_iot_com_api.h"
#ifdef __cplusplus
extern "C" {
#endif

/************************ 枚举定义 ****************************/
typedef enum {
    LIG_TM_WAKE_STATE_START   = 0,
    LIG_TM_WAKE_STATE_RUNNING = 1,
    LIG_TM_WAKE_STATE_END     = 2,
} LIG_TM_WAKE_STATE_E;

/************************ 结构体定义 ****************************/
typedef struct {
    uint8_t  enable;         ///< Byte0: 任务开关
    uint8_t duration;       ///< Byte1: 持续时长索引 1~6
    uint8_t  main_switch;    ///< Byte3: 主灯开关
    uint8_t main_percent;   ///< Byte4: 主灯亮度
    uint8_t  aux_switch;     ///< Byte5: 辅灯开关
    uint8_t aux_percent;    ///< Byte6: 辅灯亮度
    uint8_t temper;         ///< Byte7: 色温
    uint8_t start_time_en;  ///< Byte10: 启动模式 0=立即,1=定时
    uint8_t start_hour;     ///< Byte11: 启动小时
    uint8_t start_min;      ///< Byte12: 启动分钟
    uint8_t week;
} WAKE_INFO_T;

/************************ API 函数声明 ****************************/
OPERATE_RET app_light_wake_init(VOID_T);
OPERATE_RET app_light_tm_wake_dp_to_info(UCHAR_T *p_dp, UINT_T dp_len);
OPERATE_RET app_light_wake_info_to_dp(UCHAR_T **p_dp, UINT_T *p_dp_len);
OPERATE_RET app_light_start_wake_timer();
VOID app_light_stop_wake_timer(VOID_T);
/// 被抢占时完全停止当天唤醒，周期任务下一 scheduled 日再执行
VOID app_light_preempt_wake_today(VOID_T);
BOOL_T app_light_wake_is_timing(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_LIGHT_TM_WAKE_H__ */
