#ifndef UTSM_GDT_H
#define UTSM_GDT_H

#include <utsm/types.h>

/*
 * R5：内核自持 GDT。
 *
 * 历史上 GDT 由引导器（CODE/boot/main.c）建立并一直沿用——引导器映像之后
 * 不再需要，但 AP 启动与多核一致性要求内核自己拥有并安装一份已知 GDT。
 *
 * 内容：0x00 null、0x08 64 位代码（DPL0）、0x10 flat 数据（DPL0）、
 *       0x28 64 位 TSS。
 * SAS-R0 全程 Ring0、无用户栈切换、IDT 无 IST，TSS 本身不参与运行；
 * 但 VMX host-state 要求 host TR 指向**当前 GDTR 范围内**的有效 TSS
 * 描述符，所以 gdt_load_this_cpu() 每次都会装好描述符并 ltr。
 * （早期版本不装 TSS、TR 沿用固件选择子，导致离表 → vmlaunch 报
 *   "VM entry with invalid host-state fields" error 0x8。）
 *
 * 多核：GDT 与 TSS 都是 **per-CPU**。LTR 会把 TSS 描述符的 busy 位置 1，
 * 若所有 CPU 共用同一份描述符，第二个 CPU 再 ltr 就是 "TSS busy" →
 * #GP(0x28)（实机表现为启动即 UTSM-EXC0D）。因此每个 CPU 用自己的一份
 * GDT（选择子编号仍相同）与自己的 TSS，且只 ltr 一次。
 *
 * 注意：64 位模式下 mov 到 gs 选择子会把 GS base 重置为 0，因此本模块
 * 不重载 GS；per-CPU GS_BASE 由 utsm_percpu_set_self 用 wrmsr 设置。
 */

void gdt_init(void);          /* BSP 安装并重载 CS/SS/DS/ES/FS */
void gdt_load_this_cpu(void); /* AP 入口调用，幂等 */

#endif /* UTSM_GDT_H */
