/*
 * Zratro OS - Power Management Header
 * 
 * ACPI 电源管理和 CPU 省电功能
 */

#ifndef POWER_H
#define POWER_H

#include <stdint.h>
#include <stddef.h>

/* ========== 初始化 ========== */

/* 初始化电源管理 */
int power_init(void);

/* 检查 CPU 电源特性 */
void check_cpu_power_features(void);

/* ========== P-State (性能状态) ========== */

/* 设置 P-State */
int power_set_pstate(int state);

/* 获取当前 P-State */
int power_get_pstate(void);

/* 自动调整 P-State (基于负载) */
void power_adjust_pstate(void);

/* ========== C-State (空闲状态) ========== */

/* 进入 C-State */
void power_enter_cstate(int state);

/* 选择最佳 C-State */
int power_select_cstate(void);

/* 空闲循环 */
void power_idle(void);

/* ========== T-State (限流状态) ========== */

/* 设置 T-State */
int power_set_tstate(int state);

/* ========== 系统睡眠状态 ========== */

/* 进入 S1 (待机) */
int power_enter_s1(void);

/* 进入 S3 (挂起到内存) */
int power_enter_s3(void);

/* 进入 S4 (挂起到磁盘) */
int power_enter_s4(void);

/* 进入 S5 (软关机) */
int power_enter_s5(void);

/* ========== 系统关机/重启 ========== */

/* 系统关机 */
void power_off(void);

/* 系统重启 */
void power_reboot(void);

/* ========== 统计 ========== */

/* 打印电源统计 */
void power_stats(void);

#endif /* POWER_H */
