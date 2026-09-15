/**
 * @file app_led.h
 * @author www.tuya.com
 * @brief app_led module is used to 
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#ifndef __APP_PWM_H__
#define __APP_PWM_H__

#include "tuya_cloud_types.h"
#include "gw_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/
#define BRIGHT_PWM                      TUYA_PWM_NUM_2
#define TEMP_PWM                        TUYA_PWM_NUM_0
#define AUX_BRIGHT_PWM                  TUYA_PWM_NUM_4
#define AUX_TEMP_PWM                    TUYA_PWM_NUM_3
// #define BEEP_PIN                        TUYA_GPIO_NUM_26
#define BEEP_PWM                        TUYA_PWM_NUM_5
/***********************************************************
***********************typedef define***********************
***********************************************************/


/***********************************************************
********************function declaration********************
***********************************************************/

VOID app_pwm_init();
VOID app_pwm();
VOID_T app_led_init();
/** 复位/配网后固定亮度100、色温55，与 APP 对齐 */
VOID_T app_led_apply_memory_light(VOID);
#ifdef __cplusplus
}
#endif

#endif /* __APP_LED_H__ */
