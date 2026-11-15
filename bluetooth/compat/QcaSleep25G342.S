// SPDX-License-Identifier: MIT
// Additive bridges for the audited 25G342 x86_64 bluetoothd. No HCI events,
// controller identity, connection state or power state is synthesized.
.text
.set SLEEP_RVA, 0xaa2e00
.set CONTROLLER, 0xb3abb0
.set CORE, 0xb3abd0
.macro jump_native dest
    .byte 0xe9
    .long (\dest) - SLEEP_RVA - (. - Lbegin) - 4
.endm
.macro load_global reg, opcode, addr
    .byte 0x48, 0x8b, \opcode
    .long (\addr) - SLEEP_RVA - (. - Lbegin) - 4
.endm
.globl _qca_sleep_begin, _qca_sleep_hid, _qca_sleep_idle, _qca_sleep_end
.alt_entry _qca_sleep_hid
.alt_entry _qca_sleep_idle
.alt_entry _qca_sleep_end
_qca_sleep_begin:
Lbegin:
_qca_sleep_hid:
    // The native no-argument helper declares UserIsActive before examining
    // any controller power state. Check the native Core object's sleep state
    // first. Missing instances and other chipsets retain the original path.
    pushfq
    pushq %rax
    load_global rax, 0x05, CONTROLLER
    testq %rax, %rax
    je Lhid_native
    cmpl $0, 0x334(%rax)
    jne Lhid_native
    load_global rax, 0x05, CORE
    testq %rax, %rax
    je Lhid_native
    cmpb $0, 0x589(%rax)   // native fDarkSlept
    jne Lhid_sleep
    movzbl 0x58a(%rax), %eax // native sleep-transition stage
    cmpl $1, %eax
    je Lhid_sleep
    cmpl $3, %eax
    je Lhid_sleep
    cmpl $4, %eax
    je Lhid_sleep
Lhid_native:
    popq %rax
    popfq
    // Replay exactly the displaced prologue; continue in the native helper.
    pushq %rbp
    movq %rsp, %rbp
    pushq %rbx
    jump_native 0x3224d1
Lhid_sleep:
    popq %rax
    popfq
    retq

_qca_sleep_idle:
    // Enter in the manufacturer parser, with the real type in sil and its
    // bit in rax. Omit only type 21's repeated passive advertisement hold.
    // Parsing, delivery and assertions belonging to actual sessions continue.
    pushq %rcx
    load_global rcx, 0x0d, CONTROLLER
    testq %rcx, %rcx
    je Lidle_native
    cmpl $0, 0x334(%rcx)
    jne Lidle_native
    cmpb $21, %sil
    jne Lidle_native
    testq $0, %rax        // ZF=1: native parser skips the assertion block
    popq %rcx
    retq
Lidle_native:
    popq %rcx
    // Preserve the exact native TEST result for every other report/controller.
    .byte 0x48, 0x85, 0x05
    .long 0xb522c0 - SLEEP_RVA - (. - Lbegin) - 4
    retq
_qca_sleep_end:
