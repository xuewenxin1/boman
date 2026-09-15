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

#ifndef __APP_MODE_H__
#define __APP_MODE_H__

#include "tuya_cloud_types.h"
#include "gw_intf.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/
/***********************************************************
***********************typedef define***********************
***********************************************************/
#define MODE_GROUP_NUM   12     // 枚举组数（5~16 共12组）
#define MODE_ITEM_NUM    7      // 每组包含的数据点数：开关、主灯亮度、辅灯开关、辅灯亮度、色温、夜灯亮度

// 数据点索引定义
typedef enum {
    MODE_IDX_MAIN_SW = 0,   // 主灯开关
    MODE_IDX_MAIN_BRIGHT,   // 主灯亮度
    MODE_IDX_AUX_SW,        // 辅灯开关
    MODE_IDX_AUX_BRIGHT,    // 辅灯亮度
    MODE_IDX_CT,            // 色温
    MODE_IDX_NIGHT_SW,
    MODE_IDX_NIGHT_BRIGHT,  // 夜灯亮度
} MODE_ITEM_E;

// 单组模式数据
typedef struct {
    BOOL_T   main_sw;
    UINT8_T  main_bright;
    BOOL_T   aux_sw;
    UINT8_T  aux_bright;
    UINT8_T  color_temp;
    UINT8_T  night_sw;
    UINT8_T  night_bright;
} MODE_GROUP_T;

// 所有模式组
typedef struct {
    MODE_GROUP_T groups[MODE_GROUP_NUM];
} MODE_DATA_T;

extern MODE_DATA_T g_mode_data;

/***********************************************************
********************function declaration********************
***********************************************************/


// === API接口 ===
VOID app_mode_init(VOID);
VOID app_mode_save_raw(UCHAR_T *raw, UINT_T len);  // 保存 RAW 数据(dp102)
VOID app_mode_apply_group(UINT8_T group_idx);      // 根据枚举执行(dp21)
VOID app_mode_report_group(UINT8_T group_idx);     // 上报某组数据
VOID app_scene_apply_builtin(UINT8_T mode_idx);    // 内置情景 0~3

// === 与 Tuya SDK 对接 ===
VOID app_mode_dp_handler(CONST TY_OBJ_DP_S *dp);   // 处理 DP 下发
#ifdef __cplusplus
}
#endif

#endif /* __APP_LED_H__ */
