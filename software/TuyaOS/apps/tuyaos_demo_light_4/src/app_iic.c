/**
 * @file ltr_x1303.c
 * @brief LTR-X1303 手扫传感器驱动（完整版）
 * @version 2.1
 * @date 2026-02-24
 */

#include "tuya_iot_config.h"
#include "tkl_gpio.h"
#include "tdd_sw_i2c.h"
#include "app_iic.h"
#include "dp_process.h"
#include "tal_thread.h"
#include "tal_log.h"
#include "app_pwm.h"
#include "tdd_sw_i2c.h"
/***********************************************************
*********************** 硬件配置 ***************************
***********************************************************/
typedef enum {
    GS_IDLE = 0,
    GS_HAND_IN,
    GS_HAND_OUT,
    GS_HOLD
} gesture_state_t;

/* I2C引脚 */
#define LTRX1303_SCL_PIN TUYA_GPIO_NUM_15
#define LTRX1303_SDA_PIN TUYA_GPIO_NUM_17
#define LTRX1303_SLAVE_ADDR 0x53
#define I2C_NUM_ID SW_I2C_PORT_NUM_0

/***********************************************************
*********************** 参数配置 ***************************
***********************************************************/
#define GESTURE_MAX_TIME_MS 1000 /* 挥手最大时间：1秒 */
#define HOLD_DETECT_TIME_MS 2000 /* 悬停判定时间：2秒 */
#define FULL_SCALE_TIME_MS 4500  /* 无极调光时间：4.5秒 */
#define MIN_BRIGHTNESS 1         /* 最小亮度：1% */
#define MAX_BRIGHTNESS 100       /* 最大亮度：100% */
#define BEEP_DURATION_MS 150     /* 蜂鸣器持续时间：100ms */
#define GESTURE_DEBOUNCE_MS 500  /* 手扫最小间隔：500ms（防抖） */
#define GESTURE_BOOT_GUARD_MS 5000   /* 上电后禁用手势，避免PWM/电源扰动误触发 */
#define GESTURE_IDLE_CONFIRM_COUNT 3 /* 连续N次超阈值才认定手进入 */
#define GESTURE_THRESHOLD_MARGIN 20  /* 阈值 = 基线 + margin */

/***********************************************************
*********************** 寄存器地址 *************************
***********************************************************/
#define REG_MAIN_CTRL 0x00
#define REG_PS_LED 0x01
#define REG_PS_PULSES 0x02
#define REG_PS_MEAS_RATE 0x03
#define APS_RW_ALS_MEAS_RATE 0x04
#define APS_RW_ALS_GAIN			0x05
#define REG_MAIN_STATUS 0x07
#define REG_PS_DATA_0 0x08
#define REG_PS_DATA_1 0x09

/* 位定义 */
#define MAIN_CTRL_PS_EN (1 << 0)
#define MAIN_CTRL_SW_RESET (1 << 7)

/***********************************************************
*********************** 全局变量 ***************************
***********************************************************/
/* 传感器状态 */
static uint16_t g_baseline = 100;
static uint16_t g_threshold = 120;

/* 手势状态机 */
static uint32_t g_detect_start_time = 0;
static uint32_t g_hold_start_time = 0;

/* 调光方向管理（根据图片要求：若第一次为无极调暗，则第二次为无极调亮） */
static uint8_t g_last_adjust_dir = 1; /* 0:未知, 1:调暗, 2:调亮 */
static BOOL_T g_reach_limit = FALSE;  /* 是否已达到亮度极限 */

/* 双灯关灯时间记录（根据图片要求：5秒内同时关灯则同时开灯） */
uint32_t g_main_off_time = 0;
uint32_t g_aux_off_time = 0;
static BOOL_T g_main_was_on = FALSE;
static BOOL_T g_aux_was_on = FALSE;

/* 线程句柄 */
STATIC THREAD_HANDLE example_thrd_hdl = NULL;
STATIC THREAD_HANDLE example_thrd_hdl1 = NULL;
static BOOL_T g_hold_pwm_started = FALSE; /* 悬停渐变是否已启动 */

/* 手势防抖 */
static uint32_t g_last_gesture_time = 0; /* 上次手势执行时间 */

static gesture_state_t g_gesture_state = GS_IDLE;
static uint32_t g_hand_in_time = 0;
static uint32_t g_hand_out_time = 0;
static uint16_t g_ps_peak = 0;
static uint32_t g_gesture_enable_time = 0;
static uint8_t g_idle_above_cnt = 0;
static BOOL_T g_post_boot_baseline_done = FALSE;
/***********************************************************
*********************** I2C函数 ****************************
***********************************************************/
static OPERATE_RET i2c_write(uint8_t reg, uint8_t data)
{
    SW_I2C_GPIO_T gpio = {.scl = LTRX1303_SCL_PIN, .sda = LTRX1303_SDA_PIN};
    tdd_sw_i2c_init(I2C_NUM_ID, gpio);

    SW_I2C_MSG_T msg;
    uint8_t tx_buf[2] = {reg, data};
    msg.addr = LTRX1303_SLAVE_ADDR;
    msg.flags = SW_I2C_FLAG_WR;
    msg.buff = tx_buf;
    msg.len = 2;

    return tdd_sw_i2c_xfer(I2C_NUM_ID, &msg);
}

static OPERATE_RET i2c_read_bytes(uint8_t reg, uint8_t *buf, uint8_t len)
{
    OPERATE_RET rt;
    SW_I2C_GPIO_T gpio = {.scl = LTRX1303_SCL_PIN, .sda = LTRX1303_SDA_PIN};
    tdd_sw_i2c_init(I2C_NUM_ID, gpio);

    SW_I2C_MSG_T msg;

    msg.addr = LTRX1303_SLAVE_ADDR;
    msg.flags = SW_I2C_FLAG_WR;
    msg.buff = &reg;
    msg.len = 1;

    tdd_sw_i2c_xfer(I2C_NUM_ID, &msg);

    msg.addr = LTRX1303_SLAVE_ADDR;
    msg.flags = SW_I2C_FLAG_RD;
    msg.buff = buf;
    msg.len = len;

    return tdd_sw_i2c_xfer(I2C_NUM_ID, &msg);
}

static uint16_t read_ps_value(void)
{
    uint8_t buf[2] = {0};
    if (i2c_read_bytes(REG_PS_DATA_0, buf, 2) == OPRT_OK)
    {
        return ((uint16_t)(buf[1] & 0x03) << 8) | buf[0];
    }
    return 0;
}

/* 更新PS基线与阈值（上电稳定后需重新采样） */
static void update_ps_baseline(void)
{
    uint32_t sum = 0;

    for (int i = 0; i < 5; i++)
    {
        sum += read_ps_value();
        tal_system_sleep(50);
    }
    g_baseline = sum / 5;
    g_threshold = g_baseline + GESTURE_THRESHOLD_MARGIN;
}

/***********************************************************
*********************** 蜂鸣器功能 *************************
***********************************************************/

/* 蜂鸣器响一声 */
static void beep_once(void)
{
    if (sg_demo_info.beep_switch == 1)
    {
        tkl_pwm_duty_set(BEEP_PWM, 5000); /* 开始响 */
        tkl_pwm_start(BEEP_PWM);
        // tkl_gpio_write(BEEP_PIN, TUYA_GPIO_LEVEL_HIGH);
        tal_system_sleep(BEEP_DURATION_MS); /* 持续100ms */
        // tkl_gpio_write(BEEP_PIN, TUYA_GPIO_LEVEL_LOW);
        // tkl_pwm_duty_set(BEEP_PWM, 0); /* 停止 */
        tkl_pwm_stop(BEEP_PWM);
        // TAL_PR_NOTICE("beep_once\r\n");
    }
}

/***********************************************************
*********************** 传感器初始化 ***********************
***********************************************************/
static void sensor_init(void)
{
    uint8_t data = 0;
    data |= 1<<7;
    data |= (1<<4);
    /* 软件复位 */
    i2c_write(REG_MAIN_CTRL, data);
    tal_system_sleep(10);

    /* 配置PS LED：100mA, 60kHz */
    i2c_write(REG_PS_LED, 0x37);

    /* PS 脉冲数：8个 */
    i2c_write(REG_PS_PULSES, 0x18);

    /* PS 测量速率：25ms */
    // i2c_write(REG_PS_MEAS_RATE, 0x43);
    i2c_write(REG_PS_MEAS_RATE, 0x5c);

    i2c_write(APS_RW_ALS_MEAS_RATE, 0x22);

    i2c_write(APS_RW_ALS_GAIN, 0x01);
    
    /* 启用PS传感器 */
    data = 0;
    data |= 1<<0;
    data |= (1<<1);
    i2c_write(REG_MAIN_CTRL, data);

    tal_system_sleep(30);

    update_ps_baseline();
}

/***********************************************************
*********************** PWM辅助函数 ************************
***********************************************************/
/* 设置灯光亮度（无极调光） */
static void set_light_brightness() //
{
    extern DEMO_INFO_T sg_demo_info;
    uint8_t target_brightness, target_brightness1;
    /* 限制亮度范围 */
    // if (target_brightness < MIN_BRIGHTNESS) target_brightness = MIN_BRIGHTNESS;
    // if (target_brightness > MAX_BRIGHTNESS) target_brightness = MAX_BRIGHTNESS;

    switch (sg_demo_info.last_light_memory)
    {
    case 2:
    {
        sg_demo_info.white_switch = 1;
        sg_demo_info.switch_status = 1;
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_bool_status(DPID_SWITCH, 1);
        // sg_demo_info.white_bright = target_brightness;
        target_brightness = sg_demo_info.white_bright;
        /* 更新主灯PWM */
        uint16_t white_bright1 = 5 + (target_brightness - 1) * 95 / 99;
        uint8_t white_temp1 = sg_demo_info.white_temp;
        if (white_temp1 == 0)
            white_temp1 = 1;
        uint16_t ww = white_bright1 * white_temp1;
        uint16_t cw = white_bright1 * 100 - ww;

        // if (ww >= 9999)
        // {
        //     ww = 9999;
        //     cw = 1;
        // }
        // if (cw >= 9999)
        // {
        //     cw = 9999;
        //     ww = 1;
        // }

        TUYA_PWM_NUM_E channels[] = {BRIGHT_PWM, TEMP_PWM};
        UINT32_T duties[] = {ww, cw};
        pwm_gradual_duty_set_multi(2, channels, duties);

        /* 上报亮度 */
        // upload_device_value_status(DPID_WHITE_BRIGHT, target_brightness);
        // TAL_PR_DEBUG("Main light brightness: %d%%", target_brightness);
    }
    break;

    case 3:
    {
        sg_demo_info.aux_switch = 1;
        sg_demo_info.switch_status = 1;
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_bool_status(DPID_SWITCH, 1);
        // sg_demo_info.aux_bright = target_brightness;
        target_brightness1 = sg_demo_info.aux_bright;
        /* 更新辅灯PWM */
        uint16_t aux_bright1 = 5 + (target_brightness1 - 1) * 95 / 99;
        uint8_t aux_temp1 = sg_demo_info.white_temp;
        if (aux_temp1 == 0)
            aux_temp1 = 1;
        uint16_t aux_ww = aux_bright1 * aux_temp1;
        uint16_t aux_cw = aux_bright1 * 100 - aux_ww;

        // if (aux_ww >= 9999)
        // {
        //     aux_ww = 9999;
        //     aux_cw = 1;
        // }
        // if (aux_cw >= 9999)
        // {
        //     aux_cw = 9999;
        //     aux_ww = 1;
        // }

        TUYA_PWM_NUM_E channels[] = {AUX_BRIGHT_PWM, AUX_TEMP_PWM};
        UINT32_T duties[] = {aux_ww, aux_cw};
        pwm_gradual_duty_set_multi(2, channels, duties);

        /* 上报亮度 */
        // upload_device_value_status(DPID_AUX_BRIGHT_VALUE, target_brightness1);
        // TAL_PR_DEBUG("Aux light brightness: %d%%", target_brightness1);
    }
    break;

    case 1:
    {
        /* 同时设置主灯和辅灯 */
        sg_demo_info.aux_switch = 1;
        sg_demo_info.white_switch = 1;
        sg_demo_info.switch_status = 1;
        upload_device_bool_status(LIGHT_SWITCH, 1);
        upload_device_bool_status(AUX_SWITCH, 1);
        upload_device_bool_status(DPID_SWITCH, 1);

        /* 更新所有灯PWM */
        TUYA_PWM_NUM_E all_channels[4];
        UINT32_T all_duties[4];
        UINT8_T channel_count = 0;

        uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
        uint8_t white_temp1 = sg_demo_info.white_temp;
        if (white_temp1 == 0)
            white_temp1 = 1;
        uint16_t ww = white_bright1 * white_temp1;
        uint16_t cw = white_bright1 * 100 - ww;

        // if (ww >= 9999)
        // {
        //     ww = 9999;
        //     cw = 1;
        // }
        // if (cw >= 9999)
        // {
        //     cw = 9999;
        //     ww = 1;
        // }

        all_channels[channel_count] = BRIGHT_PWM;
        all_duties[channel_count++] = ww;
        all_channels[channel_count] = TEMP_PWM;
        all_duties[channel_count++] = cw;

        uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
        uint8_t aux_temp1 = sg_demo_info.white_temp;
        if (aux_temp1 == 0)
            aux_temp1 = 1;
        uint16_t aux_ww = aux_bright1 * aux_temp1;
        uint16_t aux_cw = aux_bright1 * 100 - aux_ww;

        // if (aux_ww >= 9999)
        // {
        //     aux_ww = 9999;
        //     aux_cw = 1;
        // }
        // if (aux_cw >= 9999)
        // {
        //     aux_cw = 9999;
        //     aux_ww = 1;
        // }

        all_channels[channel_count] = AUX_BRIGHT_PWM;
        all_duties[channel_count++] = aux_ww;
        all_channels[channel_count] = AUX_TEMP_PWM;
        all_duties[channel_count++] = aux_cw;

        if (channel_count > 0)
        {
            pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
        }

        /* 上报亮度 */
        upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
        // TAL_PR_NOTICE("All lights brightness: %d%%,%d%%", sg_demo_info.white_bright, sg_demo_info.aux_bright);
    }
    break;
    }
}

/* 上报当前亮度状态 */
static void upload_current_brightness_status(void)
{
    extern DEMO_INFO_T sg_demo_info;
    UINT16_T ww_pwm = 0, cw_pwm = 0, aux_ww_pwm = 0, aux_cw_pwm = 0;
    if (sg_demo_info.white_switch == 1)
    {
        ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
        cw_pwm = __get_current_pwm_duty(TEMP_PWM);

        if (sg_demo_info.white_temp == 0)
        {
            UINT16_T brightness_value = cw_pwm / 100;
            sg_demo_info.white_bright = __convert_brightness_value_to_percent(brightness_value);
        }
        else
        {
            // 正常计算：亮度值 = WW_PWM / 色温值
            UINT16_T brightness_value = ww_pwm / sg_demo_info.white_temp;
            sg_demo_info.white_bright = __convert_brightness_value_to_percent(brightness_value);
        }
        upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
    }
    if (sg_demo_info.aux_switch == 1)
    {
        aux_ww_pwm = __get_current_pwm_duty(AUX_BRIGHT_PWM);
        aux_cw_pwm = __get_current_pwm_duty(AUX_TEMP_PWM);

        if (sg_demo_info.white_temp == 0)
        {
            UINT16_T brightness_value = aux_cw_pwm / 100;
            sg_demo_info.aux_bright = __convert_brightness_value_to_percent(brightness_value);
        }
        else
        {
            // 正常计算：亮度值 = WW_PWM / 色温值
            UINT16_T brightness_value = aux_ww_pwm / sg_demo_info.white_temp;
            sg_demo_info.aux_bright = __convert_brightness_value_to_percent(brightness_value);
        }

        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
    }
}

/* 辅助函数：实时读取PWM并计算亮度 */
static uint8_t get_current_main_brightness(void)
{
    UINT16_T ww_pwm = 0, cw_pwm = 0;
    ww_pwm = __get_current_pwm_duty(BRIGHT_PWM);
    cw_pwm = __get_current_pwm_duty(TEMP_PWM);
    
    if (sg_demo_info.white_temp == 0)
    {
        UINT16_T brightness_value = cw_pwm / 100;
        return __convert_brightness_value_to_percent(brightness_value);
    }
    else
    {
        UINT16_T brightness_value = ww_pwm / sg_demo_info.white_temp;
        return __convert_brightness_value_to_percent(brightness_value);
    }
}

static uint8_t get_current_aux_brightness(void)
{
    UINT16_T aux_ww_pwm = 0, aux_cw_pwm = 0;
    aux_ww_pwm = __get_current_pwm_duty(AUX_BRIGHT_PWM);
    aux_cw_pwm = __get_current_pwm_duty(AUX_TEMP_PWM);
    
    if (sg_demo_info.white_temp == 0)
    {
        UINT16_T brightness_value = aux_cw_pwm / 100;
        return __convert_brightness_value_to_percent(brightness_value);
    }
    else
    {
        UINT16_T brightness_value = aux_ww_pwm / sg_demo_info.white_temp;
        return __convert_brightness_value_to_percent(brightness_value);
    }
}

static void handle_gesture(uint16_t ps_val)
{
    extern DEMO_INFO_T sg_demo_info;
    uint32_t current_time = tal_system_get_millisecond();

    /* 上电保护：灯刚亮/PWM未稳定时不处理手势 */
    if (current_time < g_gesture_enable_time)
    {
        g_gesture_state = GS_IDLE;
        g_idle_above_cnt = 0;
        return;
    }

    /* 保护期结束后重新采样基线，避免开灯瞬间污染阈值 */
    if (!g_post_boot_baseline_done)
    {
        update_ps_baseline();
        g_post_boot_baseline_done = TRUE;
        g_gesture_state = GS_IDLE;
        g_idle_above_cnt = 0;
        g_last_gesture_time = current_time;
        tkl_log_output("gesture ready: baseline=%d threshold=%d\r\n", g_baseline, g_threshold);
        return;
    }

    static uint8_t main_start_brightness = 0;
    static uint8_t aux_start_brightness = 0;
    static uint8_t main_target_brightness = 0;
    static uint8_t aux_target_brightness = 0;
    static uint8_t adjusting_light = 0;
    extern uint16_t gradual_time_ms;

    switch (g_gesture_state)
    {
    /* ==================================================
     * IDLE：等待手进入
     * ================================================== */
    case GS_IDLE:
        if (ps_val > g_threshold)
        {
            if (g_idle_above_cnt < GESTURE_IDLE_CONFIRM_COUNT)
            {
                g_idle_above_cnt++;
            }
            if (g_idle_above_cnt >= GESTURE_IDLE_CONFIRM_COUNT)
            {
                g_idle_above_cnt = 0;
                g_hand_in_time = current_time;
                g_gesture_state = GS_HAND_IN;
            }
        }
        else
        {
            g_idle_above_cnt = 0;
        }
        break;

    /* ==================================================
     * HAND_IN：手在位，判断挥手 / 悬停
     * ================================================== */
    case GS_HAND_IN:
    {
        tkl_log_output("hand_in ps value = %d, threshold = %d, cal value = %d\r\n",ps_val,g_threshold,g_baseline);
        uint32_t hold_time = current_time - g_hand_in_time;

        // if (ps_val > g_ps_peak)
        //     g_ps_peak = ps_val;

        /* ---------- 悬停触发 ---------- */
        if (hold_time > HOLD_DETECT_TIME_MS)
        {
            if (sg_demo_info.white_switch == 0 &&
                sg_demo_info.aux_switch == 0)
            {
                g_gesture_state = GS_IDLE;
                break;
            }

            g_gesture_state = GS_HOLD;
            g_hold_start_time = current_time;
            g_reach_limit = FALSE;
            g_hold_pwm_started = FALSE;

            if (g_last_adjust_dir == 1)
                g_last_adjust_dir = 2;
            else
                g_last_adjust_dir = 1;

            adjusting_light = 0;
            if (sg_demo_info.white_switch) adjusting_light |= 0x01;
            if (sg_demo_info.aux_switch)  adjusting_light |= 0x02;

            main_start_brightness = sg_demo_info.white_bright;
            aux_start_brightness  = sg_demo_info.aux_bright;

            if (g_last_adjust_dir == 1)
            {
                main_target_brightness = MIN_BRIGHTNESS;
                aux_target_brightness  = MIN_BRIGHTNESS;
            }
            else
            {
                main_target_brightness = MAX_BRIGHTNESS;
                aux_target_brightness  = MAX_BRIGHTNESS;
            }

            TUYA_PWM_NUM_E channels[4];
            UINT32_T duties[4];
            UINT8_T channel_count = 0;

            /* 主灯目标PWM */
            if (adjusting_light & 0x01)
            {
                uint16_t white_bright1 = 5 + (main_target_brightness - 1) * 95 / 99;
                uint8_t white_temp1 = sg_demo_info.white_temp;
                if (white_temp1 == 0)
                    white_temp1 = 1;

                uint16_t ww = white_bright1 * white_temp1;
                uint16_t cw = white_bright1 * 100 - ww;

                // if (ww >= 9999)
                // {
                //     ww = 9999;
                //     cw = 1;
                // }
                // if (cw >= 9999)
                // {
                //     cw = 9999;
                //     ww = 1;
                // }

                channels[channel_count] = BRIGHT_PWM;
                duties[channel_count++] = ww;
                channels[channel_count] = TEMP_PWM;
                duties[channel_count++] = cw;
            }

            /* 辅灯目标PWM */
            if (adjusting_light & 0x02)
            {
                uint16_t aux_bright1 = 5 + (aux_target_brightness - 1) * 95 / 99;
                uint8_t aux_temp1 = sg_demo_info.white_temp;
                if (aux_temp1 == 0)
                    aux_temp1 = 1;

                uint16_t aux_ww = aux_bright1 * aux_temp1;
                uint16_t aux_cw = aux_bright1 * 100 - aux_ww;

                // if (aux_ww >= 9999)
                // {
                //     aux_ww = 9999;
                //     aux_cw = 1;
                // }
                // if (aux_cw >= 9999)
                // {
                //     aux_cw = 9999;
                //     aux_ww = 1;
                // }

                channels[channel_count] = AUX_BRIGHT_PWM;
                duties[channel_count++] = aux_ww;
                channels[channel_count] = AUX_TEMP_PWM;
                duties[channel_count++] = aux_cw;
            }

            /* 启动一次 FULL_SCALE_TIME_MS 渐变 */
            gradual_time_ms = FULL_SCALE_TIME_MS;
            if (channel_count > 0)
            {
                pwm_gradual_duty_set_multi(channel_count, channels, duties);
            }

            g_hold_pwm_started = TRUE;
            // TAL_PR_NOTICE("jianbian\r\n");
            break;
        }

        /* ---------- 手还在 ---------- */
        if (ps_val > g_threshold)
            break;

        /* ---------- 手离开 ---------- */
        g_hand_out_time = current_time;
        g_gesture_state = GS_HAND_OUT;
    }
    break;

    /* ==================================================
     * HAND_OUT：挥手确认
     * ================================================== */
    case GS_HAND_OUT:
    {
        uint32_t out_stable = current_time - g_hand_out_time;
        uint32_t duration   = current_time - g_hand_in_time;

        if (out_stable < 40)
            break;

        if (duration >= 80 && duration <= GESTURE_MAX_TIME_MS)
        {
            if ((current_time - g_last_gesture_time) < GESTURE_DEBOUNCE_MS)
            {
                g_gesture_state = GS_IDLE;
                break;
            }
            tkl_log_output("hand_out ps value = %d, threshold = %d, cal value = %d\r\n",ps_val,g_threshold,g_baseline);
            g_last_gesture_time = current_time;
            beep_once();

            if (sg_demo_info.white_switch ||
                sg_demo_info.aux_switch)
            {
                if (sg_demo_info.white_switch &&
                    sg_demo_info.aux_switch)
                    sg_demo_info.last_light_memory = 1;
                else if (sg_demo_info.white_switch)
                    sg_demo_info.last_light_memory = 2;
                else
                    sg_demo_info.last_light_memory = 3;

                sg_demo_info.white_switch = 0;
                sg_demo_info.aux_switch = 0;
                sg_demo_info.switch_status = 0;

                gradual_time_ms = 1600;
                TUYA_PWM_NUM_E ch[4] = {
                    BRIGHT_PWM, TEMP_PWM,
                    AUX_BRIGHT_PWM, AUX_TEMP_PWM
                };
                UINT32_T d[4] = {0};
                pwm_gradual_duty_set_multi(4, ch, d);

                upload_device_bool_status(LIGHT_SWITCH, 0);
                upload_device_bool_status(AUX_SWITCH, 0);
                upload_device_bool_status(DPID_SWITCH, 0);
            }
            else
            {
                if (sg_demo_info.last_light_memory == 1)
                {
                    sg_demo_info.white_switch = 1;
                    sg_demo_info.aux_switch = 1;
                }
                else if (sg_demo_info.last_light_memory == 2)
                    sg_demo_info.white_switch = 1;
                else if (sg_demo_info.last_light_memory == 3)
                    sg_demo_info.aux_switch = 1;

                sg_demo_info.switch_status = 1;
                upload_device_bool_status(DPID_SWITCH, 1);
                set_light_brightness();
            }
        }

        g_gesture_state = GS_IDLE;
    }
    break;

    /* ==================================================
     * HOLD：手悬停中
     * ================================================== */
    case GS_HOLD:
        if (ps_val > g_threshold)
        {
            uint8_t mb = get_current_main_brightness();
            uint8_t ab = get_current_aux_brightness();

            if (!g_reach_limit)
            {
                BOOL_T beep_flag = FALSE;

                if (g_last_adjust_dir == 1)
                {
                    if ((adjusting_light & 0x01) && mb == MIN_BRIGHTNESS)
                        beep_flag = TRUE;
                    if ((adjusting_light & 0x02) && ab == MIN_BRIGHTNESS)
                        beep_flag = TRUE;
                }
                else
                {
                    if ((adjusting_light & 0x01) && mb == MAX_BRIGHTNESS)
                        beep_flag = TRUE;
                    if ((adjusting_light & 0x02) && ab == MAX_BRIGHTNESS)
                        beep_flag = TRUE;
                }

                if (beep_flag)
                {
                    tkl_log_output("GS_HOLD ps value = %d, threshold = %d, cal value = %d\r\n",ps_val,g_threshold,g_baseline);
                    beep_once();
                    g_reach_limit = TRUE;
                }
            }
        }
        else
        {
            g_gesture_state = GS_IDLE;
            g_hold_pwm_started = FALSE;
            pwm_gradual_stop_all();
            upload_current_brightness_status();
        }
        break;
    }
}
/***********************************************************
*********************** 主轮询函数 *************************
***********************************************************/
STATIC VOID ltrx1303_poll_task(PVOID_T args)
{
    for (;;)
    {
        /* 读取PS值并处理手势 */
        if (sg_demo_info.hand_sweep_switch == 1)
        {
            uint16_t ps_val = read_ps_value();
            handle_gesture(ps_val);
        }

        tal_system_sleep(50);
    }
    return;
}

STATIC VOID ltrx1303_poll_task1(PVOID_T args)
{
    uint8_t buf[1] = {0};
    uint8_t buf1[1] = {0};
    uint8_t buf2[1] = {0};
    uint8_t buf3[1] = {0};
    for (;;)
    {
        i2c_read_bytes(0x00, buf, 1);
        i2c_read_bytes(0x01, buf1, 1);
        i2c_read_bytes(0x02, buf2, 1);
        i2c_read_bytes(0x03, buf3, 1);
        if((buf1[0] != 0x37)||(buf2[0] != 0x18)||(buf3[0] != 0x5c)||(buf[0] != 3)){
            sensor_init();
            tkl_log_output("sensor_init\r\n");
        }
        tal_system_sleep(3000);
    }
    return;
}

/***********************************************************
*********************** 外部接口 ***************************
***********************************************************/
VOID ltrx1303_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    g_gesture_state = GS_IDLE;
    g_idle_above_cnt = 0;
    g_post_boot_baseline_done = FALSE;
    g_last_gesture_time = 0;
    g_gesture_enable_time = tal_system_get_millisecond() + GESTURE_BOOT_GUARD_MS;

    /* 3. 初始化I2C */
    SW_I2C_GPIO_T gpio = {
        .scl = LTRX1303_SCL_PIN,
        .sda = LTRX1303_SDA_PIN};
    tdd_sw_i2c_init(I2C_NUM_ID, gpio);

    /* 4. 初始化传感器 */
    sensor_init();

    /* 6. 初始化上报状态 */
    // g_last_reported_brightness = 0;
    // g_last_reported_state = FALSE;

    // TAL_PR_NOTICE("LTR-X1303 system initialized successfully");

    const THREAD_CFG_T thread_cfg = {
        .thrdname = "ltrx1303_poll_task",
        .stackDepth = 2 * 1024,
        .priority = THREAD_PRIO_2,
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(&example_thrd_hdl, NULL, NULL, ltrx1303_poll_task, NULL, &thread_cfg));

    const THREAD_CFG_T thread_cfg1 = {
        .thrdname = "ltrx1303_task",
        .stackDepth = 2 * 1024,
        .priority = THREAD_PRIO_2,
    };
    TUYA_CALL_ERR_LOG(tal_thread_create_and_start(&example_thrd_hdl1, NULL, NULL, ltrx1303_poll_task1, NULL, &thread_cfg1));

    return;
}
