/* Heap-resident Timer3B replacement. Native GUI waits re-enable its EIR bit.
 * Clear only Timer3B FIR bit6, preserve every GPR touched and return directly
 * from the interrupt. No ROS calls, ALR/AHR changes, or nested-IRQ enable. */
    .text
    .p2align 1
    .globl usb_platform_timer_guard
    .type usb_platform_timer_guard,@function
usb_platform_timer_guard:
    pushn %r1
    ext 0
    ext 4106
    ld.w %r0,3
    ext 1
    ld.w %r1,0
    ld.b [%r0],%r1
    popn %r1
    reti
    .size usb_platform_timer_guard,.-usb_platform_timer_guard
