// Fixed x86_64 trampolines for host code that calls a raw AArch64 guest
// symbol. Each 16-byte entry loads its table index and jumps to the common
// capture routine; no writable/executable code generation is required.

#if defined(__APPLE__)
#define DM_SYMBOL(name) _##name
#else
#define DM_SYMBOL(name) name
#endif

    .text
    .p2align 4
    .globl DM_SYMBOL(dm_guest_cpu_guest_stubs)
DM_SYMBOL(dm_guest_cpu_guest_stubs):
    .set k, 0
    .rept 65536
    movl $k, %r11d
    jmp DM_SYMBOL(dm_guest_cpu_guest_entry)
    .balign 16, 0x90
    .set k, k + 1
    .endr

// Entry: r11d = trampoline index, [rsp] = the host caller's return address.
// A 256-byte frame is captured at the new stack pointer. The extra 8-byte
// adjustment keeps rsp 16-byte aligned before the C++ callback.
    .globl DM_SYMBOL(dm_guest_cpu_guest_entry)
DM_SYMBOL(dm_guest_cpu_guest_entry):
    subq $264, %rsp
    movq %rdi, 0(%rsp)
    movq %rsi, 8(%rsp)
    movq %rdx, 16(%rsp)
    movq %rcx, 24(%rsp)
    movq %r8,  32(%rsp)
    movq %r9,  40(%rsp)
    movdqu %xmm0, 48(%rsp)
    movdqu %xmm1, 64(%rsp)
    movdqu %xmm2, 80(%rsp)
    movdqu %xmm3, 96(%rsp)
    movdqu %xmm4, 112(%rsp)
    movdqu %xmm5, 128(%rsp)
    movdqu %xmm6, 144(%rsp)
    movdqu %xmm7, 160(%rsp)
    // The old rsp is new rsp + 264; the first SysV stack argument is one
    // word above the return address, i.e. new rsp + 272.
    leaq 272(%rsp), %rax
    movq %rax, 176(%rsp)
    movl %r11d, %edi
    movq %rsp, %rsi
    call DM_SYMBOL(dm_guest_cpu_guest_entry_c)
    movq 184(%rsp), %rax
    movq 192(%rsp), %rdx
    movdqu 200(%rsp), %xmm0
    movdqu 216(%rsp), %xmm1
    addq $264, %rsp
    ret
