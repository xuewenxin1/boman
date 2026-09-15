#include "tuya_cloud_types.h"
#include "tuya_iot_com_api.h"
#include "tal_log.h"
#include "tuya_bt.h"
#include "tal_semaphore.h"
#include <string.h>
#include "calibration.h"
#include "app_pwm.h"
#include "dp_process.h"
#include "tkl_pwm.h"

/* 校准完成后直接打 50% 验证光；落盘前整理上电态（light_4：主+辅共 4 路，无夜灯 PWM） */
static void calibration_apply_verify_output(void)
{
    extern DEMO_INFO_T sg_demo_info;

    sg_demo_info.cnt1 = 0;
    sg_demo_info.switch_status = 1;
    sg_demo_info.white_switch = 1;
    sg_demo_info.aux_switch = 1;
    sg_demo_info.night_switch = 0;
    sg_demo_info.white_bright = 100;
    sg_demo_info.white_temp = 55;
    sg_demo_info.aux_bright = 100;
    sg_demo_info.last_light_memory = 1; /* 主+辅 */

    if (sg_demo_info.ww_calibration_coefficient == 0) {
        sg_demo_info.ww_calibration_coefficient = 10000;
    }
    if (sg_demo_info.cw_calibration_coefficient == 0) {
        sg_demo_info.cw_calibration_coefficient = 10000;
    }
    if (sg_demo_info.aux_ww_calibration_coefficient == 0) {
        sg_demo_info.aux_ww_calibration_coefficient = 10000;
    }
    if (sg_demo_info.aux_cw_calibration_coefficient == 0) {
        sg_demo_info.aux_cw_calibration_coefficient = 10000;
    }

    tkl_pwm_duty_set(BRIGHT_PWM, 5000);
    tkl_pwm_start(BRIGHT_PWM);
    tkl_pwm_duty_set(TEMP_PWM, 5000);
    tkl_pwm_start(TEMP_PWM);
    tkl_pwm_duty_set(AUX_BRIGHT_PWM, 5000);
    tkl_pwm_start(AUX_BRIGHT_PWM);
    tkl_pwm_duty_set(AUX_TEMP_PWM, 5000);
    tkl_pwm_start(AUX_TEMP_PWM);
}

// 阻塞式校准过程
void calibration_process(void) {
    extern DEMO_INFO_T sg_demo_info;
    
    // TAL_PR_NOTICE("=== 开始校准流程 ===\r\n");
    
    // 步骤2: 分别校准四路光源
    
    // 2.1 校准上光C
    // TAL_PR_NOTICE(">>> 校准上光C <<<\r\n");
    calibrate_single_channel(BRIGHT_PWM, &sg_demo_info.ww_calibration_coefficient, "上光C");
    
    // 2.2 校准上光W
    // TAL_PR_NOTICE(">>> 校准上光W <<<\r\n");
    calibrate_single_channel(TEMP_PWM, &sg_demo_info.cw_calibration_coefficient, "上光W");
    
    // 2.3 校准下光C
    // TAL_PR_NOTICE(">>> 校准下光C <<<\r\n");
    calibrate_single_channel(AUX_BRIGHT_PWM, &sg_demo_info.aux_ww_calibration_coefficient, "下光C");
    
    // 2.4 校准下光W
    // TAL_PR_NOTICE(">>> 校准下光W <<<\r\n");
    calibrate_single_channel(AUX_TEMP_PWM, 
                            &sg_demo_info.aux_cw_calibration_coefficient, 
                            "下光W");
    
    sg_demo_info.calibration = 3;  // 标记已校准
    calibration_apply_verify_output();
    device_config_backup_to_c();
    device_config_save1_force();  // 保存到flash保护区域
    
    // TAL_PR_NOTICE("=== 校准流程完成 ===\r\n");
}

// 单通道校准函数（阻塞式）
void calibrate_single_channel(TUYA_PWM_NUM_E ch, uint16_t *coeff_ptr, const char* channel_name) {
    TUYA_GPIO_LEVEL_E read_level = 0;
    uint16_t max_duty = 0;  // 记录最大占空比系数
    
    // TAL_PR_NOTICE("%s校准开始...\r\n", channel_name);
    
    // 步骤2.a: 输出86%占空比，维持200ms
    tkl_pwm_duty_set(ch, 8600);
    tkl_pwm_start(ch);
    tal_system_sleep(200);
    
    // 步骤2.b: 3秒内从86%线性增加到100%
    // TAL_PR_NOTICE("2. 3秒内从86%%线性增加到100%%\r\n");
    uint16_t duty = 8600;
    uint32_t start_time = tal_system_get_millisecond();
    uint32_t duration_ms = 3000;  // 3秒
    
    while (duty < 10000) {
        // 更新占空比
        tkl_pwm_duty_set(ch, duty);
        tkl_pwm_start(ch);
        
        // 步骤2.c: 同时检测IO19是否为低电平
        tkl_gpio_read(PIN_CALIB_DET, &read_level);
        if (read_level == 0) {
            // 检测到低电平，停止在当前占空比
            max_duty = duty;
            // // TAL_PR_NOTICE("检测到IO19低电平，记录系数: %d.%02d%%\r\n", duty/100, duty%100);
            break;
        }
        
        // 计算下一占空比（线性增加）
        uint32_t elapsed = tal_system_get_millisecond() - start_time;
        if (elapsed > duration_ms) elapsed = duration_ms;
        
        duty = 8600 + (1400 * elapsed) / duration_ms;
        
        if (duty > 10000) 
        {
            duty = 10000;
            break;
        }
        
        // 适当延时，避免CPU占用过高
        tal_system_sleep(10);
    }
    
    // 步骤2.c: 如果到100%都没有检测到IO19拉低
    if (max_duty == 0) {
        max_duty = 10000;  // 以100%作为最大占空比系数
        // // TAL_PR_NOTICE("未检测到IO19低电平，使用100%%作为系数\r\n");
    }
    
    // 保存系数
    *coeff_ptr = max_duty;
    
    // 关闭当前通道输出
    tkl_pwm_duty_set(ch, 0);
    tkl_pwm_start(ch);
    
    // // TAL_PR_NOTICE("%s校准完成，系数: %d.%02d%%\r\n", 
    //              channel_name, max_duty/100, max_duty%100);
    
    // 通道间延时1秒，避免相互干扰
    // // TAL_PR_NOTICE("等待1秒后进行下一路校准...\r\n\r\n");
    tal_system_sleep(1000);
}

// 系统初始化（阻塞式）
uint8_t system_init(void) {
    TUYA_GPIO_LEVEL_E read_level = 0;
    extern DEMO_INFO_T sg_demo_info;
    
    // // TAL_PR_NOTICE("=== 系统初始化 ===\r\n");
    // 初始化GPIO
    TUYA_GPIO_BASE_CFG_T in_pin_cfg = {
        .mode = TUYA_GPIO_PULLUP,
        .direct = TUYA_GPIO_INPUT,
    };
    
    tkl_gpio_init(PIN_CALIB_TRIG, &in_pin_cfg);
    tkl_gpio_init(PIN_CALIB_DET, &in_pin_cfg);
    
    // 步骤1: 模组上电检测IO18是否拉低
    tkl_gpio_read(PIN_CALIB_TRIG, &read_level);
    bool need_calibration = (read_level == 0);
    
    // // TAL_PR_NOTICE("检测IO18电平: %s\r\n", read_level == 0 ? "低电平" : "高电平");
    // // TAL_PR_NOTICE("需要校准: %s\r\n", need_calibration ? "是" : "否");
    
    if (need_calibration) {
        // 阻塞式校准，直到校准完成
        calibration_process();
        
        // 校准完成后延时100ms确保稳定
        tal_system_sleep(100);
        
        return 0;  // 返回0表示已校准
    }
    else {
        // 设置默认系数为100%
        if(sg_demo_info.calibration < 3){
            sg_demo_info.ww_calibration_coefficient = 10000;
            sg_demo_info.cw_calibration_coefficient = 10000;
            sg_demo_info.aux_ww_calibration_coefficient = 10000;
            sg_demo_info.aux_cw_calibration_coefficient = 10000;
            // sg_demo_info.calibration = 3;  // 标记已校准
            // device_config_save1();  // 保存到flash保护区域
            // sg_demo_info.calibration = 0;  // 标记为未校准
        }
        
        return 1;  // 返回1表示未校准
    }
}

// 获取校准状态
// bool is_device_calibrated(void) {
//     extern DEMO_INFO_T sg_demo_info;
//     return (sg_demo_info.calibration >= 3);
// }

// 应用校准系数到PWM输出
void apply_calibration_coefficient(TUYA_PWM_NUM_E ch, uint16_t duty_percentage) {
    extern DEMO_INFO_T sg_demo_info;
    uint16_t max_coeff = 10000;
    
    // 根据通道获取对应的最大系数
    switch(ch) {
        case BRIGHT_PWM:
            max_coeff = sg_demo_info.ww_calibration_coefficient;
            break;
        case TEMP_PWM:
            max_coeff = sg_demo_info.cw_calibration_coefficient;
            break;
        case AUX_BRIGHT_PWM:
            max_coeff = sg_demo_info.aux_ww_calibration_coefficient;
            break;
        case AUX_TEMP_PWM:
            max_coeff = sg_demo_info.aux_cw_calibration_coefficient;
            break;
        default:
            max_coeff = 10000;
    }
    
    // 计算实际占空比（考虑最大系数限制）
    uint16_t actual_duty = (duty_percentage * max_coeff) / 10000;
    
    // TAL_PR_NOTICE("通道%d: 请求%d%%, 系数%d.%02d%%, 实际%d.%02d%%\r\n",
    //              ch, duty_percentage/100, 
    //              max_coeff/100, max_coeff%100,
    //              actual_duty/100, actual_duty%100);
    
    // 设置PWM输出
    tkl_pwm_duty_set(ch, actual_duty);
    tkl_pwm_start(ch);
}