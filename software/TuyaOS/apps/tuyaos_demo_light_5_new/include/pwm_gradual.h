#ifndef __PWM_GRADUAL_H__
#define __PWM_GRADUAL_H__

#include "app_pwm.h"

#ifdef __cplusplus
extern "C" {
#endif

// 渐变请求结构
typedef struct {
    TUYA_PWM_NUM_E channels[5];      // 最多5个通道
    UINT32_T target_values[5];       // 对应目标值
    UINT8_T channel_count;           // 通道数量 (1-5)
} GRADUAL_REQUEST_T;

// 通道控制结构
typedef struct {
    BOOL_T is_initialized;           // 是否已初始化
    UINT32_T current_value;          // 当前PWM值
} PWM_CHANNEL_STATE_T;

// 全局控制结构
typedef struct {
    // 当前渐变状态
    BOOL_T is_active;                // 是否有渐变进行中
    UINT32_T start_time;             // 渐变开始时间
    UINT32_T gradual_time;           // 渐变总时间
    UINT32_T start_values[5];        // 起始值
    UINT32_T target_values[5];       // 目标值
    TUYA_PWM_NUM_E channels[5];      // 通道列表
    UINT8_T channel_count;           // 通道数量
    
    // 队列
    GRADUAL_REQUEST_T queue[40];     // 请求队列
    UINT8_T queue_head;              // 队列头
    UINT8_T queue_tail;              // 队列尾
    UINT8_T queue_count;             // 队列中元素数量
    UINT8_T curve_type;  // 新增：0=默认(亮度曲线)，1=色温曲线
} GLOBAL_GRADUAL_CTRL_T;


// 在结构体定义中增加曲线类型标识
// typedef struct {
//     BOOL_T is_active;
//     UINT8_T channel_count;
//     TUYA_PWM_NUM_E channels[MAX_CHANNELS_PER_REQUEST];
//     UINT32_T start_values[MAX_CHANNELS_PER_REQUEST];
//     UINT32_T target_values[MAX_CHANNELS_PER_REQUEST];
//     UINT32_T start_time;
//     UINT32_T gradual_time;
//     UINT8_T curve_type;  // 新增：0=默认(亮度曲线)，1=色温曲线
// } GLOBAL_GRADUAL_CTRL_T;
// 通道数量定义
#define PWM_CH_MAX 6
#define QUEUE_SIZE 40
#define MAX_CHANNELS_PER_REQUEST 5

// 函数声明
OPERATE_RET pwm_gradual_init(VOID_T);
// 在 pwm_gradual.h 中添加函数声明
OPERATE_RET pwm_gradual_duty_set(TUYA_PWM_NUM_E ch, UINT32_T duty);
OPERATE_RET pwm_gradual_duty_set_multi(UINT8_T channel_count, TUYA_PWM_NUM_E channels[], UINT32_T duties[]);
OPERATE_RET pwm_gradual_duty_set_immediate(TUYA_PWM_NUM_E ch_id, UINT32_T duty);
OPERATE_RET pwm_gradual_stop_all(VOID_T);
OPERATE_RET pwm_gradual_stop_current(VOID_T);
/** 关掉部分通道到指定 duty；若其它通道正在渐变则保留并继续渐到原目标 */
OPERATE_RET pwm_gradual_close_channels_keep_rest(UINT8_T close_count, TUYA_PWM_NUM_E close_chs[], UINT32_T close_duties[]);
UINT32_T pwm_gradual_get_current_duty(TUYA_PWM_NUM_E ch_id);
BOOL_T pwm_gradual_is_idle(VOID_T);
BOOL_T pwm_gradual_is_channel_in_queue(TUYA_PWM_NUM_E ch_id);
UINT16_T pwm_get_current_duty(UINT8_T pwm_channel);

// 等待函数
OPERATE_RET pwm_gradual_wait_idle(UINT32_T timeout_ms);
OPERATE_RET pwm_gradual_wait_current_complete(UINT32_T timeout_ms);

// 队列管理
UINT8_T pwm_gradual_get_queue_count(VOID_T);
OPERATE_RET pwm_gradual_clear_queue(VOID_T);

// 辅助宏，用于单通道调用
#define PWM_GRADUAL_SINGLE(ch, duty) { \
    TUYA_PWM_NUM_E _ch = (ch); \
    UINT32_T _duty = (duty); \
    pwm_gradual_duty_set(1, &_ch, &_duty); \
}

#ifdef __cplusplus
}
#endif

#endif /* __PWM_GRADUAL_H__ */