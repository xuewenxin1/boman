/**
 * @file kv_power_count.h
 * @brief 上电计数模块（KV 存储 + 阈值触发 + 超时清零）
 * @author www.tuya.com
 * @version 0.1
 * @date 2025-09-17
 *
 * @copyright Copyright (c) tuya.inc 2025
 */

#ifndef __POWER_COUNT_H__
#define __POWER_COUNT_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
***********************macro define************************
***********************************************************/
#define KV_POWER_CNT_DEFAULT_TIMEOUT_MS   10000   ///< 默认超时时间 10 秒
#define KV_POWER_CNT_DEFAULT_THRESHOLD    5       ///< 默认连续上电阈值

/***********************************************************
********************function declaration*******************
***********************************************************/

/**
 * @brief 上电计数器
 *
 * 每次设备上电调用一次，计数自增，超过阈值可触发恢复出厂逻辑。
 *
 * @return TRUE  达到阈值
 *         FALSE 未达到阈值
 */
BOOL_T kv_power_on_count(VOID);
VOID test_gpio(VOID);
VOID example_gpio();
VOID __execute_power_loss_recovery(VOID_T);
VOID test_gpio1(VOID);
void Load_calibration(void);

/**
 * @brief 渐变前市电检查：上电未就绪时放行；其余须 AC 在且未在恢复中
 */
BOOL_T power_ac_ok_for_gradual(VOID);
/** 掉电立变灭等特例：临时允许无 AC 也走渐变 */
VOID power_ac_pwm_bypass_begin(VOID);
VOID power_ac_pwm_bypass_end(VOID);

#ifdef __cplusplus
}
#endif

#endif /* __KV_POWER_COUNT_H__ */
