/* Firmware 39A10023 DeviceFoundEvent worker wrapper.
 * The event's first six bytes at +20 hold the Classic device address.
 * Keep this thunk self-contained so an in-flight event is safe after app exit.
 */
.syntax unified
.thumb
.section .rodata.classic_capture, "a", %progbits
.balign 4
.global classic_capture_start, classic_capture_mailbox, classic_capture_end
classic_capture_start:
    push {r1-r6}
    adr.w r1, classic_capture_mailbox
    ldr r6, [r1]
    cbz r6, 3f
    ldr r2, [r1, #4]
    cmp r2, #24
    bhs 3f
    ldr r3, [r0, #20]
    ldrh r4, [r0, #24]
    add r1, #8
    movs r5, #0
1:
    cmp r5, r2
    beq 2f
    ldr r6, [r1, r5, lsl #3]
    cmp r6, r3
    bne 4f
    add.w r6, r1, r5, lsl #3
    ldrh r6, [r6, #4]
    cmp r6, r4
    beq 3f
4:
    adds r5, #1
    b 1b
2:
    add.w r5, r1, r2, lsl #3
    str r3, [r5]
    strh r4, [r5, #4]
    dmb sy
    adds r2, #1
    str r2, [r1, #-4]
3:
    pop {r1-r6}
    ldr.w pc, 5f
.balign 4
5:
    .word 0x08435569
classic_capture_mailbox:
    .word 0, 0
    .space 24 * 8
classic_capture_end:
