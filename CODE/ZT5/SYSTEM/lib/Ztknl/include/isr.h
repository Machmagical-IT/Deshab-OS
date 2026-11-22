#ifndef ISR_H
#define ISR_H

#include "idt.h"

/* 异常处理函数声明 */
void isr_handler_divide_error(struct interrupt_frame *frame);
void isr_handler_debug(struct interrupt_frame *frame);
void isr_handler_nmi(struct interrupt_frame *frame);
void isr_handler_breakpoint(struct interrupt_frame *frame);
void isr_handler_overflow(struct interrupt_frame *frame);
void isr_handler_bound_range(struct interrupt_frame *frame);
void isr_handler_invalid_opcode(struct interrupt_frame *frame);
void isr_handler_device_not_available(struct interrupt_frame *frame);
void isr_handler_double_fault(struct interrupt_frame *frame);
void isr_handler_invalid_tss(struct interrupt_frame *frame);
void isr_handler_segment_not_present(struct interrupt_frame *frame);
void isr_handler_stack_segment_fault(struct interrupt_frame *frame);
void isr_handler_general_protection(struct interrupt_frame *frame);
void isr_handler_page_fault(struct interrupt_frame *frame);
void isr_handler_fpu_error(struct interrupt_frame *frame);
void isr_handler_alignment_check(struct interrupt_frame *frame);
void isr_handler_machine_check(struct interrupt_frame *frame);
void isr_handler_simd_exception(struct interrupt_frame *frame);

#endif
