/**
 * @file app_power_disturb.h
 * @brief 停电勿扰功能模块头文件
 * @version 1.0
 * @date 2026-02-11
 */

#ifndef __APP_POWER_DISTURB_H__
#define __APP_POWER_DISTURB_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 停电勿扰配置结构体
typedef struct {
    UINT8_T enable;        // 功能开关: 0=关闭, 1=开启
    UINT8_T end_mode;      // 结束模式: 0=始终, 1=自定义时间
    UINT16_T start_year;   // 开始年
    UINT8_T start_month;   // 开始月
    UINT8_T start_day;     // 开始日
    UINT16_T end_year;     // 结束年
    UINT8_T end_month;     // 结束月
    UINT8_T end_day;       // 结束日
} POWER_DISTURB_CFG_T;

/**
 * @brief 初始化停电勿扰功能
 * @return OPERATE_RET 错误码
 */
OPERATE_RET app_power_disturb_init(VOID_T);

/**
 * @brief 更新停电勿扰配置
 * @param raw 原始DP数据
 * @param len 数据长度
 */
VOID app_power_disturb_update(UINT8_T *raw, UINT16_T len);

/**
 * @brief 处理开关点击事件
 * @param source 控制源: 0=墙壁开关, 1=APP, 2=遥控器
 * @return OPERATE_RET 错误码
 */
OPERATE_RET app_power_disturb_handle_switch(INT_T source);

/**
 * @brief 检查停电勿扰是否应启用
 * @return BOOL_T TRUE=应启用, FALSE=不应启用
 */
BOOL_T app_power_disturb_should_enable(VOID_T);

/**
 * @brief 检查停电勿扰是否激活
 * @return BOOL_T TRUE=激活, FALSE=未激活
 */
UINT8_T app_power_disturb_is_active(VOID_T);

/**
 * @brief 获取停电勿扰配置
 * @return POWER_DISTURB_CFG_T* 配置指针
 */
POWER_DISTURB_CFG_T* app_power_disturb_get_cfg(VOID_T);

/**
 * @brief 重置状态
 */
VOID app_power_disturb_reset_state(VOID_T);

/**
 * @brief 主循环检查函数
 */
VOID app_power_disturb_check_time(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __APP_POWER_DISTURB_H__ */