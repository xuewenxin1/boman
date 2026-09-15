/**
 * @file kv_power_count.c
 * @brief 上电计数：KV 存储 + 阈值触发恢复出厂 + 写入 sg_demo_info.cnt
 */

#include "tuya_cloud_types.h"
#include "tuya_ws_db.h"
#include "tal_log.h"
#include "tal_sw_timer.h"
#include "power_count.h"
#include "dp_process.h" // 假设 sg_demo_info 在这里定义
#include "tal_thread.h"
#include "tkl_timer.h"
#include "tuya_iot_config.h"
#include "tuya_cloud_wifi_defs.h"
#include "tuya_iot_com_api.h"
#include "app_light_tm_rhythm.h"
STATIC THREAD_HANDLE sg_gpio_handle;
#define GPIO_IRQ_PIN             TUYA_GPIO_NUM_21

#define TASK_GPIO_PRIORITY      THREAD_PRIO_2
#define TASK_GPIO_SIZE          1024

STATIC TIMER_ID sg_rst_timer_id = NULL; // 超时清零定时器

// 断电检测相关变量
static BOOL_T sg_power_loss_detected = FALSE;
static UINT_T sg_power_loss_start_time = 0;
static BOOL_T sg_last_gpio_level = TUYA_GPIO_LEVEL_HIGH;
#define POWER_LOSS_DETECT_THRESHOLD_MS 50  // 50ms检测阈值
UINT_T sg_low_level_count = 0;
STATIC TIMER_ID timer_stamp = NULL; // 定时器句柄
STATIC TIMER_ID count_timer = NULL;

STATIC TIMER_ID count_timer_test = NULL;
// TIMER_ID ac_gpio_irq_id = NULL;
uint32_t count_irq = 0;
/**********************************************************
 * @brief 定时器回调（超时自动清零）
 **********************************************************/
STATIC VOID kv_rst_timeout_cb(VOID *arg)
{
    extern DEMO_INFO_T sg_demo_info;
    sg_demo_info.cnt = 0;
    sg_demo_info.cnt1 = 0;
    device_config_save1();
    // TAL_PR_NOTICE("free heap: %d", tal_system_get_free_heap_size());
    tal_sw_timer_stop(sg_rst_timer_id);
}
STATIC VOID countirq_cb(VOID *arg)
{
    count_irq ++;
}
/**********************************************************
 * @brief 上电计数器（返回是否达到阈值）
 * @return TRUE  达到阈值
 *         FALSE 未达到
 **********************************************************/

BOOL_T kv_power_on_count(VOID)
{
    extern DEMO_INFO_T sg_demo_info;
    return (sg_demo_info.cnt >= 9) ? TRUE : FALSE;
}

UINT32_T sg_fall_edge_time = 0;      // 下降沿时间戳
// // 定时器回调函数 - 定期检查GPIO状态
STATIC TIMER_ID sg_count1 = NULL; // 定时保存定时器
STATIC TIMER_ID sg_count2 = NULL; // 定时保存定时器
STATIC VOID power_check_timer_cb(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;
    tkl_log_output("duandian,cnt = %d,cnt1 = %d\r\n",sg_demo_info.cnt,sg_demo_info.cnt1);
    device_config_save1();
    if(sg_sleep_is_timing){
        if (sg_demo_info.sleep_init[0] == 0x01)
        {
            sg_demo_info.sleep_init[0] = 0x0;
            app_light_stop_sleep_timer();
            dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                                    sg_demo_info.sleep_init, 15, 5);
        }
    }
    if(sg_wake_is_timing){
        if (sg_demo_info.wake_init[0] == 0x01)
        {
            sg_demo_info.wake_init[0] = 0x0;
            app_light_stop_wake_timer();
            dev_report_dp_raw_sync(NULL, WAKEUP_MODE,
                                    sg_demo_info.wake_init, 11, 5);
        }
    }
    /* 中断脚检测到的短断电：打断今日节律；长断电冷启动不走此回调，保留上电恢复 */
    if (sg_demo_info.rhythm_switch) {
        app_light_stop_today_rhythm_timer();
    }
    tal_sw_timer_start(sg_rst_timer_id, 5000, TAL_TIMER_ONCE);
    app_pwm();
    __execute_power_loss_recovery();
}

STATIC VOID power_check_timer_cb1(TIMER_ID timer_id, VOID_T *arg)
{
    extern BOOL_T sg_sleep_is_timing;
    extern BOOL_T sg_wake_is_timing;
    extern DEMO_INFO_T sg_demo_info;
    tkl_log_output("duandian,cnt = %d,cnt1 = %d\r\n",sg_demo_info.cnt,sg_demo_info.cnt1);
    device_config_save1();
    if(sg_sleep_is_timing){
        if (sg_demo_info.sleep_init[0] == 0x01)
        {
            sg_demo_info.sleep_init[0] = 0x0;
            app_light_stop_sleep_timer();
            dev_report_dp_raw_sync(NULL, SLEEP_MODE,
                                    sg_demo_info.sleep_init, 15, 5);
        }
    }
    if(sg_wake_is_timing){
        if (sg_demo_info.wake_init[0] == 0x01)
        {
            sg_demo_info.wake_init[0] = 0x0;
            app_light_stop_wake_timer();
            dev_report_dp_raw_sync(NULL, WAKEUP_MODE,
                                    sg_demo_info.wake_init, 11, 5);
        }
    }
}

// 执行断电恢复逻辑
VOID __execute_power_loss_recovery(VOID_T)
{
    extern DEMO_INFO_T sg_demo_info;
    if (kv_power_on_count() == TRUE) {
        // 恢复出厂设置
        sg_demo_info.cnt = 0;
        sg_demo_info.cnt1 = 0;
        sg_demo_info.checksum = 0xa5;
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
        
        extern uint8_t custom_status[6];
        extern uint8_t night[6];
        extern uint8_t sleep_init[14];
        extern uint8_t wake_init[11];
        extern uint8_t switch_change_gear[21];
        extern uint8_t collect[7];
        extern uint8_t rhythm_sunlight1[66];
        
        memcpy(sg_demo_info.custom_status, custom_status, 6);
        memcpy(sg_demo_info.sleep_init, sleep_init, 14);
        memcpy(sg_demo_info.wake_init, wake_init, 11);
        memcpy(sg_demo_info.rhythm_sunlight, rhythm_sunlight1, 66);
        memcpy(sg_demo_info.switch_change_gear, switch_change_gear, 21);
        memcpy(sg_demo_info.night, night, 6);
        memcpy(sg_demo_info.collect, collect, 7);
        device_config_save1();
        tuya_iot_wf_gw_reset();
    }   
    
}

// GPIO中断回调函数（备用方案）
uint32_t sg_fall_edge_time1 =0;

STATIC VOID __gpio_irq_callback(TIMER_ID timer_id, VOID_T *arg)
{
    extern DEMO_INFO_T sg_demo_info;
    if((count_irq - sg_fall_edge_time > 50))
    {
        sg_demo_info.cnt++;
        sg_demo_info.cnt1++;
        TAL_PR_NOTICE("irq\r\n");
        tal_sw_timer_start(sg_count1, 100, TAL_TIMER_ONCE);
    }
    sg_fall_edge_time = count_irq;
}

VOID test_gpio(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    
    // 方法2：使用中断（备用）
    TUYA_GPIO_IRQ_T irq_cfg = {
        .cb = __gpio_irq_callback,
        .arg = NULL,
        .mode = TUYA_GPIO_IRQ_FALL,  // 双边沿触发
    };
    TUYA_CALL_ERR_LOG(tkl_gpio_irq_init(GPIO_IRQ_PIN, &irq_cfg));
    /*irq enable*/
    TUYA_CALL_ERR_LOG(tkl_gpio_irq_enable(GPIO_IRQ_PIN));
    // 方法1：使用定时器轮询（更可靠）
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(kv_rst_timeout_cb, NULL, &sg_rst_timer_id),__EXIT);
    tal_sw_timer_start(sg_rst_timer_id, 5000, TAL_TIMER_ONCE);
    
    tal_sw_timer_create(countirq_cb,NULL,&count_timer);
    tal_sw_timer_start(count_timer,1,TAL_TIMER_CYCLE);

    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb1, NULL, &sg_count2),__EXIT);
    tal_sw_timer_start(sg_count2, 100, TAL_TIMER_ONCE);
__EXIT:
    return;
}
VOID test_gpio1(VOID)
{
    OPERATE_RET rt = OPRT_OK;
    extern DEMO_INFO_T sg_demo_info;
    // 方法1：使用定时器轮询（更可靠）
    TUYA_CALL_ERR_GOTO(tal_sw_timer_create(power_check_timer_cb, NULL, &sg_count1),__EXIT);
    sg_demo_info.cnt++;
    sg_demo_info.cnt1++;
    app_pwm();
__EXIT:
    return;
}
