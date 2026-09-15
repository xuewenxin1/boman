/**
 * @file app_key.h
 * @author www.tuya.com
 * @brief app_key module is used to 
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#ifndef __APP_BLE_H__
#define __APP_BLE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/
// 绑定状态枚举
typedef enum {
    BIND_MODE_IDLE = 0,     // 空闲状态
    BIND_MODE_PAIRING,      // 配对中
    BIND_MODE_PAIRED        // 已配对
} BIND_MODE_E;

/***********************************************************
***********************typedef define***********************
***********************************************************/


/***********************************************************
********************function declaration********************
***********************************************************/

VOID user_ble_remote();
// 在头文件中添加这些声明
// 
OPERATE_RET user_ble_remote_add_whitelist(UINT32_T remote_id,UINT8_T group_id);
OPERATE_RET user_ble_remote_remove_whitelist(UINT32_T remote_id,UINT8_T group_id);
#ifdef __cplusplus
}
#endif

#endif /* __APP_KEY_H__ */
