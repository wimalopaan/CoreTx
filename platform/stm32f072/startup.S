/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "platform/stm32f072/measurements.hpp"

.syntax unified
.cpu cortex-m0
.thumb

.section .isr_vector, "a", %progbits
.global vector_table
.type vector_table, %object
vector_table:
    .word __stack_top
    .word Reset_Handler
    .word Default_Handler
    .word Default_Handler
    .rept 7
    .word 0
    .endr
    .word Default_Handler
    .word 0
    .word 0
    .word Default_Handler
    .word Default_Handler
    .rept 6
    .word Default_Handler
    .endr
    .word EXTI2_3_IRQHandler
    .rept 8
    .word Default_Handler
    .endr
    .word TIM2_IRQHandler
    .rept 16
    .word Default_Handler
    .endr
.size vector_table, . - vector_table

.weak TIM2_IRQHandler
.thumb_set TIM2_IRQHandler, Default_Handler
.weak EXTI2_3_IRQHandler
.thumb_set EXTI2_3_IRQHandler, Default_Handler

.section .text.Reset_Handler, "ax", %progbits
.global Reset_Handler
.type Reset_Handler, %function
.thumb_func
Reset_Handler:
    ldr r0, =__stack_top
    mov sp, r0
    ldr r0, =__data_start
    ldr r1, =__data_end
    ldr r2, =__data_load
1:
    cmp r0, r1
    bhs 2f
    ldr r3, [r2]
    str r3, [r0]
    adds r0, #4
    adds r2, #4
    b 1b
2:
    ldr r0, =__bss_start
    ldr r1, =__bss_end
    movs r2, #0
3:
    cmp r0, r1
    bhs 4f
    str r2, [r0]
    adds r0, #4
    b 3b
4:
    ldr r0, =__stack_limit
    ldr r1, =__stack_top
    ldr r2, =RC_STACK_PATTERN
6:
    cmp r0, r1
    bhs 7f
    str r2, [r0]
    adds r0, #4
    b 6b
7:
    bl main
    b Default_Handler
.size Reset_Handler, . - Reset_Handler

.section .text.coretx_enter_rom_bootloader, "ax", %progbits
.global coretx_enter_rom_bootloader
.type coretx_enter_rom_bootloader, %function
.thumb_func
coretx_enter_rom_bootloader:
    cpsid i
    ldr r1, [r0]
    ldr r2, [r0, #4]
    movs r0, #0
    msr control, r0
    msr psp, r0
    msr msp, r1
    dsb
    isb
    cpsie i
    bx r2
.size coretx_enter_rom_bootloader, . - coretx_enter_rom_bootloader

.section .text.Default_Handler, "ax", %progbits
.global Default_Handler
.type Default_Handler, %function
.thumb_func
Default_Handler:
    mrs r1, ipsr
    ldr r0, =coretx_fault
    str r1, [r0]
    ldr r0, =0x40007000
    ldr r2, [r0]
    movs r3, #1
    lsls r3, #8
    orrs r2, r3
    str r2, [r0]
    ldr r2, [r0]
    ldr r0, =0x40002858
    ldr r2, [r0]
    movs r3, #255
    ands r2, r3
    lsls r1, #8
    orrs r1, r2
    str r1, [r0]
5:
    cpsid i
    wfi
    b 5b
.size Default_Handler, . - Default_Handler

.section .note.GNU-stack, "", %progbits
