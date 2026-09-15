/**
 * @file ty_scan_test_wifi.c
 * @author www.tuya.com
 * @version 0.1
 * @date 2023-10-13
 *
 * @copyright Copyright (c) tuya.inc 2023
 *
 */

#include "tuya_devos_utils.h"
#include "tal_log.h"

#include "ws_db_gw.h"
#include "prod_test.h"
/***********************************************************
*********************** macro define ***********************
***********************************************************/


/***********************************************************
********************** typedef define **********************
***********************************************************/

/***********************************************************
********************** variable define *********************
***********************************************************/


/***********************************************************
********************** function define *********************
***********************************************************/
/**
 * @brief Check if the device is allowed to scan the Wi-Fi test.
 *
 * @param[in] gwcm_mode GW_WF_CFG_MTHD_SEL mode.
 *
 * @return BOOL_T TRUE if the device is allowed to scan Wi-Fi test, otherwise FALSE.
 */
STATIC BOOL_T __is_allowed_scan_wifi_test(GW_WF_CFG_MTHD_SEL gwcm_mode)
{
    BOOL_T mf_close = FALSE;
    GW_WORK_STAT_MAG_S read_gw_wsm = {0};

    mf_close = mf_test_is_timeout();
    // if (TRUE == mf_close) {
    //     // TAL_PR_NOTICE("have actived over 15min, don't scan prod test ssid");
    //     return FALSE;
    // }  
	
	if(GWCM_OLD == gwcm_mode) {          
        return FALSE;
    }

    wd_gw_wsm_read(&read_gw_wsm);
    if ((gwcm_mode == GWCM_OLD_PROD) || (gwcm_mode == GWCM_LOW_POWER_AUTOCFG) ||
        (gwcm_mode == GWCM_SPCL_AUTOCFG)) { /* 上电默认配网或者第一次是配网的模式 */
        if (((read_gw_wsm.nc_tp >= GWNS_TY_SMARTCFG) && (read_gw_wsm.nc_tp != GWNS_UNCFG_SMC_AP)) || read_gw_wsm.md > GWM_NORMAL) { /* 已经存在ssid等配网信息但是并不是EZ和AP共存配网 */
            return FALSE;
        }
    } else if (gwcm_mode == GWCM_SPCL_MODE || gwcm_mode == GWCM_LOW_POWER) { /* 上电默认不配网 */
        if (read_gw_wsm.nc_tp >= GWNS_UNCFG_SMC) {                           /* 处于配网的状态 */
            return FALSE;
        }
    } else {
        ;
    }

    return TRUE;
}

/**
 * @brief Get target AP from scan AP.
 *
 * @param[in] ssid_list List of SSIDs to scan.
 * @param[in] ssid_count Count of SSIDs to scan.
 * @param[in] all_ap List of all APs.
 * @param[in] all_ap_num Number of all APs.
 * @param[out] target_ap Pointer to allocated memory containing target AP information.
 *
 * @return UINT_T Number of target APs.
 */
STATIC UINT_T __get_target_ap_from_all_ap(IN CONST CHAR_T **ssid_list, UINT8_T ssid_count,\
                                          AP_IF_S *all_ap, UINT_T all_ap_num,\
                                          OUT prodtest_ssid_info_t **target_ap)
{
    prodtest_ssid_info_t *ap_tmp_buf = NULL;
    UINT_T i = 0, j =0 , target_ap_num = 0;
    BOOL_T  add_target_ap = FALSE;

    if(NULL == target_ap) {
        return 0;
    }

    //prepare ap info buf
    ap_tmp_buf = (prodtest_ssid_info_t *)tal_malloc(SIZEOF(prodtest_ssid_info_t) * ssid_count);
    if (NULL == ap_tmp_buf) {
        return 0;
    }
    memset(ap_tmp_buf, 0, SIZEOF(prodtest_ssid_info_t) * ssid_count);

    for (i = 0; i<ssid_count; i++) {
        for (j = 0; j < all_ap_num; j++) {
            if (0 != strcmp((char *)ssid_list[i], (char *)all_ap[j].ssid)) {  
                continue;
            }  

            //当前扫描到的ssid与扫描列表一致
            if (0 == strcmp((char *)ap_tmp_buf[target_ap_num].ssid, (char *)all_ap[j].ssid)) {
                //当前扫描到的ssid已存储,判断rssi是否需要更新
                if (all_ap[j].rssi > ap_tmp_buf[target_ap_num].rssi) {
                    // TAL_PR_DEBUG("[update rssi] new:%d old:%d", all_ap[j].rssi, ap_tmp_buf[target_ap_num].rssi);
                    ap_tmp_buf[target_ap_num].rssi = all_ap[j].rssi;
                }
            }else {
                // 存储目标 ssid 信息
                add_target_ap = TRUE;
                strcpy((char *)ap_tmp_buf[target_ap_num].ssid, (char *)all_ap[j].ssid);
                ap_tmp_buf[target_ap_num].rssi = all_ap[j].rssi;
                // TAL_PR_DEBUG("new index:%d ssid%s", target_ap_num, ap_tmp_buf[target_ap_num].ssid);
            }
        }

        if(TRUE == add_target_ap) {
            add_target_ap = FALSE;
            target_ap_num++;
        }
    }

    if(0 == target_ap_num) {
        tal_free(ap_tmp_buf), ap_tmp_buf = NULL;
        *target_ap = NULL;
    }else {
        *target_ap = ap_tmp_buf;
    }

    return target_ap_num;
}

/**
 * @brief Release target AP memory.
 *
 * @param[in] target_ap Pointer to the allocated memory containing target AP information.
 *
 * @return VOID_T.
 */
STATIC VOID_T __release_target_ap(prodtest_ssid_info_t *target_ap)
{
    if(target_ap) {
        tal_free(target_ap);
    }

    return;
}

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
BOOL_T ty_scan_test_wifi(GW_WF_CFG_MTHD_SEL wf_cfg_mthd, CONST CHAR_T **ssid_list, \
                         UINT8_T ssid_count, prodtest_app_cb_t scan_info_cb)
{
    OPERATE_RET ret = OPRT_OK;
    prodtest_ssid_info_t *target_ap = NULL;
    AP_IF_S *ap = NULL;
    UINT_T ap_num = 0, target_ap_num = 0;
    BOOL_T flag = TRUE;

    if (NULL == ssid_list || ssid_count == 0 || scan_info_cb == NULL) {
        // TAL_PR_NOTICE("false 1\r\n");
        return FALSE;
    }

    if(FALSE == __is_allowed_scan_wifi_test(wf_cfg_mthd)) {
        // TAL_PR_NOTICE("false 2\r\n");
        return FALSE;
    }

    //scan all ap
    tal_wifi_set_work_mode(WWM_STATION);
    ret = tal_wifi_all_ap_scan(&ap, &ap_num);
    tal_wifi_station_disconnect();
    if (OPRT_OK != ret || 0 == ap_num) {
        // TAL_PR_NOTICE("tal_wifi_all_ap_scan failed(%d) ap_num(%d)", ret, ap_num);
        return FALSE;
    }

    //compare ssid information
    target_ap_num = __get_target_ap_from_all_ap(ssid_list, ssid_count, ap, ap_num, &target_ap);
    tal_wifi_release_ap(ap);
    ap = NULL, ap_num = 0;
    if(0 == target_ap_num) {
        // TAL_PR_NOTICE("cant find target from scan result");
        return FALSE;
    }

    //check dev authorized
    flag = get_gw_auth_status();

    //clear gateway config flash data
    // TAL_PR_NOTICE("gw cfg flash info reset factory!");
    GW_WORK_STAT_MAG_S *wsm = (GW_WORK_STAT_MAG_S *)tal_malloc(SIZEOF(GW_WORK_STAT_MAG_S));
    if (NULL != wsm) {
        memset(wsm, 0, SIZEOF(GW_WORK_STAT_MAG_S));
        ret = wd_gw_wsm_write(wsm);
        if (OPRT_OK != ret) {
            // TAL_PR_NOTICE("wd_gw_wsm_write err:%d!", ret);
        }
        tal_free(wsm);
    }

    ret = scan_info_cb(flag, target_ap, target_ap_num);

    __release_target_ap(target_ap);
    target_ap = NULL, target_ap_num = 0;

    return ((OPRT_OK == ret) ? TRUE : FALSE);
}
