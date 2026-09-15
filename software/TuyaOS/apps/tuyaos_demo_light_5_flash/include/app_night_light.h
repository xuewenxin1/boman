#ifndef __APP_NIGHT_LIGHT_H__
#define __APP_NIGHT_LIGHT_H__


#include "tuya_cloud_types.h"

void app_nightlight_update(uint8_t *raw, uint16_t len);
bool_t app_nightlight_apply(VOID);
bool_t __in_night_time();
#endif /* __APP_NIGHTLIGHT_H__ */
