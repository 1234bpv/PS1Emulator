# 1 "D:\\\\VsCodeProjects\\\\SymbianProjects\\\\PS1Emulator\\\\pcsx_rearmed\\\\plugins\\\\dfsound\\\\arm_utils.S"
# 1 "<built-in>"
# 1 "<command line>"
# 1 "D:\\\\VsCodeProjects\\\\SymbianProjects\\\\PS1Emulator\\\\pcsx_rearmed\\\\plugins\\\\dfsound\\\\arm_utils.S"
# 11 "D:\\\\VsCodeProjects\\\\SymbianProjects\\\\PS1Emulator\\\\pcsx_rearmed\\\\plugins\\\\dfsound\\\\arm_utils.S"
# 1 "D://VsCodeProjects//SymbianProjects//PS1Emulator//pcsx_rearmed//include/arm_features.h" 1
# 12 "D:\\\\VsCodeProjects\\\\SymbianProjects\\\\PS1Emulator\\\\pcsx_rearmed\\\\plugins\\\\dfsound\\\\arm_utils.S" 2







.text
.align 2

.macro load_varadr reg var
# 32 "D:\\\\VsCodeProjects\\\\SymbianProjects\\\\PS1Emulator\\\\pcsx_rearmed\\\\plugins\\\\dfsound\\\\arm_utils.S"
 ldr \reg, =\var

.endm
# 107 "D:\\\\VsCodeProjects\\\\SymbianProjects\\\\PS1Emulator\\\\pcsx_rearmed\\\\plugins\\\\dfsound\\\\arm_utils.S"
.globl mix_chan; .type mix_chan, %function; mix_chan: @ (int *SSumLR, int count, int lv, int rv)
    stmfd sp!, {r4-r8,lr}
    orr r3, r2, r3, lsl #16
    lsl r3, #1 @ packed multipliers << 1
    mov r2, r0
    load_varadr r0, ChanBuf
0:
    ldmia r0!, {r4,r5}
    ldmia r2, {r6-r8,lr}
    lsl r4, #1 @ adjust for mul
    lsl r5, #1
    smlawb r6, r4, r3, r6
    smlawt r7, r4, r3, r7
    smlawb r8, r5, r3, r8
    smlawt lr, r5, r3, lr
    subs r1, #2
    blt mc_finish
    stmia r2!, {r6-r8,lr}
    bgt 0b
    ldmeqfd sp!, {r4-r8,pc}

mc_finish:
    stmia r2!, {r6,r7}
    ldmfd sp!, {r4-r8,pc}


.globl mix_chan_rvb; .type mix_chan_rvb, %function; mix_chan_rvb: @ (int *SSumLR, int count, int lv, int rv, int *rvb)
    stmfd sp!, {r4-r8,lr}
    orr lr, r2, r3, lsl #16
    lsl lr, #1
    mov r2, r0
    load_varadr r0, ChanBuf
    ldr r3, [sp, #6*4] @ rvb
0:
    ldr r4, [r0], #4
    ldmia r2, {r6,r7}
    ldmia r3, {r8,r12}
    lsl r4, #1
    smlawb r6, r4, lr, r6 @ supposedly takes single cycle?
    smlawt r7, r4, lr, r7
    smlawb r8, r4, lr, r8
    smlawt r12,r4, lr, r12
    subs r1, #1
    stmia r2!, {r6,r7}
    stmia r3!, {r8,r12}
    bgt 0b
    ldmfd sp!, {r4-r8,pc}




.section .note.GNU-stack,"",%progbits

@ vim:filetype=armasm
