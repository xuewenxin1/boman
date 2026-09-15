// /**
//  * @file app_power_disturb.c
//  * @brief 停电勿扰功能模块
//  * @version 1.1
//  * @date 2026-02-11
//  * @note 添加了线程循环检查时间功能
//  */

// #include "app_power_disturb.h"
// #include "tuya_iot_com_api.h"
// #include "tal_log.h"
// #include "dp_process.h"
// #include "tal_sw_timer.h"
// #include "tal_time_service.h"
// #include "tuya_ws_db.h"
// #include "app_pwm.h"
// #include "pwm_gradual.h"
// #include <string.h>
// #include "tuya_error_code.h"
// #include "tal_thread.h"

// // 全局变量
// static POWER_DISTURB_CFG_T sg_power_disturb_cfg = {0};
// static TIMER_ID sg_double_click_timer = NULL;      // 双击检测定时器
// static THREAD_HANDLE sg_check_thread = NULL;       // 检查线程句柄

// // 使用简单变量代替结构体
// TIMER_ID power_disturb_timer = NULL;
// uint8_t power_disturb_raw[10];  // 原始DP数据

// /**
//  * @brief 双击超时回调
//  */
// STATIC VOID_T __double_click_timeout_cb(TIMER_ID timer_id, VOID_T *arg)
// {
//     // TAL_PR_DEBUG("Double click timeout");
//     sg_demo_info.cnt1 = 0;
// }

// /**
//  * @brief 电源上电检测回调
//  */
// STATIC VOID_T __power_on_detection_cb(PVOID_T pTimerArg)
// {
//     OPERATE_RET rt = OPRT_OK;
//     extern DEMO_INFO_T sg_demo_info;
//     extern bool time_synced;
    
//     UINT32_T current_time = tal_system_get_millisecond();
//     if (current_time < 10000) return;
//     if (!time_synced) return;
    
//     tal_sw_timer_stop(power_disturb_timer);
//     tal_sw_timer_delete(power_disturb_timer);
//     // TAL_PR_NOTICE("__power_on_detection_cb,sg_demo_info.cnt1 = %d\r\n",sg_demo_info.cnt1);
//     // 如果停电勿扰功能开启且在有效时间内
//     if (app_power_disturb_is_active() == 1 && sg_demo_info.cnt1 == 0) {
        
//         // 关闭所有灯
//         TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
//         UINT32_T duties[] = {0, 0, 0, 0};
//         pwm_gradual_duty_set_multi(4, channels, duties);
        
//         // 上报状态
//         upload_device_bool_status(DPID_SWITCH, 0);
//         upload_device_bool_status(LIGHT_SWITCH, 0);
//         upload_device_bool_status(AUX_SWITCH, 0);
        
//         // 启动双击检测定时器
//         TUYA_CALL_ERR_LOG(tal_sw_timer_create(__double_click_timeout_cb, NULL, &sg_double_click_timer));
//         TUYA_CALL_ERR_LOG(tal_sw_timer_start(sg_double_click_timer, 4000, TAL_TIMER_ONCE));
//     }
//     else if(app_power_disturb_is_active() == 2)
//     {
//         sg_demo_info.power_outage[0] = 0;
//         dev_report_dp_raw_sync(NULL, POWER_OUTAGE, sg_demo_info.power_outage, 10, 5);
//     }
// }



// /**
//  * @brief 检查当前是否在停电勿扰时间段内
//  */
// UINT8_T app_power_disturb_is_active(VOID_T)
// {
//     // TAL_PR_NOTICE("sg_power_disturb_cfg.end_mode = %d\r\n",sg_power_disturb_cfg.end_mode);
//     // 结束模式：0=始终停电勿扰，1=自定义时间
//     if (sg_power_disturb_cfg.end_mode == 0) {
//         // 始终有效
//         return 1;
//     } else {
//         // 检查是否在自定义时间范围内
//         extern POSIX_TM_S local_tm;
        
//         // 获取当前日期
//         UINT_T current_date = (local_tm.tm_year + 1900) * 10000 + 
//                              (local_tm.tm_mon + 1) * 100 + 
//                              local_tm.tm_mday;
        
//         // 获取开始日期
//         UINT_T start_date = sg_power_disturb_cfg.start_year * 10000 + 
//                            sg_power_disturb_cfg.start_month * 100 + 
//                            sg_power_disturb_cfg.start_day;
        
//         // 获取结束日期
//         UINT_T end_date = sg_power_disturb_cfg.end_year * 10000 + 
//                          sg_power_disturb_cfg.end_month * 100 + 
//                          sg_power_disturb_cfg.end_day;

//         if(current_date >= start_date && current_date <= end_date)
//             return 1;
//         if(current_date > end_date)
//             return 2;
//     }
// }

// /**
//  * @brief 初始化停电勿扰功能
//  */
// OPERATE_RET app_power_disturb_init(VOID_T)
// {
//     OPERATE_RET rt = OPRT_OK;
//     extern DEMO_INFO_T sg_demo_info;
//     extern bool time_synced;
//     memcpy(&sg_power_disturb_cfg,sg_demo_info.power_outage,10);
    
//     // 检测电源上电
//     if(sg_demo_info.power_outage[0]){
//         tal_sw_timer_create(__power_on_detection_cb, NULL, &power_disturb_timer);
//         tal_sw_timer_start(power_disturb_timer, 1000, TAL_TIMER_CYCLE);
//     }

//     return OPRT_OK;
// }

// /**
//  * @brief 处理来自 DP 的停电勿扰 raw 数据
//  */
// VOID app_power_disturb_update(UINT8_T *raw, UINT16_T len)
// {
//     OPERATE_RET rt = OPRT_OK;
    
//     if (!raw || len != 10) {
//         // TAL_PR_NOTICE("Invalid power disturb data");
//         return;
//     }
    
//     // TAL_PR_DEBUG("Update power disturb config");
    
//     // 保存原始数据
//     memcpy(power_disturb_raw, raw, 10);
    
//     // 解析配置
//     sg_power_disturb_cfg.enable = raw[0];
//     sg_power_disturb_cfg.end_mode = raw[1];
//     sg_power_disturb_cfg.start_year = (raw[2] << 8) | raw[3];
//     sg_power_disturb_cfg.start_month = raw[4];
//     sg_power_disturb_cfg.start_day = raw[5];
//     sg_power_disturb_cfg.end_year = (raw[6] << 8) | raw[7];
//     sg_power_disturb_cfg.end_month = raw[8];
//     sg_power_disturb_cfg.end_day = raw[9];
// }
