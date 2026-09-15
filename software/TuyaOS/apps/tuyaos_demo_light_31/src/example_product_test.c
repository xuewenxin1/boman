/**
 * @file example_product_test
 * @author www.tuya.com
 * @version 0.1
 * @date 2023-09-28
 *
 * @copyright Copyright (c) tuya.inc 2023
 *
 */
#include "tuya_iot_config.h"
#include "tuya_cloud_wifi_defs.h"
#include "tal_log.h"
#include "prod_test.h"
#include "dp_process.h"
#include "app_pwm1.h"
#include "pwm_gradual.h"
/***********************************************************
*********************** macro define ***********************
***********************************************************/
#define APP_WF_CFG_MTHD                     GWCM_LOW_POWER 

#define PRODUCT_TEST_WIFI_1                 "tuya_mdev_test1"//"tuya_mdev_test1"
#define PRODUCT_TEST_WIFI_2                 "tuya_mdev_test2"
#define PRODUCT_TEST_WIFI_3                 "tuya_mdev_test3"
#define PRODUCT_TEST_WIFI_4                 "tuya_mdev_test4"

#define PROD_TEST_WEAK_SIGNAL               -60

/***********************************************************
********************** typedef define **********************
***********************************************************/
typedef UCHAR_T PRODUCT_TEST_TYPE_E;
#define PRODUCT_TEST_IDLE              0x00    // 空闲状态
#define PRODUCT_TEST_AGING_MAIN        0x01    // 主光老化
#define PRODUCT_TEST_AGING_NIGHT       0x02    // 夜灯老化
#define PRODUCT_TEST_AGING_WW          0x03    // 主光WW路100%老化
#define PRODUCT_TEST_AGING_CW          0x04    // 主光CW路100%老化

/***********************************************************
********************** variable define *********************
***********************************************************/
STATIC CONST CHAR_T *cWIFI_SCAN_SSID_LIST[] = {
    PRODUCT_TEST_WIFI_1, 
    PRODUCT_TEST_WIFI_2,
    PRODUCT_TEST_WIFI_3,
    PRODUCT_TEST_WIFI_4,
};

// 产测模块状态变量
STATIC PRODUCT_TEST_TYPE_E   sg_product_test_state = PRODUCT_TEST_IDLE;  // 当前测试状态
STATIC BOOL_T                sg_testing_active = FALSE;                   // 测试是否激活
STATIC BOOL_T                sg_scan_complete = FALSE;                    // 扫描是否完成

// 全局标志位 - 外部可通过 is_product_test_active() 函数查询
BOOL_T g_product_test_mode_active = FALSE;  // TRUE: 产测模式激活，其他程序应暂停
                                            // FALSE: 正常模式，执行正常业务逻辑

/***********************************************************
********************** extern  define *********************
***********************************************************/
/**
 * @brief  Scan Wi-Fi test.
 *
 * @param[in] wf_cfg_mthd GW_WF_CFG_MTHD_SEL mode.
 * @param[in] ssid_list List of SSIDs to scan.
 * @param[in] ssid_count Count of SSIDs to scan.
 * @param[in] scan_info_cb Callback function to handle the scan result.
 *
 * @return BOOL_T TRUE if the scan is successful, otherwise FALSE.
 */
extern BOOL_T ty_scan_test_wifi(GW_WF_CFG_MTHD_SEL wf_cfg_mthd, CONST CHAR_T **ssid_list, \
                                UINT8_T ssid_count, prodtest_app_cb_t scan_info_cb);

/***********************************************************
********************** function define *********************
***********************************************************/
STATIC prodtest_ssid_info_t *__ty_app_find_target_ssid(CHAR_T *target_ssid, prodtest_ssid_info_t *wifi_arr, UINT8_T arr_cnt)
{
    UINT8_T i = 0;

    if(NULL == target_ssid || NULL == wifi_arr || 0 == arr_cnt) {
        return NULL;
    }

    for(i = 0; i < arr_cnt; i++) {
        if(0 == strcmp(wifi_arr[i].ssid, target_ssid)) {
            return &wifi_arr[i];
        }
    }

    return NULL;
}

/**
 * @brief 启动老化测试
 * 
 * @param aging_type 老化类型
 * @return VOID
 */
STATIC VOID __start_aging_test(PRODUCT_TEST_TYPE_E aging_type)
{
    sg_product_test_state = aging_type;
    extern DEMO_INFO_T sg_demo_info;
    sg_testing_active = TRUE;
    g_product_test_mode_active = TRUE;  // 设置全局产测模式标志位
    uint16_t calibration_num = 10000;
    
    if(aging_type == PRODUCT_TEST_AGING_MAIN) {
        // TAL_PR_NOTICE("Starting MAIN aging test...");
        // 设置主光老化模式：CW路50%亮度，WW路50%亮度，夜灯关
        uint16_t white_bright1 = 5 + (100 - 1) * 95 / 99;
        uint8_t white_temp1 = 50;
        uint16_t ww = white_bright1 * white_temp1;
        uint16_t cw = white_bright1 * 100 - ww;
        calibration_num = sg_demo_info.ww_calibration_coefficient;
        tkl_pwm_duty_set(BRIGHT_PWM, calibration_num * ww/10000);
        tkl_pwm_start(BRIGHT_PWM);
        calibration_num = sg_demo_info.cw_calibration_coefficient;
        tkl_pwm_duty_set(TEMP_PWM, calibration_num * cw/10000);
        tkl_pwm_start(TEMP_PWM);
        tkl_pwm_duty_set(NIGHT_BRIGHT_PWM, 0);
        tkl_pwm_start(NIGHT_BRIGHT_PWM);
    } 
    else if(aging_type == PRODUCT_TEST_AGING_NIGHT) {
        // TAL_PR_NOTICE("Starting NIGHT aging test...");
        // 设置夜灯老化模式：夜灯100%亮度
        tkl_pwm_duty_set(BRIGHT_PWM, 0);
        tkl_pwm_start(BRIGHT_PWM);
        tkl_pwm_duty_set(TEMP_PWM, 0);
        tkl_pwm_start(TEMP_PWM);
        tkl_pwm_duty_set(NIGHT_BRIGHT_PWM, 10000);
        tkl_pwm_start(NIGHT_BRIGHT_PWM);
    }
    else if(aging_type == PRODUCT_TEST_AGING_WW) {
        // 主光WW路100%，CW路0%
        calibration_num = sg_demo_info.ww_calibration_coefficient;
        tkl_pwm_duty_set(BRIGHT_PWM, calibration_num);
        tkl_pwm_start(BRIGHT_PWM);
        tkl_pwm_duty_set(TEMP_PWM, 0);
        tkl_pwm_start(TEMP_PWM);
        tkl_pwm_duty_set(NIGHT_BRIGHT_PWM, 0);
        tkl_pwm_start(NIGHT_BRIGHT_PWM);
    }
    else if(aging_type == PRODUCT_TEST_AGING_CW) {
        // 主光WW路0%，CW路100%
        tkl_pwm_duty_set(BRIGHT_PWM, 0);
        tkl_pwm_start(BRIGHT_PWM);
        calibration_num = sg_demo_info.cw_calibration_coefficient;
        tkl_pwm_duty_set(TEMP_PWM, calibration_num);
        tkl_pwm_start(TEMP_PWM);
        tkl_pwm_duty_set(NIGHT_BRIGHT_PWM, 0);
        tkl_pwm_start(NIGHT_BRIGHT_PWM);
    }
}

/**
 * @brief 停止老化测试
 * 
 * @return VOID
 */
STATIC VOID __stop_aging_test(VOID)
{
    // TAL_PR_NOTICE("Aging test FINISHED, returning to normal program flow...");
    sg_product_test_state = PRODUCT_TEST_IDLE;
    sg_testing_active = FALSE;
    sg_scan_complete = TRUE;
    g_product_test_mode_active = FALSE;  // 清除全局产测模式标志位
}

STATIC OPERATE_RET __scan_wifi_test_info_cb(INT_T flag, prodtest_ssid_info_t *ssid_info, UINT8_T info_count)
{
    prodtest_ssid_info_t *p_ssid_test1 = NULL;
    prodtest_ssid_info_t *p_ssid_test2 = NULL;
    prodtest_ssid_info_t *p_ssid_test3 = NULL;
    prodtest_ssid_info_t *p_ssid_test4 = NULL;
    
    // TAL_PR_NOTICE("WiFi scan completed, found %d networks", info_count);
    
    // 如果正在测试中，只检查当前的测试SSID是否消失
    if(sg_testing_active) {
        if(sg_product_test_state == PRODUCT_TEST_AGING_MAIN) {
            p_ssid_test1 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_1, ssid_info, info_count);
            
            // 检查授权状态
            if(flag != 1) {
                // TAL_PR_NOTICE("Main aging test: Device not authorized, waiting...");
                __restart_wifi_scan();
                return OPRT_OK;
            }
            
            if(p_ssid_test1 == NULL) {
                // test1路由器信号消失，结束老化测试
                // TAL_PR_NOTICE("Main aging test SSID disappeared, stopping test");
                __stop_aging_test();
            } else {
                // test1仍然存在，继续老化
                if(p_ssid_test1->rssi < PROD_TEST_WEAK_SIGNAL) {
                    // TAL_PR_NOTICE("Main aging test: weak signal (rssi:%d), but continuing...", p_ssid_test1->rssi);
                }
                // TAL_PR_NOTICE("Main aging test continuing... RSSI: %d", p_ssid_test1->rssi);
                // 继续老化，稍后重新扫描
                __restart_wifi_scan();
            }
        } 
        else if(sg_product_test_state == PRODUCT_TEST_AGING_NIGHT) {
            p_ssid_test2 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_2, ssid_info, info_count);
            
            // 检查授权状态
            if(flag != 1) {
                // TAL_PR_NOTICE("Night aging test: Device not authorized, waiting...");
                __restart_wifi_scan();
                return OPRT_OK;
            }
            
            if(p_ssid_test2 == NULL) {
                // test2路由器信号消失，结束老化测试
                // TAL_PR_NOTICE("Night aging test SSID disappeared, stopping test");
                __stop_aging_test();
            } else {
                // test2仍然存在，继续老化
                if(p_ssid_test2->rssi < PROD_TEST_WEAK_SIGNAL) {
                    // TAL_PR_NOTICE("Night aging test: weak signal (rssi:%d), but continuing...", p_ssid_test2->rssi);
                }
                // TAL_PR_NOTICE("Night aging test continuing... RSSI: %d", p_ssid_test2->rssi);
                // 继续老化，稍后重新扫描
                __restart_wifi_scan();
            }
        }
        else if(sg_product_test_state == PRODUCT_TEST_AGING_WW) {
            p_ssid_test3 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_3, ssid_info, info_count);
            
            if(flag != 1) {
                __restart_wifi_scan();
                return OPRT_OK;
            }
            
            if(p_ssid_test3 == NULL) {
                __stop_aging_test();
            } else {
                __restart_wifi_scan();
            }
        }
        else if(sg_product_test_state == PRODUCT_TEST_AGING_CW) {
            p_ssid_test4 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_4, ssid_info, info_count);
            
            if(flag != 1) {
                __restart_wifi_scan();
                return OPRT_OK;
            }
            
            if(p_ssid_test4 == NULL) {
                __stop_aging_test();
            } else {
                __restart_wifi_scan();
            }
        }
        return OPRT_OK;
    }
    
    // 如果没有激活测试，同时搜索四个SSID
    p_ssid_test1 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_1, ssid_info, info_count);
    p_ssid_test2 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_2, ssid_info, info_count);
    p_ssid_test3 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_3, ssid_info, info_count);
    p_ssid_test4 = __ty_app_find_target_ssid(PRODUCT_TEST_WIFI_4, ssid_info, info_count);
    
    // 检查授权状态
    if(flag != 1) {
        // TAL_PR_NOTICE("Device not authorized, waiting for authorization...");
        __restart_wifi_scan();
        return OPRT_OK;
    }
    
    // 优先检查test1（主光老化）
    if(p_ssid_test1 != NULL) {
        if(p_ssid_test1->rssi < PROD_TEST_WEAK_SIGNAL) {
            // TAL_PR_NOTICE("Found test1 but weak signal (rssi:%d), continuing scan...", p_ssid_test1->rssi);
            __restart_wifi_scan();
        } else {
            // TAL_PR_NOTICE("Found test1 with good signal (rssi:%d), starting MAIN aging test", p_ssid_test1->rssi);
            __start_aging_test(PRODUCT_TEST_AGING_MAIN);
            // 开始老化后，需要监控信号是否消失
            __restart_wifi_scan();
        }
        return OPRT_OK;
    }
    
    // 如果没有test1，检查test2（夜灯老化）
    if(p_ssid_test2 != NULL) {
        if(p_ssid_test2->rssi < PROD_TEST_WEAK_SIGNAL) {
            // TAL_PR_NOTICE("Found test2 but weak signal (rssi:%d), continuing scan...", p_ssid_test2->rssi);
            __restart_wifi_scan();
        } else {
            // TAL_PR_NOTICE("Found test2 with good signal (rssi:%d), starting NIGHT aging test", p_ssid_test2->rssi);
            __start_aging_test(PRODUCT_TEST_AGING_NIGHT);
            // 开始老化后，需要监控信号是否消失
            __restart_wifi_scan();
        }
        return OPRT_OK;
    }

    // test3：主光WW路100%
    if(p_ssid_test3 != NULL) {
        if(p_ssid_test3->rssi < PROD_TEST_WEAK_SIGNAL) {
            __restart_wifi_scan();
        } else {
            __start_aging_test(PRODUCT_TEST_AGING_WW);
            __restart_wifi_scan();
        }
        return OPRT_OK;
    }

    // test4：主光CW路100%
    if(p_ssid_test4 != NULL) {
        if(p_ssid_test4->rssi < PROD_TEST_WEAK_SIGNAL) {
            __restart_wifi_scan();
        } else {
            __start_aging_test(PRODUCT_TEST_AGING_CW);
            __restart_wifi_scan();
        }
        return OPRT_OK;
    }
    
    // 四个SSID都没有找到
    // TAL_PR_NOTICE("No target SSID found, program continues...");
    sg_scan_complete = TRUE;
    
    return OPRT_COM_ERROR; // 返回错误，表示没有找到目标SSID
}

/**
 * @brief 重启Wi-Fi扫描
 * 
 * @return VOID
 */
VOID __restart_wifi_scan(VOID)
{
    // 只在没有激活老化测试时才重新扫描
    if(!sg_testing_active) {
        // TAL_PR_NOTICE("Restarting WiFi scan...");
        ty_scan_test_wifi(APP_WF_CFG_MTHD, cWIFI_SCAN_SSID_LIST, 
                         CNTSOF(cWIFI_SCAN_SSID_LIST), __scan_wifi_test_info_cb);
    }
}

STATIC OPERATE_RET __user_product_test_cmd_cb(USHORT_T cmd, UCHAR_T *data, UINT_T len,\
                                              OUT UCHAR_T **ret_data, OUT USHORT_T *ret_len)
{
    //todo: process user product test command from production
    return OPRT_OK;
}

/**
 * @brief Function that performs the product test.
 *
 * The function scans for Wi-Fi access points specified in cWIFI_SCAN_SSID_LIST
 * and performs the product test using the __scan_wifi_test_info_cb callback function.
 * If the scan is successful and the target SSID is found, the function proceeds with the appropriate product test. 
 * The function also registers the __user_product_test_cmd_cb callback
 * to process any user product test command from production.
 *
 * @return None.
 */ 
VOID example_product_test(VOID)
{
    // 初始化测试状态
    sg_product_test_state = PRODUCT_TEST_IDLE;
    sg_testing_active = FALSE;
    sg_scan_complete = FALSE;
    g_product_test_mode_active = FALSE;  // 确保标志位初始化为FALSE
    
    // TAL_PR_NOTICE("Product test STARTING...");
    
    // 启动Wi-Fi扫描
    if(TRUE == ty_scan_test_wifi(APP_WF_CFG_MTHD, cWIFI_SCAN_SSID_LIST, \
                                 CNTSOF(cWIFI_SCAN_SSID_LIST), __scan_wifi_test_info_cb)) {
        // TAL_PR_NOTICE("Initial WiFi scan started");
    }
    
    return;
}

/**
 * @brief 检查产测是否完成
 * 
 * @return BOOL_T TRUE: 产测完成，程序可以继续执行后续流程
 */
BOOL_T is_product_test_complete(VOID)
{
    return sg_scan_complete;
}

/**
 * @brief 检查是否处于产测/老化模式
 * 
 * @return BOOL_T TRUE: 设备处于产测/老化模式，其他程序应暂停
 *                FALSE: 设备处于正常模式
 */
BOOL_T is_product_test_active(VOID)
{
    return g_product_test_mode_active;
}
