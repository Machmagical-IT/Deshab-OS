/*
 * boot_diag.h - 启动诊断和自检模块头文件
 */

#ifndef BOOT_DIAG_H
#define BOOT_DIAG_H

/* 执行并打印诊断报告 */
void boot_print_diag_report(void);

/* 获取诊断结果: 1=全部通过, 0=有失败项 */
int boot_diag_get_result(void);

#endif /* BOOT_DIAG_H */
