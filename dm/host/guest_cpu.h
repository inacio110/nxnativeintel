// Guest CPU backend boundary.  The native Apple-silicon host does not use this
// interface; the Intel host uses it to run the Switch's AArch64 code through a
// dynamic translator.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dm_guest_cpu_state {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t v[32][2];
    uint32_t fpcr;
    uint32_t fpsr;
    uint32_t pstate;
} dm_guest_cpu_state;

void dm_guest_cpu_set_pause_hooks(int (*requested)(void), void (*pause)(const dm_guest_cpu_state *));
void dm_guest_cpu_release_processor_id(uint32_t thread_handle);
uintptr_t dm_guest_cpu_initial_sp(void);
int dm_guest_cpu_stack_region(uint64_t addr, uint64_t *base, uint64_t *size);

typedef void (*dm_guest_cpu_svc_fn)(void *context, uint32_t number, dm_guest_cpu_state *state);

enum {
    DM_GUEST_CPU_SVC_UNIMPLEMENTED_BASE = 0x1000,
    DM_GUEST_CPU_SVC_HLE_BASE = 0x4000,
    DM_GUEST_CPU_SVC_GATE = 0x7ffe,
    DM_GUEST_CPU_SVC_STOP = 0x7fff,
};

// The rewriter addresses these four slots relative to TPIDRRO_EL0. Native
// macOS uses pthread's implementation-specific TSD layout for the same
// offsets; the translated backend owns a small per-thread array instead.
enum {
    DM_GUEST_CPU_TSD_K18 = 0x40,
    DM_GUEST_CPU_TSD_TLS = 0x48,
    DM_GUEST_CPU_TSD_TLR = 0x50,
    DM_GUEST_CPU_TSD_GATE = 0x58,
    // The host ABI bridge may speculatively copy a bounded AArch64 stack
    // argument window. Keep this much mapped above an entry stack pointer.
    DM_GUEST_CPU_STACK_ARG_BYTES = 256,
};

// The stop stub is an AArch64 instruction sequence.  A translated call uses
// its address as the synthetic return address instead of returning into host
// machine code.
const void *dm_guest_cpu_stop_stub(void);

// Install the translated thread environment. `tls` and `tlr` are the same
// guest pointers exposed through the native pthread keys; `gate` is an
// AArch64 synthetic gate stub and may be null before guest_gate_stack().
void dm_guest_cpu_set_thread_slots(uint64_t k18, uint64_t tls, uint64_t tlr, uint64_t gate);
void dm_guest_cpu_set_k18(uint64_t value);
const void *dm_guest_cpu_gate_stub(void);

// Run one guest entry point until it returns to stop_pc.  Guest memory is
// addressed by the same 64-bit values the loader places in the CPU state; the
// backend's memory callbacks perform the actual host access.
int dm_guest_cpu_run(dm_guest_cpu_state *state, uint64_t entry, uint64_t stop_pc,
                     dm_guest_cpu_svc_fn svc, void *context);

// Call a host HLE implementation with the guest's integer (x0..x7) and
// floating-point (v0..v7) argument registers, then copy scalar return values
// back to x0/x1 and v0/v1.  AAPCS64 and the Intel macOS ABI share these
// register banks, which makes this a useful first bridge. x6/x7 and a bounded
// window of guest stack arguments are placed in the SysV stack area; composite
// arguments and more complex mixed signatures get a separate descriptor
// adapter in the HLE integration milestone.
void dm_guest_cpu_invoke_host(void *fn, dm_guest_cpu_state *state);
// Variant for AAPCS64 functions whose result is returned through x8. The
// guest-provided result address is passed as the hidden first argument to the
// SysV function, so C structs larger than 16 bytes can be copied in place.
void dm_guest_cpu_invoke_host_sret(void *fn, dm_guest_cpu_state *state);

// Make a guest-visible AArch64 `svc; ret` stub.  The stub itself lives in
// ordinary host memory and is read by Dynarmic's guest memory callback; it is
// never executed by the Intel CPU as x86 machine code.
void *dm_guest_cpu_hle_stub(void *fn);
void *dm_guest_cpu_hle_sret_stub(void *fn);
void *dm_guest_cpu_unimplemented_stub(unsigned index);
int dm_guest_cpu_hle_dispatch(uint32_t number, dm_guest_cpu_state *state);

// Host-to-guest call boundary. `dm_guest_cpu_guest_stub` returns an ordinary
// x86_64 function pointer for a raw AArch64 entry point. The fixed trampoline
// captures the host ABI registers, translates the call and returns x0/x1 and
// v0/v1 using the host ABI. It currently treats arguments as 64-bit scalar
// slots; composite and variadic signatures need a descriptor adapter.
void *dm_guest_cpu_guest_stub(void *entry);

typedef struct dm_guest_cpu_host_call_frame {
    uint64_t gpr[6];
    uint8_t v[8][16];
    const uint64_t *stack_args;
    uint64_t ret_x[2];
    uint8_t ret_v[2][16];
} dm_guest_cpu_host_call_frame;
void dm_guest_cpu_guest_entry_c(uint32_t index, dm_guest_cpu_host_call_frame *frame);

const char *dm_guest_cpu_last_error(void);

#ifdef __cplusplus
}
#endif
