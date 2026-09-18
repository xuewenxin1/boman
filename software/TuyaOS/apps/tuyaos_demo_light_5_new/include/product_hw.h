/**
 * @file product_hw.h
 * @brief 5_new 机型：PID + PIN14/16 大小档负载系数（不考虑校准系数时的占空比%）
 *
 * 烧录前只改 LIGHT_HW_SHAPE 一处即可：
 *   LIGHT_HW_SQUARE — 超薄方形  PID awvabsknifonseve  主小81% 辅小49%
 *   LIGHT_HW_ARC    — 飞碟弧形  PID 6w506oozgioq3adi  主小76% 辅小60%
 *
 * PIN14 高=主大档100%，低=主小档；PIN16 高=辅大档100%，低=辅小档。
 */
#ifndef __PRODUCT_HW_H__
#define __PRODUCT_HW_H__

#define LIGHT_HW_SQUARE  0
#define LIGHT_HW_ARC     1

/* ★ 改机型只改这里 */
#ifndef LIGHT_HW_SHAPE
#define LIGHT_HW_SHAPE   LIGHT_HW_SQUARE
#endif

#if (LIGHT_HW_SHAPE == LIGHT_HW_SQUARE)
#define PID                      "awvabsknifonseve"
#define LIGHT_MAIN_LOAD_HIGH     100
#define LIGHT_MAIN_LOAD_LOW      81
#define LIGHT_AUX_LOAD_HIGH      100
#define LIGHT_AUX_LOAD_LOW       49
#define LIGHT_HW_NAME            "square"
#elif (LIGHT_HW_SHAPE == LIGHT_HW_ARC)
#define PID                      "6w506oozgioq3adi"
#define LIGHT_MAIN_LOAD_HIGH     100
#define LIGHT_MAIN_LOAD_LOW      76
#define LIGHT_AUX_LOAD_HIGH      100
#define LIGHT_AUX_LOAD_LOW       60
#define LIGHT_HW_NAME            "arc"
#else
#error "LIGHT_HW_SHAPE must be LIGHT_HW_SQUARE or LIGHT_HW_ARC"
#endif

#endif /* __PRODUCT_HW_H__ */
