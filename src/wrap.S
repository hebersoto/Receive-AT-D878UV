/* Exception-entry wrappers. st_hook must be first (image base). */
    .syntax unified
    .cpu cortex-m4
    .thumb
    .equ ST_ORIG, 0x08007ca9        /* firmware SysTick handler (thumb) */

    .section .text.entry,"ax",%progbits
    .align 2
    .global _start
    .thumb_func
_start:
    .global st_hook
    .thumb_func
st_hook:
    push    {r4, lr}
    bl      st_worker
    pop     {r4, lr}
    ldr     r12, =ST_ORIG
    bx      r12                     /* chain to firmware SysTick */

    .text
    .align 2
    .global t6_hook
    .thumb_func
t6_hook:
    push    {r4, lr}
    bl      t6_worker
    pop     {r4, lr}
    bx      lr                      /* return from exception (we own TIM6) */
