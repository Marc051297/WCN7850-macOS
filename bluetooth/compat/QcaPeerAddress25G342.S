// SPDX-License-Identifier: MIT
// Independently implemented bridges for the audited x86_64 25G342 binary.
// No device identity, pairing key, authentication result or HCI event is made up.
// All offsets below are validated by tools/build-peer-address-patches.py.
// This source is an offline candidate until page verification integration passes.
.text
.set QCA_HELPERS_RVA, 0xaa2c18
.set QCA_CONTROLLER_GLOBAL, 0xb3abb0
.macro native_jump dest
    .byte 0xe9
    .long (\dest) - QCA_HELPERS_RVA - (. - Lqca_begin) - 4
.endm
.macro load_controller_rax
    .byte 0x48, 0x8b, 0x05
    .long QCA_CONTROLLER_GLOBAL - QCA_HELPERS_RVA - (. - Lqca_begin) - 4
.endm
.macro address_rax dest
    .byte 0x48, 0x8d, 0x05
    .long (\dest) - QCA_HELPERS_RVA - (. - Lqca_begin) - 4
.endm

.globl _qca_peer_begin, _qca_peer_clear, _qca_peer_keep, _qca_peer_device
.globl _qca_peer_get, _qca_peer_list, _qca_peer_end
.alt_entry _qca_peer_clear
.alt_entry _qca_peer_keep
.alt_entry _qca_peer_device
.alt_entry _qca_peer_get
.alt_entry _qca_peer_list
.alt_entry _qca_peer_end
_qca_peer_begin:
Lqca_begin:
_qca_peer_clear:
    // The legacy descriptor is reused for every report. Its seven unused
    // direct-address bytes must not retain the previous peer's RPA.
    movl $0, 0x33(%rdi)
    movw $0, 0x37(%rdi)
    movb $0, 0x39(%rdi)
    native_jump 0x223424

_qca_peer_keep:
    // Enter only after native AH verification succeeds. The return frame
    // proves this is a legacy report, and event 0 proves it is undirected.
    address_rax 0x20c3f1
    cmpq %rax, 8(%rbp)
    jne Lqca_copy_identity
    cmpb $0, (%rbx)
    jne Lqca_copy_identity
    load_controller_rax
    testq %rax, %rax
    je Lqca_copy_identity
    cmpl $0, 0x334(%rax)
    jne Lqca_copy_identity
    movl 1(%r12), %eax
    movl %eax, 0x34(%rbx)
    movw 5(%r12), %ax
    movw %ax, 0x38(%rbx)
    movb $0x81, 0x33(%rbx)
Lqca_copy_identity:
    // Preserve the native resolved address for identity/cache/security code.
    movl -0x48(%rbp), %eax
    movl -0x45(%rbp), %ecx
    movl %ecx, 3(%r12)
    movl %eax, (%r12)
    retq

_qca_peer_device:
    // Inside the native device setter's mutex. Preserve its bool argument
    // and volatile registers that the replaced instructions did not modify.
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rax
    address_rax 0x73b88c
    cmpq %rax, 8(%rbp)
    movq (%rsp), %rax
    jne Lqca_set_scanned
    movq (%rbp), %rcx
    movq -0x480(%rcx), %rcx
    movq 8(%rcx), %rcx
    cmpb $0x81, 0xd6(%rcx)
    jne Lqca_set_scanned
    movq 0xd0(%rcx), %rdx
    movq $0, 0xd0(%rcx)
    // Consume the private marker even if the device lookup returned null.
    testq %rax, %rax
    je Lqca_set_scanned
    movq %rax, %rsi
    load_controller_rax
    testq %rax, %rax
    je Lqca_recover_device
    cmpl $0, 0x334(%rax)
    jne Lqca_recover_device
    cmpq $0, 0x18(%rsi)
    jne Lqca_store_air_address
    movq 0x10(%rsi), %rcx
    movq %rcx, 0x18(%rsi)
Lqca_store_air_address:
    btrq $55, %rdx
    movq %rdx, 0x10(%rsi)
Lqca_recover_device:
    movq %rsi, %rax
Lqca_set_scanned:
    testq %rax, %rax
    je Lqca_device_done
    movb %r14b, 0x61(%rax)
Lqca_device_done:
    popq %rax
    popq %rsi
    popq %rdx
    popq %rcx
    retq

_qca_peer_get:
    // Only the two initiating/list-maintenance sites call this bridge.
    // Native getAddress(..., true) prefers the last advertised address.
    xorl %edx, %edx
    load_controller_rax
    testq %rax, %rax
    je Lqca_get_native
    cmpl $0, 0x334(%rax)
    sete %dl
Lqca_get_native:
    native_jump 0x706900

_qca_peer_list:
    // The third-party controller uses one current radio address here. The
    // global paired predicate and all authentication callers stay native.
    load_controller_rax
    testq %rax, %rax
    je Lqca_list_native
    cmpl $0, 0x334(%rax)
    jne Lqca_list_native
    xorl %eax, %eax
    retq
Lqca_list_native:
    native_jump 0x714166
_qca_peer_end:

.section __TEXT,__qca_decode,regular,pure_instructions
.globl _qca_decode_begin, _qca_decode_end
_qca_decode_begin:
    movq 0x1768(%r14), %rsi
    movzbl (%rsi), %eax
    cmpl $1, %eax
    jbe Lqca_pack
    cmpl $0x81, %eax
    jne Lqca_no_address
    cmpb $0, (%r14)
    jne Lqca_no_address
    cmpb $0, 0x1756(%r14)
    jne Lqca_no_address
    movzbl 1(%rsi), %eax
    andl $0xc0, %eax
    cmpl $0x40, %eax
    jne Lqca_no_address
Lqca_pack:
    // Encode exactly seven bytes; do not read the adjacent live flag.
    pushq %rcx
    movl $7, %ecx
    xorl %eax, %eax
Lqca_pack_next:
    shlq $8, %rax
    movb (%rsi), %al
    incq %rsi
    loop Lqca_pack_next
    popq %rcx
    jmp Lqca_decode_done
Lqca_no_address:
    xorl %eax, %eax
Lqca_decode_done:
.org 90, 0x90
_qca_decode_end:
