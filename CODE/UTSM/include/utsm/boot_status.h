#ifndef UTSM_BOOT_STATUS_H
#define UTSM_BOOT_STATUS_H

/* 深蓝色初始化画面：正中「我们正在初始化你的系统」+ 转动的小圆点。
 * start 画整屏；pump 按 TSC 推进圆点（由日志路径调用，初始化期间持续转动）；
 * dismiss 在会话 Logo 接管前停掉，避免画到 Logo 上。 */
void boot_status_start(void);
void boot_status_pump(void);
void boot_status_spawn(void);   /* 最高优先级转圈任务 + 打开抢占 */
void boot_status_register(void);
void boot_status_dismiss(void);

#endif
