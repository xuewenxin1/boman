/**
 * @file app_light_countdown.h
 * @author Tuya
 * @brief Countdown timer module for light control
 * @version 0.1
 * @date 2025-09-11
 *
 * @copyright Copyright (c) tuya.inc 2025
 */

#ifndef __APP_LIGHT_COUNTDOWN_H__
#define __APP_LIGHT_COUNTDOWN_H__

#include "tuya_cloud_types.h"
#include "gw_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
 ************************macro define************************
 ***********************************************************/
typedef VOID_T (*TM_COUNT_DOWN_INFORM_CB)(VOID_T);

/***********************************************************
 ********************function declaration********************
 ***********************************************************/
/**
 * @brief Initialize countdown module
 * 
 * @param[in] over_cb Callback when countdown ends
 * @param[in] report_interval_s Countdown report interval in seconds
 * 
 * @return OPRT_OK on success, otherwise error code
 */
OPERATE_RET app_light_countdown_module_init(VOID_T);
/**
 * @brief Start countdown
 * 
 * @param[in] time_s Countdown time in seconds
 * 
 * @return OPRT_OK on success, otherwise error code
 */
OPERATE_RET app_light_start_count_down_timer(UINT_T time_s);

/**
 * @brief Stop countdown
 * 
 * @return OPRT_OK on success, otherwise error code
 */
OPERATE_RET app_light_stop_count_down_timer(VOID_T);

/**
 * @brief Sync countdown (report remaining time)
 * 
 * @return OPRT_OK on success, otherwise error code
 */
OPERATE_RET app_light_tm_count_down_syn(VOID_T);
OPERATE_RET app_light_countdown_module_init(VOID_T);
#ifdef __cplusplus
}
#endif

#endif /* __APP_LIGHT_COUNTDOWN_H__ */
