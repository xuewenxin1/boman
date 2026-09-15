#include "app_light_tm_rhythm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tal_thread.h"
#include "tal_log.h"
#include "tal_system.h"
#include <unistd.h>
#include "dp_process.h"
#include "app_light_tm_schedule.h"
#include "tal_time_service.h"
#include "app_pwm.h"
#include "pwm_gradual.h"
#include "light_pwm_mix.h"

RHYTHM_NODE_DATA_T *node;
TM_RHYTHM_INFO_T sg_rhythm_info;
STATIC THREAD_HANDLE rhythm_thread_id = NULL;
#define RHYTHM_THREAD_SLEEP 60 // 每分钟轮询

// 渐变状态结构体（修改：分别记录主灯和辅灯的状态）
typedef struct {
    bool active;                // 渐变是否激活
    bool is_power_on_recovery;  // 是否上电恢复渐变
    uint32_t start_time;        // 渐变开始时间
    uint32_t duration_ms;       // 渐变总时长
    
    // 主灯渐变参数
    uint8_t white_target_bright;   // 主灯目标亮度
    uint8_t white_target_temp;     // 主灯目标色温
    uint8_t white_start_bright;    // 主灯起始亮度
    uint8_t white_start_temp;      // 主灯起始色温
    
    // 辅灯渐变参数
    uint8_t aux_target_bright;     // 辅灯目标亮度
    uint8_t aux_target_temp;       // 辅灯目标色温
    uint8_t aux_start_bright;      // 辅灯起始亮度
    uint8_t aux_start_temp;        // 辅灯起始色温
    
    uint8_t update_interval;    // 更新间隔（秒）
    uint32_t last_update_time;  // 上次更新时间
} FADE_STATE_T;

FADE_STATE_T sg_fade_state = {0};

// 节律打断状态
bool sg_rhythm_interrupted = false;
int sg_interrupted_node = -1;

// 节点执行状态
static int sg_current_active_node = -1;
static int sg_last_processed_node = -1;

// 设备状态
static bool sg_device_just_booted = true;
static uint32_t sg_last_user_operation_time = 0;
static int sg_interrupt_clock_minutes = -1; // 打断时的本地钟点（分钟，0~1439）
static bool sg_user_interrupted_rhythm = false;
static bool sg_power_on_recovered = false;

/* 从 from 正向走到 to 的分钟数（跨天按 24h 环） */
static int rhythm_minutes_forward(int from, int to)
{
    int d = (to - from) % (24 * 60);
    if (d < 0) {
        d += 24 * 60;
    }
    return d;
}

static bool rhythm_refresh_local_time(void)
{
    extern POSIX_TM_S local_tm;
    extern bool time_synced;

    if (OPRT_OK != tal_time_check_time_sync()) {
        time_synced = false;
        return false;
    }

    memset((UCHAR_T *)&local_tm, 0x00, SIZEOF(POSIX_TM_S));
    if (OPRT_OK != tal_time_get_local_time_custom(0, &local_tm)) {
        return false;
    }

    time_synced = true;
    return true;
}

static void rhythm_sleep_until_next_minute(void)
{
    extern POSIX_TM_S local_tm;
    uint32_t sec_to_next = 60 - (local_tm.tm_sec % 60);

    if (sec_to_next == 0) {
        sec_to_next = 60;
    }
    tal_system_sleep(sec_to_next * 1000);
}

static void rhythm_persist_light_state(bool immediate)
{
    if (immediate) {
        device_config_save1();
    } else {
        device_config_save();
    }
}

static const char *rhythm_fade_type_name(uint8_t fade_type)
{
    switch (fade_type) {
        case 0: return "立即变化";
        case 1: return "全程渐变";
        case 2: return "15分钟";
        case 3: return "30分钟";
        case 4: return "45分钟";
        case 5: return "60分钟";
        default: return "未知";
    }
}

static void rhythm_log_minute_time(const char *tag, int total_min)
{
    int norm = (total_min % (24 * 60) + 24 * 60) % (24 * 60);
    // tkl_log_output("[节律]%s %02d:%02d (%d min)\n", tag, norm / 60, norm % 60, total_min);
}

static int rhythm_minutes_until(int from_min, int to_min)
{
    int diff = to_min - from_min;
    if (diff < 0) {
        diff += 24 * 60;
    }
    return diff;
}

static void rhythm_log_countdown(const char *prefix, int minutes)
{
    if (minutes <= 0) {
        // tkl_log_output("%s0分钟\n", prefix);
    } else if (minutes >= 60) {
        // tkl_log_output("%s%d小时%d分钟(%d分钟)\n", prefix, minutes / 60, minutes % 60, minutes);
    } else {
        // tkl_log_output("%s%d分钟\n", prefix, minutes);
    }
}

// 检查是否为"不调节"标志
#define NO_CHANGE_FLAG 101

// 工具函数：设置灯光状态（支持分别设置主灯和辅灯）
static void set_light_state_separate(uint8_t white_bright, uint8_t white_temp, 
                                     uint8_t aux_bright, uint8_t aux_temp)
{
    extern DEMO_INFO_T sg_demo_info;
    // 记录用户操作时间
    sg_last_user_operation_time = tal_system_get_millisecond();
    sg_user_interrupted_rhythm = true;
    
    // 处理NO_CHANGE_FLAG
    if (white_bright == NO_CHANGE_FLAG) white_bright = sg_demo_info.white_bright;
    if (white_temp == NO_CHANGE_FLAG) white_temp = sg_demo_info.white_temp;
    if (aux_bright == NO_CHANGE_FLAG) aux_bright = sg_demo_info.aux_bright;
    if (aux_temp == NO_CHANGE_FLAG) aux_temp = sg_demo_info.white_temp; // 辅灯色温使用主灯色温

    // tkl_log_output("set_light_state_separate: 主灯(bright=%d, temp=%d), 辅灯(bright=%d, temp=%d)", white_bright, white_temp, aux_bright, aux_temp);
    
    // ========== 关灯处理 ==========
    if ((white_bright == 0 && sg_demo_info.white_switch) || 
        (aux_bright == 0 && sg_demo_info.aux_switch)) {
        TUYA_PWM_NUM_E all_channels[5];
        UINT32_T all_duties[5];
        UINT8_T channel_count = 0;
        
        if(sg_demo_info.white_switch == 1 && sg_demo_info.aux_switch == 1)
            sg_demo_info.last_light_memory = 1;
        else if(sg_demo_info.white_switch == 1)
            sg_demo_info.last_light_memory = 2;
        else if(sg_demo_info.aux_switch == 1)
            sg_demo_info.last_light_memory = 3;

        // 处理主灯
        if (white_bright == 0 && sg_demo_info.white_switch) {
            sg_demo_info.white_switch = 0;
            // sg_demo_info.white_bright = 1;
            upload_device_bool_status(LIGHT_SWITCH, 0);
            all_channels[channel_count] = BRIGHT_PWM;
            all_duties[channel_count++] = 0;
            all_channels[channel_count] = TEMP_PWM;
            all_duties[channel_count++] = 0;
        }
        
        // 处理辅灯
        if (aux_bright == 0 && sg_demo_info.aux_switch) {
            sg_demo_info.aux_switch = 0;
            // sg_demo_info.aux_bright = 1;
            upload_device_bool_status(AUX_SWITCH, 0);
            all_channels[channel_count] = AUX_BRIGHT_PWM;
            all_duties[channel_count++] = 0;
            all_channels[channel_count] = AUX_TEMP_PWM;
            all_duties[channel_count++] = 0;
        }
        
        // 确保夜灯关闭
        if (sg_demo_info.night_switch) {
            sg_demo_info.night_switch = 0;
            upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
            all_channels[channel_count] = NIGHT_BRIGHT_PWM;
            all_duties[channel_count++] = 0;
        }
        
        // 更新总开关状态
        if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) && 
            (sg_demo_info.night_switch == 0)) {
            sg_demo_info.switch_status = 0;
            upload_device_bool_status(DPID_SWITCH, 0);
        }

        if (channel_count > 0) {
            pwm_gradual_duty_set_multi(channel_count, all_channels, all_duties);
        }
        return;
    }
    
    // ========== 开灯分支（至少一个灯亮度>0） ==========
    TUYA_PWM_NUM_E all_channels1[5];
    UINT32_T all_duties1[5];
    UINT8_T channel_count1 = 0;

    // 确保夜灯关闭（节律只控制主灯和辅灯）
    if (sg_demo_info.night_switch) {
        sg_demo_info.night_switch = 0;
        all_channels1[channel_count1] = NIGHT_BRIGHT_PWM;
        all_duties1[channel_count1++] = 0;
        upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
        if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) && 
            (sg_demo_info.night_switch == 0)) {
            sg_demo_info.switch_status = 0;
            upload_device_bool_status(DPID_SWITCH, 0);
        }
    }

    // sg_demo_info.switch_status = 1;
    // upload_device_bool_status(DPID_SWITCH, 1);
    
    // 【关键修改】检查是否有任何灯亮着
    bool any_light_on = sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch;
    
    // 设置主灯
    if (white_bright > 0) {
        // 允许设置的条件：主灯当前亮 或 所有灯都关（自动开灯场景）
        sg_demo_info.white_bright = white_bright;
        sg_demo_info.white_temp = white_temp;
        if (sg_demo_info.white_switch || !any_light_on) {
            // 如果主灯原本是关的，现在要开启
            if (!sg_demo_info.white_switch) {
                sg_demo_info.white_switch = 1;
                upload_device_bool_status(LIGHT_SWITCH, 1);
                sg_demo_info.switch_status = 1;
                upload_device_bool_status(DPID_SWITCH, 1);
            }

            // tkl_log_output("设置主灯状态：亮度=%d, 色温=%d", sg_demo_info.white_bright, sg_demo_info.white_temp);
            uint16_t white_bright1 = 5 + (sg_demo_info.white_bright - 1) * 95 / 99;
            uint8_t white_temp1 = (sg_demo_info.white_temp == 0) ? 1 : sg_demo_info.white_temp;
            uint16_t ww = white_bright1 * white_temp1;
            uint16_t cw = white_bright1 * 100 - ww;
            
            light_pwm_clamp_mix(&ww, &cw);
            all_channels1[channel_count1] = BRIGHT_PWM;
            all_duties1[channel_count1++] = ww;
            
            all_channels1[channel_count1] = TEMP_PWM;
            all_duties1[channel_count1++] = cw;
        }
            
        // 上报变化的参数
        upload_device_value_status(DPID_WHITE_BRIGHT, sg_demo_info.white_bright);
        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
    }
    
    // 设置辅灯
    if (aux_bright > 0) {
        sg_demo_info.aux_bright = aux_bright;
        // 辅灯色温跟随主灯色温
        sg_demo_info.white_temp = white_temp; // 注意：这里更新主灯色温，辅灯计算时使用主灯色温
        if (sg_demo_info.aux_switch || !any_light_on) {
            if (!sg_demo_info.aux_switch) {
                sg_demo_info.aux_switch = 1;
                upload_device_bool_status(AUX_SWITCH, 1);
                sg_demo_info.switch_status = 1;
                upload_device_bool_status(DPID_SWITCH, 1);
            }
            
            // tkl_log_output("设置辅灯状态：亮度=%d, 色温=%d", sg_demo_info.aux_bright, sg_demo_info.white_temp);
            uint16_t aux_bright1 = 5 + (sg_demo_info.aux_bright - 1) * 95 / 99;
            uint8_t aux_temp1 = (sg_demo_info.white_temp == 0) ? 1 : sg_demo_info.white_temp;
            uint16_t aux_ww = aux_bright1 * aux_temp1;
            uint16_t aux_cw = aux_bright1 * 100 - aux_ww;
            
            light_pwm_clamp_mix(&aux_ww, &aux_cw);
all_channels1[channel_count1] = AUX_BRIGHT_PWM;
            all_duties1[channel_count1++] = aux_ww;
            
            all_channels1[channel_count1] = AUX_TEMP_PWM;
            all_duties1[channel_count1++] = aux_cw;
            
            
            // tkl_log_output("设置辅灯状态：亮度=%d, 色温=%d", sg_demo_info.aux_bright, sg_demo_info.white_temp);
        } 
        // 上报变化的参数
        upload_device_value_status(DPID_AUX_BRIGHT_VALUE, sg_demo_info.aux_bright);
        upload_device_value_status(DPID_TEMP_VALUE, sg_demo_info.white_temp);
    }

    if (channel_count1 > 0) {
        pwm_gradual_duty_set_multi(channel_count1, all_channels1, all_duties1);
    }
}
// 兼容旧接口（使用相同值设置主灯和辅灯）
static void set_light_state_conditional(uint8_t bright, uint8_t temp)
{
    set_light_state_separate(bright, temp, bright, temp);
}

// 检查是否有主灯或辅灯开启
static bool is_main_light_on()
{
    extern DEMO_INFO_T sg_demo_info;
    bool result = (sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch);
    return result;
}

void auto_light_on_action(RHYTHM_NODE_DATA_T *node, int node_pos, int enabled_nodes[], int enabled_count, int actual_fade_minutes)
{
    extern DEMO_INFO_T sg_demo_info;
    extern TM_RHYTHM_INFO_T sg_rhythm_info;

    // tkl_log_output("[AUTO_ON] start: node=%02d:%02d, white_switch=%d, aux_switch=%d",
    // node->hour, node->minute,
    // sg_demo_info.white_switch, sg_demo_info.aux_switch);

    if (!node || node->auto_light_on == 0) {
        return;
    }

    // 如果已经有灯亮着，不执行自动开灯（只应在全关时触发）
    if (sg_demo_info.white_switch || sg_demo_info.aux_switch) {
        return;
    }

    // 获取当前节点的目标亮度、色温
    uint8_t target_bright = node->per_bright;
    uint8_t target_temp   = node->per_temper;

    // 查找前一个非101的值（处理NO_CHANGE_FLAG）
    uint8_t prev_non_101_bright, prev_non_101_temp;
    find_previous_non_101_value(node_pos, enabled_nodes, enabled_count, 
                                 &prev_non_101_bright, &prev_non_101_temp);

    if (target_bright == NO_CHANGE_FLAG) target_bright = prev_non_101_bright;
    if (target_temp  == NO_CHANGE_FLAG) target_temp   = prev_non_101_temp;

    if (target_bright == 0) {
        // tkl_log_output("自动开灯：目标亮度为0，不执行自动开灯");
        return;
    }

    // ===== 获取当前时间点的节律值作为起始值 =====
    uint8_t saved_white_switch = sg_demo_info.white_switch;
    uint8_t saved_aux_switch   = sg_demo_info.aux_switch;
    sg_demo_info.white_switch = 1;
    sg_demo_info.aux_switch   = 1;
    apply_correct_rhythm_settings(true); // 计算模式，结果存入 sg_demo_info
    uint8_t current_bright = sg_demo_info.white_bright;
    uint8_t current_temp   = sg_demo_info.white_temp;
    sg_demo_info.white_switch = saved_white_switch;
    sg_demo_info.aux_switch   = saved_aux_switch;

    if (current_bright == NO_CHANGE_FLAG || current_bright == 0) current_bright = 1;
    if (current_temp  == NO_CHANGE_FLAG) current_temp = 0;

    // ===== 根据 last_light_memory 决定哪些灯应该点亮 =====
    uint8_t white_switch_target = 0;
    uint8_t aux_switch_target   = 0;
    switch (sg_demo_info.last_light_memory) {
        case 1: white_switch_target = 1; aux_switch_target = 1; break; // 两个灯都开
        case 2: white_switch_target = 1; aux_switch_target = 0; break; // 只开主灯
        case 3: white_switch_target = 0; aux_switch_target = 1; break; // 只开辅灯
        default: white_switch_target = 1; aux_switch_target = 1; break;
    }

    // 目标亮度均为节律目标值（两个灯的内存数据都要更新）
    uint8_t white_target_bright = target_bright;
    uint8_t aux_target_bright   = target_bright;

    // 起始亮度为当前计算值（两个灯的内存起始值相同）
    uint8_t white_start_bright = current_bright;
    uint8_t aux_start_bright   = current_bright;

    // 设置开关状态（直接赋值，后续 set_light_state_separate 会根据开关输出）
    sg_demo_info.white_switch = white_switch_target;
    sg_demo_info.aux_switch   = aux_switch_target;

    // tkl_log_output("自动开灯：开关目标 - 主灯=%d, 辅灯=%d；亮度目标 - 主灯=%d, 辅灯=%d",white_switch_target, aux_switch_target, white_target_bright, aux_target_bright);

    // 先设置起始值（更新内存并输出PWM，开关状态已设好）
    set_light_state_separate(white_start_bright, current_temp,
                              aux_start_bright,   current_temp);

    // 上报总开关状态（只要至少一个灯要开）
    if (white_switch_target || aux_switch_target) {
        upload_device_bool_status(DPID_SWITCH, 1);
        sg_demo_info.switch_status = 1;
    }
    if(white_switch_target){
        sg_demo_info.white_switch = 1;
        upload_device_bool_status(LIGHT_SWITCH, 1);
    }
    if(aux_switch_target){
        sg_demo_info.aux_switch = 1;
        upload_device_bool_status(AUX_SWITCH, 1);
    }
    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);

    // 如果渐变时间大于0，启动渐变
    if (actual_fade_minutes > 0) {
        start_fade(false,
                   white_start_bright, current_temp,
                   aux_start_bright,   current_temp,
                   white_target_bright, target_temp,
                   aux_target_bright,   target_temp,
                   actual_fade_minutes * 60 * 1000,
                   10);
    } else {
        // 没有渐变，直接设置目标值
        set_light_state_separate(white_target_bright, target_temp,
                                  aux_target_bright,   target_temp);
    }

    rhythm_persist_light_state(actual_fade_minutes > 0 ? false : true);
}
// 修改find_previous_non_101_value函数，确保能找到有效的值：
void find_previous_non_101_value(int current_pos, int enabled_nodes[], int enabled_count, 
                                        uint8_t *prev_bright, uint8_t *prev_temp)
{
    extern DEMO_INFO_T sg_demo_info;
    *prev_bright = NO_CHANGE_FLAG;
    *prev_temp = NO_CHANGE_FLAG;
    
    // 从当前节点向前查找
    for (int i = current_pos - 1; i >= 0; i--) {
        RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[enabled_nodes[i]];
        if (*prev_bright == NO_CHANGE_FLAG && node->per_bright != NO_CHANGE_FLAG) {
            *prev_bright = node->per_bright;
        }
        if (*prev_temp == NO_CHANGE_FLAG && node->per_temper != NO_CHANGE_FLAG) {
            *prev_temp = node->per_temper;
        }
        
        if (*prev_bright != NO_CHANGE_FLAG && *prev_temp != NO_CHANGE_FLAG) {
            break;
        }
    }
    
    // 如果向前没找到，从后向前查找（处理跨天）
    if (*prev_bright == NO_CHANGE_FLAG || *prev_temp == NO_CHANGE_FLAG) {
        for (int i = enabled_count - 1; i > current_pos; i--) {
            RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[enabled_nodes[i]];
            if (*prev_bright == NO_CHANGE_FLAG && node->per_bright != NO_CHANGE_FLAG) {
                *prev_bright = node->per_bright;
            }
            if (*prev_temp == NO_CHANGE_FLAG && node->per_temper != NO_CHANGE_FLAG) {
                *prev_temp = node->per_temper;
            }
            
            if (*prev_bright != NO_CHANGE_FLAG && *prev_temp != NO_CHANGE_FLAG) {
                break;
            }
        }
    }
    
    // 如果还是没找到非101的值，使用默认值
    if (*prev_bright == NO_CHANGE_FLAG) {
        // 如果灯是关着的，使用默认值1
        if (sg_demo_info.white_switch || sg_demo_info.aux_switch) {
            *prev_bright = sg_demo_info.white_switch ? sg_demo_info.white_bright : sg_demo_info.aux_bright;
        } else {
            *prev_bright = 1;  // 默认开灯亮度
        }
    }
    
    if (*prev_temp == NO_CHANGE_FLAG) {
        *prev_temp = sg_demo_info.white_temp > 0 ? sg_demo_info.white_temp : 0;  // 默认色温
    }
}

// 开始渐变（修改：分别记录主灯和辅灯的起始值和目标值）
void start_fade(bool is_power_on_recovery, 
                       uint8_t white_start_bright, uint8_t white_start_temp,
                       uint8_t aux_start_bright, uint8_t aux_start_temp,
                       uint8_t white_target_bright, uint8_t white_target_temp,
                       uint8_t aux_target_bright, uint8_t aux_target_temp,
                       uint32_t duration_ms, uint8_t update_interval)
{
    extern DEMO_INFO_T sg_demo_info;
    
    // 检查是否有有效的目标值
    bool has_valid_target = false;
    
    // 检查主灯目标值
    if (sg_demo_info.white_switch && 
        (white_target_bright != 0 || white_target_temp != 0)) {
        has_valid_target = true;
    }
    
    // 检查辅灯目标值
    if (sg_demo_info.aux_switch && 
        (aux_target_bright != 0 || aux_target_temp != 0)) {
        has_valid_target = true;
    }
    
    if (!has_valid_target) {
        return;
    }
    
    // 检查主灯起始值和目标值是否相同
    bool white_needs_fade = false;
    if (sg_demo_info.white_switch) {
        white_needs_fade = (white_start_bright != white_target_bright) || 
                          (white_start_temp != white_target_temp);
    }
    
    // 检查辅灯起始值和目标值是否相同
    bool aux_needs_fade = false;
    if (sg_demo_info.aux_switch) {
        aux_needs_fade = (aux_start_bright != aux_target_bright) || 
                        (aux_start_temp != aux_target_temp);
    }
    
    if (!white_needs_fade && !aux_needs_fade) {
        return;
    }
    
    sg_fade_state.active = true;
    sg_fade_state.is_power_on_recovery = is_power_on_recovery;
    sg_fade_state.start_time = tal_system_get_millisecond();
    sg_fade_state.duration_ms = duration_ms;
    
    // 设置主灯渐变参数
    sg_fade_state.white_start_bright = white_start_bright;
    sg_fade_state.white_start_temp = white_start_temp;
    sg_fade_state.white_target_bright = white_target_bright;
    sg_fade_state.white_target_temp = white_target_temp;
    
    // 设置辅灯渐变参数
    sg_fade_state.aux_start_bright = aux_start_bright;
    sg_fade_state.aux_start_temp = aux_start_temp;
    sg_fade_state.aux_target_bright = aux_target_bright;
    sg_fade_state.aux_target_temp = aux_target_temp;
    
    sg_fade_state.update_interval = update_interval;
    sg_fade_state.last_update_time = 0;
}

// 停止渐变
void stop_fade(void)
{
    if (sg_fade_state.active) {
        sg_fade_state.active = false;
        sg_fade_state.is_power_on_recovery = false;
    }
}

// 处理渐变更新（修改：分别计算主灯和辅灯的渐变值）
static void process_fade_update(void)
{
    if (!sg_fade_state.active) {
        return;
    }
    
    uint32_t current_time = tal_system_get_millisecond();
    
    // 防止时间溢出
    if (current_time < sg_fade_state.start_time) {
        sg_fade_state.start_time = current_time;
        return;
    }
    
    uint32_t elapsed_time = current_time - sg_fade_state.start_time;
    
    if (elapsed_time >= sg_fade_state.duration_ms) {
        // 停止渐变
        stop_fade();
        
        // 设置最终值
        set_light_state_separate(
            sg_fade_state.white_target_bright,
            sg_fade_state.white_target_temp,
            sg_fade_state.aux_target_bright,
            sg_fade_state.aux_target_temp
        );
        rhythm_persist_light_state(true);
    } else {
        // 计算当前渐变进度
        float progress = (float)elapsed_time / (float)sg_fade_state.duration_ms;
        if (progress > 1.0f) progress = 1.0f;
        
        // 计算主灯当前值
        uint8_t current_white_bright = sg_fade_state.white_start_bright;
        uint8_t current_white_temp = sg_fade_state.white_start_temp;
        
        if (sg_fade_state.white_target_bright != NO_CHANGE_FLAG && 
            sg_fade_state.white_start_bright != NO_CHANGE_FLAG) {
            int16_t bright_diff = (int16_t)sg_fade_state.white_target_bright - 
                                 (int16_t)sg_fade_state.white_start_bright;
            current_white_bright = (int16_t)sg_fade_state.white_start_bright + 
                                  (int16_t)(bright_diff * progress);
            if (current_white_bright > 100) current_white_bright = 100;
            if (current_white_bright < 0) current_white_bright = 0;
        }
        
        if (sg_fade_state.white_target_temp != NO_CHANGE_FLAG && 
            sg_fade_state.white_start_temp != NO_CHANGE_FLAG) {
            int16_t temp_diff = (int16_t)sg_fade_state.white_target_temp - 
                               (int16_t)sg_fade_state.white_start_temp;
            current_white_temp = (int16_t)sg_fade_state.white_start_temp + 
                                (int16_t)(temp_diff * progress);
            if (current_white_temp > 100) current_white_temp = 100;
            if (current_white_temp < 0) current_white_temp = 0;
        }
        
        // 计算辅灯当前值
        uint8_t current_aux_bright = sg_fade_state.aux_start_bright;
        uint8_t current_aux_temp = sg_fade_state.aux_start_temp;
        
        if (sg_fade_state.aux_target_bright != NO_CHANGE_FLAG && 
            sg_fade_state.aux_start_bright != NO_CHANGE_FLAG) {
            int16_t bright_diff = (int16_t)sg_fade_state.aux_target_bright - 
                                 (int16_t)sg_fade_state.aux_start_bright;
            current_aux_bright = (int16_t)sg_fade_state.aux_start_bright + 
                                (int16_t)(bright_diff * progress);
            if (current_aux_bright > 100) current_aux_bright = 100;
            if (current_aux_bright < 0) current_aux_bright = 0;
        }
        
        if (sg_fade_state.aux_target_temp != NO_CHANGE_FLAG && 
            sg_fade_state.aux_start_temp != NO_CHANGE_FLAG) {
            int16_t temp_diff = (int16_t)sg_fade_state.aux_target_temp - 
                               (int16_t)sg_fade_state.aux_start_temp;
            current_aux_temp = (int16_t)sg_fade_state.aux_start_temp + 
                              (int16_t)(temp_diff * progress);
            if (current_aux_temp > 100) current_aux_temp = 100;
            if (current_aux_temp < 0) current_aux_temp = 0;
        }
        
        // 设置当前灯光状态
        set_light_state_separate(
            current_white_bright,
            current_white_temp,
            current_aux_bright,
            current_aux_temp
        );
        rhythm_persist_light_state(false);
    }
}

// 计算未来时间点的节律值（需要适配新接口）
static void calculate_future_rhythm_values(int minutes_ahead, 
                                          uint8_t *white_bright, uint8_t *white_temp,
                                          uint8_t *aux_bright, uint8_t *aux_temp)
{
    if (!white_bright || !white_temp || !aux_bright || !aux_temp) return;
    
    *white_bright = NO_CHANGE_FLAG;
    *white_temp = NO_CHANGE_FLAG;
    *aux_bright = NO_CHANGE_FLAG;
    *aux_temp = NO_CHANGE_FLAG;
    
    // 获取当前时间
    extern POSIX_TM_S local_tm;
    
    // 计算未来时间
    int future_total = (local_tm.tm_hour * 60 + local_tm.tm_min + minutes_ahead) % (24 * 60);
    int future_hour = future_total / 60;
    int future_minute = future_total % 60;
    
    // 查找所有启用的节点
    int enabled_nodes[RHYTHM_NODE_COUNT];
    int enabled_count = 0;
    
    for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
        if (sg_rhythm_info.cfg.nodes[i].node_enable) {
            enabled_nodes[enabled_count++] = i;
        }
    }
    
    if (enabled_count == 0) {
        return;
    }
    
    // 对节点按时间排序
    for (int i = 0; i < enabled_count - 1; i++) {
        for (int j = i + 1; j < enabled_count; j++) {
            int time_i = sg_rhythm_info.cfg.nodes[enabled_nodes[i]].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[enabled_nodes[i]].minute;
            int time_j = sg_rhythm_info.cfg.nodes[enabled_nodes[j]].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[enabled_nodes[j]].minute;
            if (time_i > time_j) {
                int temp_idx = enabled_nodes[i];
                enabled_nodes[i] = enabled_nodes[j];
                enabled_nodes[j] = temp_idx;
            }
        }
    }
    
    // 查找未来时间所在的前后节点
    int prev_node_index = -1;
    int next_node_index = -1;
    int prev_node_time = -1;
    int next_node_time = -1;
    
    for (int i = 0; i < enabled_count; i++) {
        int node_time = sg_rhythm_info.cfg.nodes[enabled_nodes[i]].hour * 60 + 
                       sg_rhythm_info.cfg.nodes[enabled_nodes[i]].minute;
        
        if (node_time <= future_total) {
            prev_node_index = enabled_nodes[i];
            prev_node_time = node_time;
        } else {
            next_node_index = enabled_nodes[i];
            next_node_time = node_time;
            break;
        }
    }
    
    // 处理跨天情况
    if (prev_node_index == -1) {
        prev_node_index = enabled_nodes[enabled_count - 1];
        prev_node_time = sg_rhythm_info.cfg.nodes[prev_node_index].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[prev_node_index].minute - 24 * 60;
        next_node_index = enabled_nodes[0];
        next_node_time = sg_rhythm_info.cfg.nodes[next_node_index].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[next_node_index].minute;
    } else if (next_node_index == -1) {
        next_node_index = enabled_nodes[0];
        next_node_time = sg_rhythm_info.cfg.nodes[next_node_index].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[next_node_index].minute + 24 * 60;
    }
    
    RHYTHM_NODE_DATA_T *prev_node = &sg_rhythm_info.cfg.nodes[prev_node_index];
    RHYTHM_NODE_DATA_T *next_node = &sg_rhythm_info.cfg.nodes[next_node_index];
    
    // 获取起始值和目标值
    uint8_t start_bright = prev_node->per_bright;
    uint8_t start_temp = prev_node->per_temper;
    uint8_t target_bright = next_node->per_bright;
    uint8_t target_temp = next_node->per_temper;
    
    // 处理101值
    if (start_bright == NO_CHANGE_FLAG || start_temp == NO_CHANGE_FLAG) {
        uint8_t prev_non_101_bright, prev_non_101_temp;
        find_previous_non_101_value(0, enabled_nodes, enabled_count, 
                                   &prev_non_101_bright, &prev_non_101_temp);
        
        if (start_bright == NO_CHANGE_FLAG) start_bright = prev_non_101_bright;
        if (start_temp == NO_CHANGE_FLAG) start_temp = prev_non_101_temp;
    }
    
    if (target_bright == NO_CHANGE_FLAG || target_temp == NO_CHANGE_FLAG) {
        uint8_t prev_non_101_bright, prev_non_101_temp;
        find_previous_non_101_value(1, enabled_nodes, enabled_count, 
                                   &prev_non_101_bright, &prev_non_101_temp);
        
        if (target_bright == NO_CHANGE_FLAG) target_bright = prev_non_101_bright;
        if (target_temp == NO_CHANGE_FLAG) target_temp = prev_non_101_temp;
    }
    
    // 根据节点类型和未来时间计算值
    int node_interval = next_node_time - prev_node_time;
    if (node_interval < 0) node_interval += 24 * 60;
    
    int fade_minutes = 0;
    switch (next_node->fade_type) {
        case 0: fade_minutes = 0; break;
        case 1: fade_minutes = node_interval; break;
        case 2: fade_minutes = 15; break;
        case 3: fade_minutes = 30; break;
        case 4: fade_minutes = 45; break;
        case 5: fade_minutes = 60; break;
        default: fade_minutes = 0; break;
    }
    
    if (node_interval < fade_minutes) {
        fade_minutes = node_interval;
    }
    
    // 计算未来时间点的值
    if (fade_minutes == 0) {
        // 立即变化模式
        if (future_total >= next_node_time) {
            *white_bright = target_bright;
            *white_temp = target_temp;
            *aux_bright = target_bright;
            *aux_temp = target_temp;
        } else {
            *white_bright = start_bright;
            *white_temp = start_temp;
            *aux_bright = start_bright;
            *aux_temp = start_temp;
        }
    } else {
        // 渐变模式
        int fade_start_time = next_node_time - fade_minutes;
        if (fade_start_time < 0) fade_start_time += 24 * 60;
        
        if (future_total < fade_start_time) {
            *white_bright = start_bright;
            *white_temp = start_temp;
            *aux_bright = start_bright;
            *aux_temp = start_temp;
        } else if (future_total >= next_node_time) {
            *white_bright = target_bright;
            *white_temp = target_temp;
            *aux_bright = target_bright;
            *aux_temp = target_temp;
        } else {
            int elapsed_in_fade = future_total - fade_start_time;
            if (elapsed_in_fade < 0) elapsed_in_fade += 24 * 60;
            
            float progress = (float)elapsed_in_fade / (float)fade_minutes;
            if (progress > 1.0f) progress = 1.0f;
            
            // 计算未来亮度
            int16_t future_bright = (int16_t)start_bright + 
                                  (int16_t)((target_bright - start_bright) * progress);
            if (future_bright > 100) future_bright = 100;
            if (future_bright < 0) future_bright = 0;
            
            // 计算未来色温
            int16_t future_temp = (int16_t)start_temp + 
                                (int16_t)((target_temp - start_temp) * progress);
            if (future_temp > 100) future_temp = 100;
            if (future_temp < 0) future_temp = 0;
            
            *white_bright = (uint8_t)future_bright;
            *white_temp = (uint8_t)future_temp;
            *aux_bright = (uint8_t)future_bright;
            *aux_temp = (uint8_t)future_temp;
        }
    }
}

// 上电恢复处理（修改：分别获取主灯和辅灯的当前值）
static void handle_power_on_recovery(void)
{
    static bool recovery_checked = false;
    extern DEMO_INFO_T sg_demo_info;
    
    if (sg_power_on_recovered || recovery_checked) {
        return;
    }
    
    // 标记已执行
    recovery_checked = true;
    sg_power_on_recovered = true;
    sg_device_just_booted = false;
    
    if (!sg_demo_info.rhythm_switch) {
        return;
    }
    if (sg_rhythm_interrupted) {
        return;
    }
    
    extern bool time_synced;
    if (!time_synced) {
        return;
    }

    /* 长断电冷启动：时间同步后若当前时段应为关灯，打断节律（保留当前灯态），不做上电恢复 */
    if (app_light_rhythm_expect_bright_zero_now()) {
        app_light_stop_today_rhythm_timer();
        return;
    }

    /* 注意：不要用「距上次操作<5s」跳过。节律 init 会刷新 sg_last_user_operation_time，
     * 会导致几乎总是跳过 60s 上电恢复，随后被 apply_correct 约 2s 直接拉到节律。 */
    
    // 只有主灯或辅灯开启时才需要恢复
    if (!sg_demo_info.white_switch && !sg_demo_info.aux_switch && !sg_demo_info.night_switch) {
        return;
    }
    
    // 获取主灯和辅灯的当前值
    uint8_t current_white_bright = sg_demo_info.white_bright;
    uint8_t current_white_temp = sg_demo_info.white_temp;
    uint8_t current_aux_bright = sg_demo_info.aux_bright;
    uint8_t current_aux_temp = sg_demo_info.white_temp; // 辅灯色温使用主灯色温
    
    // 计算节律目标值
    uint8_t white_target_bright, white_target_temp, aux_target_bright, aux_target_temp;
    calculate_future_rhythm_values(1, &white_target_bright, &white_target_temp, 
                                  &aux_target_bright, &aux_target_temp);
    
    // 检查目标值是否有效
    bool has_valid_target = false;
    if (sg_demo_info.white_switch && 
        (white_target_bright != 0 || white_target_temp != 0)) {
        has_valid_target = true;
    }
    if (sg_demo_info.aux_switch && 
        (aux_target_bright != 0 || aux_target_temp != 0)) {
        has_valid_target = true;
    }
    
    if (!has_valid_target) {
        return;
    }
    
    // 处理NO_CHANGE_FLAG
    if (white_target_bright == NO_CHANGE_FLAG) white_target_bright = current_white_bright;
    if (white_target_temp == NO_CHANGE_FLAG) white_target_temp = current_white_temp;
    if (aux_target_bright == NO_CHANGE_FLAG) aux_target_bright = current_aux_bright;
    if (aux_target_temp == NO_CHANGE_FLAG) aux_target_temp = current_aux_temp;
    
    // 检查差异是否显著（大于5%才需要渐变）
    int white_bright_diff = abs(white_target_bright - current_white_bright);
    int white_temp_diff = abs(white_target_temp - current_white_temp);
    int aux_bright_diff = abs(aux_target_bright - current_aux_bright);
    int aux_temp_diff = abs(aux_target_temp - current_aux_temp);
    
    // 只有当差异较大时才使用渐变（避免微小变化产生渐变）
    if ((white_bright_diff > 5 || white_temp_diff > 5) && sg_demo_info.white_switch ||
        (aux_bright_diff > 5 || aux_temp_diff > 5) && sg_demo_info.aux_switch) {
        // 1分钟渐变，每10秒更新一次
        start_fade(true, 
                   current_white_bright, current_white_temp,
                   current_aux_bright, current_aux_temp,
                   white_target_bright, white_target_temp,
                   aux_target_bright, aux_target_temp,
                   60 * 1000, 10);
    } else {
        // 直接设置节律值
        set_light_state_separate(
            white_target_bright, white_target_temp,
            aux_target_bright, aux_target_temp
        );
        rhythm_persist_light_state(true);
    }
}
// 应用正确的节律设置（修改：分别处理主灯和辅灯）
void apply_correct_rhythm_settings(bool calculate_only)
{
    extern DEMO_INFO_T sg_demo_info;
    // 节律开关关闭，直接返回
    if (!sg_demo_info.rhythm_switch) {
        return;
    }
    
    // 如果正在上电恢复渐变，直接返回，不执行节律设置
    if (sg_fade_state.active && sg_fade_state.is_power_on_recovery) {
        return;
    }
    
    // 获取当前时间
    extern POSIX_TM_S local_tm;
    int current_total = local_tm.tm_hour * 60 + local_tm.tm_min;
    
    // 查找所有启用的节点
    int enabled_nodes[RHYTHM_NODE_COUNT];
    int enabled_count = 0;
    
    for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
        if (sg_rhythm_info.cfg.nodes[i].node_enable) {
            enabled_nodes[enabled_count++] = i;
        }
    }
    
    if (enabled_count < 2) {
        if (enabled_count == 1) {
            RHYTHM_NODE_DATA_T *single_node = &sg_rhythm_info.cfg.nodes[enabled_nodes[0]];
            uint8_t target_bright = single_node->per_bright;
            uint8_t target_temp = single_node->per_temper;
            
            if (target_bright == NO_CHANGE_FLAG || target_temp == NO_CHANGE_FLAG) {
                uint8_t prev_bright, prev_temp;
                find_previous_non_101_value(0, enabled_nodes, enabled_count, &prev_bright, &prev_temp);
                
                if (target_bright == NO_CHANGE_FLAG) target_bright = prev_bright;
                if (target_temp == NO_CHANGE_FLAG) target_temp = prev_temp;
            }
            
            if (calculate_only) {
                sg_demo_info.white_bright = target_bright;
                sg_demo_info.white_temp = target_temp;
                sg_demo_info.aux_bright = target_bright;
            } else {
                uint8_t prev_white_bright = sg_demo_info.white_bright;
                uint8_t prev_white_temp = sg_demo_info.white_temp;
                uint8_t prev_aux_bright = sg_demo_info.aux_bright;

                set_light_state_conditional(target_bright, target_temp);

                if (sg_demo_info.white_bright != prev_white_bright ||
                    sg_demo_info.white_temp != prev_white_temp ||
                    sg_demo_info.aux_bright != prev_aux_bright) {
                    rhythm_persist_light_state(false);
                }
            }
        }
        return;
    }
    
    // 对节点按时间排序
    for (int i = 0; i < enabled_count - 1; i++) {
        for (int j = i + 1; j < enabled_count; j++) {
            int time_i = sg_rhythm_info.cfg.nodes[enabled_nodes[i]].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[enabled_nodes[i]].minute;
            int time_j = sg_rhythm_info.cfg.nodes[enabled_nodes[j]].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[enabled_nodes[j]].minute;
            if (time_i > time_j) {
                int temp = enabled_nodes[i];
                enabled_nodes[i] = enabled_nodes[j];
                enabled_nodes[j] = temp;
            }
        }
    }
    
    // 查找当前时间所在的前后节点
    int prev_node_index = -1;
    int next_node_index = -1;
    int prev_node_time = -1;
    int next_node_time = -1;
    int prev_node_pos = -1;
    int next_node_pos = -1;
    
    for (int i = 0; i < enabled_count; i++) {
        int node_time = sg_rhythm_info.cfg.nodes[enabled_nodes[i]].hour * 60 + 
                       sg_rhythm_info.cfg.nodes[enabled_nodes[i]].minute;
        
        if (node_time <= current_total) {
            prev_node_index = enabled_nodes[i];
            prev_node_time = node_time;
            prev_node_pos = i;
        } else {
            next_node_index = enabled_nodes[i];
            next_node_time = node_time;
            next_node_pos = i;
            break;
        }
    }
    
    // 处理跨天情况
    if (prev_node_index == -1) {
        prev_node_index = enabled_nodes[enabled_count - 1];
        prev_node_time = sg_rhythm_info.cfg.nodes[prev_node_index].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[prev_node_index].minute - 24 * 60;
        prev_node_pos = enabled_count - 1;
        
        next_node_index = enabled_nodes[0];
        next_node_time = sg_rhythm_info.cfg.nodes[next_node_index].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[next_node_index].minute;
        next_node_pos = 0;
    } else if (next_node_index == -1) {
        next_node_index = enabled_nodes[0];
        next_node_time = sg_rhythm_info.cfg.nodes[next_node_index].hour * 60 + 
                        sg_rhythm_info.cfg.nodes[next_node_index].minute + 24 * 60;
        next_node_pos = 0;
    }
    
    RHYTHM_NODE_DATA_T *prev_node = &sg_rhythm_info.cfg.nodes[prev_node_index];
    RHYTHM_NODE_DATA_T *next_node = &sg_rhythm_info.cfg.nodes[next_node_index];
    
    // 获取起始值和目标值
    uint8_t start_bright = prev_node->per_bright;
    uint8_t start_temp = prev_node->per_temper;
    uint8_t target_bright = next_node->per_bright;
    uint8_t target_temp = next_node->per_temper;
    
    // 处理101值
    if (start_bright == NO_CHANGE_FLAG || start_temp == NO_CHANGE_FLAG) {
        uint8_t prev_non_101_bright, prev_non_101_temp;
        find_previous_non_101_value(prev_node_pos, enabled_nodes, enabled_count, 
                                   &prev_non_101_bright, &prev_non_101_temp);
        
        if (start_bright == NO_CHANGE_FLAG) start_bright = prev_non_101_bright;
        if (start_temp == NO_CHANGE_FLAG) start_temp = prev_non_101_temp;
    }
    
    if (target_bright == NO_CHANGE_FLAG || target_temp == NO_CHANGE_FLAG) {
        uint8_t prev_non_101_bright, prev_non_101_temp;
        find_previous_non_101_value(next_node_pos, enabled_nodes, enabled_count, 
                                   &prev_non_101_bright, &prev_non_101_temp);
        
        if (target_bright == NO_CHANGE_FLAG) target_bright = prev_non_101_bright;
        if (target_temp == NO_CHANGE_FLAG) target_temp = prev_non_101_temp;
    }
    
    // 计算节点间隔
    int node_interval = next_node_time - prev_node_time;
    if (node_interval < 0) {
        node_interval += 24 * 60;
    }
    
    // 根据后一个节点的渐变类型确定渐变时间
    int fade_minutes = 0;
    switch (next_node->fade_type) {
        case 0: fade_minutes = 0; break;   // 立即变化
        case 1: fade_minutes = node_interval; break;  // 全程渐变
        case 2: fade_minutes = 15; break;  // 15分钟
        case 3: fade_minutes = 30; break;  // 30分钟
        case 4: fade_minutes = 45; break;  // 45分钟
        case 5: fade_minutes = 60; break;  // 60分钟
        default: fade_minutes = 0; break;
    }
    
    // 如果节点间隔小于渐变时间，使用节点间隔作为渐变时间
    if (node_interval < fade_minutes) {
        fade_minutes = node_interval;
    }

    // tkl_log_output("[节律]节点区间: prev=%d(%02d:%02d) -> next=%d(%02d:%02d), 间隔=%d min\n",
                //    prev_node_index, prev_node->hour, prev_node->minute,
                //    next_node_index, next_node->hour, next_node->minute, node_interval);
    // tkl_log_output("[节律]下一节点渐变配置: fade_type=%d(%s), fade_minutes=%d\n",
                //    next_node->fade_type, rhythm_fade_type_name(next_node->fade_type), fade_minutes);
    
    // ===== 计算当前时间点应该设置的亮度色温 =====
    uint8_t current_bright, current_temp;
    static int sg_last_fade_phase = -1;
    static int sg_last_fade_prev = -1;
    static int sg_last_fade_next = -1;
    int fade_phase = 0; /* 0=渐变前 1=渐变中 2=节点后 3=立即切换 */
    
    if (fade_minutes == 0) {
        // 立即变化模式
        bool use_next_node = false;
        
        if (next_node_time >= 24 * 60) {
            // 跨天
            if (current_total >= prev_node_time) {
                use_next_node = false;
            } else {
                use_next_node = (current_total >= (next_node_time - 24 * 60));
            }
        } else {
            use_next_node = (current_total >= next_node_time);
        }
        
        if (use_next_node) {
            fade_phase = 3;
            current_bright = target_bright;
            current_temp = target_temp;
        } else {
            fade_phase = 0;
            current_bright = start_bright;
            current_temp = start_temp;
        }
        if (!use_next_node) {
            int remain_to_node = rhythm_minutes_until(current_total, next_node_time % (24 * 60));
            // tkl_log_output("[节律]立即模式: 下一节点=%02d:%02d 到达时瞬间切换, 当前=%02d:%02d\n",
                        //    next_node->hour, next_node->minute,
                        //    local_tm.tm_hour, local_tm.tm_min);
            rhythm_log_countdown("[节律]距下一节点切换还剩 ", remain_to_node);
        } else {
            // tkl_log_output("[节律]立即模式: 已到达下一节点 %02d:%02d, 已切换\n",
                        //    next_node->hour, next_node->minute);
        }
    } else {
        // 渐变模式
        int actual_fade_minutes = fade_minutes;
        if (node_interval < actual_fade_minutes) {
            actual_fade_minutes = node_interval;
        }
        
        int fade_start_time_actual = next_node_time - actual_fade_minutes;
        if (fade_start_time_actual < 0) {
            fade_start_time_actual += 24 * 60;
        }

        int fade_start_h = (fade_start_time_actual % (24 * 60) + 24 * 60) % (24 * 60) / 60;
        int fade_start_m = (fade_start_time_actual % (24 * 60) + 24 * 60) % (24 * 60) % 60;
        int next_node_h = (next_node_time % (24 * 60) + 24 * 60) % (24 * 60) / 60;
        int next_node_m = (next_node_time % (24 * 60) + 24 * 60) % (24 * 60) % 60;

        // tkl_log_output("[节律]下一节点=%02d:%02d, 渐变提前=%dmin, 渐变开始=%02d:%02d, 当前=%02d:%02d\n",
                    //    next_node_h, next_node_m, actual_fade_minutes,
                    //    fade_start_h, fade_start_m,
                    //    local_tm.tm_hour, local_tm.tm_min);
        
        if (current_total < fade_start_time_actual) {
            fade_phase = 0;
            current_bright = start_bright;
            current_temp = start_temp;
            int remain_to_fade = fade_start_time_actual - current_total;
            // tkl_log_output("[节律]阶段=渐变前(保持上一节点值 bright=%d temp=%d)\n",
                        //    current_bright, current_temp);
            rhythm_log_countdown("[节律]距渐变开始还剩 ", remain_to_fade);
        } else if (current_total >= next_node_time) {
            fade_phase = 2;
            current_bright = target_bright;
            current_temp = target_temp;
            // tkl_log_output("[节律]阶段=节点后(已达下一节点值 bright=%d temp=%d)\n",
                        //    current_bright, current_temp);
            // tkl_log_output("[节律]下一节点 %02d:%02d 已到达\n", next_node_h, next_node_m);
        } else {
            fade_phase = 1;
            int elapsed_in_fade = current_total - fade_start_time_actual;
            int remain_to_node = next_node_time - current_total;
            float progress = (float)elapsed_in_fade / (float)actual_fade_minutes;
            if (progress > 1.0f) progress = 1.0f;
            
            int16_t cur_bright = (int16_t)start_bright + (int16_t)((target_bright - start_bright) * progress);
            if (cur_bright > 100) cur_bright = 100;
            if (cur_bright < 0) cur_bright = 0;
            
            int16_t cur_temp = (int16_t)start_temp + (int16_t)((target_temp - start_temp) * progress);
            if (cur_temp > 100) cur_temp = 100;
            if (cur_temp < 0) cur_temp = 0;
            
            current_bright = (uint8_t)cur_bright;
            current_temp = (uint8_t)cur_temp;
            // tkl_log_output("[节律]阶段=渐变中 progress=%.2f bright=%d temp=%d\n",
                        //    progress, current_bright, current_temp);
            // tkl_log_output("[节律]渐变已于 %02d:%02d 开始, 已进行%d分钟\n",
                        //    fade_start_h, fade_start_m, elapsed_in_fade);
            rhythm_log_countdown("[节律]距下一节点还剩 ", remain_to_node);
        }

        if (fade_phase == 1 &&
            (sg_last_fade_phase != 1 ||
             sg_last_fade_prev != prev_node_index ||
             sg_last_fade_next != next_node_index)) {
            // tkl_log_output("[节律]>>> 到达渐变开始时刻 %02d:%02d, 开始向节点%d渐变\n",
                        //    fade_start_h, fade_start_m, next_node_index);
        }
        sg_last_fade_phase = fade_phase;
        sg_last_fade_prev = prev_node_index;
        sg_last_fade_next = next_node_index;
    }
    
    // 根据 calculate_only 决定行为
        if (calculate_only) {
        // 仅计算，存入全局变量（只影响当前开着的灯）
        if (sg_demo_info.white_switch) {
            sg_demo_info.white_bright = current_bright;
            sg_demo_info.white_temp = current_temp;
        }
        if (sg_demo_info.aux_switch) {
            sg_demo_info.aux_bright = current_bright;
            sg_demo_info.white_temp = current_temp; // 辅灯色温跟随主灯
        }
        return;
    }
    
    // 正常设置灯光
    uint8_t prev_white_bright = sg_demo_info.white_bright;
    uint8_t prev_white_temp = sg_demo_info.white_temp;
    uint8_t prev_aux_bright = sg_demo_info.aux_bright;

    set_light_state_conditional(current_bright, current_temp);

    if (sg_demo_info.white_bright != prev_white_bright ||
        sg_demo_info.white_temp != prev_white_temp ||
        sg_demo_info.aux_bright != prev_aux_bright) {
        rhythm_persist_light_state(false);
    }
    
    // 更新当前活动节点
    int prev_active_node = sg_last_processed_node;
    if (fade_minutes == 0) {
        bool use_next_node = false;
        if (next_node_time >= 24 * 60) {
            if (current_total >= prev_node_time) {
                use_next_node = false;
            } else {
                use_next_node = (current_total >= (next_node_time - 24 * 60));
            }
        } else {
            use_next_node = (current_total >= next_node_time);
        }
        
        if (use_next_node) {
            sg_current_active_node = next_node_index;
        } else {
            sg_current_active_node = prev_node_index;
        }
    } else {
        if (current_total >= next_node_time) {
            sg_current_active_node = next_node_index;
        } else {
            sg_current_active_node = prev_node_index;
        }
    }

    /* 节律到新节点：打断伴眠/唤醒 */
    if (sg_current_active_node != prev_active_node && prev_active_node != -1) {
        app_light_schedule_before_rhythm_start();
    }
    sg_last_processed_node = sg_current_active_node;
}

// 默认回调
static void my_rhythm_inform_cb(RHYTHM_NODE_DATA_T *node, TM_RHYTHM_STATE_E state)
{
    if (!node) {
        return;
    }
    
    // 只有在主灯或辅灯开启时才执行节律
    if (!is_main_light_on()) {
        return;
    }
    
    // 应用节律设置
    apply_correct_rhythm_settings(false);
}

// 检查是否到了新节点（与打断时的节点不同）
static bool is_new_node_after_interruption(int current_node_idx)
{
    if (sg_interrupted_node == -1) {
        return true;
    }
    
    if (current_node_idx != sg_interrupted_node) {
        return true;
    }
    
    return false;
}

// 检查今天是否在生效日中
static bool is_today_active()
{
    extern POSIX_TM_S local_tm;
    uint8_t current_day_bit = 0;
    
    switch (local_tm.tm_wday) {
        case 0: current_day_bit = (1 << 0); break; // 周日
        case 1: current_day_bit = (1 << 1); break; // 周一
        case 2: current_day_bit = (1 << 2); break; // 周二
        case 3: current_day_bit = (1 << 3); break; // 周三
        case 4: current_day_bit = (1 << 4); break; // 周四
        case 5: current_day_bit = (1 << 5); break; // 周五
        case 6: current_day_bit = (1 << 6); break; // 周六
    }
    
    bool active = (sg_rhythm_info.cfg.active_day_mask & current_day_bit) != 0;
    
    return active;
}

// 获取正确的节点渐变开始时间
// prev_node_time: 上一启用节点的时刻（分钟）。全程渐变从上一节点渐变到本节点，
// 必须用「上一节点→本节点」间隔，不能用「本节点→下一节点」。
int get_node_fade_start_time(RHYTHM_NODE_DATA_T *node, int node_time, int prev_node_time)
{
    // 计算节点间隔（上一节点 → 本节点）
    int node_interval = node_time - prev_node_time;
    if (node_interval < 0) {
        node_interval += 24 * 60;
    }
    
    // 根据渐变类型确定渐变时间
    int fade_minutes = 0;
    switch (node->fade_type) {
        case 0: fade_minutes = 0; break;
        case 1: fade_minutes = node_interval; break;
        case 2: fade_minutes = 15; break;
        case 3: fade_minutes = 30; break;
        case 4: fade_minutes = 45; break;
        case 5: fade_minutes = 60; break;
        default: fade_minutes = 0; break;
    }
    
    // 如果节点间隔小于渐变时间，使用节点间隔
    if (node_interval < fade_minutes) {
        fade_minutes = node_interval;
    }
    
    // 计算渐变开始时间
    int fade_start_time = node_time - fade_minutes;
    if (fade_start_time < 0) {
        fade_start_time += 24 * 60;
    }

    // tkl_log_output("[节律]get_node_fade_start_time: node=%02d:%02d prev=%02d:%02d fade_type=%d(%s) fade=%dmin start=%02d:%02d\n",
                //    node->hour, node->minute,
                //    prev_node_time / 60, prev_node_time % 60,
                //    node->fade_type, rhythm_fade_type_name(node->fade_type),
                //    fade_minutes, fade_start_time / 60, fade_start_time % 60);
    
    return fade_start_time;
}


// 节律线程主函数
static void *rhythm_thread(void *arg)
{
    int last_update_day = -1;
    extern DEMO_INFO_T sg_demo_info;
    extern int8_t sunrise_hour, sunrise_min, sunset_hour, sunset_min;
    extern POSIX_TM_S local_tm;
    extern bool time_synced;
    
    for (;;) {
        if (!rhythm_refresh_local_time()) {
            tal_system_sleep(1000);
            continue;
        }
        
        // 节律总开关关闭时，不执行节律
        if (!sg_demo_info.rhythm_switch) {
            stop_fade();
            rhythm_sleep_until_next_minute();
            continue;
        }
        
        // 检查生效日
        if (!is_today_active()) {
            stop_fade();
            rhythm_sleep_until_next_minute();
            continue;
        }
        
        // 每天更新节点时间（基于日出日落）
        if (sunrise_hour > 0) {
            if (local_tm.tm_mday != last_update_day) {
                for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
                    update_node_time(i, sunrise_hour, sunrise_min, sunset_hour, sunset_min);
                }
                last_update_day = local_tm.tm_mday;
            }
        }
        
        // 如果节律被打断，必须停止所有渐变
        if (sg_rhythm_interrupted) {
            if (sg_fade_state.active) {
                stop_fade();
                // 清理渐变状态
                memset(&sg_fade_state, 0, sizeof(FADE_STATE_T));
            }
        }

        // 处理渐变更新（如果有）
        if (sg_fade_state.active) {
            process_fade_update();
            if (sg_fade_state.is_power_on_recovery) {
                tal_system_sleep(3 * 1000);
            } else {
                rhythm_sleep_until_next_minute();
            }
            continue;
        }
        
        // 执行上电恢复（只在第一次进入时执行）
        if (!sg_power_on_recovered && sg_device_just_booted) {
            handle_power_on_recovery();
            
            // 标记已执行上电恢复检查
            sg_power_on_recovered = true;
            sg_device_just_booted = false;
            
            // 如果启动了上电恢复渐变，跳过正常节律逻辑
            if (sg_fade_state.active && sg_fade_state.is_power_on_recovery) {
                continue;
            }
        }
        
        // 获取当前时间
        extern POSIX_TM_S local_tm;
        int current_total = local_tm.tm_hour * 60 + local_tm.tm_min;
        
        // ============ 节律打断检查 ============
        if (sg_rhythm_interrupted) {
            // 如果渐变正在进行，停止它
            if (sg_fade_state.active) {
                stop_fade();
                memset(&sg_fade_state, 0, sizeof(FADE_STATE_T));
            }

            /* 打断期间到新节点：若伴眠/唤醒仍在跑则完全停止，并在灯仍亮时恢复节律 */
            {
                extern DEMO_INFO_T sg_demo_info;
                extern BOOL_T sg_sleep_is_timing;
                extern BOOL_T sg_wake_is_timing;
                extern int sleep_state;
                int cur_node = get_current_node_index();
                if (cur_node >= 0 && is_new_node_after_interruption(cur_node) &&
                    (sg_sleep_is_timing || sleep_state || sg_wake_is_timing)) {
                    app_light_schedule_before_rhythm_start();
                    if (sg_demo_info.white_switch || sg_demo_info.aux_switch ||
                        sg_demo_info.night_switch) {
                        sg_rhythm_interrupted = false;
                        sg_interrupted_node = -1;
                        sg_interrupt_clock_minutes = -1;
                        sg_current_active_node = cur_node;
                        upload_device_bool_status(RHYTHM_STATUS, 0);
                    }
                }
            }
            
            // 检查是否到了恢复时间
            bool should_recover = false;
            int target_node_idx = -1; // 要渐变到的目标节点
            
            // 获取所有启用节点并按时间排序
            int enabled_nodes[RHYTHM_NODE_COUNT];
            int enabled_count = 0;
            
            for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
                if (sg_rhythm_info.cfg.nodes[i].node_enable) {
                    enabled_nodes[enabled_count++] = i;
                }
            }
            
            if (enabled_count >= 2) {
                // 对节点按时间排序
                for (int i = 0; i < enabled_count - 1; i++) {
                    for (int j = i + 1; j < enabled_count; j++) {
                        int time_i = sg_rhythm_info.cfg.nodes[enabled_nodes[i]].hour * 60 + 
                                   sg_rhythm_info.cfg.nodes[enabled_nodes[i]].minute;
                        int time_j = sg_rhythm_info.cfg.nodes[enabled_nodes[j]].hour * 60 + 
                                   sg_rhythm_info.cfg.nodes[enabled_nodes[j]].minute;
                        if (time_i > time_j) {
                            int temp = enabled_nodes[i];
                            enabled_nodes[i] = enabled_nodes[j];
                            enabled_nodes[j] = temp;
                        }
                    }
                }
                
                // 计算每个节点的渐变开始时间（与自动开灯/正常节律一致：相对上一节点）
                for (int i = 0; i < enabled_count; i++) {
                    int node_idx = enabled_nodes[i];
                    RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[node_idx];
                    
                    int prev_i = (i + enabled_count - 1) % enabled_count;
                    int prev_node_idx = enabled_nodes[prev_i];
                    RHYTHM_NODE_DATA_T *prev_node = &sg_rhythm_info.cfg.nodes[prev_node_idx];
                    
                    int node_time = node->hour * 60 + node->minute;
                    int prev_node_time = prev_node->hour * 60 + prev_node->minute;
                    
                    // 检查当前时间是否刚到达该节点的渐变开始时间
                    int fade_start_time = get_node_fade_start_time(node, node_time, prev_node_time);
                    // tkl_log_output("[节律]打断恢复检查: node=%d 当前=%02d:%02d fade_start=%02d:%02d\n",
                                //    node_idx, current_total / 60, current_total % 60,
                                //    fade_start_time / 60, fade_start_time % 60);

                    // 仅在刚到达渐变起点时恢复（0 或滞后 1 分钟），避免 current>=start 跨夜误判
                    int since_fade_start = rhythm_minutes_forward(fade_start_time, current_total);
                    if (since_fade_start > 1) {
                        continue;
                    }

                    // 打断钟点：优先用打断时记录的本地时间，避免毫秒反推 + 有符号混算出错
                    int interrupt_total = sg_interrupt_clock_minutes;
                    if (interrupt_total < 0) {
                        uint32_t interrupt_ms = sg_last_user_operation_time;
                        uint32_t current_ms = tal_system_get_millisecond();
                        uint32_t elapsed_ms = current_ms - interrupt_ms;
                        int elapsed_minutes = (int)(elapsed_ms / (60 * 1000));
                        interrupt_total = current_total - elapsed_minutes;
                        interrupt_total %= (24 * 60);
                        if (interrupt_total < 0) {
                            interrupt_total += 24 * 60;
                        }
                    }

                    // 打断发生在该渐变起点之前（按 24h 环，从打断正向走到起点 ≤ 走到当前）
                    int to_fade_start = rhythm_minutes_forward(interrupt_total, fade_start_time);
                    int to_current = rhythm_minutes_forward(interrupt_total, current_total);
                    if (to_fade_start > 0 && to_fade_start <= to_current) {
                        should_recover = true;
                        target_node_idx = node_idx;
                        break;
                    }
                }
                
                // 在恢复逻辑中修改：
                if (should_recover && target_node_idx != -1) {
                    extern DEMO_INFO_T sg_demo_info;
                    /* 伴眠等主动关灯后保持打断，勿自动恢复开灯 */
                    if (sg_demo_info.white_switch || sg_demo_info.aux_switch ||
                        sg_demo_info.night_switch) {
                    /* 恢复到新节点前先打断伴眠/唤醒 */
                    app_light_schedule_before_rhythm_start();
                    // 执行恢复逻辑
                    // tkl_log_output("在节点%d的渐变开始时间恢复节律\n", target_node_idx);
                    
                    // 获取目标节点
                    RHYTHM_NODE_DATA_T *target_node = &sg_rhythm_info.cfg.nodes[target_node_idx];
                    
                    // 获取起始状态（当前实际状态）
                    uint8_t start_white_bright = sg_demo_info.white_bright;
                    uint8_t start_white_temp = sg_demo_info.white_temp;
                    uint8_t start_aux_bright = sg_demo_info.aux_bright;
                    uint8_t start_aux_temp = sg_demo_info.white_temp;
                    
                    // 获取目标状态
                    uint8_t target_white_bright = target_node->per_bright;
                    uint8_t target_white_temp = target_node->per_temper;
                    uint8_t target_aux_bright = target_node->per_bright;
                    uint8_t target_aux_temp = target_node->per_temper;
                    
                    // 处理NO_CHANGE_FLAG
                    if (target_white_bright == NO_CHANGE_FLAG || target_white_temp == NO_CHANGE_FLAG) {
                        // 查找目标节点在启用列表中的位置
                        int target_node_pos = -1;
                        for (int i = 0; i < enabled_count; i++) {
                            if (enabled_nodes[i] == target_node_idx) {
                                target_node_pos = i;
                                break;
                            }
                        }
                        
                        if (target_node_pos != -1) {
                            uint8_t prev_non_101_bright, prev_non_101_temp;
                            find_previous_non_101_value(target_node_pos, enabled_nodes, enabled_count, 
                                                    &prev_non_101_bright, &prev_non_101_temp);
                            
                            if (target_white_bright == NO_CHANGE_FLAG) target_white_bright = prev_non_101_bright;
                            if (target_white_temp == NO_CHANGE_FLAG) target_white_temp = prev_non_101_temp;
                            if (target_aux_bright == NO_CHANGE_FLAG) target_aux_bright = prev_non_101_bright;
                            if (target_aux_temp == NO_CHANGE_FLAG) target_aux_temp = prev_non_101_temp;
                        }
                    }
                    
                    // !!! 重要：不要在这里设置灯光状态，直接启动渐变 !!!
                    // 不调用 set_light_state_separate 或 set_light_state_conditional
                    
                    // 计算渐变时长
                    // 找到目标节点的上一个节点（全程渐变 = 上一节点 → 本节点）
                    int target_node_pos = -1;
                    for (int i = 0; i < enabled_count; i++) {
                        if (enabled_nodes[i] == target_node_idx) {
                            target_node_pos = i;
                            break;
                        }
                    }
                    
                    if (target_node_pos != -1) {
                        int prev_target_idx = enabled_nodes[(target_node_pos + enabled_count - 1) % enabled_count];
                        RHYTHM_NODE_DATA_T *prev_target = &sg_rhythm_info.cfg.nodes[prev_target_idx];
                        
                        int target_node_time = target_node->hour * 60 + target_node->minute;
                        int prev_target_time = prev_target->hour * 60 + prev_target->minute;
                        
                        // 计算节点间隔（上一节点 → 目标节点）
                        int target_interval = target_node_time - prev_target_time;
                        if (target_interval < 0) target_interval += 24 * 60;
                        
                        // 根据目标节点的渐变类型确定渐变时间
                        int target_fade_minutes = 0;
                        switch (target_node->fade_type) {
                            case 0: target_fade_minutes = 0; break;
                            case 1: target_fade_minutes = target_interval; break;
                            case 2: target_fade_minutes = 15; break;
                            case 3: target_fade_minutes = 30; break;
                            case 4: target_fade_minutes = 45; break;
                            case 5: target_fade_minutes = 60; break;
                            default: target_fade_minutes = 0; break;
                        }
                        
                        if (target_interval < target_fade_minutes) {
                            target_fade_minutes = target_interval;
                        }
                        
                        // 启动渐变
                        if (target_fade_minutes > 0) {
                            uint32_t duration_ms = target_fade_minutes * 60 * 1000;
                            
                            // tkl_log_output("恢复渐变：从当前状态向节点%d渐变\n", target_node_idx);
                            // tkl_log_output("起始值：亮度=%d, 色温=%d\n", start_white_bright, start_white_temp);
                            // tkl_log_output("目标值：亮度=%d, 色温=%d\n", target_white_bright, target_white_temp);
                            // tkl_log_output("渐变时长：%d分钟\n", target_fade_minutes);
                            
                            start_fade(false, 
                                    start_white_bright, start_white_temp,
                                    start_aux_bright, start_aux_temp,
                                    target_white_bright, target_white_temp,
                                    target_aux_bright, target_aux_temp,
                                    duration_ms, 10);
                            rhythm_persist_light_state(false);
                        } else {
                            // 立即变化 - 只设置目标值
                            // tkl_log_output("立即设置到节点%d的值\n", target_node_idx);
                            set_light_state_separate(
                                target_white_bright, target_white_temp,
                                target_aux_bright, target_aux_temp
                            );
                            rhythm_persist_light_state(true);
                        }
                    }
                    
                    // 恢复节律状态
                    sg_rhythm_interrupted = false;
                    sg_interrupted_node = -1;
                    sg_interrupt_clock_minutes = -1;
                    sg_current_active_node = target_node_idx;
                    upload_device_bool_status(RHYTHM_STATUS, 0);
                    upload_device_enum_status(DPID_WORK_MODE, 16);
                    
                    // tkl_log_output("节律状态已恢复，打断标志已清除\n");
                    }
                }
            }
        }
        
        // ============ 核心逻辑 ============
        // tkl_log_output("[节律]线程轮询: %02d:%02d:%02d interrupted=%d fade_active=%d\n",
                    //    local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
                    //    sg_rhythm_interrupted, sg_fade_state.active);

        // 1. 检查自动开灯
        bool auto_light_triggered = check_and_perform_auto_light_on();
        if (auto_light_triggered) {
            // tkl_log_output("[节律]已触发自动开灯，跳过本轮 apply_correct_rhythm_settings\n");
        }

        // 2. 只有在非打断状态且没有触发自动开灯时才执行节律设置
        if (!sg_rhythm_interrupted && !auto_light_triggered) {
            if (app_light_schedule_should_skip_rhythm_at_minute(local_tm.tm_hour, local_tm.tm_min)) {
                rhythm_sleep_until_next_minute();
                continue;
            }
            if (sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch) {
                // tkl_log_output("[节律]执行 apply_correct_rhythm_settings\n");
                apply_correct_rhythm_settings(false);
            }
        }

        // 3. 处理渐变更新（如果有）
        if (sg_fade_state.active) {
            process_fade_update();
        }
        
        rhythm_sleep_until_next_minute();
    }
    return NULL;
}

// 计算节点的渐变开始时间
static int calculate_node_fade_start_time(int node_idx, int enabled_nodes[], int enabled_count)
{
    if (node_idx < 0 || node_idx >= RHYTHM_NODE_COUNT) return -1;
    
    RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[node_idx];
    int node_time = node->hour * 60 + node->minute;
    
    // 查找节点在启用列表中的位置
    int node_pos = -1;
    for (int i = 0; i < enabled_count; i++) {
        if (enabled_nodes[i] == node_idx) {
            node_pos = i;
            break;
        }
    }
    
    if (node_pos == -1) return -1;
    
    // 获取上一节点（全程渐变相对上一节点）
    int prev_node_idx = enabled_nodes[(node_pos + enabled_count - 1) % enabled_count];
    RHYTHM_NODE_DATA_T *prev_node = &sg_rhythm_info.cfg.nodes[prev_node_idx];
    int prev_node_time = prev_node->hour * 60 + prev_node->minute;
    
    return get_node_fade_start_time(node, node_time, prev_node_time);
}

// 记录中断时的节点和时间
// 停止今天的节律
void app_light_stop_today_rhythm_timer(void)
{
    extern DEMO_INFO_T sg_demo_info;
    
    // 停止所有节律计时和渐变
    sg_rhythm_interrupted = true;
    
    // 记录打断时的当前活动节点
    sg_interrupted_node = sg_current_active_node;
    
    // 记录打断时间（毫秒戳 + 本地钟点，供跨夜恢复判断）
    sg_last_user_operation_time = tal_system_get_millisecond();
    {
        extern POSIX_TM_S local_tm;
        if (rhythm_refresh_local_time()) {
            sg_interrupt_clock_minutes = local_tm.tm_hour * 60 + local_tm.tm_min;
        } else {
            sg_interrupt_clock_minutes = -1;
        }
    }
    sg_user_interrupted_rhythm = true;
    
    // 停止所有渐变
    if (sg_fade_state.active) {
        stop_fade();
        memset(&sg_fade_state, 0, sizeof(FADE_STATE_T));
    }
    
    // 上报节律状态为中断
    upload_device_bool_status(RHYTHM_STATUS, 1);
    
    // tkl_log_output("节律被打断，当前活动节点：%d，打断时间：%lu\n", sg_current_active_node, sg_last_user_operation_time);
}

// 初始化节律功能
int app_light_tm_rhythm_init(TM_RHYTHM_INFORM_CB inform_cb)
{
    extern DEMO_INFO_T sg_demo_info;
    memset(&sg_rhythm_info, 0, sizeof(sg_rhythm_info));
    sg_rhythm_info.inform_cb = inform_cb ? inform_cb : my_rhythm_inform_cb;

    sg_power_on_recovered = false;
    stop_fade();
    sg_device_just_booted = true;
    sg_last_user_operation_time = tal_system_get_millisecond();
    /* 短断电可能已打断节律，初始化时不要清掉 */
    if (!sg_rhythm_interrupted) {
        sg_user_interrupted_rhythm = false;
    }

    // 解析DP数据
    LIG_TM_RHYTHM_CFG_T *cfg = NULL;
    if (app_light_tm_rhythm_dp_analysis(sg_demo_info.rhythm_sunlight,
                                        RHYTHM_RAW_DATA_LEN, &cfg) == 0) {
        app_light_set_rhythm_timer(cfg);
        free(cfg);
    }

    if (sg_demo_info.rhythm_switch) {
        sg_demo_info.rhythm_switch = 1;
    } else {
        sg_demo_info.rhythm_switch = 0;
    }

    // 创建节律线程
    const THREAD_CFG_T thread_cfg = {
        .thrdname = "rhythm_thread",
        .stackDepth = 4 * 1024,
        .priority = THREAD_PRIO_4,
    };
    tal_thread_create_and_start(&rhythm_thread_id, NULL, NULL, rhythm_thread, NULL, &thread_cfg);

    return 0;
}

// 设置节律定时器
void app_light_set_rhythm_timer(LIG_TM_RHYTHM_CFG_T *p_cfg)
{
    if (!p_cfg) return;
    
    memcpy(&sg_rhythm_info.cfg, p_cfg, sizeof(LIG_TM_RHYTHM_CFG_T));
}

// DP数据分析
int app_light_tm_rhythm_dp_analysis(uint8_t *p_dp, uint32_t dp_len,
                                    LIG_TM_RHYTHM_CFG_T **p_rhythm_cfg)
{
    if (!p_dp || !p_rhythm_cfg || dp_len != RHYTHM_RAW_DATA_LEN)
        return -1;

    LIG_TM_RHYTHM_CFG_T *cfg = (LIG_TM_RHYTHM_CFG_T *)malloc(sizeof(LIG_TM_RHYTHM_CFG_T));
    if (!cfg) return -2;
    memset(cfg, 0, sizeof(*cfg));

    cfg->mode = p_dp[0];
    cfg->active_day_mask = p_dp[1];

    for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
        uint8_t *node_data = &p_dp[2 + i * 8];
        node = &cfg->nodes[i];
        node->node_enable = node_data[0];
        node->hour = node_data[1];
        node->minute = node_data[2];
        node->per_bright = node_data[3];
        node->per_temper = node_data[4];
        node->fade_type = node_data[5];
        node->auto_light_on = node_data[6];
        node->time_type = node_data[7];
    }

    *p_rhythm_cfg = cfg;

    // 更新节点时间（只有在有日出日落时间时才更新）
    extern int8_t sunrise_hour, sunrise_min, sunset_hour, sunset_min;
    if (sunrise_hour >= 0) {
        for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
            update_node_time(i, sunrise_hour, sunrise_min, sunset_hour, sunset_min);
        }
    }

    memcpy(&sg_rhythm_info.cfg, cfg, sizeof(LIG_TM_RHYTHM_CFG_T));
    return 0;
}

// 更新节点时间（基于日出日落）
void update_node_time(uint8_t node_index, int8_t sunrise_hour, int8_t sunrise_min,
                             int8_t sunset_hour, int8_t sunset_min)
{
    node = &sg_rhythm_info.cfg.nodes[node_index];
    if (!node->node_enable || node->time_type == 0) return;

    int old_hour = node->hour;
    int old_minute = node->minute;
    switch (node->time_type) {
        case 1: // 日出前10分钟
            node->hour = sunrise_hour;
            node->minute = (sunrise_min - 10 + 60) % 60;
            if (sunrise_min < 10) {
                node->hour = (sunrise_hour - 1 + 24) % 24;
            }
            break;
        case 2: // 日出后1小时
            node->hour = (sunrise_hour + 1) % 24;
            node->minute = sunrise_min;
            break;
        case 3: // 正午（日出+（日落-日出）/2）
            {
                int sunrise_total = sunrise_hour * 60 + sunrise_min;
                int sunset_total = sunset_hour * 60 + sunset_min;
                int noon_total = sunrise_total + (sunset_total - sunrise_total) / 2;
                node->hour = noon_total / 60;
                node->minute = noon_total % 60;
            }
            break;
        case 4: // 日落前2小时
            node->hour = (sunset_hour - 2 + 24) % 24;
            node->minute = sunset_min;
            break;
        case 5: // 日落后10分钟
            node->hour = sunset_hour;
            node->minute = (sunset_min + 10) % 60;
            if (sunset_min >= 50) {
                node->hour = (sunset_hour + 1) % 24;
            }
            break;
        case 6: // 睡觉时间
            node->hour = 23;
            node->minute = 0;
            break;
        default:
            break;
    }
}

// 获取当前时间对应的节律节点索引
int get_current_node_index(void)
{
    extern POSIX_TM_S local_tm;
    int current_total = local_tm.tm_hour * 60 + local_tm.tm_min;
    int latest_node_idx = -1;
    int latest_node_time = -1;
    
    // 查找最后一个已过时间的启用节点
    for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
        if (sg_rhythm_info.cfg.nodes[i].node_enable) {
            RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[i];
            int node_time = node->hour * 60 + node->minute;
            
            // 这个节点已经过了当前时间
            if (node_time <= current_total) {
                // 找到最接近当前时间的已过节点
                if (node_time > latest_node_time) {
                    latest_node_time = node_time;
                    latest_node_idx = i;
                }
            }
        }
    }
    
    // 如果没有找到已过时间的节点，使用最后一个节点（考虑跨天）
    if (latest_node_idx == -1) {
        for (int i = RHYTHM_NODE_COUNT - 1; i >= 0; i--) {
            if (sg_rhythm_info.cfg.nodes[i].node_enable) {
                latest_node_idx = i;
                latest_node_time = sg_rhythm_info.cfg.nodes[i].hour * 60 + 
                                 sg_rhythm_info.cfg.nodes[i].minute;
                break;
            }
        }
    }
    return latest_node_idx;
}


bool app_light_rhythm_expect_bright_zero_now(void)
{
    extern DEMO_INFO_T sg_demo_info;

    if (!sg_demo_info.rhythm_switch) {
        return false;
    }

    int enabled_nodes[RHYTHM_NODE_COUNT];
    int enabled_count = 0;
    for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
        if (sg_rhythm_info.cfg.nodes[i].node_enable) {
            enabled_nodes[enabled_count++] = i;
        }
    }
    if (enabled_count <= 0) {
        return false;
    }

    int cur = get_current_node_index();
    if (cur < 0) {
        return false;
    }

    int pos = -1;
    for (int i = 0; i < enabled_count; i++) {
        if (enabled_nodes[i] == cur) {
            pos = i;
            break;
        }
    }
    if (pos < 0) {
        return false;
    }

    uint8_t bright = sg_rhythm_info.cfg.nodes[cur].per_bright;
    uint8_t temper = sg_rhythm_info.cfg.nodes[cur].per_temper;
    if (bright == NO_CHANGE_FLAG || temper == NO_CHANGE_FLAG) {
        uint8_t prev_bright = NO_CHANGE_FLAG;
        uint8_t prev_temp = NO_CHANGE_FLAG;
        find_previous_non_101_value(pos, enabled_nodes, enabled_count, &prev_bright, &prev_temp);
        if (bright == NO_CHANGE_FLAG) {
            bright = prev_bright;
        }
    }
    return (bright == 0);
}

void app_light_rhythm_interrupt_on_user_change(bool actually_changed, RHYTHM_USER_OP_E op)
{
    extern DEMO_INFO_T sg_demo_info;

    /* 关灯：不打断节律。三路/全部灭：停节律渐变；五路仅关主灯且辅灯仍亮：不停，辅灯继续渐变 */
    if (op == RHYTHM_USER_OP_LIGHT_OFF) {
        if (!actually_changed) {
            return;
        }
        if (!(sg_demo_info.white_switch || sg_demo_info.aux_switch)) {
            stop_fade();
        }
        return;
    }

    if (!actually_changed) {
        return;
    }
    if (!sg_demo_info.rhythm_switch || sg_rhythm_interrupted) {
        return;
    }

    /* 调光/调色：开着灯且真变化才打断；开灯：仅当前时段期望亮度为0时打断 */
    if (op == RHYTHM_USER_OP_DIM_TEMP) {
        if (!(sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch)) {
            return;
        }
    } else if (op == RHYTHM_USER_OP_LIGHT_ON) {
        if (!app_light_rhythm_expect_bright_zero_now()) {
            return;
        }
    } else {
        return;
    }

    app_light_stop_today_rhythm_timer();
}

bool app_light_rhythm_should_open_main_for_running(void)
{
    extern DEMO_INFO_T sg_demo_info;

    if (!sg_demo_info.rhythm_switch || sg_rhythm_interrupted) {
        return false;
    }
    return !app_light_rhythm_expect_bright_zero_now();
}

void app_light_rhythm_interrupt_on_night_open(void)
{
    extern DEMO_INFO_T sg_demo_info;

    if (!sg_demo_info.rhythm_switch || sg_rhythm_interrupted) {
        return;
    }
    app_light_stop_today_rhythm_timer();
}


// 恢复今天的节律
void app_light_resume_today_rhythm_timer(void)
{
    extern DEMO_INFO_T sg_demo_info;
    sg_rhythm_interrupted = false;
    sg_interrupted_node = -1;
    sg_interrupt_clock_minutes = -1;
    
    // 只要有任何灯开启就恢复节律
    if (sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch) {
        apply_correct_rhythm_settings(false);
        upload_device_bool_status(RHYTHM_STATUS, 0);
        upload_device_enum_status(DPID_WORK_MODE, 16);
    }
}

// 同步节律
void app_light_tm_rhythm_syn(void)
{
    extern DEMO_INFO_T sg_demo_info;
    if (!sg_demo_info.rhythm_switch || sg_rhythm_interrupted || 
        !(sg_demo_info.white_switch || sg_demo_info.aux_switch || sg_demo_info.night_switch)) {
        return;
    }
    
    // 强制重新计算当前节点
    int current_node_idx = get_current_node_index();
    if (current_node_idx >= 0) {
        node = &sg_rhythm_info.cfg.nodes[current_node_idx];
        if (sg_rhythm_info.inform_cb) {
            sg_rhythm_info.inform_cb(node, TM_RHYTHM_STATE_RUNNING);
        }
        sg_current_active_node = current_node_idx;
        sg_last_processed_node = current_node_idx;
    }

    /* 开灯后立刻按当前节律亮/色温落到已开的灯（先停节律渐变，避免旧渐变盖掉） */
    stop_fade();
    apply_correct_rhythm_settings(false);
}

// 统一的自动开灯检查函数
bool check_and_perform_auto_light_on()
{
    extern DEMO_INFO_T sg_demo_info;
    extern POSIX_TM_S local_tm;
    extern TM_RHYTHM_INFO_T sg_rhythm_info;

    /* 伴眠/手动打断后灯已关：禁止节律自动开灯，直到用户再开 */
    if (sg_rhythm_interrupted) {
        return false;
    }
    
    // 如果已经是亮灯状态，不再检查
    if(sg_demo_info.white_switch == 0 && sg_demo_info.aux_switch == 0&&sg_demo_info.night_switch == 0)
    {
        sg_demo_info.switch_status = 0;
        upload_device_bool_status(DPID_SWITCH, 0);
    }
    if (sg_demo_info.white_switch || sg_demo_info.aux_switch) {
        return false;
    }
    
    // 节律开关关闭，不检查
    if (!sg_demo_info.rhythm_switch) {
        return false;
    }
    
    // 获取当前时间
    int current_total = local_tm.tm_hour * 60 + local_tm.tm_min;
    
    // 查找所有启用的节点
    int enabled_nodes[RHYTHM_NODE_COUNT];
    int enabled_count = 0;
    for (int i = 0; i < RHYTHM_NODE_COUNT; i++) {
        if (sg_rhythm_info.cfg.nodes[i].node_enable) {
            enabled_nodes[enabled_count++] = i;
        }
    }
    
    if (enabled_count < 2) {
        return false;
    }
    
    // 对启用节点按时间排序
    for (int i = 0; i < enabled_count - 1; i++) {
        for (int j = i + 1; j < enabled_count; j++) {
            int time_i = sg_rhythm_info.cfg.nodes[enabled_nodes[i]].hour * 60 + 
                       sg_rhythm_info.cfg.nodes[enabled_nodes[i]].minute;
            int time_j = sg_rhythm_info.cfg.nodes[enabled_nodes[j]].hour * 60 + 
                       sg_rhythm_info.cfg.nodes[enabled_nodes[j]].minute;
            if (time_i > time_j) {
                int temp = enabled_nodes[i];
                enabled_nodes[i] = enabled_nodes[j];
                enabled_nodes[j] = temp;
            }
        }
    }
    
    // 遍历所有节点，检查是否需要自动开灯
    for (int i = 0; i < enabled_count; i++) {
        RHYTHM_NODE_DATA_T *node = &sg_rhythm_info.cfg.nodes[enabled_nodes[i]];
        
        if (node->auto_light_on == 1) {
            int node_time = node->hour * 60 + node->minute;
            
            // 获取前一个节点
            int prev_node_index;
            int prev_node_time;
            if (i == 0) {
                prev_node_index = enabled_nodes[enabled_count - 1];
                prev_node_time = sg_rhythm_info.cfg.nodes[prev_node_index].hour * 60 + 
                               sg_rhythm_info.cfg.nodes[prev_node_index].minute - 24 * 60;
            } else {
                prev_node_index = enabled_nodes[i - 1];
                prev_node_time = sg_rhythm_info.cfg.nodes[prev_node_index].hour * 60 + 
                               sg_rhythm_info.cfg.nodes[prev_node_index].minute;
            }
            
            int auto_on_time;
            int actual_fade_minutes;

            if (node->fade_type == 1) {
                // 全程渐变：从上一节点下一分钟开始开灯，渐变到当前节点
                int prev_actual_time = sg_rhythm_info.cfg.nodes[prev_node_index].hour * 60 +
                                       sg_rhythm_info.cfg.nodes[prev_node_index].minute;
                auto_on_time = prev_actual_time + 1;
                actual_fade_minutes = node_time - auto_on_time;
                if (actual_fade_minutes <= 0) {
                    actual_fade_minutes += 24 * 60;
                }
            } else {
                int fade_minutes = 0;
                switch (node->fade_type) {
                    case 0: fade_minutes = 0; break;
                    case 2: fade_minutes = 15; break;
                    case 3: fade_minutes = 30; break;
                    case 4: fade_minutes = 45; break;
                    case 5: fade_minutes = 60; break;
                    default: fade_minutes = 0; break;
                }

                int expected_auto_on_time = node_time - fade_minutes;
                if (expected_auto_on_time < 0) {
                    expected_auto_on_time += 24 * 60;
                }

                auto_on_time = expected_auto_on_time;
                actual_fade_minutes = fade_minutes;
                if (expected_auto_on_time <= prev_node_time) {
                    auto_on_time = prev_node_time + 1;
                    actual_fade_minutes = node_time - auto_on_time;
                    if (actual_fade_minutes <= 0) {
                        actual_fade_minutes += 24 * 60;
                    }
                }

                if (auto_on_time > node_time) {
                    continue;
                }
            }
            
            // 检查当前时间是否在自动开灯时间±1分钟内
            int time_diff = current_total - auto_on_time;
            if(time_diff > 12 * 60) time_diff -= 24 * 60;
            else if(time_diff < -12 * 60) time_diff += 24 * 60;

            // tkl_log_output("[节律]自动开灯检查: node=%d(%02d:%02d) fade=%dmin auto_on=%02d:%02d 当前=%02d:%02d diff=%d\n",
                        //    enabled_nodes[i], node->hour, node->minute, fade_minutes,
                        //    auto_on_time / 60, auto_on_time % 60,
                        //    current_total / 60, current_total % 60, time_diff);
            
            // 只允许在当前时间等于auto_on_time或提前1分钟时触发
            if (time_diff == 0 || time_diff == -1) {
                // tkl_log_output("[节律]>>> 触发自动开灯: node=%d 渐变时长=%d min\n",
                            //    enabled_nodes[i], actual_fade_minutes); 
                // 如果夜灯开启，先关闭夜灯
                if (sg_demo_info.night_switch) {
                    sg_demo_info.night_switch = 0;
                    upload_device_bool_status(DPID_SWITCH_NIGHT_LIGHT, 0);
                    if ((sg_demo_info.white_switch == 0) && (sg_demo_info.aux_switch == 0) && 
                        (sg_demo_info.night_switch == 0)) {
                        sg_demo_info.switch_status = 0;
                        upload_device_bool_status(DPID_SWITCH, 0);
                    }

                    pwm_gradual_duty_set(NIGHT_BRIGHT_PWM, 0);
                }
                
                // 执行自动开灯，传入实际渐变时间
                auto_light_on_action(node, i, enabled_nodes, enabled_count, actual_fade_minutes);

                /* 自动开灯对应新节点，打断伴眠/唤醒 */
                app_light_schedule_before_rhythm_start();
                
                // 恢复节律状态
                sg_rhythm_interrupted = false;
                sg_interrupted_node = -1;
                sg_interrupt_clock_minutes = -1;
                upload_device_bool_status(RHYTHM_STATUS, 0);
                upload_device_enum_status(DPID_WORK_MODE, 16);
                return true;
            }
        }
    }
    
    return false;
}