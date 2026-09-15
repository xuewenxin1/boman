/**
 * @file dp_process.h
 * @author www.tuya.com
 * @brief dp_process module is used to 
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#ifndef __DP_PROCESS_H__
#define __DP_PROCESS_H__

#include "tuya_cloud_types.h"
#include "tuya_cloud_com_defs.h"
#ifdef __cplusplus
extern "C" {
#endif
#define KEY_NAME "key_name"
/***********************************************************
************************macro define************************
***********************************************************/
#define DPID_SWITCH               20 /* bool */
// #define DPID_MODE                 21 /* enum */
#define DPID_WHITE_BRIGHT         22 /* value */
#define DPID_WORK_MODE            21
#define DPID_TEMP_VALUE           23
#define DPID_SCENE_DATA           102
// #define DPID_COLOR                24 /* string */
#define COUNTDOWN                 26
#define SLEEP_MODE                31
#define WAKEUP_MODE               32
// #define DPID_POWER_MEMORY         33 /* raw */
#define DPID_SWITCH_NIGHT_LIGHT   53
#define NIGHT_LIGHT_VALUE         103
#define NIGHT_PREVIEW             104
#define NIGHT_COUNTDOWN           105
#define LIGHT_SWITCH              106
#define CUSTOM_STATUS             108
#define DEFAULT_STATE             113
#define DPID_RESET                112
#define CHANGE_LIGHT_STATUS       109
#define SWITCH_CHANGE_GEAR        110
#define RHYTHM_MODE               30
#define RHYTHM_SWITCH             116
#define RHYTHM_STATUS             115
#define SMART_NIGHT_LIGHT         111
#define OBJ_DP_NUM_MAX            40
// #define POWER_MEMORY_RAW_LEN      12

#pragma pack(push, 1)
typedef struct {
    uint8_t checksum;               // 校验和，必须是第一个字段
    uint32_t time_stamp;            // 时间戳
    uint8_t cnt;                   // 计数器
    uint8_t cnt1;                  // 第二个计数器
    uint8_t gear_memory;            // 档位记忆
    uint8_t switch_status;          // 开关状态
    uint8_t default_state;          // 默认状态
    uint8_t white_switch;           // 白光开关
    uint8_t white_bright;           // 白光亮度
    uint8_t white_temp;             // 白光色温
    uint8_t night_switch;           // 夜灯开关
    uint8_t night_bright;           // 夜灯亮度
    uint8_t change_light_status;    // 灯光变化状态
    uint8_t rhythm_switch;          // 节律开关
    uint8_t first_network;          // 首次联网标志
    uint8_t last_light_memory;
    uint8_t calibration;            //校准
    uint16_t ww_calibration_coefficient;
    uint16_t cw_calibration_coefficient;
    
    // 数组字段
    uint8_t custom_status[6];        // 自定义状态
    uint8_t sleep_init[14];         // 睡眠初始化
    uint8_t wake_init[11];          // 唤醒初始化
    uint8_t rhythm_sunlight[66];    // 节律日光
    uint8_t work_mode_value[84];    // 工作模式值
    uint8_t switch_change_gear[21]; // 开关档位变化
    uint8_t night[6];               // 夜灯
    uint8_t collect[7];             // 收集
    uint8_t remote_group[5];
    uint32_t sg_remote_whitelist[5];
} DEMO_INFO_T;
#pragma pack(pop)
/***********************************************************
********************function declaration********************
***********************************************************/
extern DEMO_INFO_T sg_demo_info;

/***********************************************************
***********************typedef define***********************
***********************************************************/

#define TY_APP_PARAM_CHECK(condition)                                                                                  \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \                                     \
            return OPRT_INVALID_PARM;                                                                                  \
        }                                                                                                              \
    } while (0)
/***********************************************************
********************function declaration********************
***********************************************************/
/**
 * @brief Output the received data and reply to the cloud
 *
 * @param[in] dp_data_arr: the array of recevie dp
 * @param[in] dp_cnt: the number of dp
 *
 * @return none
 */
VOID dp_obj_process(CONST TY_OBJ_DP_S *dp_data_arr, UINT_T dp_cnt);

/**
 * @brief Output the received raw type data and reply to the cloud
 *
 * @param[in] dpid: received raw dp id
 * @param[in] p_data: raw dp data
 * @param[in] data_len: the length of data
 * 
 * @return none
 */
VOID dp_raw_process(UINT8_T dpid, CONST UINT8_T *p_data, UINT_T data_len);

/**
 * @brief   upload swtich status
 *
 * @param[in] : state   the state of switch
 *
 * @return none
 */
VOID upload_device_bool_status(BYTE_T dpid, BOOL_T state);
VOID upload_device_value_status(BYTE_T dpid, INT_T state);
VOID upload_device_enum_status(BYTE_T dpid, UINT_T state);
/**
 * @brief report all dp to the cloud
 *
 * @param[in] none: 
 *
 * @return none
 */
VOID upload_device_all_status(VOID_T);

/**
 * @brief respone all dp status when receive query from cloud or app
 *
 * @param[in] none: 
 *
 * @return none
 */
VOID_T respone_device_all_status(VOID_T);

int device_config_save(void);
int device_config_load(void);
int device_config_save1(void);
int device_config_backup_to_c(void);
#ifdef __cplusplus
}
#endif

#endif /* __DP_PROCESS_H__ */
