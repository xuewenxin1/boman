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

#ifndef __PRODUCT_TEST_H__
#define __PRODUCT_TEST_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/
VOID example_product_test(VOID);
BOOL_T is_product_test_complete(VOID);
BOOL_T is_product_test_active(VOID);
VOID __restart_wifi_scan(VOID);

/***********************************************************
********************function declaration********************
***********************************************************/
#ifdef __cplusplus
}
#endif

#endif /* __APP_KEY_H__ */
