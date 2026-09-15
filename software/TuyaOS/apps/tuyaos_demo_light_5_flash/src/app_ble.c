/**
 * @file example_user_ble_remote.c
 * @author www.tuya.com
 * @version 0.1
 * @date 2022-05-20
 */

#include "tuya_cloud_types.h"
#include "tuya_iot_com_api.h"
#define ENABLE_BT_REMOTE_CTRL 1
#if defined(ENABLE_BT_REMOTE_CTRL) && (ENABLE_BT_REMOTE_CTRL == 1)
#include "tal_log.h"
#include "tuya_bt.h"
#include "app_ble.h"
#include "app_pwm.h"
#include "dp_process.h"
#include "pwm_gradual.h"
#include "tal_sw_timer.h"
#include "app_night_light.h"
#include "light_pwm_mix.h"
#include "app_light_tm_schedule.h"
#include "app_light_tm_rhythm.h"
/***********************************************************
*************************micro define***********************
***********************************************************/
#define ADV_DATA_TYPE_SERVICE_UUID 0x07
#define ADV_DATA_TYPE_FLAGS 0x01
#define SERVICE_UUID_AD_LENGTH 0x11

#define VENDOR_PRODUCT_ID 0x0201
STATIC UINT32_T sg_power_on_time = 0;                 // 上电时间戳
STATIC UINT32_T sg_auto_stop_timeout = 5 * 60 * 1000; // 5分钟超时（毫秒）
STATIC THREAD_HANDLE example_thrd_hdl = NULL;
// 按键ID定义
#define KEY_ID_POWER 0xa1
#define KEY_ID_BRIGHT_UP 0xa2
#define KEY_ID_BRIGHT_DOWN 0xa3
#define KEY_ID_GROUP1_SWITCH 0xa4
#define KEY_ID_GROUP2_SWITCH 0xa5
#define KEY_ID_COLOR_UP 0xa6
#define KEY_ID_MAIN_SWITCH 0xa7
#define KEY_ID_MAIN_BRIGHT_UP 0xa8
#define KEY_ID_MAIN_BRIGHT_DOWN 0xa9
#define KEY_ID_COLOR_DOWN 0xaa
#define KEY_ID_AMBIENT_SWITCH 0xab
#define KEY_ID_AMBIENT_BRIGHT_UP 0xac
#define KEY_ID_AMBIENT_BRIGHT_DOWN 0xad
#define KEY_ID_NIGHT_LIGHT 0xae
#define KEY_ID_MODE 0xaf
#define KEY_RESET 0xb5
#define KEY_COLLECT 0xb3
#define KEY_ALIGNMENT 0xb6
#define KEY_CLEAR 0xb7
#define KEY_RELEASE 0xb4
#define KEY_ID_MODE1 0xb8

// 操作类型定义
#define OP_TYPE_SHORT_PRESS 0x00
#define OP_TYPE_LONG_PRESS 0x01
#define OP_TYPE_LONG_RELEASE 0x02
#define OP_TYPE_COMBO 0x03

// 错误码定义
#define PARSE_SUCCESS 0
#define PARSE_ERROR 1
#define CHECKSUM_ERROR 2

// 亮度档位定义 (6档)
#define BRIGHTNESS_LEVEL_1 1
#define BRIGHTNESS_LEVEL_2 20
#define BRIGHTNESS_LEVEL_3 40
#define BRIGHTNESS_LEVEL_4 60
#define BRIGHTNESS_LEVEL_5 80
#define BRIGHTNESS_LEVEL_6 100
#define BRIGHTNESS_LEVEL_COUNT 6

// 色温档位定义 (6档)
#define COLOR_TEMP_LEVEL_1 0
#define COLOR_TEMP_LEVEL_2 20
#define COLOR_TEMP_LEVEL_3 40
#define COLOR_TEMP_LEVEL_4 60
#define COLOR_TEMP_LEVEL_5 80
#define COLOR_TEMP_LEVEL_6 100
#define COLOR_TEMP_LEVEL_COUNT 6

// 灯光类型
#define LIGHT_TYPE_NIGHT 0
#define LIGHT_TYPE_MAIN 1
#define LIGHT_TYPE_AUX 2
#define LIGHT_TYPE_ALL 3

// 调节类型
#define ADJUST_TYPE_BRIGHTNESS_UP 0
#define ADJUST_TYPE_BRIGHTNESS_DOWN 1
#define ADJUST_TYPE_COLOR_UP 2
#define ADJUST_TYPE_COLOR_DOWN 3

#define LONG_PRESS_TIME 1000
#define COLLECT_LONG_PRESS_TIME 1500
#define LONG_PRESS_REPEAT_INTERVAL 500  // 长按重复触发间隔
/***********************************************************
***********************typedef define***********************
***********************************************************/

typedef struct
{
    UINT16_T vendor_product_id;
    UINT32_T remote_id;
    UINT8_T command_id;
    UINT8_T group_id;
    UINT8_T random_id;
    UINT8_T key_id;
    UINT8_T reserved[5];
    UINT8_T checksum;
} USER_REMOTE_DATA_T;

typedef struct
{
    USER_REMOTE_DATA_T remote_data;
    UINT8_T mac_addr[6];
    INT8_T rssi;
    UINT8_T parse_result;
    UINT8_T operation_type;
    UINT8_T additional_param;
} REMOTE_PARSE_RESULT_T;

/***********************************************************
***********************variable define**********************
***********************************************************/

// 全局变量
STATIC BOOL_T sg_in_pairing_mode = FALSE;

STATIC BOOL_T sg_is_long_press_active = FALSE; // 是否正在长按调光
STATIC UINT8_T sg_continuous_light_type = 0;   // 当前调节的灯光类型
STATIC UINT8_T sg_continuous_adjust_type = 0;  // 当前调节类型

STATIC BOOL_T sg_night_light_last_direction = TRUE;     // 夜灯上次调光方向（TRUE:正向, FALSE:反向）
STATIC BOOL_T sg_night_light_is_adjusting = FALSE;      // 夜灯是否正在调光
STATIC BOOL_T sg_night_light_direction_decided = FALSE; // 夜灯方向是否已确定（防止重复触发）
static uint32_t sg_last_long_press_trigger_time = 0;  // 上次长按触发时间
static uint8_t sg_long_press_key = 0;  // 当前长按的按键
// 按键防抖相关
STATIC UINT32_T sg_last_key_time = 0;
STATIC UINT8_T sg_last_key_id = 0;
STATIC UINT32_T sg_debounce_delay = 800;
STATIC UINT8_T sg_whitelist_count = 0;

static uint32_t sg_long_press_start_time = 0;
static uint8_t sg_long_press_active = 0;
// 当前模式状态
STATIC UINT8_T sg_current_mode = 0x11; // 默认白光模式

// 亮度档位数组
STATIC UINT8_T sg_brightness_levels[BRIGHTNESS_LEVEL_COUNT] = {
    BRIGHTNESS_LEVEL_1, BRIGHTNESS_LEVEL_2, BRIGHTNESS_LEVEL_3,
    BRIGHTNESS_LEVEL_4, BRIGHTNESS_LEVEL_5, BRIGHTNESS_LEVEL_6};

// 色温档位数组
STATIC UINT8_T sg_color_temp_levels[COLOR_TEMP_LEVEL_COUNT] = {
    COLOR_TEMP_LEVEL_1, COLOR_TEMP_LEVEL_2, COLOR_TEMP_LEVEL_3,
    COLOR_TEMP_LEVEL_4, COLOR_TEMP_LEVEL_5, COLOR_TEMP_LEVEL_6};

int dp_report = 0;
/***********************************************************
***********************function define**********************
***********************************************************/

// /**
//  * @brief 计算校验和
//  */
STATIC UINT8_T __calculate_checksum(UINT8_T *data, UINT8_T len)
{
    UINT8_T sum = 0;
    while (len--)
    {
        sum += *data++;
    }
    return sum;
}

/**
 * @brief 检查遥控器ID是否在白名单中
 */
STATIC uint8_t __check_remote_whitelist(UINT32_T remote_id, uint8_t group_id)
{
    uint8_t data = 6;
    extern DEMO_INFO_T sg_demo_info;
    for (UINT8_T i = 0; i < 5; i++)
    {
        if ((sg_demo_info.sg_remote_whitelist[i] == remote_id) && (sg_demo_info.remote_group[i] == group_id))
        {
            data = i;
        }
    }
    return data;
}

/**
 * @brief 进入配对模式
 */
OPERATE_RET user_ble_remote_start_pairing(VOID)
{
    UINT32_T current_time = tal_system_get_millisecond();
    if ((current_time - sg_power_on_time) >= sg_auto_stop_timeout)
    {
        return OPRT_COM_ERROR;
    }

    return OPRT_OK;
}

/**
 * @brief 解析遥控器数据
 */
uint8_t remote_port = 0;
uint32_t remote_time = 0;
uint32_t collect_time = 0;
STATIC UINT8_T __parse_remote_data(UINT8_T *service_data, USER_REMOTE_DATA_T *remote_data, UINT8_T *op_type,UINT8_T *add_param)
{
    extern DEMO_INFO_T sg_demo_info;
    // 解析16字节的服务数据
    remote_data->vendor_product_id = (service_data[0] << 8) | service_data[1];
    remote_data->remote_id = (service_data[2] << 24) | (service_data[3] << 16) |
                             (service_data[4] << 8) | service_data[5];
    remote_data->command_id = service_data[6];
    remote_data->group_id = service_data[7];
    remote_data->random_id = service_data[8];
    remote_data->key_id = service_data[9];
    memcpy(remote_data->reserved, &service_data[10], 5);
    remote_data->checksum = service_data[15];

    // 验证厂商产品ID
    if (remote_data->vendor_product_id != VENDOR_PRODUCT_ID)
    {
        return PARSE_ERROR;
    }

    // 验证校验和
    UINT8_T calculated_checksum = __calculate_checksum(service_data, 15);
    if (calculated_checksum != remote_data->checksum)
    {
        return CHECKSUM_ERROR;
    }
    // 检查遥控器ID白名单
    uint8_t i = __check_remote_whitelist(remote_data->remote_id, remote_data->group_id);

    // 根据协议表格解析操作类型和附加参数
    if (i >= 6)
    {
        // 只允许未绑定的遥控器进行配对操作
        switch (remote_data->group_id)
        {
        case 0x01:
            if ((remote_data->key_id == 0xb0) && (remote_data->reserved[0] == 0x00) && (remote_data->reserved[4] >= 0x05))
            {
                remote_data->key_id = KEY_ALIGNMENT;
                *op_type = OP_TYPE_LONG_PRESS;
                return PARSE_SUCCESS;
            }
        break;
        case 0x02:
            if ((remote_data->key_id == 0xb0) && (remote_data->reserved[0] == 0x00) && (remote_data->reserved[4] >= 0x05))
            {
                remote_data->key_id = KEY_ALIGNMENT;
                *op_type = OP_TYPE_LONG_PRESS;
                return PARSE_SUCCESS;
            }
            break;
        default:
            break;
        }
        // 未绑定的遥控器，忽略其他所有按键
        return PARSE_ERROR;
    }
    *op_type = OP_TYPE_SHORT_PRESS; // 默认短按
    // TAL_PR_NOTICE("key_id = %02x,reserved = %02x,group = %02x\r\n",remote_data->key_id,remote_data->reserved[0],remote_data->group_id);
    if (remote_data->group_id == sg_demo_info.remote_group[i])
    {
        switch (remote_data->key_id)
        {
            case 0xb0:
                if((remote_data->reserved[0] == 0x01) && (remote_data->reserved[4] >= 0x05)&&(remote_data->group_id == 0x01||remote_data->group_id == 0x02)){
                    remote_data->key_id = KEY_CLEAR;
                    *op_type = OP_TYPE_LONG_PRESS;
                }
            break;
        case 0x1c:
            if (remote_data->group_id == 0x01)
            {
                remote_data->key_id = KEY_ID_GROUP1_SWITCH;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            if (remote_data->group_id == 0x02)
            {
                remote_data->key_id = KEY_ID_GROUP2_SWITCH;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
        break;
        case 0x97: // 开关类命令
            if ((remote_data->key_id == 0x97) && (remote_data->reserved[0] == 0x00) &&(remote_data ->reserved[4] == 0x00))
            {
                remote_data->key_id = KEY_ID_POWER;
                // *add_param = remote_data->reserved[1];
            }
            else if ((remote_data->key_id == 0x97) && (remote_data->reserved[0] == 0x01))
            {
                remote_data->key_id = KEY_ID_MAIN_SWITCH;
                // *add_param = remote_data->reserved[1];
            }
            else if ((remote_data->key_id == 0x97) && (remote_data->reserved[0] == 0x02))
            {
                remote_data->key_id = KEY_ID_AMBIENT_SWITCH;
                // *add_param = remote_data->reserved[1];
            }
            break;

        case 0x25: // 亮度增加类命令
            if (remote_data->reserved[0] == 0x00)
            {
                remote_data->key_id = KEY_ID_BRIGHT_UP;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            else if (remote_data->reserved[0] == 0x01)
            {
                remote_data->key_id = KEY_ID_MAIN_BRIGHT_UP;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            else if (remote_data->reserved[0] == 0x02)
            {
                remote_data->key_id = KEY_ID_AMBIENT_BRIGHT_UP;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            break;

        case 0x26: // 亮度减少类命令
            if (remote_data->reserved[0] == 0x00)
            {
                // 亮度减少 K8
                remote_data->key_id = KEY_ID_BRIGHT_DOWN;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            else if (remote_data->reserved[0] == 0x01)
            {
                // 主光/下光亮度- K15
                remote_data->key_id = KEY_ID_MAIN_BRIGHT_DOWN;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            else if (remote_data->reserved[0] == 0x02)
            {
                // 氛围光/上光亮度- K20
                remote_data->key_id = KEY_ID_AMBIENT_BRIGHT_DOWN;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            break;

        case 0x27: // 色温增加 K12
            if (remote_data->reserved[0] == 0x00)
            {
                remote_data->key_id = KEY_ID_COLOR_UP;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            break;

        case 0x28: // 色温减少 K16
            if (remote_data->reserved[0] == 0x00)
            {
                remote_data->key_id = KEY_ID_COLOR_DOWN;
                *op_type = OP_TYPE_SHORT_PRESS;
            }
            break;
        case 0x32:
            remote_data->key_id = KEY_ID_NIGHT_LIGHT;
            *op_type = OP_TYPE_SHORT_PRESS;
            break;

        case 0x3E: // 长按亮度增加
            if (remote_data->reserved[0] == 0x00)
            {
                // 亮度增加长按 K4
                remote_data->key_id = KEY_ID_BRIGHT_UP;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            else if (remote_data->reserved[0] == 0x01)
            {
                // 主光/下光亮度+长按 K14
                remote_data->key_id = KEY_ID_MAIN_BRIGHT_UP;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            else if (remote_data->reserved[0] == 0x02)
            {
                // 氛围光/上光亮度+长按 K19
                remote_data->key_id = KEY_ID_AMBIENT_BRIGHT_UP;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            break;

        case 0x3F: // 长按亮度减少
            if (remote_data->reserved[0] == 0x00)
            {
                // 亮度减少长按 K8
                remote_data->key_id = KEY_ID_BRIGHT_DOWN;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            else if (remote_data->reserved[0] == 0x01)
            {
                // 主光/下光亮度-长按 K15
                remote_data->key_id = KEY_ID_MAIN_BRIGHT_DOWN;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            else if (remote_data->reserved[0] == 0x02)
            {
                // 氛围光/上光亮度-长按 K20
                remote_data->key_id = KEY_ID_AMBIENT_BRIGHT_DOWN;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            break;

        case 0x40: // 色温增加长按 K12
            remote_data->key_id = KEY_ID_COLOR_UP;
            *op_type = OP_TYPE_LONG_PRESS;
            break;

        case 0x41: // 色温减少长按 K16
            remote_data->key_id = KEY_ID_COLOR_DOWN;
            *op_type = OP_TYPE_LONG_PRESS;
            break;

        case 0x29: // 模式 K25
            remote_data->key_id = KEY_ID_MODE;
            *op_type = OP_TYPE_SHORT_PRESS;
            *add_param = remote_data->reserved[0]; // 模式参数
            break;

        case 0x42: // 模式 K25
            if((remote_data ->reserved[4] == 0x00)){
                remote_data->key_id = KEY_ID_MODE1;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            break;
        case 0x74: // 模式 K25
            // if(remote_data->reserved[4] == 0x03)
            // if(tal_system_get_millisecond() - remote_time >= sg_debounce_delay)
            //     sg_last_key_id = 0;
            remote_time = tal_system_get_millisecond();
            remote_port = 1;
            if (remote_data->reserved[4] >= 0x02)
            {
                remote_port = 0;
                remote_data->key_id = KEY_COLLECT;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            break;

        case 0x01: // 长按松手
            // 长按松手命令，通过reserved字段判断
            if (remote_data->reserved[0] == 0x1F &&
                remote_data->reserved[1] == 0xFF &&
                remote_data->reserved[2] == 0xFF &&
                remote_data->reserved[3] == 0xFF)
            {
                *op_type = OP_TYPE_LONG_RELEASE;
                remote_data->key_id = KEY_RELEASE;
                if (sg_is_long_press_active && sg_continuous_light_type == LIGHT_TYPE_NIGHT)
                {
                    __stop_continuous_adjustment(); // 松手时改变方向
                }
            }
            break;

        case 0xBB:
            if (remote_data->reserved[4] >= 0x03)
            {
                remote_data->key_id = KEY_RESET;
                *op_type = OP_TYPE_LONG_PRESS;
            }
            break;
        case 0x4b:
            remote_data->key_id = KEY_ID_NIGHT_LIGHT;
            *op_type = OP_TYPE_LONG_PRESS;
            break;
        default:
            break;
        }
    }
    return PARSE_SUCCESS;
}
/**
 * @brief 获取临近的下一档亮度
 * 规则：直接跳到临近档位
 * 增加：找到第一个大于当前亮度的档位
 * 减少：找到最后一个小于当前亮度的档位
 */
STATIC UINT8_T __get_next_brightness_level(UINT8_T current_brightness, BOOL_T is_increase)
{
    if (is_increase)
    {
        // 增加：找到第一个大于当前亮度的档位
        for (INT8_T i = 0; i < BRIGHTNESS_LEVEL_COUNT; i++)
        {
            if (sg_brightness_levels[i] > current_brightness)
            {
                return sg_brightness_levels[i];
            }
        }
        // 没有更大的档位，返回最大档位
        return sg_brightness_levels[BRIGHTNESS_LEVEL_COUNT - 1];
    }
    else
    {
        // 减少：找到最后一个小于当前亮度的档位
        for (INT8_T i = BRIGHTNESS_LEVEL_COUNT - 1; i >= 0; i--)
        {
            if (sg_brightness_levels[i] < current_brightness)
            {
                return sg_brightness_levels[i];
            }
        }
        // 没有更小的档位，返回最小档位
        return sg_brightness_levels[0];
    }
}
/**
 * @brief 获取临近的下一档色温
 * 规则：直接跳到临近档位
 * 增加：找到第一个大于当前色温的档位
 * 减少：找到最后一个小于当前色温的档位
 */
STATIC UINT8_T __get_next_color_temp_level(UINT8_T current_temp, BOOL_T is_increase)
{
    if (is_increase)
    {
        // 增加：找到第一个大于当前色温的档位
        for (INT8_T i = 0; i < COLOR_TEMP_LEVEL_COUNT; i++)
        {
            if (sg_color_temp_levels[i] > current_temp)
            {
                return sg_color_temp_levels[i];
            }
        }
        // 没有更大的档位，返回最大档位
        return sg_color_temp_levels[COLOR_TEMP_LEVEL_COUNT - 1];
    }
    else
    {
        // 减少：找到最后一个小于当前色温的档位
        for (INT8_T i = COLOR_TEMP_LEVEL_COUNT - 1; i >= 0; i--)
        {
            if (sg_color_temp_levels[i] < current_temp)
            {
                return sg_color_temp_levels[i];
            }
        }
        // 没有更小的档位，返回最小档位
        return sg_color_temp_levels[0];
    }
}



/**
 * @brief 获取当前活动的灯光类型
 */
STATIC UINT8_T __get_active_light_type(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    if (sg_demo_info.night_switch == 1)
    {
        return LIGHT_TYPE_NIGHT;
    }
    if (sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1)
    {
        return LIGHT_TYPE_ALL;
    }
    else if (sg_demo_info.white_switch == 1)
    {
        return LIGHT_TYPE_MAIN;
    }
    else if (sg_demo_info.aux_switch == 1)
    {
        return LIGHT_TYPE_AUX;
    }
    return LIGHT_TYPE_MAIN; // 默认
}

/**
 * @brief 根据按键ID获取要调节的灯光类型
 */
STATIC UINT8_T __get_target_light_type_by_key(UINT8_T key_id)
{
    switch (key_id)
    {
    case KEY_ID_BRIGHT_UP:                // 总亮度+
    case KEY_ID_BRIGHT_DOWN:              // 总亮度-
        return __get_active_light_type();

    case KEY_ID_COLOR_UP:                 // 主灯色温+
    case KEY_ID_COLOR_DOWN:               // 主灯色温-
        return __get_active_light_type(); // 根据当前灯光状态决定

    case KEY_ID_MAIN_BRIGHT_UP:   // 主灯亮度+
    case KEY_ID_MAIN_BRIGHT_DOWN: // 主灯亮度-
    case KEY_ID_MAIN_SWITCH:      // 主灯开关
        return LIGHT_TYPE_MAIN;   // 明确调节主灯

    case KEY_ID_AMBIENT_BRIGHT_UP:   // 辅灯亮度+
    case KEY_ID_AMBIENT_BRIGHT_DOWN: // 辅灯亮度-
    case KEY_ID_AMBIENT_SWITCH:      // 辅灯开关
        return LIGHT_TYPE_AUX;       // 明确调节辅灯

    case KEY_ID_NIGHT_LIGHT:     // 夜灯
        return LIGHT_TYPE_NIGHT; // 明确调节夜灯

    default:
        return LIGHT_TYPE_MAIN; // 默认主灯
    }
}

/**
 * @brief 获取当前PWM通道的占空比
 */
UINT16_T __get_current_pwm_duty(TUYA_PWM_NUM_E ch_id)
{
    TUYA_PWM_BASE_CFG_T pwm_info;
    OPERATE_RET ret = tkl_pwm_info_get(ch_id, &pwm_info);
    if (ret == OPRT_OK)
    {
        uint16_t calibration_num = 10000;
        if(ch_id == BRIGHT_PWM)
            calibration_num = sg_demo_info.ww_calibration_coefficient;
        if(ch_id == TEMP_PWM)
            calibration_num = sg_demo_info.cw_calibration_coefficient;
        if(ch_id == AUX_BRIGHT_PWM)
            calibration_num = sg_demo_info.aux_ww_calibration_coefficient;
        if(ch_id == AUX_TEMP_PWM)
            calibration_num = sg_demo_info.aux_cw_calibration_coefficient;
        
        return pwm_info.duty*10000/calibration_num;
    }
    return 0;
}

/**
 * @brief 停止长按调光
 */
VOID __stop_continuous_adjustment(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    if (!sg_is_long_press_active)
    {
        return;
    }

    // 获取当前的PWM值用于调试
    UINT16_T ww_pwm = 0, cw_pwm = 0;

    switch (sg_continuous_light_type)
    {
    case LIGHT_TYPE_MAIN:
        ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(TEMP_PWM);
        break;
    case LIGHT_TYPE_AUX:
        ww_pwm = __get_current_pwm_duty(AUX_BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(AUX_TEMP_PWM);
        break;
    case LIGHT_TYPE_ALL:
        ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(TEMP_PWM);
        break;
    }

    // 记录停止前的值用于调试
    UINT8_T before_brightness = 0, before_temp = 0;
    switch (sg_continuous_light_type)
    {
    case LIGHT_TYPE_NIGHT:
        before_brightness = sg_demo_info.night_bright;
        break;
    case LIGHT_TYPE_MAIN:
        before_brightness = sg_demo_info.white_bright;
        before_temp = sg_demo_info.white_temp;
        break;
    case LIGHT_TYPE_AUX:
        before_brightness = sg_demo_info.aux_bright;
        before_temp = sg_demo_info.white_temp;
        break;
    case LIGHT_TYPE_ALL:
        before_brightness = sg_demo_info.white_bright;
        before_temp = sg_demo_info.white_temp;
        break;
    }

    // 停止PWM渐变
    pwm_gradual_stop_all();

    // 获取停止时的实际PWM值，并反向计算亮度和色温
    UINT8_T current_brightness = 0;
    UINT8_T current_temp = 0;

    __get_current_gradient_values(sg_continuous_light_type, sg_continuous_adjust_type,
                                  &current_brightness, &current_temp);

    // 根据调节类型更新对应的值（保持另一个参数不变）
    BOOL_T is_brightness_adjust = (sg_continuous_adjust_type == ADJUST_TYPE_BRIGHTNESS_UP ||
                                   sg_continuous_adjust_type == ADJUST_TYPE_BRIGHTNESS_DOWN);

    // 更新全局变量
    switch (sg_continuous_light_type)
    {
    case LIGHT_TYPE_NIGHT:
        sg_demo_info.night_bright = current_brightness;
        break;

    case LIGHT_TYPE_MAIN:
        if (is_brightness_adjust)
        {
            sg_demo_info.white_bright = current_brightness;
            // 色温保持不变
        }
        else
        {
            // if(current_temp == 99)
            //     current_temp = 100;
            sg_demo_info.white_temp = current_temp;
            // 亮度保持不变
        }
        break;

    case LIGHT_TYPE_AUX:
        if (is_brightness_adjust)
        {
            sg_demo_info.aux_bright = current_brightness;
            // 色温保持不变
        }
        else
        {
            // if(current_temp == 99)
            //     current_temp = 100;
            sg_demo_info.white_temp = current_temp;
            // 亮度保持不变
        }
        break;

    case LIGHT_TYPE_ALL:
        if (is_brightness_adjust)
        {
            if (sg_demo_info.white_switch == 1) {
                uint8_t main_bright = sg_demo_info.white_bright;
                uint8_t main_temp = sg_demo_info.white_temp;
                ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
                cw_pwm = __get_current_pwm_duty(TEMP_PWM);
                __reverse_calculate_from_pwm(ww_pwm, cw_pwm, sg_continuous_adjust_type,
                                             &main_bright, &main_temp);
                sg_demo_info.white_bright = main_bright;
            }
            if (sg_demo_info.aux_switch == 1) {
                uint8_t aux_bright = sg_demo_info.aux_bright;
                uint8_t aux_temp = sg_demo_info.white_temp;
                ww_pwm = __get_current_pwm_duty(AUX_BRIGHT_PWM);
                cw_pwm = __get_current_pwm_duty(AUX_TEMP_PWM);
                __reverse_calculate_from_pwm(ww_pwm, cw_pwm, sg_continuous_adjust_type,
                                             &aux_bright, &aux_temp);
                sg_demo_info.aux_bright = aux_bright;
            }
        }
        else
        {
            sg_demo_info.white_temp = current_temp;
        }
        break;
    }

    // 上报实际停止值
    __upload_adjusted_value_only(sg_continuous_light_type, sg_continuous_adjust_type);

    // 重置状态
    sg_is_long_press_active = FALSE;
    sg_night_light_is_adjusting = FALSE;

    // 夜灯方向切换
    if (sg_continuous_light_type == LIGHT_TYPE_NIGHT && sg_night_light_direction_decided)
    {
        sg_night_light_last_direction = !sg_night_light_last_direction;
        sg_night_light_direction_decided = FALSE;
    }
}

/**
 * @brief 只上报正在调节的值
 */
VOID __upload_adjusted_value_only(UINT8_T light_type, UINT8_T adjust_type)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;
    BOOL_T is_brightness_adjust = (adjust_type == ADJUST_TYPE_BRIGHTNESS_UP || adjust_type == ADJUST_TYPE_BRIGHTNESS_DOWN);
            app_light_schedule_user_interrupt_sleep_wake();

    switch (light_type)
    {
    case LIGHT_TYPE_NIGHT:
        if (sg_demo_info.night_switch == 1)
        {
            // 夜灯只有亮度调节
            upload_device_value_status(NIGHT_LIGHT_VALUE, sg_demo_info.night_bright);
        }
        break;

    case LIGHT_TYPE_MAIN:
        if (sg_demo_info.white_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 只上报亮度变化
                upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
            }
            else
            {
                // 只上报色温变化
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
            }
        }
        break;

    case LIGHT_TYPE_AUX:
        if (sg_demo_info.aux_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 只上报亮度变化
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
            }
            else
            {
                // 只上报色温变化
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
            }
        }
        break;

    case LIGHT_TYPE_ALL:
        if (sg_demo_info.white_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 只上报主灯亮度变化
                upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
            }
            else
            {
                // 只上报主灯色温变化
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
            }
        }
        if (sg_demo_info.aux_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 只上报辅灯亮度变化
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
            }
            else
            {
                // 只上报辅灯色温变化
                upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
            }
        }
        break;
    }
}
/**
 * @brief 将亮度百分比(1-100)转换为亮度值(5-100)
 */
UINT16_T __convert_brightness_percent_to_value(UINT8_T brightness_percent)
{
    if (brightness_percent <= 1)
    {
        return 5;
    }
    else if (brightness_percent >= 100)
    {
        return 100;
    }
    else
    {
        float brightness_value_float = 5.0f + (brightness_percent - 1.0f) * 95.0f / 99.0f;
        UINT16_T brightness_value = (UINT16_T)(brightness_value_float + 0.5f); // 四舍五入
        if (brightness_value > 100)
            brightness_value = 100;
        if (brightness_value < 5)
            brightness_value = 5;
        return brightness_value;
    }
}

/**
 * @brief 应用PWM渐变 - 只改变调节的参数
 */
STATIC VOID __apply_pwm_gradient(UINT8_T light_type, UINT8_T adjust_type,
                                 UINT8_T target_brightness, UINT8_T target_temp)
{
    extern DEMO_INFO_T sg_demo_info;
    BOOL_T is_brightness_adjust = (adjust_type == ADJUST_TYPE_BRIGHTNESS_UP || adjust_type == ADJUST_TYPE_BRIGHTNESS_DOWN);
    BOOL_T is_color_temp_adjust = (adjust_type == ADJUST_TYPE_COLOR_UP || adjust_type == ADJUST_TYPE_COLOR_DOWN);

    switch (light_type)
    {
    case LIGHT_TYPE_MAIN:
        if (sg_demo_info.white_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 只渐变亮度，保持色温不变
                uint16_t target_bright = __convert_brightness_percent_to_value(target_brightness);
                uint8_t current_temp = sg_demo_info.white_temp;

                // 确保值有效
                if (target_bright < 5)
                    target_bright = 5;
                if (target_bright > 100)
                    target_bright = 100;
                if (current_temp > 100)
                    current_temp = 100;
                // if (current_temp == 0)
                //     current_temp = 1;

                uint16_t ww = target_bright * current_temp;
                uint16_t cw = target_bright * 100 - ww;

                // 确保PWM值有效
                light_pwm_clamp_mix(&ww, &cw);

                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {ww, cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            else if (is_color_temp_adjust)
            {
                // 只渐变色温，保持亮度不变
                uint16_t current_bright = __convert_brightness_percent_to_value(sg_demo_info.white_bright);
                uint8_t target_temp_val = target_temp;

                // 确保值有效
                if (current_bright < 5)
                    current_bright = 5;
                if (current_bright > 100)
                    current_bright = 100;
                if (target_temp_val > 100)
                    target_temp_val = 100;
                // if (target_temp_val == 0)
                //     target_temp_val = 1;

                uint16_t ww = current_bright * target_temp_val;
                uint16_t cw = current_bright * 100 - ww;

                // 确保PWM值有效
                light_pwm_clamp_mix(&ww, &cw);

                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {ww, cw};
                // pwm_gradual_duty_set_multi(2, channels, duties);
                pwm_gradual_colortemp_set(2, channels, duties);
            }
        }
        break;

    case LIGHT_TYPE_AUX:
        if (sg_demo_info.aux_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 只渐变亮度，保持色温不变
                uint16_t target_bright = __convert_brightness_percent_to_value(target_brightness);
                uint8_t current_temp = sg_demo_info.white_temp;

                if (target_bright < 5)
                    target_bright = 5;
                if (target_bright > 100)
                    target_bright = 100;
                if (current_temp > 100)
                    current_temp = 100;
                // if (current_temp == 0)
                //     current_temp = 1;

                uint16_t aux_ww = target_bright * current_temp;
                uint16_t aux_cw = (target_bright * 100) - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);

                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw};
                pwm_gradual_duty_set_multi(2, channels, duties);
            }
            else if (is_color_temp_adjust)
            {
                uint16_t current_bright = __convert_brightness_percent_to_value(sg_demo_info.aux_bright);
                uint8_t target_temp_val = target_temp;

                if (current_bright < 5)
                    current_bright = 5;
                if (current_bright > 100)
                    current_bright = 100;
                if (target_temp_val > 100)
                    target_temp_val = 100;
                // if (target_temp_val == 0)
                //     target_temp_val = 1;

                uint16_t aux_ww = current_bright * target_temp_val;
                uint16_t aux_cw = current_bright * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);

                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw};
                // pwm_gradual_duty_set_multi(2, channels, duties);
                pwm_gradual_colortemp_set(2, channels, duties);
            }
        }
        break;

    case LIGHT_TYPE_ALL:{
        TUYA_PWM_NUM_E all_channels[4];
        UINT32_T all_duties[4];
        UINT8_T channel_count = 0;

        /* 总亮度/总色温须一次提交全部 PWM 通道；分两次调用时第二次会打断第一次，
         * 导致主灯被立即设到目标值而无渐变（辅灯正常）。 */
        if (is_brightness_adjust)
        {
            uint16_t target_bright = __convert_brightness_percent_to_value(target_brightness);
            uint8_t current_temp = sg_demo_info.white_temp;

            if (target_bright < 5)
                target_bright = 5;
            if (target_bright > 100)
                target_bright = 100;
            if (current_temp > 100)
                current_temp = 100;

            if (sg_demo_info.white_switch == 1)
            {
                uint16_t ww = target_bright * current_temp;
                uint16_t cw = target_bright * 100 - ww;

                light_pwm_clamp_mix(&ww, &cw);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
            }
            if (sg_demo_info.aux_switch == 1)
            {
                uint16_t aux_ww = target_bright * current_temp;
                uint16_t aux_cw = target_bright * 100 - aux_ww;

                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            if (channel_count > 0)
            {
                pwm_gradual_brightness_set(channel_count, all_channels, all_duties);
            }
        }
        else if (is_color_temp_adjust)
        {
            uint8_t target_temp_val = target_temp;

            if (target_temp_val > 100)
                target_temp_val = 100;

            if (sg_demo_info.white_switch == 1)
            {
                uint16_t current_bright = __convert_brightness_percent_to_value(sg_demo_info.white_bright);

                if (current_bright < 5)
                    current_bright = 5;
                if (current_bright > 100)
                    current_bright = 100;

                uint16_t ww = current_bright * target_temp_val;
                uint16_t cw = current_bright * 100 - ww;

                light_pwm_clamp_mix(&ww, &cw);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;
                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
            }
            if (sg_demo_info.aux_switch == 1)
            {
                uint16_t current_bright = __convert_brightness_percent_to_value(sg_demo_info.aux_bright);

                if (current_bright < 5)
                    current_bright = 5;
                if (current_bright > 100)
                    current_bright = 100;

                uint16_t aux_ww = current_bright * target_temp_val;
                uint16_t aux_cw = current_bright * 100 - aux_ww;

                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;
                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            if (channel_count > 0)
            {
                pwm_gradual_colortemp_set(channel_count, all_channels, all_duties);
            }
        }
    }
        break;

    case LIGHT_TYPE_NIGHT:
        if (sg_demo_info.night_switch == 1)
        {
            if (is_brightness_adjust)
            {
                // 夜灯只有亮度调节
                uint16_t target_pwm = target_brightness * 100;
                if (target_pwm > 10000)
                    target_pwm = 10000;
                if (target_pwm < 100)
                    target_pwm = 100;

                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, target_pwm);
                
            }
        }
        break;
    }
}
/**
 * @brief 长按调光处理 - 直接设置目标值并使用PWM渐变
 */
STATIC VOID __handle_long_press_adjustment(UINT8_T light_type, UINT8_T adjust_type, BOOL_T is_start)
{
    extern uint16_t gradual_time_ms;
    extern DEMO_INFO_T sg_demo_info;

    BOOL_T is_brightness_adjust = (adjust_type == ADJUST_TYPE_BRIGHTNESS_UP || adjust_type == ADJUST_TYPE_BRIGHTNESS_DOWN);
    BOOL_T is_color_temp_adjust = (adjust_type == ADJUST_TYPE_COLOR_UP || adjust_type == ADJUST_TYPE_COLOR_DOWN);

    if (is_start)
    {
        // 正确获取当前值
        UINT8_T start_brightness = 0;
        UINT8_T start_temp = 0;

        switch (light_type)
        {
        case LIGHT_TYPE_NIGHT:
            start_brightness = sg_demo_info.night_bright;
            break;
        case LIGHT_TYPE_MAIN:
            start_brightness = sg_demo_info.white_bright;
            start_temp = sg_demo_info.white_temp;
            break;
        case LIGHT_TYPE_AUX:
            start_brightness = sg_demo_info.aux_bright;
            start_temp = sg_demo_info.white_temp;
            break;
        case LIGHT_TYPE_ALL:
            start_brightness = sg_demo_info.white_bright;
            start_temp = sg_demo_info.white_temp;
            break;
        }

        // 确保值有效
        if (start_brightness < 1)
            start_brightness = 1;
        if (start_brightness > 100)
            start_brightness = 100;
        if (start_temp > 100)
            start_temp = 100;

        // 长按开始
        sg_is_long_press_active = TRUE;
        sg_continuous_light_type = light_type;
        sg_continuous_adjust_type = adjust_type;

        // 设置渐变时间
        gradual_time_ms = 3010;

        // 根据调节类型设置目标值
        UINT8_T target_brightness = start_brightness;
        UINT8_T target_temp = start_temp;

        if (is_brightness_adjust)
        {
            // 亮度调节
            target_brightness = (adjust_type == ADJUST_TYPE_BRIGHTNESS_UP) ? 100 : 1;
        }
        else if (is_color_temp_adjust)
        {
            // 色温调节
            target_temp = (adjust_type == ADJUST_TYPE_COLOR_UP) ? 100 : 0;
        }

        // 确保目标值有效
        if (target_brightness < 1)
            target_brightness = 1;
        if (target_brightness > 100)
            target_brightness = 100;
        if (target_temp > 100)
            target_temp = 100;

        // 应用PWM渐变
        __apply_pwm_gradient(light_type, adjust_type, target_brightness, target_temp);
    }
    else
    {
        // 长按结束
        __stop_continuous_adjustment();
    }
}

/**
 * @brief 夜灯长按调光处理
 */
STATIC VOID __handle_night_light_long_press(VOID)
{
    extern uint16_t gradual_time_ms;
    extern DEMO_INFO_T sg_demo_info;
    if (sg_is_long_press_active)
    {
        // __stop_continuous_adjustment();
        return;
    }

    if (!sg_demo_info.night_switch)
    {
        // 如果夜灯未开启，先开启夜灯
        sg_demo_info.night_switch = 1;
        if(sg_demo_info.white_switch == 1&&sg_demo_info.aux_switch == 1)
            sg_demo_info.last_light_memory = 1;
        else if(sg_demo_info.white_switch == 1)
            sg_demo_info.last_light_memory = 2;
        else if(sg_demo_info.aux_switch == 1)
            sg_demo_info.last_light_memory = 3;
        sg_demo_info.white_switch = 0;
        sg_demo_info.aux_switch = 0;
        sg_demo_info.switch_status = 1;

        tkl_pwm_duty_set(BRIGHT_PWM, 0);
        tkl_pwm_start(BRIGHT_PWM);
        tkl_pwm_duty_set(TEMP_PWM, 0);
        tkl_pwm_start(TEMP_PWM);
        tkl_pwm_duty_set(AUX_BRIGHT_PWM, 0);
        tkl_pwm_start(AUX_BRIGHT_PWM);
        tkl_pwm_duty_set(AUX_TEMP_PWM, 0);
        tkl_pwm_start(AUX_TEMP_PWM);
        dp_report = 1;
    }

    // 确定调光方向（交替切换）
    UINT8_T adjust_type;
    if (sg_night_light_last_direction)
    {
        // 上次是正向，这次用反向（减少到最小值）
        adjust_type = ADJUST_TYPE_BRIGHTNESS_DOWN;
    }
    else
    {
        // 上次是反向，这次用正向（增加到最大值）
        adjust_type = ADJUST_TYPE_BRIGHTNESS_UP;
    }

    sg_is_long_press_active = TRUE;
    sg_night_light_direction_decided = TRUE;
    sg_night_light_is_adjusting = TRUE;
    sg_continuous_light_type = LIGHT_TYPE_NIGHT;
    sg_continuous_adjust_type = adjust_type;

    // 设置渐变时间
    gradual_time_ms = 3010; // 4秒渐变
    // 直接设置目标值并使用PWM渐变

    UINT8_T target_value = (adjust_type == ADJUST_TYPE_BRIGHTNESS_UP) ? 100 : 1;
    pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, target_value * 100);
    // sg_demo_info.night_bright = target_value;
}

/**
 * @brief 更新PWM输出
 */
VOID __update_pwm_output(UINT8_T light_type)
{
    extern DEMO_INFO_T sg_demo_info;
    switch (light_type)
    {
    case LIGHT_TYPE_NIGHT:
        if (sg_demo_info.night_switch == 1)
        {
            pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, sg_demo_info.night_bright * 100);
        }
        break;

    case LIGHT_TYPE_MAIN:
        if (sg_demo_info.white_switch == 1)
        {
            uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
            uint8_t white_temp1 = sg_demo_info.white_temp;
            uint16_t ww = white_bright1 * white_temp1;
            uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
            TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
            UINT32_T duties[] = {ww, cw};
            pwm_gradual_duty_set_multi(2, channels, duties);
        }
        break;

    case LIGHT_TYPE_AUX:
        if (sg_demo_info.aux_switch == 1)
        {
            uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
            uint8_t aux_temp1 = sg_demo_info.white_temp;
            uint16_t aux_ww = aux_bright1 * aux_temp1;
            uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
            TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
            UINT32_T duties[] = {aux_ww, aux_cw};
            pwm_gradual_duty_set_multi(2, channels, duties);
        }
        break;

    case LIGHT_TYPE_ALL:
    {
        TUYA_PWM_NUM_E all_channels[4];
        UINT32_T all_duties[4];
        UINT8_T channel_count = 0;
        if (sg_demo_info.white_switch == 1)
        {
            uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
            uint8_t white_temp1 = sg_demo_info.white_temp;
            uint16_t ww = white_bright1 * white_temp1;
            uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
            all_channels[channel_count] = BRIGHT_PWM;
            all_duties[channel_count++] = ww;

            all_channels[channel_count] = TEMP_PWM;
            all_duties[channel_count++] = cw;
        }
        if (sg_demo_info.aux_switch == 1)
        {
            uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
            uint8_t aux_temp1 = sg_demo_info.white_temp;
            uint16_t aux_ww = aux_bright1 * aux_temp1;
            uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
            all_channels[channel_count] = AUX_BRIGHT_PWM;
            all_duties[channel_count++] = aux_ww;

            all_channels[channel_count] = AUX_TEMP_PWM;
            all_duties[channel_count++] = aux_cw;
        }
        if (channel_count > 0)
        {
            pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
        }
    }
    break;
    }
}

/**
 * @brief 单按亮度调节处理
 */
STATIC VOID __handle_single_press_brightness(UINT8_T light_type, BOOL_T is_increase)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;
            app_light_schedule_user_interrupt_sleep_wake();

    switch (light_type)
    {
    case LIGHT_TYPE_NIGHT:
        if (sg_demo_info.night_switch == 1)
        {
            UINT8_T new_bright = __get_next_brightness_level(sg_demo_info.night_bright, is_increase);
            if (new_bright != sg_demo_info.night_bright)
            {
                sg_demo_info.night_bright = new_bright;
                __update_pwm_output(LIGHT_TYPE_NIGHT);
                upload_device_value_status(NIGHT_LIGHT_VALUE, new_bright);
            }
        }
        break;

    case LIGHT_TYPE_MAIN:
        if (sg_demo_info.white_switch == 1)
        {
            UINT8_T new_bright = __get_next_brightness_level(sg_demo_info.white_bright, is_increase);
            if (new_bright != sg_demo_info.white_bright)
            {
                sg_demo_info.white_bright = new_bright;
                __update_pwm_output(LIGHT_TYPE_MAIN);
                upload_device_value_status(DPID_WHITE_BRIGHT, new_bright);
            }
        }
        break;

    case LIGHT_TYPE_AUX:
        if (sg_demo_info.aux_switch == 1)
        {
            UINT8_T new_bright = __get_next_brightness_level(sg_demo_info.aux_bright, is_increase);
            if (new_bright != sg_demo_info.aux_bright)
            {
                sg_demo_info.aux_bright = new_bright;
                __update_pwm_output(LIGHT_TYPE_AUX);
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, new_bright);
            }
        }
        break;

    case LIGHT_TYPE_ALL:
        if (sg_demo_info.white_switch == 1)
        {
            UINT8_T white_new = __get_next_brightness_level(sg_demo_info.white_bright, is_increase);
            if (white_new != sg_demo_info.white_bright)
            {
                sg_demo_info.white_bright = white_new;
                upload_device_value_status(DPID_WHITE_BRIGHT, white_new);
            }
        }
        if (sg_demo_info.aux_switch == 1)
        {
            UINT8_T aux_new = __get_next_brightness_level(sg_demo_info.aux_bright, is_increase);
            if (aux_new != sg_demo_info.aux_bright)
            {
                sg_demo_info.aux_bright = aux_new;
                upload_device_value_status(DPID_AUX_BRIGHT_VALUE, aux_new);
            }
        }
        __update_pwm_output(LIGHT_TYPE_ALL);
        break;
    }
}

/**
 * @brief 单按色温调节处理
 */
STATIC VOID __handle_single_press_color_temp(BOOL_T is_increase)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;
    UINT8_T new_temp = __get_next_color_temp_level(sg_demo_info.white_temp, is_increase);
            app_light_schedule_user_interrupt_sleep_wake();

    if (new_temp != sg_demo_info.white_temp)
    {
        sg_demo_info.white_temp = new_temp;

        // 更新PWM输出
        if(sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1){
            __update_pwm_output(LIGHT_TYPE_ALL);
            upload_device_value_status(DPID_TEMP_VALUE, new_temp);
        }
        else if(sg_demo_info.white_switch == 1)
        {
            __update_pwm_output(LIGHT_TYPE_MAIN);
            upload_device_value_status(DPID_TEMP_VALUE, new_temp);
        }
        else if (sg_demo_info.aux_switch == 1)
        {
            __update_pwm_output(LIGHT_TYPE_AUX);
            upload_device_value_status(DPID_TEMP_VALUE, new_temp);
        }
    }
}
/**
 * @brief 从单个PWM通道反向计算亮度（用于夜灯）
 */
UINT8_T __reverse_calculate_single_pwm(UINT16_T pwm_value)
{
    // 夜灯亮度映射：PWM值范围 0-10000 -> 0-100%
    UINT8_T brightness = pwm_value / 100; // 四舍五入到整数百分比
    if (brightness >= 100)
        brightness = 100;
    if (brightness <= 1)
        brightness = 1;
    return brightness;
}

/**
 * @brief 将亮度值(5-100)转换为亮度百分比(1-100)
 */
UINT8_T __convert_brightness_value_to_percent(UINT16_T brightness_value)
{
    if (brightness_value <= 5)
    {
        return 1;
    }
    else if (brightness_value >= 99)
    {
        return 100;
    }
    else
    {
        // 使用浮点计算确保精度
        float brightness_float = 1.0f + (brightness_value - 5.0f) * 99.0f / 95.0f;
        UINT8_T brightness = (UINT8_T)(brightness_float + 0.5f); // 四舍五入
        if (brightness > 99)
            brightness = 100;
        if (brightness < 1)
            brightness = 1;
        return brightness;
    }
}

/**
 * @brief 从WW和CW PWM值反向计算亮度和色温
 */
VOID __reverse_calculate_from_pwm(UINT16_T ww_pwm, UINT16_T cw_pwm, UINT8_T adjust_type,
                                         UINT8_T *brightness, UINT8_T *temp)
{
    // 防止除零
    if (ww_pwm == 0 && cw_pwm == 0)
    {
        *brightness = 0;
        *temp = 0;
        return;
    }

    BOOL_T is_brightness_adjust = (adjust_type == ADJUST_TYPE_BRIGHTNESS_UP || adjust_type == ADJUST_TYPE_BRIGHTNESS_DOWN);
    BOOL_T is_color_temp_adjust = (adjust_type == ADJUST_TYPE_COLOR_UP || adjust_type == ADJUST_TYPE_COLOR_DOWN);

    if (is_brightness_adjust)
    {

        // 获取当前的色温值（保持不变）
        UINT8_T current_temp = *temp;

        if (current_temp == 0)
        {
            UINT16_T brightness_value = cw_pwm / 100;
            *brightness = __convert_brightness_value_to_percent(brightness_value);
        }
        else
        {
            // 正常计算：亮度值 = WW_PWM / 色温值
            UINT16_T brightness_value = ww_pwm / current_temp;
            *brightness = __convert_brightness_value_to_percent(brightness_value);
        }
    }
    else if (is_color_temp_adjust)
    {
        // 获取当前的亮度值（保持不变）
        UINT8_T current_brightness = *brightness;
        UINT16_T brightness_value = __convert_brightness_percent_to_value(current_brightness);

        if (brightness_value == 0)
        {
            *temp = 0;
        }
        else
        {
            // 色温值 = WW_PWM / 亮度值
            if (ww_pwm > 0 && brightness_value > 0)
            {
                if(ww_pwm >= 9997){
                    ww_pwm = 10000;
                }
                if(ww_pwm <= 3){
                    *temp = 0;
                }
                *temp = ww_pwm / brightness_value;
                
                if (*temp >= 99)
                    *temp = 100;
                if (*temp <= 0)
                    *temp = 0;
            }
            else
            {
                *temp = 0;
            }
        }
    }
}

/**
 * @brief 获取当前渐变的实际亮度和色温值
 */
VOID __get_current_gradient_values(UINT8_T light_type, UINT8_T adjust_type,
                                   UINT8_T *current_brightness, UINT8_T *current_temp)
{
    UINT16_T ww_pwm = 0, cw_pwm = 0;
    UINT16_T single_pwm = 0;
    extern DEMO_INFO_T sg_demo_info;

    // 先获取当前的亮度和色温值（保持不变的那个参数）
    switch (light_type)
    {
    case LIGHT_TYPE_NIGHT:
        single_pwm = __get_current_pwm_duty(NIGHT_BRIGHT_PWM);
        *current_brightness = __reverse_calculate_single_pwm(single_pwm);
        *current_temp = 0;
        break;

    case LIGHT_TYPE_MAIN:
        // 先获取当前值
        *current_brightness = sg_demo_info.white_bright;
        *current_temp = sg_demo_info.white_temp;

        ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(TEMP_PWM);

        // 使用正确的反向计算（基于固定参数）
        __reverse_calculate_from_pwm(ww_pwm, cw_pwm, adjust_type, current_brightness, current_temp);
        break;

    case LIGHT_TYPE_AUX:
        // 先获取当前值
        *current_brightness = sg_demo_info.aux_bright;
        *current_temp = sg_demo_info.white_temp;

        ww_pwm = __get_current_pwm_duty(AUX_BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(AUX_TEMP_PWM);

        __reverse_calculate_from_pwm(ww_pwm, cw_pwm, adjust_type, current_brightness, current_temp);
        break;

    case LIGHT_TYPE_ALL:
        // 使用主灯的值作为参考
        *current_brightness = sg_demo_info.white_bright;
        *current_temp = sg_demo_info.white_temp;

        ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(TEMP_PWM);

        __reverse_calculate_from_pwm(ww_pwm, cw_pwm, adjust_type, current_brightness, current_temp);
        break;

    default:
        *current_brightness = 0;
        *current_temp = 0;
        break;
    }
}

/**
 * @brief 处理按键事件
 */
STATIC VOID __handle_key_event(REMOTE_PARSE_RESULT_T *result)
{
    extern uint16_t gradual_time_ms;
    extern TIMER_ID night_light_id;
    extern uint8_t custom_status[6];
    extern int night_light_mode;
    extern bool sg_rhythm_interrupted;
    extern DEMO_INFO_T sg_demo_info;
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;

    if (result == NULL || result->parse_result != PARSE_SUCCESS)
    {
        return;
    }

    USER_REMOTE_DATA_T *remote_data = &result->remote_data;
    UINT32_T current_time = tal_system_get_millisecond();
    
    // 防抖检查
    if (remote_data->key_id == sg_last_key_id &&
        (current_time - sg_last_key_time) < sg_debounce_delay) {
        return;
    }
    
    // 端口防抖
    if(remote_port == 1 && ((current_time - remote_time) < sg_debounce_delay)){
        return;
    } else {
        remote_port = 0;
    }

    // 按键变化时重置长按状态
    if (remote_data->key_id != sg_last_key_id) {
        sg_long_press_active = 0;
        sg_long_press_key = 0;
    }

    // 长按逻辑
    if (result->operation_type == OP_TYPE_LONG_PRESS) {
        if (!sg_long_press_active) {
            // 首次进入长按状态
            sg_long_press_start_time = current_time;
            sg_long_press_active = 1;
            sg_long_press_key = remote_data->key_id;
            sg_last_long_press_trigger_time = 0;
        } else if (sg_long_press_key == remote_data->key_id) {
            // 已经在长按状态
            uint32_t long_press_elapsed = current_time - sg_long_press_start_time;
            uint32_t threshold = LONG_PRESS_TIME;
            uint8_t is_single = (remote_data->key_id == KEY_ID_COLOR_UP || 
                                    remote_data->key_id == KEY_ID_COLOR_DOWN ||
                                    remote_data->key_id == KEY_ID_MAIN_BRIGHT_UP || 
                                    remote_data->key_id == KEY_ID_MAIN_BRIGHT_DOWN ||
                                    remote_data->key_id == KEY_ID_BRIGHT_UP ||
                                    remote_data->key_id == KEY_ID_BRIGHT_DOWN ||
                                    remote_data->key_id == KEY_ID_AMBIENT_BRIGHT_UP||
                                    remote_data->key_id == KEY_ID_AMBIENT_BRIGHT_DOWN
                                );
            if (is_single && sg_last_key_id != 0) {
                // 单键长按：可重复触发
                return;
            }
            if (long_press_elapsed >= threshold) {
                // 判断按键类型
                if (!is_single ) {
                    // 其他长按：只触发一次
                    uint32_t interval = current_time - sg_last_long_press_trigger_time;
                    if (sg_last_long_press_trigger_time != 0 && interval >= 1000) {
                        return;  // 已触发过，不再触发
                    }
                    sg_last_long_press_trigger_time = current_time;
                }
            }
        }
    }

    // 更新记录
    sg_last_key_id = remote_data->key_id;
    sg_last_key_time = current_time;

    UINT8_T target_light_type = __get_target_light_type_by_key(remote_data->key_id);
    device_config_save();
    
    // 根据按键处理
    switch (remote_data->key_id)
    {
    case KEY_ID_POWER: // K3 总开/关
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            // 确保退出夜灯模式
            if (sg_demo_info.change_light_status == 0)
                gradual_time_ms = 20;
            else
                gradual_time_ms = 1600;
            app_light_schedule_user_interrupt_sleep_wake();

            // TAL_PR_NOTICE("result->additional_param = %02x\r\n",result->additional_param);
            // if ((app_nightlight_should_enter()) && (result->additional_param == 0x11))
            /* 关灯态手动开灯：仅当节律期望亮度为0且开关实际变化时打断 */
            if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) && (sg_demo_info.night_switch == 0)) {
                app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_LIGHT_ON);
            }
            if ((app_nightlight_should_enter()) &&(sg_demo_info.white_switch == 0&&sg_demo_info.aux_switch == 0&&sg_demo_info.night_switch == 0))
            {
                app_nightlight_apply();
                if (night_light_mode == 1)
                {
                    night_light_mode = 0;
                    // sg_demo_info.switch_status = 0;
                    return;
                }
            }
            // else if (result->additional_param == 0x11)
            else if ((sg_demo_info.aux_switch == 0) && (sg_demo_info.white_switch == 0)&&(sg_demo_info.night_switch == 0))
            {
                upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
                dp_report = 1;
                // 正常开灯模式
                // 根据lsat_light_memory值决定哪些灯要打开
                uint8_t memory_state = sg_demo_info.last_light_memory;

                // 根据记忆状态设置开关
                switch (memory_state)
                {
                case 1:
                { // 关的总开关 - 所有灯都不开
                    // 正常开灯模式
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.aux_switch = 1;
                    sg_demo_info.night_switch = 0;
                    sg_demo_info.last_light_memory = 4;
                    // 主灯亮度计算
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                    uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);

                    // 辅灯亮度计算
                    uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                    uint8_t aux_temp1 = sg_demo_info.white_temp;
                    uint16_t aux_ww = aux_bright1 * aux_temp1;
                    uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);

                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                    UINT32_T duties[] = {ww, cw, aux_ww, aux_cw, 0};
                    pwm_gradual_duty_set_multi(5, channels, duties);
                }
                break;

                case 2:
                { // 关的主灯 - 只开辅灯和夜灯
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.night_switch = 0;
                    // 主灯亮度计算
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                    uint16_t ww = white_bright1 * white_temp1;
                    uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM,NIGHT_BRIGHT_PWM};
                    UINT32_T duties[] = {ww, cw,0};
                    pwm_gradual_duty_set_multi(3, channels, duties);
                }
                break;

                case 3:
                { // 关的辅灯 - 只开主灯和夜灯
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.aux_switch = 1;
                    sg_demo_info.night_switch = 0;
                    uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                    uint8_t aux_temp1 = sg_demo_info.white_temp;
                    uint16_t aux_ww = aux_bright1 * aux_temp1;
                    uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                    TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM,NIGHT_BRIGHT_PWM};
                    UINT32_T duties[] = {aux_ww, aux_cw,0};
                    pwm_gradual_duty_set_multi(2, channels, duties);
                }
                break;

                case 4: // 夜灯记忆：开夜灯并打断节律
                    sg_demo_info.switch_status = 1;
                    sg_demo_info.white_switch = 0;
                    sg_demo_info.aux_switch = 0;
                    sg_demo_info.night_switch = 1;
                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                    UINT32_T duties[] = {0, 0, 0, 0, sg_demo_info.night_bright * 100};
                    pwm_gradual_duty_set_multi(5, channels, duties);
                    app_light_rhythm_interrupt_on_night_open();
                    break;
                }
                if (memory_state != 4) {
                    app_light_tm_rhythm_syn();
                }
            }
            else
            {
                // ==================== 关灯逻辑 ====================
                upload_device_enum_status(DPID_WORK_MODE, 16);
                dp_report = 1;
                app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_LIGHT_OFF);
                sg_demo_info.switch_status = 0;
                if (sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1)
                {
                    sg_demo_info.white_switch = 0;
                    sg_demo_info.aux_switch = 0;
                    sg_demo_info.night_switch = 0;
                    sg_demo_info.last_light_memory = 1;
                    // 关闭所有PWM输出
                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                    UINT32_T duties[] = {0, 0, 0, 0};
                    pwm_gradual_duty_set_multi(4, channels, duties);
                }
                else if (sg_demo_info.white_switch == 1)
                {
                    sg_demo_info.white_switch = 0;
                    sg_demo_info.last_light_memory = 2;
                    TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                    UINT32_T duties[] = {0, 0};
                    pwm_gradual_duty_set_multi(2, channels, duties);
                }
                else if (sg_demo_info.aux_switch == 1)
                {
                    sg_demo_info.aux_switch = 0;
                    sg_demo_info.last_light_memory = 3;
                    TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                    UINT32_T duties[] = {0, 0};
                    pwm_gradual_duty_set_multi(2, channels, duties);
                }
                if (sg_demo_info.night_switch == 1)
                {
                    sg_demo_info.night_switch = 0;
                    pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
                    sg_demo_info.last_light_memory = 4;
                }
            }
        }
        break;

    case KEY_ID_BRIGHT_UP:         // 亮度增加
    case KEY_ID_MAIN_BRIGHT_UP:    // 主灯亮度+
    case KEY_ID_AMBIENT_BRIGHT_UP: // 辅灯亮度+
            app_light_schedule_user_interrupt_sleep_wake();

        upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
        app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_DIM_TEMP);
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            // 单按：档位调节
            sg_is_long_press_active = FALSE;
            __handle_single_press_brightness(target_light_type, TRUE);
        }
        else if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            gradual_time_ms = 3010;
            // 长按开始：直接设置目标值并使用PWM渐变
            __handle_long_press_adjustment(target_light_type, ADJUST_TYPE_BRIGHTNESS_UP, TRUE);
        }
        break;

    case KEY_ID_BRIGHT_DOWN:         // 亮度减少
    case KEY_ID_MAIN_BRIGHT_DOWN:    // 主灯亮度-
    case KEY_ID_AMBIENT_BRIGHT_DOWN: // 辅灯亮度-
        upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
            app_light_schedule_user_interrupt_sleep_wake();

        app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_DIM_TEMP);
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            // 单按：档位调节
            sg_is_long_press_active = FALSE;
            __handle_single_press_brightness(target_light_type, FALSE);
        }
        else if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            gradual_time_ms = 3010;
            // 长按开始：直接设置目标值并使用PWM渐变
            __handle_long_press_adjustment(target_light_type, ADJUST_TYPE_BRIGHTNESS_DOWN, TRUE);
        }
        break;

    case KEY_ID_COLOR_UP: // 色温增加
        upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
            app_light_schedule_user_interrupt_sleep_wake();

        app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_DIM_TEMP);
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            sg_is_long_press_active = FALSE;
            __handle_single_press_color_temp(TRUE);
        }
        else if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            gradual_time_ms = 3010;
            // 长按开始：直接设置目标值并使用PWM渐变
            UINT8_T target_light_type = __get_target_light_type_by_key(remote_data->key_id);
            __handle_long_press_adjustment(target_light_type, ADJUST_TYPE_COLOR_UP, TRUE);
        }
        break;

    case KEY_ID_COLOR_DOWN: // 色温减少
        upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
            app_light_schedule_user_interrupt_sleep_wake();

        app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_DIM_TEMP);
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            // 单按：档位调节
            sg_is_long_press_active = FALSE;
            __handle_single_press_color_temp(FALSE);
        }
        else if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            gradual_time_ms = 3010;
            UINT8_T target_light_type = __get_target_light_type_by_key(remote_data->key_id);
            // 长按开始：直接设置目标值并使用PWM渐变
            __handle_long_press_adjustment(target_light_type, ADJUST_TYPE_COLOR_DOWN, TRUE);
        }
        break;

    case KEY_RELEASE:
        if (result->operation_type == OP_TYPE_LONG_RELEASE)
        {
            // 长按松手：停止调光并上报
            if (sg_demo_info.change_light_status == 0)
                gradual_time_ms = 20;
            else
                gradual_time_ms = 1600;
            sg_last_key_id = 0; 
            // 确保调光状态是活跃的
            if (sg_is_long_press_active)
            {
                __handle_long_press_adjustment(sg_continuous_light_type, sg_continuous_adjust_type, FALSE);
            }
        }
        break;

    case KEY_ID_MODE: // K25 模式
        upload_device_enum_status(DPID_WORK_MODE, 16); // 正常模式
            app_light_schedule_user_interrupt_sleep_wake();

        if (sg_demo_info.rhythm_switch == 1) {
            app_light_stop_today_rhythm_timer();
        }
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            /* 模式键切主灯：之前夜灯亮则保留夜灯记忆，否则不改 last_light_memory */
            UINT8_T night_was_on = sg_demo_info.night_switch;
            sg_last_key_id = 0;
            if (night_was_on) {
                sg_demo_info.last_light_memory = 4;
            }
            // TAL_PR_NOTICE("reverse[0] = %02x\r\n",result->additional_param);
            switch (result->additional_param)
            {
            case 0x11:                  // 白光
                // sg_current_mode = 0x12; // 切换到黄光
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.white_temp = 100;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.night_switch = 0;
                TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
                pwm_gradual_duty_set_multi(5, channels1, duties1);
                dp_report = 1;
                break;

            case 0x12:                  // 黄光
                // sg_current_mode = 0x13; // 切换到中性光
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 100;
                sg_demo_info.white_temp = 55;
                sg_demo_info.aux_bright = 100;
                sg_demo_info.night_switch = 0;
                TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties2[] = {5500, 4500, 5500, 4500, 0};
                pwm_gradual_duty_set_multi(5, channels2, duties2);
                dp_report = 1;
                break;

            case 0x13:                  // 中性光
                // sg_current_mode = 0x11; // 切换回白光
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 1;
                sg_demo_info.white_bright = 60;
                sg_demo_info.white_temp = 35;
                sg_demo_info.aux_bright = 60;
                sg_demo_info.night_switch = 0;
                TUYA_PWM_NUM_E channels3[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties3[] = {60 * 35, 60 * 65, 60 * 35, 60 * 65, 0};
                pwm_gradual_duty_set_multi(5, channels3, duties3);
                dp_report = 1;
                break;
            }
        }
        break;
    case KEY_ID_MODE1:
        if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            app_light_schedule_user_interrupt_sleep_wake();

            upload_device_enum_status(DPID_WORK_MODE,16);
            // 收藏模式
            sg_demo_info.white_switch = sg_demo_info.collect[0];
            sg_demo_info.white_bright = sg_demo_info.collect[1];
            sg_demo_info.aux_switch = sg_demo_info.collect[2];
            sg_demo_info.aux_bright = sg_demo_info.collect[3];
            sg_demo_info.white_temp = sg_demo_info.collect[4];
            sg_demo_info.night_switch = sg_demo_info.collect[5];
            if(sg_demo_info.night_switch == 1)
                sg_demo_info.night_bright = sg_demo_info.collect[6];

            TUYA_PWM_NUM_E all_channels[5];
            UINT32_T all_duties[5];
            UINT8_T channel_count = 0;

            if (sg_demo_info.white_switch == 1)
            {
                sg_demo_info.switch_status = 1;
                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;

                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;
            }
            else
            {
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = 0;

                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = 0;
            }

            if (sg_demo_info.aux_switch == 1)
            {
                sg_demo_info.switch_status = 1;
                pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;

                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            else
            {
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = 0;

                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = 0;
            }

            if (sg_demo_info.night_switch == 1)
            {
                sg_demo_info.white_switch = 0;
                sg_demo_info.switch_status = 1;
                sg_demo_info.aux_switch = 0;
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = sg_demo_info.night_bright * 100;
            }
            else
            {
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
            }

            if (channel_count > 0)
            {
                pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
            }
            dp_report = 1;
            // sg_last_key_id = 0;
        }
        break;

    case KEY_ID_NIGHT_LIGHT: // K21 夜灯
        app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_LIGHT_ON);
        // upload_device_bool_status(RHYTHM_STATUS, 1);
        upload_device_enum_status(DPID_WORK_MODE, 16);
            app_light_schedule_user_interrupt_sleep_wake();

        
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            // TAL_PR_NOTICE("yedeng duanan\r\n");
            if (sg_is_long_press_active)
            {
                // 如果正在调光，先停止
                __handle_long_press_adjustment(sg_continuous_light_type, sg_continuous_adjust_type, FALSE);
            }
            sg_last_key_id = 0;
            sg_demo_info.night_switch = 1;
            sg_demo_info.white_switch = 0;
            sg_demo_info.switch_status = 1;
            sg_demo_info.aux_switch = 0;
            if(sg_demo_info.night_bright == 0)
                sg_demo_info.night_bright = 1;
            TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
            UINT32_T duties[] = {0, 0, 0, 0, sg_demo_info.night_bright * 100};
            pwm_gradual_duty_set_multi(5, channels, duties);
            dp_report = 1;
        }
        else if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            // TAL_PR_NOTICE("yedeng changan\r\n");
            if(sg_demo_info.night_switch == 1)
                __handle_night_light_long_press();
        }
    break;

    case KEY_ID_MAIN_SWITCH: // K13 主光/下光开/关

        if (sg_demo_info.change_light_status == 0)
            gradual_time_ms = 20;
        else
            gradual_time_ms = 1600;
            app_light_schedule_user_interrupt_sleep_wake();

        upload_device_enum_status(DPID_WORK_MODE, 16);
        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            
            dp_report = 1;
            // if (result->additional_param == 0x11)
            if(sg_demo_info.white_switch == 0)
            {
                /* 手动开主灯打断节律 */
                app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_LIGHT_ON);
                sg_demo_info.white_switch = 1;
                sg_demo_info.switch_status = 1;
                if (sg_demo_info.night_switch == 1)
                {
                    // pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
                    sg_demo_info.night_switch = 0;
                    sg_demo_info.last_light_memory = 4;
                }
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM,NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {ww, cw,0};
                pwm_gradual_duty_set_multi(3, channels, duties);
            }
            // if (result->additional_param == 0x10)
            else
            {
                sg_demo_info.white_switch = 0;
                TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
                UINT32_T duties[] = {0, 0};
                pwm_gradual_duty_set_multi(2, channels, duties);
                sg_demo_info.last_light_memory = 2;
                if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0)&&(sg_demo_info.night_switch == 0))
                {
                    // sg_demo_info.last_light_memory = 1;
                    sg_demo_info.switch_status = 0;
                }
            }
        }
        break;

    case KEY_ID_AMBIENT_SWITCH: // K18 氛围光/上光开/关
        if (sg_demo_info.change_light_status == 0)
            gradual_time_ms = 20;
        else
            gradual_time_ms = 1600;
        upload_device_enum_status(DPID_WORK_MODE, 16);
            app_light_schedule_user_interrupt_sleep_wake();

        if (result->operation_type == OP_TYPE_SHORT_PRESS)
        {
            dp_report = 1;
            
            // if (result->additional_param == 0x10)
            if(sg_demo_info.aux_switch == 0)
            {
                /* 手动开辅灯打断节律 */
                app_light_rhythm_interrupt_on_user_change(true, RHYTHM_USER_OP_LIGHT_ON);
                sg_demo_info.aux_switch = 1;
                sg_demo_info.switch_status = 1;
                if (sg_demo_info.night_switch == 1)
                {
                    // pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
                    sg_demo_info.night_switch = 0;
                    sg_demo_info.last_light_memory = 4;
                }
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM,NIGHT_BRIGHT_PWM};
                UINT32_T duties[] = {aux_ww, aux_cw,0};
                pwm_gradual_duty_set_multi(3, channels, duties);
            }
            // if (result->additional_param == 0x11)
            else
            {
                sg_demo_info.aux_switch = 0;
                
                TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties[] = {0, 0};
                pwm_gradual_duty_set_multi(2, channels, duties);
                sg_demo_info.last_light_memory = 3;
                if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0)&&(sg_demo_info.night_switch == 0))
                {
                    sg_demo_info.switch_status = 0;
                    // sg_demo_info.last_light_memory = 1;
                }
            }
        }
        break;
        
    case KEY_COLLECT: // 收藏键
        if (result->operation_type == OP_TYPE_LONG_PRESS) {
            // 长按处理
            if (sg_long_press_active && sg_long_press_key == KEY_COLLECT) {
                uint32_t long_press_elapsed = current_time - sg_long_press_start_time;
                
                if (long_press_elapsed >= COLLECT_LONG_PRESS_TIME) {
                    if (sg_demo_info.white_switch == 0 && sg_demo_info.aux_switch == 0 && sg_demo_info.night_switch == 0)
                        return;
                    // remote_port = 1;
                    sg_last_key_id = 0; 
                    remote_port = 1;
                    // 更新收藏模式
                    sg_demo_info.collect[0] = sg_demo_info.white_switch;
                    sg_demo_info.collect[1] = sg_demo_info.white_bright;
                    sg_demo_info.collect[2] = sg_demo_info.aux_switch;
                    sg_demo_info.collect[3] = sg_demo_info.aux_bright;
                    sg_demo_info.collect[4] = sg_demo_info.white_temp;
                    sg_demo_info.collect[5] = sg_demo_info.night_switch;
                    sg_demo_info.collect[6] = sg_demo_info.night_bright;
                    device_config_save1();
                    // 第一阶段：设置高亮度
                    gradual_time_ms = 1000;
                    TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                    UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
                    pwm_gradual_duty_set_multi(5, channels1, duties1);

                    // 等待渐变完成
                    pwm_gradual_wait_current_complete(1000); // 等待1秒

                    // 第二阶段：设置为低亮度
                    TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                    UINT32_T duties2[] = {3, 3, 3, 3};
                    pwm_gradual_duty_set_multi(4, channels2, duties2);

                    pwm_gradual_wait_current_complete(1000); // 等待1秒
                    if (sg_demo_info.change_light_status == 0)
                        gradual_time_ms = 20;
                    else
                        gradual_time_ms = 1600;
                    // 创建所有通道的数组
                    TUYA_PWM_NUM_E all_channels[5];
                    UINT32_T all_duties[5];
                    UINT8_T channel_count = 0;

                    // 设置主灯
                    uint16_t ww = 0;
                    uint16_t cw = 0;
                    if (sg_demo_info.white_switch == 1)
                    {
                        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                        uint8_t white_temp1 = sg_demo_info.white_temp;
                        ww = white_bright1 * white_temp1;
                        cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                        all_channels[channel_count] = BRIGHT_PWM;
                        all_duties[channel_count++] = ww;

                        all_channels[channel_count] = TEMP_PWM;
                        all_duties[channel_count++] = cw;
                    }
                    else
                    {
                        all_channels[channel_count] = BRIGHT_PWM;
                        all_duties[channel_count++] = 0;

                        all_channels[channel_count] = TEMP_PWM;
                        all_duties[channel_count++] = 0;
                    }

                    // 设置辅灯
                    uint16_t aux_ww = 0;
                    uint16_t aux_cw = 0;
                    if (sg_demo_info.aux_switch == 1)
                    {
                        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                        uint8_t aux_temp1 = sg_demo_info.white_temp;
                        aux_ww = aux_bright1 * aux_temp1;
                        aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                        all_channels[channel_count] = AUX_BRIGHT_PWM;
                        all_duties[channel_count++] = aux_ww;

                        all_channels[channel_count] = AUX_TEMP_PWM;
                        all_duties[channel_count++] = aux_cw;
                    }
                    else
                    {
                        all_channels[channel_count] = AUX_BRIGHT_PWM;
                        all_duties[channel_count++] = 0;

                        all_channels[channel_count] = AUX_TEMP_PWM;
                        all_duties[channel_count++] = 0;
                    }

                    // 设置夜灯
                    if (sg_demo_info.night_switch == 1)
                    {
                        uint8_t night_bright1 = sg_demo_info.night_bright;

                        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                        all_duties[channel_count++] = night_bright1 * 100;
                    }
                    else
                    {
                        all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                        all_duties[channel_count++] = 0;
                    }

                    // 一次性提交所有通道
                    if (channel_count > 0)
                    {
                        pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                    }
                    collect_time = tal_system_get_millisecond();
                    // 更新收藏模式
                    // sg_demo_info.collect[0] = sg_demo_info.white_switch;
                    // sg_demo_info.collect[1] = sg_demo_info.white_bright;
                    // sg_demo_info.collect[2] = sg_demo_info.aux_switch;
                    // sg_demo_info.collect[3] = sg_demo_info.aux_bright;
                    // sg_demo_info.collect[4] = sg_demo_info.white_temp;
                    // sg_demo_info.collect[5] = sg_demo_info.night_switch;
                    // sg_demo_info.collect[6] = sg_demo_info.night_bright;
                    
                }
            }
        }
        break;

    case KEY_RESET: // 配网复位
        if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            extern uint8_t custom_status[6];
            extern uint8_t sleep_init[14];
            extern uint8_t wake_init[11];
            extern uint8_t switch_change_gear[21];
            extern uint8_t collect[7];
            extern uint8_t rhythm_sunlight1[66];
            extern uint8_t night[6];
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
            // tuya_iot_wf_gw_unactive();
            tuya_iot_wf_gw_reset();
            // 执行配网复位操作
        }
        break;

    case KEY_ALIGNMENT: // 对码
        if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            UINT32_T current_time = tal_system_get_millisecond();
            if ((current_time - sg_power_on_time) >= sg_auto_stop_timeout)
            {
                return; // 直接退出，不处理对码
            }
            // 只有在配对模式下才允许对码
            if (sg_in_pairing_mode)
            {
                OPERATE_RET ret = user_ble_remote_add_whitelist(remote_data->remote_id, remote_data->group_id);
            }
            else
                return;
            device_config_save1();
            gradual_time_ms = 1000;
            // 第一阶段：设置高亮度
            TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
            UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
            pwm_gradual_duty_set_multi(5, channels1, duties1);

            // 等待渐变完成
            pwm_gradual_wait_current_complete(1000); // 等待1秒

            // 第二阶段：设置为低亮度
            TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
            UINT32_T duties2[] = {3, 3, 3,3};
            pwm_gradual_duty_set_multi(4, channels2, duties2);
            pwm_gradual_wait_current_complete(1000); // 等待1秒

            if (sg_demo_info.change_light_status == 0)
                gradual_time_ms = 20;
            else
                gradual_time_ms = 1600;
            // 创建所有通道的数组
            TUYA_PWM_NUM_E all_channels[5];
            UINT32_T all_duties[5];
            UINT8_T channel_count = 0;

            // 设置主灯
            uint16_t ww = 0;
            uint16_t cw = 0;
            if (sg_demo_info.white_switch == 1)
            {
                uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                ww = white_bright1 * white_temp1;
                cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = ww;

                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = cw;

            }
            else
            {
                all_channels[channel_count] = BRIGHT_PWM;
                all_duties[channel_count++] = 0;

                all_channels[channel_count] = TEMP_PWM;
                all_duties[channel_count++] = 0;
            }

            // 设置辅灯
            uint16_t aux_ww = 0;
            uint16_t aux_cw = 0;
            if (sg_demo_info.aux_switch == 1)
            {
                uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                aux_ww = aux_bright1 * aux_temp1;
                aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = aux_ww;

                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = aux_cw;
            }
            else
            {
                all_channels[channel_count] = AUX_BRIGHT_PWM;
                all_duties[channel_count++] = 0;

                all_channels[channel_count] = AUX_TEMP_PWM;
                all_duties[channel_count++] = 0;
            }

            // 设置夜灯
            if (sg_demo_info.night_switch == 1)
            {
                uint8_t night_bright1 = sg_demo_info.night_bright;

                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = night_bright1 * 100;
            }
            else
            {
                all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                all_duties[channel_count++] = 0;
            }

            // 一次性提交所有通道
            if (channel_count > 0)
            {
                pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
            }
        }
        break;

    case KEY_CLEAR: // 清码
        if (result->operation_type == OP_TYPE_LONG_PRESS)
        {
            extern uint8_t collect[7];
            UINT32_T current_time = tal_system_get_millisecond();
            if ((current_time - sg_power_on_time) >= sg_auto_stop_timeout)
            {
                return; // 直接退出，不处理清码
            }
            if (sg_in_pairing_mode)
            {
                OPERATE_RET ret = user_ble_remote_remove_whitelist(remote_data->remote_id, remote_data->group_id);
                memset(sg_demo_info.collect, 0, 7);
                memcpy(sg_demo_info.collect,collect,7);
                device_config_save1();
                sg_in_pairing_mode = FALSE;
                // 第一阶段：设置高亮度
                gradual_time_ms = 1000;
                TUYA_PWM_NUM_E channels1[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM, NIGHT_BRIGHT_PWM};
                UINT32_T duties1[] = LIGHT_PWM_BLINK_DUTIES5_INIT;
                pwm_gradual_duty_set_multi(5, channels1, duties1);

                // 等待渐变完成
                pwm_gradual_wait_current_complete(1000); // 等待1秒

                // 第二阶段：设置为低亮度
                TUYA_PWM_NUM_E channels2[] = {BRIGHT_PWM, TEMP_PWM, AUX_BRIGHT_PWM, AUX_TEMP_PWM};
                UINT32_T duties2[] = {3, 3, 3, 3};
                pwm_gradual_duty_set_multi(4, channels2, duties2);

                pwm_gradual_wait_current_complete(1000); // 等待1秒

                if (sg_demo_info.change_light_status == 0)
                    gradual_time_ms = 20;
                else
                    gradual_time_ms = 1600;
                // 创建所有通道的数组
                TUYA_PWM_NUM_E all_channels[5];
                UINT32_T all_duties[5];
                UINT8_T channel_count = 0;

                // 设置主灯
                uint16_t ww = 0;
                uint16_t cw = 0;
                if (sg_demo_info.white_switch == 1)
                {
                    uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
                    uint8_t white_temp1 = sg_demo_info.white_temp;
                    ww = white_bright1 * white_temp1;
                    cw = white_bright1 * 100 - ww;
                light_pwm_clamp_mix(&ww, &cw);
                    all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = ww;

                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = cw;
                }
                else
                {
                    all_channels[channel_count] = BRIGHT_PWM;
                    all_duties[channel_count++] = 0;

                    all_channels[channel_count] = TEMP_PWM;
                    all_duties[channel_count++] = 0;
                }

                // 设置辅灯
                uint16_t aux_ww = 0;
                uint16_t aux_cw = 0;
                if (sg_demo_info.aux_switch == 1)
                {
                    uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
                    uint8_t aux_temp1 = sg_demo_info.white_temp;
                    aux_ww = aux_bright1 * aux_temp1;
                    aux_cw = aux_bright1 * 100 - aux_ww;
                light_pwm_clamp_mix(&aux_ww, &aux_cw);
                    all_channels[channel_count] = AUX_BRIGHT_PWM;
                    all_duties[channel_count++] = aux_ww;

                    all_channels[channel_count] = AUX_TEMP_PWM;
                    all_duties[channel_count++] = aux_cw;
                }
                else
                {
                    all_channels[channel_count] = AUX_BRIGHT_PWM;
                    all_duties[channel_count++] = 0;

                    all_channels[channel_count] = AUX_TEMP_PWM;
                    all_duties[channel_count++] = 0;
                }

                // 设置夜灯
                if (sg_demo_info.night_switch == 1)
                {
                    uint8_t night_bright1 = sg_demo_info.night_bright;

                    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                    all_duties[channel_count++] = night_bright1 * 100;
                }
                else
                {
                    all_channels[channel_count] = NIGHT_BRIGHT_PWM;
                    all_duties[channel_count++] = 0;
                }

                // 一次性提交所有通道
                if (channel_count > 0)
                {
                    pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
                }
            }
        }
        break;

    default:
        break;
    }
}

/**
 * @brief 查找服务UUID数据
 */
STATIC UINT8_T *__find_service_uuid_data(UINT8_T *adv_data, UINT8_T adv_len)
{
    UINT8_T *p = adv_data;
    UINT8_T pos = 0;

    while (pos < adv_len)
    {
        UINT8_T field_len = p[0];
        if (field_len == 0)
            break;

        if (pos + field_len + 1 > adv_len)
            break;

        UINT8_T field_type = p[1];

        if (field_type == ADV_DATA_TYPE_SERVICE_UUID && field_len == SERVICE_UUID_AD_LENGTH)
        {
            // 找到服务UUID数据，返回数据部分（跳过length和type）
            return &p[2];
        }

        p += (field_len + 1);
        pos += (field_len + 1);
    }

    return NULL;
}

/**
 * @brief 解析完整的BLE广播包
 */
STATIC VOID __parse_ble_adv_packet(TAL_BLE_ADV_REPORT_T *scan_info, REMOTE_PARSE_RESULT_T *result)
{
    if (scan_info == NULL || result == NULL)
    {
        return;
    }

    // 初始化结果结构
    memset(result, 0, sizeof(REMOTE_PARSE_RESULT_T));
    result->rssi = scan_info->rssi;
    memcpy(result->mac_addr, scan_info->peer_addr.addr, 6);

    // 查找服务UUID数据
    UINT8_T *service_data = __find_service_uuid_data(scan_info->p_data, scan_info->data_len);
    if (service_data == NULL)
    {
        result->parse_result = PARSE_ERROR;
        return;
    }
    // 解析遥控器数据
    result->parse_result = __parse_remote_data(service_data, &result->remote_data, &result->operation_type,&result->additional_param);
}

STATIC VOID __ty_remote_receive_callback(UCHAR_T *data, UCHAR_T len, UCHAR_T type, UCHAR_T *mac)
{
    return;
}

STATIC VOID __user_remote_receive_callback(TAL_BLE_ADV_REPORT_T *scan_info)
{
    // 检查配对超时
    if (sg_in_pairing_mode)
    {
        UINT32_T current_time = tal_system_get_millisecond();
        if ((current_time - sg_power_on_time) >= sg_auto_stop_timeout)
        {
            sg_in_pairing_mode = FALSE;
        }
    }

    // 解析BLE广播包
    REMOTE_PARSE_RESULT_T parse_result;
    __parse_ble_adv_packet(scan_info, &parse_result);
    if (parse_result.parse_result == PARSE_SUCCESS)
    {
        // 处理按键事件
        __handle_key_event(&parse_result);
    }
}

// /***********************************************************
// ***********************external API*************************
// ***********************************************************/

/**
 * @brief 添加遥控器到白名单
 */
OPERATE_RET user_ble_remote_add_whitelist(UINT32_T remote_id, UINT8_T group_id)
{
    extern DEMO_INFO_T sg_demo_info;
    OPERATE_RET rt = OPRT_COM_ERROR;
    // 检查是否已存在
    for (UINT8_T i = 0; i < 5; i++)
    {
        if ((sg_demo_info.sg_remote_whitelist[i] == remote_id) && (sg_demo_info.remote_group[i] == group_id))
        {
            return rt;
        }
        if ((sg_demo_info.sg_remote_whitelist[i] == remote_id) && (sg_demo_info.remote_group[i] != group_id))
        {
            rt = OPRT_OK;
            sg_in_pairing_mode = FALSE;
            sg_demo_info.remote_group[i] = group_id;
            return rt;
        }
    }
    rt = OPRT_OK;
    sg_in_pairing_mode = FALSE;
    if(sg_whitelist_count > 4)
        sg_whitelist_count = 0;
    // 添加到白名单
    sg_demo_info.sg_remote_whitelist[sg_whitelist_count] = remote_id;
    sg_demo_info.remote_group[sg_whitelist_count] = group_id;
    sg_whitelist_count++;
    
    return rt;
}

/**
 * @brief 移除遥控器从白名单
 */
OPERATE_RET user_ble_remote_remove_whitelist(UINT32_T remote_id, UINT8_T group_id)
{
    extern DEMO_INFO_T sg_demo_info;
    extern uint8_t collect[7];
    for (UINT8_T i = 0; i < 5; i++)
    {
        if (sg_demo_info.sg_remote_whitelist[i] == remote_id && (sg_demo_info.remote_group[i] == group_id))
        {
            // 移动后续元素
            for (UINT8_T j = i; j < 4; j++)
            {
                sg_demo_info.sg_remote_whitelist[j] = sg_demo_info.sg_remote_whitelist[j + 1];
                sg_demo_info.remote_group[j] = sg_demo_info.remote_group[j + 1];
            }
            sg_whitelist_count--;
        }
    }
    if (sg_whitelist_count == 0)
    {

        memset(sg_demo_info.collect, 0, 7);
        memcpy(sg_demo_info.collect, collect, 7);
    }
    return OPRT_OK;
}

/**
 * @brief 清空白名单
 */
OPERATE_RET user_ble_remote_clear_whitelist(VOID)
{
    sg_whitelist_count = 0;
    return OPRT_OK;
}

/**
 * @brief 初始化绑定状态
 */
VOID user_ble_remote_init_bind_status(VOID)
{
    user_ble_remote_start_pairing();
}

/**
 * @brief ble scanf adv bind check callback
 *
 * @param[in] type: bind type for bluetooth remote controller
 * @param[in] data: data
 * @param[in] len: data len
 * @return OPRT_OK
 */
STATIC OPERATE_RET __ty_remote_bind_check_callback(TUYA_BLE_BIND_TYPE type, UCHAR_T *data, UCHAR_T len)
{
    return OPRT_OK;
}

/**
 * @brief ble scan adv bind notify callback
 *
 * @param[in] type: bind type for bluetooth remote controller
 * @return none
 */
STATIC VOID_T __ty_remote_bind_notify_callback(TUYA_BLE_BIND_TYPE type, int rslt)
{
    return;
}

TY_OBJ_DP_S *p_all_obj_dp = NULL;
VOID upload_device_all_status1(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    extern uint8_t sunrise_hour;
    extern uint8_t sunrise_min;
    extern uint8_t sunset_hour;
    extern uint8_t sunset_min;
    extern DEMO_INFO_T sg_demo_info;
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
            app_light_schedule_user_interrupt_sleep_wake();


    TY_OBJ_DP_S *p_obj_dp = NULL;

    memset((UCHAR_T *)p_all_obj_dp, 0, 8 * SIZEOF(TY_OBJ_DP_S));

    p_obj_dp = p_all_obj_dp;
    p_obj_dp->dpid = DPID_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.switch_status;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_WHITE_BRIGHT;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_bright;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_TEMP_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.white_temp;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_SWITCH_NIGHT_LIGHT;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.night_switch;

    p_obj_dp++;
    p_obj_dp->dpid = DPID_AUX_BRIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.aux_bright;

    p_obj_dp++;
    p_obj_dp->dpid = NIGHT_LIGHT_VALUE;
    p_obj_dp->type = PROP_VALUE;
    p_obj_dp->value.dp_value = sg_demo_info.night_bright;

    p_obj_dp++;
    p_obj_dp->dpid = LIGHT_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.white_switch;

    p_obj_dp++;
    p_obj_dp->dpid = AUX_SWITCH;
    p_obj_dp->type = PROP_BOOL;
    p_obj_dp->value.dp_bool = sg_demo_info.aux_switch;

    rt = dev_report_dp_json_async(NULL, p_all_obj_dp, 8);

    

}
STATIC VOID example_task(PVOID_T args)
{
    for (;;) {
        
        if(dp_report == 1){
            tal_system_sleep(500);
            upload_device_all_status1();
            dp_report = 0;
        }
        tal_system_sleep(50);
    }
    return;
}

/**
 * @brief tuya bluetooth remote initialization
 */
VOID user_ble_remote()
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;

    TUYA_BLE_SCAN_ADV_HANDLE_CBS ble_scan_cfg = {0};
    ble_scan_cfg.bind_check = __ty_remote_bind_check_callback;
    ble_scan_cfg.bind_notify = __ty_remote_bind_notify_callback;
    sg_power_on_time = tal_system_get_millisecond();

    /* enable ble scan function */
    TUYA_CALL_ERR_GOTO(tuya_ble_reg_app_scan_adv_cb(__ty_remote_receive_callback), __EXIT);

    /* register user remote */
    TUYA_CALL_ERR_GOTO(tuya_ble_reg_raw_scan_adv_cb(__user_remote_receive_callback), __EXIT);

    /* tuya remote bind notify */
    TUYA_CALL_ERR_GOTO(tuya_ble_reg_app_scan_adv_handle_cbs(&ble_scan_cfg), __EXIT);

    // 初始化绑定状态
    sg_in_pairing_mode = TRUE;

    // 打开涂鸦的绑定窗口（确保能接收到蓝牙数据）
    tuya_ble_open_bind_window();
    // 设置超时值为最大值，确保窗口一直开启
    tuya_ble_set_bind_window(0xFFFFFFFF);

    const THREAD_CFG_T thread_cfg = {
        .thrdname = "example_task",
        .stackDepth = 4*1024,
        .priority = THREAD_PRIO_4,
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(&example_thrd_hdl, NULL, NULL, example_task, NULL, &thread_cfg));
    p_all_obj_dp = (TY_OBJ_DP_S *)tal_malloc(8 * SIZEOF(TY_OBJ_DP_S));
    if (NULL == p_all_obj_dp)
    {
        return;
    }

    for (UINT8_T i = 0; i < 5; i++)
    {
        if (sg_demo_info.sg_remote_whitelist[i] > 0 &&sg_demo_info.sg_remote_whitelist[i] < 0xffffffff)
            sg_whitelist_count += 1;
        // tkl_log_output("sg_whitelist_count = %d\r\n",sg_whitelist_count);
        // tkl_log_output("remote_id = 0x%08X,group_id =%d,sg_whitelist_count = %d\r\n", (uint32_t)sg_demo_info.sg_remote_whitelist[i], sg_demo_info.remote_group[i], sg_whitelist_count);
    }
__EXIT:
    return;
}

#endif /* ENABLE_BT_REMOTE_CTRL */
