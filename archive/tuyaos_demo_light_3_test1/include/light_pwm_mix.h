/**
 * @file light_pwm_mix.h
 * @brief 双色温混光限幅
 *
 * Lighting_modification_0.3%（宏名 Lighting_modification_0_3pct）:
 *   1 = 非关灯时弱侧逻辑 PWM 最小 LIGHT_PWM_RETAIN_MIN（默认万分之5）
 *       边界限幅配对为 9995/5（校准系数约80%时硬件弱侧仍>=万分之3）
 *   0 = 保留原先 9997/3 边界限幅，不做弱侧补值
 */
#ifndef __LIGHT_PWM_MIX_H__
#define __LIGHT_PWM_MIX_H__

#include <stdint.h>

#ifndef Lighting_modification_0_3pct
#define Lighting_modification_0_3pct 1
#endif

#define LIGHT_PWM_FULL_RANGE       10000
#define LIGHT_PWM_RETAIN_MIN          5   /* 弱侧逻辑最小值；8000*5/10000=4 >= 3 */
#define LIGHT_PWM_BOUNDARY_MIN_OLD    3
#define LIGHT_PWM_BOUNDARY_MAX_OLD 9997

#if Lighting_modification_0_3pct
#define LIGHT_PWM_BOUNDARY_MIN  LIGHT_PWM_RETAIN_MIN
#define LIGHT_PWM_BOUNDARY_MAX  (LIGHT_PWM_FULL_RANGE - LIGHT_PWM_BOUNDARY_MIN)
#else
#define LIGHT_PWM_BOUNDARY_MIN  LIGHT_PWM_BOUNDARY_MIN_OLD
#define LIGHT_PWM_BOUNDARY_MAX  LIGHT_PWM_BOUNDARY_MAX_OLD
#endif

/* 配网/闪烁等写死五路占空比：主WW, 主CW, 辅WW, 辅CW, 夜灯 */
#define LIGHT_PWM_BLINK_DUTIES5_INIT \
    { LIGHT_PWM_BOUNDARY_MAX, LIGHT_PWM_BOUNDARY_MIN, \
      LIGHT_PWM_BOUNDARY_MAX, LIGHT_PWM_BOUNDARY_MIN, 0 }

static inline void light_pwm_clamp_mix(uint16_t *ww, uint16_t *cw)
{
    if (!ww || !cw) {
        return;
    }

    if (*ww == 0 && *cw == 0) {
        return;
    }

#if Lighting_modification_0_3pct
    if (*ww == 0 && *cw > 0) {
        *ww = LIGHT_PWM_RETAIN_MIN;
    }
    if (*cw == 0 && *ww > 0) {
        *cw = LIGHT_PWM_RETAIN_MIN;
    }
#endif

    if (*ww >= LIGHT_PWM_BOUNDARY_MAX) {
        *ww = LIGHT_PWM_BOUNDARY_MAX;
        *cw = LIGHT_PWM_BOUNDARY_MIN;
    }
    if (*cw >= LIGHT_PWM_BOUNDARY_MAX) {
        *cw = LIGHT_PWM_BOUNDARY_MAX;
        *ww = LIGHT_PWM_BOUNDARY_MIN;
    }
}

#endif /* __LIGHT_PWM_MIX_H__ */
