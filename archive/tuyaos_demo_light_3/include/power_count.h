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
#ifdef __cplusplus
}
#endif

#endif /* __KV_POWER_COUNT_H__ */
