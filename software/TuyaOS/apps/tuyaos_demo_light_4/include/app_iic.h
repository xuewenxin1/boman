/**
 * @file ltr_x1303.h
 * @brief LTR-X1303 手扫传感器驱动头文件（完整版）
 * @version 2.0
 * @date 2026-02-24
 */

#ifndef __LTR_X1303_H__
#define __LTR_X1303_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化LTR-X1303传感器系统
 * @return OPERATE_RET 操作结果
 */
VOID ltrx1303_init(VOID);

/**
 * @brief 获取当前亮度值
 * @return uint8_t 当前亮度（0-100%）
 */
UINT8_T ltrx1303_get_brightness(VOID);

#ifdef __cplusplus
}
#endif

#endif /* __LTR_X1303_H__ */