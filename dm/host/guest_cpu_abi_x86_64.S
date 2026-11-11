// AAPCS64 register banks to the x86_64 System V register banks used by the
// host HLE functions.  This intentionally covers the common scalar HLE shape:
// six integer arguments and eight floating-point arguments.  The full bridge
// will add guest-stack and composite-value descriptors before it is used for
// every import.

#if defined(__APPLE__)
#define DM_SYMBOL(name) _##name
#else
#define DM_SYMBOL(name) name
#endif

    .text
    .globl DM_SYMBOL(dm_guest_cpu_invoke_host)
    .p2align 4
DM_SYMBOL(dm_guest_cpu_invoke_host):
    // The caller enters with rsp % 16 == 8. Save the state pointer in a
    // callee-saved register and reserve a generous outgoing stack argument
    // area; the resulting rsp is 16-byte aligned before the host call.
    pushq %r12
    subq $256, %rsp
    movq %rdi, %r11                    // host function
    movq %rsi, %r12                    // dm_guest_cpu_state

    movq   0(%r12), %rdi              // x0 -> first integer argument
    movq   8(%r12), %rsi              // x1
    movq  16(%r12), %rdx              // x2
    movq  24(%r12), %rcx              // x3
    movq  32(%r12), %r8               // x4
    movq  40(%r12), %r9               // x5
    movq  48(%r12), %rax              // x6 -> first SysV stack argument
    movq %rax, 0(%rsp)
    movq  56(%r12), %rax              // x7 -> second SysV stack argument
    movq %rax, 8(%rsp)
    // AAPCS64 puts x8 and later integer arguments at the guest SP.  The
    // common HLE signatures are scalar and use no more than this copied
    // window; copying it unconditionally lets ordinary C prototypes with
    // more than eight integer arguments use the same bridge.
    movq 248(%r12), %rax              // state->sp
    testq %rax, %rax
    jz 1f
    movq 0(%rax), %rcx; movq %rcx, 16(%rsp)
    movq 8(%rax), %rcx; movq %rcx, 24(%rsp)
    movq 16(%rax), %rcx; movq %rcx, 32(%rsp)
    movq 24(%rax), %rcx; movq %rcx, 40(%rsp)
    movq 32(%rax), %rcx; movq %rcx, 48(%rsp)
    movq 40(%rax), %rcx; movq %rcx, 56(%rsp)
    movq 48(%rax), %rcx; movq %rcx, 64(%rsp)
    movq 56(%rax), %rcx; movq %rcx, 72(%rsp)
    movq 64(%rax), %rcx; movq %rcx, 80(%rsp)
    movq 72(%rax), %rcx; movq %rcx, 88(%rsp)
    movq 80(%rax), %rcx; movq %rcx, 96(%rsp)
    movq 88(%rax), %rcx; movq %rcx, 104(%rsp)
    movq 96(%rax), %rcx; movq %rcx, 112(%rsp)
    movq 104(%rax), %rcx; movq %rcx, 120(%rsp)
    movq 112(%rax), %rcx; movq %rcx, 128(%rsp)
    movq 120(%rax), %rcx; movq %rcx, 136(%rsp)
    movq 128(%rax), %rcx; movq %rcx, 144(%rsp)
    movq 136(%rax), %rcx; movq %rcx, 152(%rsp)
    movq 144(%rax), %rcx; movq %rcx, 160(%rsp)
    movq 152(%rax), %rcx; movq %rcx, 168(%rsp)
    movq 160(%rax), %rcx; movq %rcx, 176(%rsp)
    movq 168(%rax), %rcx; movq %rcx, 184(%rsp)
    movq 176(%rax), %rcx; movq %rcx, 192(%rsp)
    movq 184(%rax), %rcx; movq %rcx, 200(%rsp)
    movq 192(%rax), %rcx; movq %rcx, 208(%rsp)
    movq 200(%rax), %rcx; movq %rcx, 216(%rsp)
    movq 208(%rax), %rcx; movq %rcx, 224(%rsp)
    movq 216(%rax), %rcx; movq %rcx, 232(%rsp)
    movq 224(%rax), %rcx; movq %rcx, 240(%rsp)
1:

    movdqu 264(%r12), %xmm0           // v0..v7
    movdqu 280(%r12), %xmm1
    movdqu 296(%r12), %xmm2
    movdqu 312(%r12), %xmm3
    movdqu 328(%r12), %xmm4
    movdqu 344(%r12), %xmm5
    movdqu 360(%r12), %xmm6
    movdqu 376(%r12), %xmm7

    call *%r11

    movq %rax, 0(%r12)                // scalar return x0
    movq %rdx, 8(%r12)                // second scalar return x1
    movdqu %xmm0, 264(%r12)           // scalar/vector return v0
    movdqu %xmm1, 280(%r12)           // v1
    addq $256, %rsp
    popq %r12
    ret

    .globl DM_SYMBOL(dm_guest_cpu_invoke_host_sret)
    .p2align 4
DM_SYMBOL(dm_guest_cpu_invoke_host_sret):
    // AAPCS64's x8 indirect result pointer is the first SysV argument. This
    // shifts the normal integer argument bank by one register; floating-point
    // arguments remain in xmm0..xmm7 on both ABIs.
    pushq %r12
    subq $256, %rsp
    movq %rdi, %r11                    // host function
    movq %rsi, %r12                    // dm_guest_cpu_state

    movq 64(%r12), %rdi               // x8 -> hidden result pointer
    movq 0(%r12), %rsi                // x0 -> first normal integer argument
    movq 8(%r12), %rdx                // x1
    movq 16(%r12), %rcx               // x2
    movq 24(%r12), %r8                // x3
    movq 32(%r12), %r9                // x4
    movq 40(%r12), %rax               // x5 -> first SysV stack argument
    movq %rax, 0(%rsp)
    movq 48(%r12), %rax               // x6 -> second stack argument
    movq %rax, 8(%rsp)
    movq 56(%r12), %rax               // x7 -> third stack argument
    movq %rax, 16(%rsp)
    movq 248(%r12), %rax              // state->sp: arguments after x7
    testq %rax, %rax
    jz 2f
    movq 0(%rax), %rcx; movq %rcx, 24(%rsp)
    movq 8(%rax), %rcx; movq %rcx, 32(%rsp)
    movq 16(%rax), %rcx; movq %rcx, 40(%rsp)
    movq 24(%rax), %rcx; movq %rcx, 48(%rsp)
    movq 32(%rax), %rcx; movq %rcx, 56(%rsp)
    movq 40(%rax), %rcx; movq %rcx, 64(%rsp)
    movq 48(%rax), %rcx; movq %rcx, 72(%rsp)
    movq 56(%rax), %rcx; movq %rcx, 80(%rsp)
    movq 64(%rax), %rcx; movq %rcx, 88(%rsp)
    movq 72(%rax), %rcx; movq %rcx, 96(%rsp)
    movq 80(%rax), %rcx; movq %rcx, 104(%rsp)
    movq 88(%rax), %rcx; movq %rcx, 112(%rsp)
    movq 96(%rax), %rcx; movq %rcx, 120(%rsp)
    movq 104(%rax), %rcx; movq %rcx, 128(%rsp)
    movq 112(%rax), %rcx; movq %rcx, 136(%rsp)
    movq 120(%rax), %rcx; movq %rcx, 144(%rsp)
    movq 128(%rax), %rcx; movq %rcx, 152(%rsp)
    movq 136(%rax), %rcx; movq %rcx, 160(%rsp)
    movq 144(%rax), %rcx; movq %rcx, 168(%rsp)
    movq 152(%rax), %rcx; movq %rcx, 176(%rsp)
    movq 160(%rax), %rcx; movq %rcx, 184(%rsp)
    movq 168(%rax), %rcx; movq %rcx, 192(%rsp)
    movq 176(%rax), %rcx; movq %rcx, 200(%rsp)
    movq 184(%rax), %rcx; movq %rcx, 208(%rsp)
    movq 192(%rax), %rcx; movq %rcx, 216(%rsp)
    movq 200(%rax), %rcx; movq %rcx, 224(%rsp)
    movq 208(%rax), %rcx; movq %rcx, 232(%rsp)
    movq 216(%rax), %rcx; movq %rcx, 240(%rsp)
    movq 224(%rax), %rcx; movq %rcx, 248(%rsp)
2:

    movdqu 264(%r12), %xmm0
    movdqu 280(%r12), %xmm1
    movdqu 296(%r12), %xmm2
    movdqu 312(%r12), %xmm3
    movdqu 328(%r12), %xmm4
    movdqu 344(%r12), %xmm5
    movdqu 360(%r12), %xmm6
    movdqu 376(%r12), %xmm7

    call *%r11

    movq %rax, 0(%r12)
    movq %rdx, 8(%r12)
    movdqu %xmm0, 264(%r12)
    movdqu %xmm1, 280(%r12)
    addq $256, %rsp
    popq %r12
    ret
