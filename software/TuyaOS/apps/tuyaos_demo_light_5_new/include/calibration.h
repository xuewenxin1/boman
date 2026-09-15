#ifndef __CALIBRATION_H__
#define __CALIBRATION_H__

#include "tuya_cloud_types.h"
#include "gw_intf.h"

#ifdef __cplusplus
extern "C" {
#endif
// 引脚定义
#define PIN_CALIB_TRIG   TUYA_GPIO_NUM_23  // IO18，校准触发
#define PIN_CALIB_DET    TUYA_GPIO_NUM_22  // IO19，校准检测

// // PWM通道定义
// enum {
//     PWM_UPPER_C = 0,
//     PWM_UPPER_W = 1,
//     PWM_LOWER_C = 2,
//     PWM_LOWER_W = 3,
//     PWM_MAX
// };
uint8_t system_init(void);
#ifdef __cplusplus
}
#endif

#endif /* __APP_LED_H__ */