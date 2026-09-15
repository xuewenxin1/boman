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

uint8_t system_init(void);
#ifdef __cplusplus
}
#endif

#endif /* __APP_LED_H__ */