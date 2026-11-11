#include "guest_cpu.h"

#include <cassert>
#include <cstdio>
#include <cstdint>

// The complete host supplies this callback from hle.c.  The standalone
// translator smoke target does not link the host service layer, but the weak
// hook still needs a concrete symbol on macOS when the test executable links.
extern "C" void dm_guest_cpu_svc_dispatch(void *, uint32_t, dm_guest_cpu_state *) {}

extern "C" uint64_t host_add(uint64_t a, uint64_t b) {
    return a + b;
}

extern "C" double host_mix(uint64_t a, double b) {
    return static_cast<double>(a) + b;
}

extern "C" uint64_t host_sum8(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                               uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    return a + b + c + d + e + f + g + h;
}

extern "C" uint64_t host_sum12(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                uint64_t e, uint64_t f, uint64_t g, uint64_t h,
                                uint64_t i, uint64_t j, uint64_t k, uint64_t l) {
    return a + b + c + d + e + f + g + h + i + j + k + l;
}

struct host_large_result {
    uint64_t a, b, c;
};

extern "C" host_large_result host_large(uint64_t a, uint64_t b) {
    return {a, b, a + b};
}

static void dispatch_hle(void *, uint32_t number, dm_guest_cpu_state *state) {
    assert(dm_guest_cpu_hle_dispatch(number, state) == 1);
}

static void dispatch_gate(void *, uint32_t number, dm_guest_cpu_state *state) {
    assert(number == DM_GUEST_CPU_SVC_GATE);
    state->x[0] = 99;
}

int main() {
    // mov x0, #5; add x0, x0, x1; ret
    const uint32_t code[] = {0xd28000a0u, 0x8b010000u, 0xd65f03c0u};
    alignas(16) uint8_t stack[4096]{};
    dm_guest_cpu_state state{};
    state.x[1] = 37;
    state.sp = reinterpret_cast<uintptr_t>(stack + sizeof stack - DM_GUEST_CPU_STACK_ARG_BYTES);

    assert(dm_guest_cpu_run(&state, reinterpret_cast<uintptr_t>(code), 0, nullptr, nullptr) == 0);
    assert(state.x[0] == 42);

    // A raw AArch64 symbol can also be exposed as a normal x86_64 function
    // pointer for host code that calls back into the SDK.
    const uint32_t return_code[] = {0xd2800f60u, 0xd65f03c0u}; // mov x0, #123; ret
    using guest_fn0 = uint64_t (*)();
    auto guest_entry = reinterpret_cast<guest_fn0>(dm_guest_cpu_guest_stub(
        const_cast<uint32_t *>(return_code)));
    assert(guest_entry != nullptr);
    assert(guest_entry() == 123);

    // Host callbacks into the SDK use the inverse bridge. Six arguments start
    // in SysV registers and the remaining two arrive on the host stack; the
    // guest must see the same values in AAPCS64 x0..x7.
    const uint32_t guest_sum8_code[] = {
        0x8b010000u, // add x0, x0, x1
        0x8b020000u, // add x0, x0, x2
        0x8b030000u, // add x0, x0, x3
        0x8b040000u, // add x0, x0, x4
        0x8b050000u, // add x0, x0, x5
        0x8b060000u, // add x0, x0, x6
        0x8b070000u, // add x0, x0, x7
        0xd65f03c0u,
    };
    using guest_fn8 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, uint64_t, uint64_t);
    auto guest_sum8 = reinterpret_cast<guest_fn8>(dm_guest_cpu_guest_stub(
        const_cast<uint32_t *>(guest_sum8_code)));
    assert(guest_sum8 != nullptr);
    assert(guest_sum8(1, 2, 3, 4, 5, 6, 7, 8) == 36);

    // Arguments beyond x7 are copied into a translated guest stack window.
    // This is the shape used by less common SDK callbacks with long parameter
    // lists, and catches an accidental host-stack/guest-stack offset change.
    const uint32_t guest_sum10_code[] = {
        0x8b010000u, // add x0, x0, x1
        0x8b020000u, // add x0, x0, x2
        0x8b030000u, // add x0, x0, x3
        0x8b040000u, // add x0, x0, x4
        0x8b050000u, // add x0, x0, x5
        0x8b060000u, // add x0, x0, x6
        0x8b070000u, // add x0, x0, x7
        0xf94003e9u, // ldr x9, [sp, #0]
        0x8b090000u, // add x0, x0, x9
        0xf94007e9u, // ldr x9, [sp, #8]
        0x8b090000u, // add x0, x0, x9
        0xd65f03c0u,
    };
    using guest_fn10 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t);
    auto guest_sum10 = reinterpret_cast<guest_fn10>(dm_guest_cpu_guest_stub(
        const_cast<uint32_t *>(guest_sum10_code)));
    assert(guest_sum10 != nullptr);
    assert(guest_sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) == 55);

    state.x[0] = 5;
    state.x[1] = 7;
    dm_guest_cpu_invoke_host(reinterpret_cast<void *>(host_add), &state);
    assert(state.x[0] == 12);

    state.x[0] = 9;
    double mix = 2.5;
    static_assert(sizeof mix <= sizeof state.v[0]);
    __builtin_memcpy(state.v[0], &mix, sizeof mix);
    dm_guest_cpu_invoke_host(reinterpret_cast<void *>(host_mix), &state);
    double result;
    __builtin_memcpy(&result, state.v[0], sizeof result);
    assert(result == 11.5);

    for (unsigned i = 0; i < 8; i++) state.x[i] = i + 1;
    dm_guest_cpu_invoke_host(reinterpret_cast<void *>(host_sum8), &state);
    assert(state.x[0] == 36);

    // The next arguments after x7 live in the guest caller's stack. The
    // bridge must carry them to the host stack rather than dropping them.
    for (unsigned i = 0; i < 12; i++) state.x[i] = i + 1;
    state.sp = reinterpret_cast<uintptr_t>(stack + sizeof stack - DM_GUEST_CPU_STACK_ARG_BYTES);
    reinterpret_cast<uint64_t *>(state.sp)[0] = 9;
    reinterpret_cast<uint64_t *>(state.sp)[1] = 10;
    reinterpret_cast<uint64_t *>(state.sp)[2] = 11;
    reinterpret_cast<uint64_t *>(state.sp)[3] = 12;
    dm_guest_cpu_invoke_host(reinterpret_cast<void *>(host_sum12), &state);
    assert(state.x[0] == 78);

    // AAPCS64 returns a result larger than 16 bytes through x8. The Intel
    // bridge passes that pointer as SysV's hidden first argument.
    host_large_result large{};
    state = {};
    state.x[0] = 17;
    state.x[1] = 25;
    state.x[8] = reinterpret_cast<uintptr_t>(&large);
    state.sp = reinterpret_cast<uintptr_t>(stack + sizeof stack - DM_GUEST_CPU_STACK_ARG_BYTES);
    state.x[2] = reinterpret_cast<uintptr_t>(dm_guest_cpu_hle_sret_stub(reinterpret_cast<void *>(host_large)));
    const uint32_t sret_call_code[] = {0xd63f0040u, 0xd65f03c0u};
    assert(dm_guest_cpu_run(&state, reinterpret_cast<uintptr_t>(sret_call_code), 0, dispatch_hle, nullptr) == 0);
    assert(large.a == 17 && large.b == 25 && large.c == 42);

    // blr x2; ret, with x2 pointing at an AArch64 synthetic HLE stub.
    const uint32_t call_code[] = {0xd63f0040u, 0xd65f03c0u};
    state = {};
    state.x[0] = 20;
    state.x[1] = 22;
    state.x[2] = reinterpret_cast<uintptr_t>(dm_guest_cpu_hle_stub(reinterpret_cast<void *>(host_add)));
    state.sp = reinterpret_cast<uintptr_t>(stack + sizeof stack - DM_GUEST_CPU_STACK_ARG_BYTES);
    assert(dm_guest_cpu_run(&state, reinterpret_cast<uintptr_t>(call_code), 0, dispatch_hle, nullptr) == 0);
    assert(state.x[0] == 42);

    // The rewritten image obtains K18 from TPIDRRO_EL0 + 0x40 and enters the
    // per-thread gate through the same synthetic AArch64 boundary.
    dm_guest_cpu_set_thread_slots(0x12345678, 0x2000, 0x3000,
                                  reinterpret_cast<uintptr_t>(dm_guest_cpu_gate_stub()));
    const uint32_t tls_code[] = {
        0xd53bd040u,             // mrs x0, tpidrro_el0
        0xf9402000u,             // ldr x0, [x0, #0x40]
        0xd65f03c0u,             // ret
    };
    state = {};
    state.sp = reinterpret_cast<uintptr_t>(stack + sizeof stack - DM_GUEST_CPU_STACK_ARG_BYTES);
    assert(dm_guest_cpu_run(&state, reinterpret_cast<uintptr_t>(tls_code), 0, nullptr, nullptr) == 0);
    assert(state.x[0] == 0x12345678);

    const uint32_t gate_code[] = {
        0xd63f0040u,             // blr x2
        0xd65f03c0u,             // ret
    };
    state = {};
    state.x[2] = reinterpret_cast<uintptr_t>(dm_guest_cpu_gate_stub());
    state.sp = reinterpret_cast<uintptr_t>(stack + sizeof stack);
    assert(dm_guest_cpu_run(&state, reinterpret_cast<uintptr_t>(gate_code), 0, dispatch_gate, nullptr) == 0);
    assert(state.x[0] == 99);
    std::puts("translated CPU, TLS, ABI and HLE/gate smoke passed");
    return 0;
}
