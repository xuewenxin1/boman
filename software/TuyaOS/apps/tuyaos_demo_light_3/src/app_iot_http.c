/**
 * @file example_iot_http.c
 * @author www.tuya.com
 * @version 0.1
 * @date 2022-05-20
 *
 * @copyright Copyright (c) tuya.inc 2022
 *
 */

#include "tuya_cloud_types.h"
#include "iot_httpc.h"
#include "ty_cJSON.h"

#include "tal_log.h"
#include "tal_system.h"

#include "tal_sw_timer.h"
#include "tal_time_service.h"

/***********************************************************
*************************micro define***********************
***********************************************************/

#define WEATHER_API "thing.weather.get"
#define API_VERSION "1.0"

#define POST_WEATHER_REAL_TIME "{\"codes\": [\"w.currdate\",\"w.humidity\",\"w.conditionNum\",\"w.pressure\",\"w.uvi\",\"w.windDir\",\"w.windSpeed\",\"w.sunrise\",\"w.sunset\",\"w.temp\",\"c.city\",\"c.area\",\"t.local\"]}"
#define POST_WEATHER_FORECAST "{\"codes\": [\"w.date.2\",\"w.conditionNum\",\"w.pressure\",\"w.uvi\",\"w.windDir\",\"w.windSpeed\",\"w.sunrise\",\"w.sunset\",\"c.city\",\"c.area\",\"t.local\"]}"

#define POST_CONTENT POST_WEATHER_REAL_TIME

/***********************************************************
***********************typedef define***********************
***********************************************************/
STATIC TIMER_ID sw_timer_id = NULL;
/***********************************************************
***********************variable define**********************
***********************************************************/

/**
 * @brief  http task
 *
 * @param[in] param:Task parameters
 * @return none
 */
uint8_t sunrise_hour = 0;
uint8_t sunrise_min = 0;
uint8_t sunset_hour = 0;
uint8_t sunset_min = 0;
VOID app_iot_http()
{
    OPERATE_RET rt = OPRT_OK;
    ty_cJSON *result = NULL;
    int tmp_hour = 0;
    int tmp_min = 0;

    // 获取天气信息
    TUYA_CALL_ERR_LOG(iot_httpc_common_post_simple(WEATHER_API, API_VERSION, POST_CONTENT, NULL, &result));
    if (NULL == result)
    {
        return;
    }

    // 获取 data 节点
    ty_cJSON *data = ty_cJSON_GetObjectItem(result, "data");
    if (data)
    {
        ty_cJSON *sunrise = ty_cJSON_GetObjectItem(data, "w.sunrise");
        ty_cJSON *sunset = ty_cJSON_GetObjectItem(data, "w.sunset");
        char buf[6] = {0}; // "HH:MM\0"
        if (sunrise && sunrise->valuestring)
        {

            const char *s = sunrise->valuestring; // "2025-09-11 05:37"
            int len = strlen(s);
            if (len >= 5)
            {
                strncpy(buf, s + len - 5, 5);
                buf[5] = '\0';
                if (sscanf(buf, "%d:%d", &tmp_hour, &tmp_min) == 2)
                {
                    sunrise_hour = tmp_hour;
                    sunrise_min = tmp_min;
                }
            }
        }
        if (sunset && sunset->valuestring)
        {
            const char *s = sunset->valuestring; // "2025-09-11 18:12"
            int len = strlen(s);
            if (len >= 5)
            {
                strncpy(buf, s + len - 5, 5);
                buf[5] = '\0';
                if (sscanf(buf, "%d:%d", &tmp_hour, &tmp_min) == 2)
                {
                    sunset_hour = tmp_hour;
                    sunset_min = tmp_min;
                }
            }
        }
    }

    ty_cJSON_Delete(result);
    result = NULL;
}

/**
 * @brief software timer callback
 *
 * @param[in] :
 *
 * @return none
 */
POSIX_TM_S local_tm;
bool time_synced = false; // 标记时间是否已同步
STATIC VOID_T __timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    OPERATE_RET rt = OPRT_OK;
    
    static int last_weather_hour = -1; // 记录上次更新天气的小时

    uint32_t current_time = tal_system_get_millisecond();

    if(current_time < 10000){
        return;
    }
    
    if (OPRT_OK == tal_time_check_time_sync())
    {
        memset((UCHAR_T *)&local_tm, 0x00, SIZEOF(POSIX_TM_S));

        rt = tal_time_get_local_time_custom(0, &local_tm);
        
        if (!time_synced) {
            // 时间首次同步成功
            app_iot_http(); // 调用天气接口，提取日出日落
            time_synced = true;
            last_weather_hour = local_tm.tm_hour;
        }
        else {
            // 检查是否是整点且小时发生变化
            if (local_tm.tm_min == 0 && local_tm.tm_hour != last_weather_hour) {
                app_iot_http(); // 调用天气接口，提取日出日落
                last_weather_hour = local_tm.tm_hour;
            }
        }
        
        tal_sw_timer_start(sw_timer_id, 60 * 1000, TAL_TIMER_CYCLE);
    }
    else
    {
        // 时间未同步
        if (time_synced) {
            time_synced = false;
            last_weather_hour = -1;
        }
        tal_sw_timer_start(sw_timer_id, 60 * 1000, TAL_TIMER_CYCLE);
    }
}
/**
 * @brief time service example
 *
 * @param[in] :
 *
 * @return none
 */
VOID app_time_service()
{
    OPERATE_RET rt = OPRT_OK;

    TUYA_CALL_ERR_LOG(tal_sw_timer_create(__timer_cb, NULL, &sw_timer_id));
    // 第一次 3 秒后触发，先校时
    tal_sw_timer_start(sw_timer_id, 1000, TAL_TIMER_CYCLE);
}
