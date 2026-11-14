/* Firmware 39A10023 pairing event workers. The resident copy holds no
 * app pointers, so queued events can safely finish after app unload. */
.syntax unified
.thumb
.section .rodata.classic_pair_capture, "a", %progbits
.balign 4
.global classic_pair_capture_start, classic_pair_capture_complete
.global classic_pair_capture_mailbox, classic_pair_capture_end
.global classic_pair_capture_mode_branch

classic_pair_capture_start:
    push {r4-r6}
    adr.w r4, classic_pair_capture_mailbox
    ldr r5, [r4]         /* Armed agent handle */
    cbz r5, 1f
    dmb sy
    ldr r6, [r0, #28]    /* Event agent handle */
    cmp r5, r6
    bne 1f
    ldr r5, [r4, #4]
    ldr r6, [r0, #32]    /* Event device handle */
    cmp r5, r6
    bne 1f
    ldrb r5, [r0, #40]   /* Numeric comparison flag */
classic_pair_capture_mode_branch:
    cbz r5, 1f
    ldr r5, [r0, #36]    /* Numeric comparison code */
    ldr r6, =999999
    cmp r5, r6
    bhi 1f
    movs r6, #0
    str r6, [r4, #12]
    adds r5, #1
    dmb sy
    str r5, [r4, #8]
    pop {r4-r6}
    bx lr
1:
    pop {r4-r6}
    ldr.w pc, 3f

.balign 4
classic_pair_capture_complete:
    push {r4-r6}
    adr.w r4, classic_pair_capture_mailbox
    ldr r5, [r4]
    cbz r5, 2f
    dmb sy
    ldr r6, [r0, #28]
    cmp r5, r6
    bne 2f
    ldr r5, [r4, #4]
    ldr r6, [r0, #36]
    cmp r5, r6
    bne 2f
    ldrb r5, [r0, #32]
    cmp r5, #3           /* Pairing completed */
    bne 2f
    ldr r5, [r0, #40]
    adds r5, #1
    str r5, [r4, #12]
    movs r5, #0
    dmb sy
    str r5, [r4, #8]
2:
    pop {r4-r6}
    ldr.w pc, 4f

.balign 4
3:  .word 0x08431b49
4:  .word 0x08430b61
.ltorg
classic_pair_capture_mailbox:
    .word 0, 0, 0, 0      /* Agent, device, code + 1, result + 1 */
classic_pair_capture_end:
