/**
 * @file tuya_app_main.c
 * @author www.tuya.com
 * @brief tuya_app_main module is used to
 * @version 0.1
 * @date 2022-10-28
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include <stdlib.h>

#include "tuya_cloud_types.h"
#include "tuya_svc_netmgr.h"
#if defined(ENABLE_WIFI_SERVICE) && (ENABLE_WIFI_SERVICE == 1)
#include "tuya_iot_wifi_api.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "tuya_iot_base_api.h"
#endif
#include "tuya_iot_com_api.h"
#include "tuya_ws_db.h"

#include "tal_system.h"
#include "tal_log.h"
#include "base_event.h"
#include "mf_test.h"
#include "mqc_app.h"
#if defined(ENABLE_LWIP) && (ENABLE_LWIP == 1)
#include "lwip_init.h"
#endif

#include "tal_uart.h"
#include "dp_process.h"
#include "app_ble.h"
#include "power_count.h"
#include "app_light_tm_rhythm.h"
#include "light_count_down.h"
#include "app_night_light.h"
#include "app_pwm.h"
#include "tal_sw_timer.h"
#include "power_count.h"
#include "calibration.h"
#include "ty_meta_report.h"
#include "tkl_thread.h"
#include "example_product_test.h"
/***********************************************************
************************macro define************************
***********************************************************/

#define PID "key7ga5dmfcrq3mk"
// #define UUID        "uuid8c979a2cea91955c"
// #define AUTHKEY     "8TxpnLMkuovxRtlh5TfICLipjzZKnc07"

/* The registration code here does not work, you need to apply for a new one.
 * https://developer.tuya.com/cn/docs/iot/lisence-management?id=Kb4qlem97idl0
 */
// #define UUID     "f998xxxxxxxx2409"
// #define AUTHKEY  "WEHAxxxxxxxxxxxxxxxxxxxxxxxxVVkf"

/* network button, LED pin */

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/
extern void tuya_ble_enable_debug(bool enable);

/***********************************************************
***********************variable define**********************
***********************************************************/
/* app thread handle */
STATIC THREAD_HANDLE ty_app_thread = NULL;

/***********************************************************
***********************function define**********************
***********************************************************/
// output qrcode when ENABLE_QRCODE_ACTIVE == 1 && ENABLE_WIFI_QRCODE == 0
#if (defined(ENABLE_QRCODE_ACTIVE) && (ENABLE_QRCODE_ACTIVE == 1)) && (!(defined(ENABLE_WIFI_QRCODE) && (ENABLE_WIFI_QRCODE == 1)))
// qrcode打印
extern INT_T qrcode_exec(INT_T argc, CHAR_T **argv);
STATIC INT_T __qrcode_printf(CHAR_T *msg)
{
    CHAR_T *qrcode_argv[] = {
        "qrcode_exec", "-m", "3", "-t", "ansiutf8", msg};

    return qrcode_exec(sizeof(qrcode_argv) / sizeof(qrcode_argv[0]), qrcode_argv);
}

// TuyaOS获取到短链接之后调用此接口输出qrcode打印
STATIC VOID __qrcode_active_shourturl_cb(CONST CHAR_T *shorturl)
{
    if (NULL == shorturl)
    {
        return;
    }

    ty_cJSON *item = ty_cJSON_Parse(shorturl);
    __qrcode_printf(ty_cJSON_GetObjectItem(item, "shortUrl")->valuestring);
    ty_cJSON_Delete(item);

    return;
}
#endif

/**
 * @brief SOC device upgrade entry
 *
 * @param[in] fw: firmware info
 *
 * @return OPRT_OK on success. Others on error, please refer to "tuya_error_code.h".
 */
STATIC OPERATE_RET __soc_dev_rev_upgrade_info_cb(IN CONST FW_UG_S *fw)
{
    return OPRT_OK;
}

/**
 * @brief SOC device cloud state change callback
 *
 * @param[in] status: current status
 *
 * @return none
 */
STATIC VOID_T __soc_dev_status_changed_cb(IN CONST GW_STATUS_E status)
{
    return;
}

/**
 * @brief SOC device DP query entry
 *
 * @param[in] dp_qry: DP query list
 *
 * @return none
 */
STATIC VOID_T __soc_dev_dp_query_cb(IN CONST TY_DP_QUERY_S *dp_qry)
{
    UINT32_T index = 0;

    if (dp_qry->cid != NULL)
    {
    }

    if (dp_qry->cnt == 0)
    {
        respone_device_all_status();
    }
    else
    {
        for (index = 0; index < dp_qry->cnt; index++)
        {
        }
    }

    return;
}

/**
 * @brief SOC device format command data delivery entry
 *
 * @param[in] dp: obj dp info
 *
 * @return none
 */
STATIC VOID_T __soc_dev_obj_dp_cmd_cb(IN CONST TY_RECV_OBJ_DP_S *dp)
{
    dp_obj_process(dp->dps, dp->dps_cnt);

    return;
}

/**
 * @brief SOC device transparently transmits command data delivery entry
 *
 * @param[in] dp: raw dp info
 *
 * @return none
 */
STATIC VOID_T __soc_dev_raw_dp_cmd_cb(IN CONST TY_RECV_RAW_DP_S *dp)
{
    dp_raw_process(dp->dpid, dp->data, dp->len);

    return;
}

/**
 * @brief  app process when device reset
 *
 * @param[in] type: gateway reset type
 *
 * @return none
 */
uint8_t custom_status[6] = {
    0x01, 0x01, 0x64, 0x01, 0x64, 0x37}; /* [0]=记忆; 主开/100; 色温55 */
uint8_t sleep_init[14] = {
    0x00, 0x03, 0x01, 0x01, 0x64, 0x01, 0x64, 0x37, 0x00, 0x00, 0x00, 0x17, 0x00, 0x00};
uint8_t wake_init[11] = {
    0x00, 0x03, 0x01, 0x64, 0x01, 0x64, 0x64, 0x00, 0x07, 0x00, 0x00};
uint8_t night[6] = {0x00, 0x32, 0x17, 0x00, 0x06, 0x00};
uint8_t switch_change_gear[21] = {
    0x01, 0x01, 0x01, 0x64, 0x01, 0x64, 0x64, 0x01, 0x01, 0x64, 0x01, 0x64, 0x37, 0x01, 0x01, 0x3C, 0x01, 0x3C, 0x23, 0x01, 0x32};
uint8_t collect[7] = {
    0x01, 0x50, 0x01, 0x50, 0x00, 0x00, 0x32};
STATIC VOID_T __soc_dev_reset_inform_cb(GW_RESET_TYPE_E type)
{
    extern DEMO_INFO_T sg_demo_info;
    extern uint8_t rhythm_sunlight1[66];

    if (type == GW_REMOTE_UNACTIVE)
    {
        sg_demo_info.first_network = 0;
        device_config_save1();
        tuya_iot_wf_gw_unactive();
    }
    else if (type == GW_REMOTE_RESET_FACTORY)
    {
        sg_demo_info.checksum = 0xa5;
        sg_demo_info.cnt = 0;
        sg_demo_info.cnt1 = 0;
        sg_demo_info.gear_memory = 1;
        sg_demo_info.switch_status = 1;
        sg_demo_info.default_state = 0;
        sg_demo_info.white_switch = 1;
        sg_demo_info.white_bright = 100;
        sg_demo_info.last_light_memory = 1; /* 重置后默认主+辅记忆 */
        sg_demo_info.white_temp = 55;
        sg_demo_info.aux_switch = 1;
        sg_demo_info.aux_bright = 100;
        sg_demo_info.night_switch = 0;
        sg_demo_info.night_bright = 50;
        sg_demo_info.change_light_status = 1;
        sg_demo_info.rhythm_switch = 0;
        sg_demo_info.first_network = 0;
        memset(sg_demo_info.custom_status, 0, 6);
        memset(sg_demo_info.sleep_init, 0, 14);
        memset(sg_demo_info.wake_init, 0, 11);
        memset(sg_demo_info.rhythm_sunlight, 0, 66);
        memset(sg_demo_info.work_mode_value, 0, 84);
        memset(sg_demo_info.switch_change_gear, 0, 21);
        memset(sg_demo_info.night, 0, 6);
        memset(sg_demo_info.collect, 0, 7);
        memcpy(sg_demo_info.custom_status, custom_status, 6);
        memcpy(sg_demo_info.sleep_init, sleep_init, 14);
        memcpy(sg_demo_info.wake_init, wake_init, 11);
        memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);
        memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
        memcpy(sg_demo_info.night, night, 6);
        memcpy(sg_demo_info.collect, collect, 7);
        device_config_save1();
    }
    return;
}

/**
 * @brief SOC external network status change callback
 *
 * @param[in/out] data
 * @return STATIC
 */

STATIC OPERATE_RET __soc_dev_net_status_cb(VOID *data)
{
    STATIC BOOL_T s_syn_all_status = FALSE;

    if (tuya_svc_netmgr_linkage_is_up(LINKAGE_TYPE_DEFAULT))
    {
        if (get_mqc_conn_stat())
        {
            if (FALSE == s_syn_all_status)
            {
                upload_device_all_status();
                s_syn_all_status = TRUE;
            }
        }
    }

    return OPRT_OK;
}

/**
 * @brief mf uart init
 *
 * @param[in] baud: Baud rate
 * @param[in] bufsz: uart receive buffer size
 *
 * @return none
 */
VOID mf_uart_init_callback(UINT_T baud, UINT_T bufsz)
{
    TAL_UART_CFG_T cfg;
    memset(&cfg, 0, sizeof(TAL_UART_CFG_T));
    cfg.base_cfg.baudrate = baud;
    cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    cfg.base_cfg.parity = TUYA_UART_PARITY_TYPE_NONE;
    cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    cfg.rx_buffer_size = bufsz;

    tal_uart_init(TUYA_UART_NUM_0, &cfg);

    return;
}

/**
 * @brief mf uart free
 *
 * @param[in] none
 *
 * @return none
 */
VOID mf_uart_free_callback(VOID)
{
    tal_uart_deinit(TUYA_UART_NUM_0);
    return;
}

/**
 * @brief mf uart send function
 *
 * @param[in] data: send data
 * @param[in] len: send data length
 *
 * @return none
 */
VOID mf_uart_send_callback(IN BYTE_T *data, IN CONST UINT_T len)
{
    tal_uart_write(TUYA_UART_NUM_0, data, len);
    return;
}

/**
 * @brief mf uart receive function
 *
 * @param[in] buf: receive buffer
 * @param[in] len: receive buffer max length
 *
 * @return receive data length
 */
UINT_T mf_uart_recv_callback(OUT BYTE_T *buf, IN CONST UINT_T len)
{
    return tal_uart_read(TUYA_UART_NUM_0, buf, len);
}

/**
 * @brief Product test callback function
 *
 * @param[in] cmd: Command
 * @param[in] data: data
 * @param[out] ret_data: Resulting data
 * @param[out] ret_len: Resulting data length
 *
 * @return OPRT_OK on success. Others on error, please refer to "tuya_error_code.h".
 */
OPERATE_RET mf_user_product_test_callback(USHORT_T cmd, UCHAR_T *data, UINT_T len, OUT UCHAR_T **ret_data, OUT USHORT_T *ret_len)
{
    /* USER todo */
    // gpio  test  refer to tuyaos_demo_examples -> src/examples/service_mf_test

    return OPRT_OK;
}

/**
 * @brief mf configure write callback functions
 *
 * @param[in] none
 *
 * @return none
 */
VOID mf_user_callback(VOID)
{
    return;
}

/**
 * @brief Callback function before entering the production test
 *
 * @param[in] none
 *
 * @return none
 */
VOID mf_user_enter_mf_callback(VOID)
{
    return;
}

STATIC THREAD_HANDLE blink_thread_id = NULL;

STATIC VOID blink_thread(PVOID_T args)
{
    // network is not connected and not activated//
    app_led_init();
    tal_thread_delete(blink_thread_id);
}

VOID ty_app_wf_nw_stat_cb(IN CONST GW_WIFI_NW_STAT_E stat)
{
    extern uint16_t gradual_time_ms;
    extern DEMO_INFO_T sg_demo_info;
    const THREAD_CFG_T thread_cfg = {
        .thrdname = "blink_thread",
        .stackDepth = 4 * 1024,
        .priority = THREAD_PRIO_4,
    };

    if (((stat == 1) || (stat == 2) || (stat == 13))&&(sg_demo_info.first_network == 0)) {
        tal_thread_create_and_start(&blink_thread_id, NULL, NULL, blink_thread, NULL, &thread_cfg);
    }else if(((stat == 1) || (stat == 2) || (stat == 13))&&(sg_demo_info.first_network > 0)){
        TUYA_PWM_NUM_E channels7[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
        UINT32_T duties7[] = {5500, 4500, 5500, 4500, 0};
        pwm_gradual_duty_set_multi(5, channels7, duties7);
        if (sg_demo_info.change_light_status == 0)
            gradual_time_ms = 20;
        else
            gradual_time_ms = 1600;
    }
}
/**
 * @brief SOC device initialization
 *
 * @param[in] none
 *
 * @return OPRT_OK on success. Others on error, please refer to "tuya_error_code.h".
 */
uint16_t gradual_time_ms = 1600;
uint8_t calibration = 0;
OPERATE_RET ty_app_light_start1(VOID *data)
{
    app_time_service();
    extern DEMO_INFO_T sg_demo_info;
    extern uint8_t rhythm_sunlight1[66];
    // tkl_flash_erase(0x1EF000, 2 * 1024);
    if(device_config_load() > 0)
    {
        if (sg_demo_info.change_light_status == 0)
            gradual_time_ms = 20;
        else
            gradual_time_ms = 1600;
    }
    
    if (!(is_product_test_active())) {
        calibration = system_init();
        if(calibration == 1)
            test_gpio1();
    }
}

OPERATE_RET __soc_device_init(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    ty_subscribe_event(EVENT_LINK_UP, "quickstart", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    ty_subscribe_event(EVENT_LINK_DOWN, "quickstart", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    ty_subscribe_event(EVENT_MQTT_CONNECTED, "quickstart", __soc_dev_net_status_cb, SUBSCRIBE_TYPE_NORMAL);
    // 
#if (defined(UUID) && defined(AUTHKEY))
#ifndef ENABLE_KV_FILE
    ws_db_init_mf();
#endif
    /* Set authorization information
     * Note that if you use the default authorization information of the code, there may be problems of multiple users and conflicts,
     * so try to use all the authorizations purchased from the tuya iot platform.
     * Buying guide: https://developer.tuya.com/cn/docs/iot/lisence-management?id=Kb4qlem97idl0.
     * You can also apply for two authorization codes for free in the five-step hardware development stage of the Tuya IoT platform.
     * Authorization information can also be written through the production testing tool.
     * When the production testing function is started and the authorization is burned with the Tuya Cloud module tool,
     * please comment out this piece of code.
     */
#ifdef ENABLE_WIFI_SERVICE
    WF_GW_PROD_INFO_S prod_info = {UUID, AUTHKEY};
    TUYA_CALL_ERR_RETURN(tuya_iot_set_wf_gw_prod_info(&prod_info));
#else
    GW_PROD_INFO_S prod_info = {UUID, AUTHKEY};
    TUYA_CALL_ERR_RETURN(tuya_iot_set_gw_prod_info(&prod_info));
#endif

#else
    /*authorization is burned with the Tuya Cloud module tool
     *If you want to get the specific details, such as GPIO TEST,
     *please refer to the tuyaos_demo_examples -> src/examples/service_mf_test.*/
    MF_IMPORT_INTF_S intf = {0};

    intf.uart_init = mf_uart_init_callback;
    intf.uart_free = mf_uart_free_callback;
    intf.uart_send = mf_uart_send_callback;
    intf.uart_recv = mf_uart_recv_callback;

    intf.mf_user_product_test = mf_user_product_test_callback;
    intf.user_callback = mf_user_callback;
    intf.user_enter_mf_callback = mf_user_enter_mf_callback;
    // mf_test_ignore_close_flag();

    TUYA_CALL_ERR_RETURN(mf_init(&intf, APP_BIN_NAME, USER_SW_VER, TRUE));
    set_wf_netcfg_timeout(180);
#endif
    
    /* Initialize TuyaOS product information */
    TY_IOT_CBS_S iot_cbs = {0};
    iot_cbs.gw_status_cb = __soc_dev_status_changed_cb;
    iot_cbs.gw_ug_cb = __soc_dev_rev_upgrade_info_cb;
    iot_cbs.gw_reset_cb = __soc_dev_reset_inform_cb;
    iot_cbs.dev_obj_dp_cb = __soc_dev_obj_dp_cmd_cb;
    iot_cbs.dev_raw_dp_cb = __soc_dev_raw_dp_cmd_cb;
    iot_cbs.dev_dp_query_cb = __soc_dev_dp_query_cb;


#if (defined(ENABLE_QRCODE_ACTIVE) && (ENABLE_QRCODE_ACTIVE == 1)) && (!(defined(ENABLE_WIFI_QRCODE) && (ENABLE_WIFI_QRCODE == 1)))
    iot_cbs.active_shorturl = __qrcode_active_shourturl_cb;
#endif
    tuya_iot_oem_set(TRUE);
#ifdef ENABLE_WIFI_SERVICE
    TUYA_CALL_ERR_RETURN(tuya_iot_wf_soc_dev_init(GWCM_SPCL_MODE, WF_START_AP_FIRST, &iot_cbs, PID, USER_SW_VER));
#ifdef ENABLE_WIRED
    // init wired linkage
    TUYA_CALL_ERR_RETURN(tuya_svc_wired_init());
#endif
#else
    TUYA_CALL_ERR_RETURN(tuya_iot_soc_init(&iot_cbs, PID, USER_SW_VER));
#endif

#ifdef ENABLE_BT_SERVICE
    tuya_ble_enable_debug(false);
#endif
    if(calibration == 1){//&&sg_demo_info.calibration == 1
        tuya_iot_reg_get_wf_nw_stat_cb(ty_app_wf_nw_stat_cb);
        example_product_test();
        user_ble_remote();
        test_gpio();
        __execute_power_loss_recovery();
        app_light_on();
        app_mode_init();
        
        app_light_tm_rhythm_init(NULL);
        app_light_tm_sleep_init();
        app_light_wake_init();
        app_light_countdown_module_init();
        device_config_save1();
    }
    
    return 0;
}

STATIC VOID_T user_main(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    tuya_base_utilities_init();
    //TUYA_CALL_ERR_LOG(ty_app_msg_queue_init());
    ty_subscribe_event(EVENT_SDK_EARLY_INIT_OK, "start", ty_app_light_start1, SUBSCRIBE_TYPE_NORMAL);

    /*pwm init*/
    TUYA_PWM_BASE_CFG_T pwm_cfg1 = {
        .duty = 0, /* 1-10000 */
        .frequency = 3000,
        .polarity = TUYA_PWM_NEGATIVE,
    };
    tkl_pwm_init(BRIGHT_PWM, &pwm_cfg1);
    tkl_pwm_init(TEMP_PWM, &pwm_cfg1);
    tkl_pwm_init(AUX_BRIGHT_PWM, &pwm_cfg1);
    tkl_pwm_init(AUX_TEMP_PWM, &pwm_cfg1);
    tkl_pwm_init(NIGHT_BRIGHT_PWM, &pwm_cfg1);
    
    /* Initialization, because DB initialization takes a long time,
     * which affects the startup efficiency of some devices,
     * so special processing is performed during initialization to delay initialization of DB
     */
    //  ty_subscribe_event(EVENT_BLE_INIT, "quickstart", __soc_dev_cb, SUBSCRIBE_TYPE_NORMAL);
#if OPERATING_SYSTEM == SYSTEM_LINUX
    rt = system("mkdir -p ./tuya_db_files/");
    TUYA_CALL_ERR_LOG(tuya_iot_init_params("./tuya_db_files/", NULL));
#else
    TY_INIT_PARAMS_S init_param = {0};
    // init_param.init_db = TRUE;
    init_param.init_db = FALSE;
    strcpy(init_param.sys_env, TARGET_PLATFORM);
    TUYA_CALL_ERR_LOG(tuya_iot_init_params(NULL, &init_param));
#endif
    
    tal_log_set_manage_attr(TAL_LOG_LEVEL_NOTICE);
    // KV flash 数据存储模块初始化
    tuya_iot_kv_flash_init(NULL);

    /* Initialization device */
    
    TUYA_CALL_ERR_LOG(__soc_device_init());

    return;
}

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
STATIC VOID_T tuya_app_thread(VOID_T *arg)
{
    /* Initialization LWIP first!!! */
#if defined(ENABLE_LWIP) && (ENABLE_LWIP == 1)
    TUYA_LwIP_Init();
#endif

    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

/**
 * @brief user entry function
 *
 * @param[in] none:
 *
 * @return none
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
INT_T main(INT_T argc, CHAR_T **argv)
#else
VOID_T tuya_app_main(VOID)
#endif
{
    THREAD_CFG_T thrd_param = {4096, 4, "tuya_app_main"};
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
#if OPERATING_SYSTEM == SYSTEM_LINUX
    while (1)
    {
        tal_system_sleep(1000);
    }
#endif
}