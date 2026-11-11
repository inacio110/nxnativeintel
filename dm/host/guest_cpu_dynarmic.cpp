// Intel guest CPU backend.
//
// Dynarmic supplies the AArch64 frontend and x86_64 JIT.  This first layer is
// deliberately small: it owns CPU state, memory callbacks and the synthetic
// return/SVC boundary.  Horizon SVCs, host HLE calls and thread suspension are
// connected by later layers so that the Apple-silicon execution path remains
// unchanged while the translated path is brought up.

#include "guest_cpu.h"

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace {

constexpr uint32_t kStopSvc = 0x7fff;
constexpr uint32_t kStopInstruction = 0xd4000001u | (kStopSvc << 5);
constexpr uint32_t kGateInstruction = 0xd4000001u | (DM_GUEST_CPU_SVC_GATE << 5);
constexpr unsigned kMaxHleStubs = 8192;
constexpr unsigned kMaxUnimplementedStubs = 4096;
constexpr unsigned kMaxGuestStubs = 65536;
constexpr size_t kGuestCallStackSize = 1 << 20;
constexpr size_t kGuestStackArgBytes = 256;
constexpr uint32_t kRetInstruction = 0xd65f03c0u;

alignas(4) uint32_t g_stop_stub = kStopInstruction;
alignas(4) uint32_t g_gate_stub[2] = {kGateInstruction, kRetInstruction};
alignas(16) uint32_t g_hle_stubs[kMaxHleStubs][2];
alignas(16) uint32_t g_unimplemented_stubs[kMaxUnimplementedStubs][2];
void *g_hle_functions[kMaxHleStubs];
bool g_hle_sret[kMaxHleStubs];
unsigned g_nhle_stubs;
std::mutex g_hle_lock;
void *g_guest_entries[kMaxGuestStubs];
std::unordered_map<void *, unsigned> g_guest_indices;
std::mutex g_guest_lock;
thread_local const char *g_last_error;
thread_local char g_last_error_detail[256];
alignas(16) thread_local uint8_t g_guest_tsd[DM_GUEST_CPU_TSD_GATE + sizeof(uint64_t)]{};
thread_local uint64_t g_guest_tpidrro_el0;
thread_local uint64_t g_guest_tpidr_el0;
Dynarmic::ExclusiveMonitor g_guest_global_monitor{1};

template <typename T>
T load(uint64_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void *>(static_cast<uintptr_t>(address)), sizeof value);
    return value;
}

template <typename T>
void store(uint64_t address, T value) {
    std::memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(address)), &value, sizeof value);
}

class Callbacks final : public Dynarmic::A64::UserCallbacks {
public:
    dm_guest_cpu_state *state = nullptr;
    Dynarmic::A64::Jit *cpu = nullptr;
    dm_guest_cpu_svc_fn svc = nullptr;
    void *context = nullptr;
    bool stopped = false;

    std::optional<uint32_t> MemoryReadCode(uint64_t vaddr) override {
        return load<uint32_t>(vaddr);
    }

    uint8_t MemoryRead8(uint64_t vaddr) override { return load<uint8_t>(vaddr); }
    uint16_t MemoryRead16(uint64_t vaddr) override { return load<uint16_t>(vaddr); }
    uint32_t MemoryRead32(uint64_t vaddr) override { return load<uint32_t>(vaddr); }
    uint64_t MemoryRead64(uint64_t vaddr) override { return load<uint64_t>(vaddr); }

    Dynarmic::A64::Vector MemoryRead128(uint64_t vaddr) override {
        return {load<uint64_t>(vaddr), load<uint64_t>(vaddr + 8)};
    }

    void MemoryWrite8(uint64_t vaddr, uint8_t value) override { store(vaddr, value); }
    void MemoryWrite16(uint64_t vaddr, uint16_t value) override { store(vaddr, value); }
    void MemoryWrite32(uint64_t vaddr, uint32_t value) override { store(vaddr, value); }
    void MemoryWrite64(uint64_t vaddr, uint64_t value) override { store(vaddr, value); }

    void MemoryWrite128(uint64_t vaddr, Dynarmic::A64::Vector value) override {
        store(vaddr, value[0]);
        store(vaddr + 8, value[1]);
    }

    void InterpreterFallback(uint64_t pc, size_t instructions) override {
        (void)instructions;
        g_last_error = "Dynarmic requested interpreter fallback";
        if (cpu) cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        (void)pc;
    }

    void CallSVC(uint32_t number) override {
        if (number == kStopSvc) {
            stopped = true;
            if (cpu) cpu->HaltExecution(Dynarmic::HaltReason::UserDefined1);
            return;
        }
        if (!svc) {
            g_last_error = "guest executed an SVC before the SVC bridge was installed";
            if (cpu) cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        sync_from_cpu();
        svc(context, number, state);
        sync_to_cpu();
    }

    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception exception) override {
        (void)pc;
        (void)exception;
        g_last_error = "Dynarmic raised an AArch64 exception";
        if (cpu) cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return std::numeric_limits<uint64_t>::max(); }
    uint64_t GetCNTPCT() override { return 0; }

    void sync_to_cpu() {
        if (!cpu || !state) return;
        std::array<uint64_t, 31> x{};
        std::memcpy(x.data(), state->x, sizeof state->x);
        cpu->SetRegisters(x);
        cpu->SetSP(state->sp);
        cpu->SetPC(state->pc);
        for (size_t i = 0; i < 32; i++) {
            cpu->SetVector(i, {state->v[i][0], state->v[i][1]});
        }
        cpu->SetFpcr(state->fpcr);
        cpu->SetFpsr(state->fpsr);
        cpu->SetPstate(state->pstate);
    }

    void sync_from_cpu() {
        if (!cpu || !state) return;
        const auto x = cpu->GetRegisters();
        std::memcpy(state->x, x.data(), sizeof state->x);
        state->sp = cpu->GetSP();
        state->pc = cpu->GetPC();
        for (size_t i = 0; i < 32; i++) {
            const auto v = cpu->GetVector(i);
            state->v[i][0] = v[0];
            state->v[i][1] = v[1];
        }
        state->fpcr = cpu->GetFpcr();
        state->fpsr = cpu->GetFpsr();
        state->pstate = cpu->GetPstate();
    }
};

}  // namespace

extern "C" uint8_t dm_guest_cpu_guest_stubs[];
extern "C" void dm_guest_cpu_svc_dispatch(void *, uint32_t, dm_guest_cpu_state *) __attribute__((weak));

extern "C" const void *dm_guest_cpu_stop_stub(void) {
    return &g_stop_stub;
}

extern "C" void dm_guest_cpu_set_thread_slots(uint64_t k18, uint64_t tls, uint64_t tlr, uint64_t gate) {
    std::memcpy(g_guest_tsd + DM_GUEST_CPU_TSD_K18, &k18, sizeof k18);
    std::memcpy(g_guest_tsd + DM_GUEST_CPU_TSD_TLS, &tls, sizeof tls);
    std::memcpy(g_guest_tsd + DM_GUEST_CPU_TSD_TLR, &tlr, sizeof tlr);
    std::memcpy(g_guest_tsd + DM_GUEST_CPU_TSD_GATE, &gate, sizeof gate);
    g_guest_tpidr_el0 = tls;
}

extern "C" void dm_guest_cpu_set_k18(uint64_t value) {
    std::memcpy(g_guest_tsd + DM_GUEST_CPU_TSD_K18, &value, sizeof value);
}

extern "C" const void *dm_guest_cpu_gate_stub(void) {
    return g_gate_stub;
}

extern "C" void *dm_guest_cpu_guest_stub(void *entry) {
    if (!entry) return nullptr;
    std::lock_guard lock{g_guest_lock};
    const auto found = g_guest_indices.find(entry);
    unsigned index;
    if (found != g_guest_indices.end()) {
        index = found->second;
    } else {
        index = static_cast<unsigned>(g_guest_indices.size());
        if (index == kMaxGuestStubs) return nullptr;
        g_guest_indices.emplace(entry, index);
        g_guest_entries[index] = entry;
    }
    return dm_guest_cpu_guest_stubs + index * 16;
}

extern "C" void dm_guest_cpu_guest_entry_c(uint32_t index, dm_guest_cpu_host_call_frame *frame) {
    void *entry;
    {
        std::lock_guard lock{g_guest_lock};
        entry = index < kMaxGuestStubs ? g_guest_entries[index] : nullptr;
    }
    if (!entry || !frame) {
        g_last_error = "invalid host-to-guest trampoline";
        std::abort();
    }

    dm_guest_cpu_state state{};
    for (unsigned i = 0; i < 6; i++) state.x[i] = frame->gpr[i];
    if (frame->stack_args) {
        // SysV places integer arguments after r9 on the caller's stack. The
        // first two become AAPCS64 x6/x7; the remainder are copied to the
        // translated callee's stack argument area.
        state.x[6] = frame->stack_args[0];
        state.x[7] = frame->stack_args[1];
    }
    for (unsigned i = 0; i < 8; i++) std::memcpy(state.v[i], frame->v[i], sizeof frame->v[i]);

    std::unique_ptr<uint8_t[]> stack{new (std::nothrow) uint8_t[kGuestCallStackSize]};
    if (!stack) {
        g_last_error = "host-to-guest stack allocation failed";
        std::abort();
    }
    uintptr_t sp = reinterpret_cast<uintptr_t>(stack.get() + kGuestCallStackSize - kGuestStackArgBytes) & ~uintptr_t(0xf);
    if (frame->stack_args) std::memcpy(reinterpret_cast<void *>(sp), frame->stack_args + 2, kGuestStackArgBytes);
    state.sp = sp;

    if (dm_guest_cpu_run(&state, reinterpret_cast<uintptr_t>(entry), 0,
                         dm_guest_cpu_svc_dispatch, nullptr) != 0) {
        std::fprintf(stderr, "[dm] FATAL translated host-to-guest call %p: %s\n", entry, dm_guest_cpu_last_error());
        std::abort();
    }
    frame->ret_x[0] = state.x[0];
    frame->ret_x[1] = state.x[1];
    std::memcpy(frame->ret_v[0], state.v[0], sizeof frame->ret_v[0]);
    std::memcpy(frame->ret_v[1], state.v[1], sizeof frame->ret_v[1]);
}

static void *make_hle_stub(void *fn, bool sret) {
    if (!fn) return nullptr;
    std::lock_guard lock{g_hle_lock};
    for (unsigned i = 0; i < g_nhle_stubs; i++) {
        if (g_hle_functions[i] == fn && g_hle_sret[i] == sret) return g_hle_stubs[i];
    }
    if (g_nhle_stubs == kMaxHleStubs) return nullptr;
    const unsigned i = g_nhle_stubs++;
    g_hle_functions[i] = fn;
    g_hle_sret[i] = sret;
    g_hle_stubs[i][0] = 0xd4000001u | ((DM_GUEST_CPU_SVC_HLE_BASE + i) << 5);
    g_hle_stubs[i][1] = kRetInstruction;
    return g_hle_stubs[i];
}

extern "C" void *dm_guest_cpu_hle_stub(void *fn) { return make_hle_stub(fn, false); }
extern "C" void *dm_guest_cpu_hle_sret_stub(void *fn) { return make_hle_stub(fn, true); }

extern "C" void *dm_guest_cpu_unimplemented_stub(unsigned index) {
    if (index >= kMaxUnimplementedStubs) return nullptr;
    g_unimplemented_stubs[index][0] = 0xd4000001u | ((DM_GUEST_CPU_SVC_UNIMPLEMENTED_BASE + index) << 5);
    g_unimplemented_stubs[index][1] = kRetInstruction;
    return g_unimplemented_stubs[index];
}

extern "C" void dm_guest_cpu_invoke_host(void *fn, dm_guest_cpu_state *state);
extern "C" void dm_guest_cpu_invoke_host_sret(void *fn, dm_guest_cpu_state *state);

extern "C" int dm_guest_cpu_hle_dispatch(uint32_t number, dm_guest_cpu_state *state) {
    if (number < DM_GUEST_CPU_SVC_HLE_BASE || number >= DM_GUEST_CPU_SVC_HLE_BASE + kMaxHleStubs) return 0;
    const unsigned i = number - DM_GUEST_CPU_SVC_HLE_BASE;
    void *fn = nullptr;
    bool sret = false;
    {
        std::lock_guard lock{g_hle_lock};
        if (i >= g_nhle_stubs || !g_hle_functions[i]) {
            g_last_error = "guest entered an unregistered HLE stub";
            return -1;
        }
        fn = g_hle_functions[i];
        sret = g_hle_sret[i];
    }
    // An HLE implementation may lazily resolve another import. Do not hold
    // the registration mutex while it is running.
    if (sret) dm_guest_cpu_invoke_host_sret(fn, state);
    else dm_guest_cpu_invoke_host(fn, state);
    return 1;
}

// The implementation lives in guest_cpu_abi_x86_64.S.  Keep this declaration
// in the same translation unit so a missing assembly object fails at link time
// instead of silently falling back to a native function-pointer call.
extern "C" void dm_guest_cpu_invoke_host(void *fn, dm_guest_cpu_state *state);
extern "C" void dm_guest_cpu_invoke_host_sret(void *fn, dm_guest_cpu_state *state);

extern "C" int dm_guest_cpu_run(dm_guest_cpu_state *state, uint64_t entry, uint64_t stop_pc,
                                 dm_guest_cpu_svc_fn svc, void *context) {
    if (!state || !entry) {
        g_last_error = "invalid translated guest CPU entry";
        return -1;
    }
    g_last_error = nullptr;
    if (!stop_pc) stop_pc = reinterpret_cast<uintptr_t>(&g_stop_stub);

    Callbacks callbacks;
    callbacks.state = state;
    callbacks.svc = svc;
    callbacks.context = context;

    Dynarmic::A64::UserConfig config{};
    config.callbacks = &callbacks;
    config.global_monitor = &g_guest_global_monitor;
    config.cntfrq_el0 = 19'200'000;
    config.wall_clock_cntpct = true;
    // Dynarmic's UserConfig fields point to host storage containing the
    // register value. TPIDRRO_EL0 itself is the address of the translated TSD
    // table; pointing the field at g_guest_tsd would instead load its first
    // slot (K18), making every guest TSD access relative to zero.
    g_guest_tpidrro_el0 = reinterpret_cast<uint64_t>(g_guest_tsd);
    config.tpidrro_el0 = &g_guest_tpidrro_el0;
    config.tpidr_el0 = &g_guest_tpidr_el0;
    Dynarmic::A64::Jit cpu{config};
    callbacks.cpu = &cpu;

    callbacks.sync_to_cpu();
    cpu.SetRegister(30, stop_pc);
    cpu.SetPC(entry);
    // Dynarmic returns with no halt reason when a translated block reaches a
    // not-yet-compiled branch target.  A guest function is not done until it
    // reaches the synthetic SVC return, so keep dispatching those block
    // boundaries instead of treating the first one as a failed call.
    Dynarmic::HaltReason reason{};
    for (;;) {
        reason = cpu.Run();
        if (callbacks.stopped || g_last_error) break;
        if (static_cast<uint32_t>(reason) != 0) break;
    }
    callbacks.sync_from_cpu();

    if (!callbacks.stopped) {
        if (!g_last_error) {
            std::snprintf(g_last_error_detail, sizeof g_last_error_detail,
                          "translated guest CPU stopped before its synthetic return (reason=%#x pc=%#llx x30=%#llx)",
                          static_cast<unsigned>(reason),
                          static_cast<unsigned long long>(cpu.GetPC()),
                          static_cast<unsigned long long>(cpu.GetRegister(30)));
            g_last_error = g_last_error_detail;
        }
        return -1;
    }
    return 0;
}

extern "C" const char *dm_guest_cpu_last_error(void) {
    return g_last_error ? g_last_error : "";
}
