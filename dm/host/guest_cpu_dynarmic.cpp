// Intel guest CPU backend.
//
// Dynarmic supplies the AArch64 frontend and x86_64 JIT.  This first layer is
// deliberately small: it owns CPU state, memory callbacks and the synthetic
// return/SVC boundary.  Horizon SVCs, host HLE calls and thread suspension are
// connected by later layers so that the Apple-silicon execution path remains
// unchanged while the translated path is brought up.

#include "guest_cpu.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"

extern "C" uint32_t thr_cur_handle(void) __attribute__((weak));

namespace {

int (*thr_translated_pause_requested)(void);
void (*thr_translated_pause)(const dm_guest_cpu_state *);

constexpr uint32_t kStopSvc = 0x7fff;
constexpr uint32_t kStopInstruction = 0xd4000001u | (kStopSvc << 5);
constexpr uint32_t kGateInstruction = 0xd4000001u | (DM_GUEST_CPU_SVC_GATE << 5);
constexpr unsigned kMaxHleStubs = 8192;
constexpr unsigned kMaxUnimplementedStubs = 4096;
constexpr unsigned kMaxGuestStubs = 65536;
constexpr size_t kGuestCallStackSize = 1 << 20;
constexpr size_t kGuestStackArgBytes = 256;
// ExclusiveMonitor emits one invalidation check for each slot. Keep a
// bounded pool for concurrently executing guest threads, assigning IDs on
// demand instead of aliasing every thread pinned to the same Horizon core.
constexpr size_t kGuestProcessorCount = 64;
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
thread_local char g_last_error_detail[512];
alignas(16) thread_local uint8_t g_guest_tsd[DM_GUEST_CPU_TSD_GATE + sizeof(uint64_t)]{};
thread_local uint64_t g_guest_tpidrro_el0;
thread_local uint64_t g_guest_tpidr_el0;
Dynarmic::ExclusiveMonitor g_guest_global_monitor{kGuestProcessorCount};
std::mutex g_processor_lock;
std::unordered_map<uint32_t, size_t> g_processor_ids;
std::array<bool, kGuestProcessorCount> g_processor_used = [] {
    std::array<bool, kGuestProcessorCount> used{};
    used[0] = true; // CPU smoke / non-guest callers use the reserved slot.
    return used;
}();

size_t current_guest_processor_id() {
    if (!thr_cur_handle) return 0;
    const uint32_t handle = thr_cur_handle();
    if (!handle) return 0;
    std::lock_guard guard{g_processor_lock};
    if (auto it = g_processor_ids.find(handle); it != g_processor_ids.end()) return it->second;
    for (size_t id = 1; id < kGuestProcessorCount; id++) {
        if (g_processor_used[id]) continue;
        g_processor_used[id] = true;
        g_processor_ids.emplace(handle, id);
        return id;
    }
    g_last_error = "Dynarmic exclusive monitor slots exhausted (64 concurrent guest threads maximum)";
    return 0;
}

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

template <typename T>
bool exclusive_store(uint64_t address, T value, T expected) {
    return __atomic_compare_exchange_n(
        reinterpret_cast<T *>(static_cast<uintptr_t>(address)), &expected, value, false,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

class Callbacks final : public Dynarmic::A64::UserCallbacks {
public:
    dm_guest_cpu_state *state = nullptr;
    Dynarmic::A64::Jit *cpu = nullptr;
    dm_guest_cpu_svc_fn svc = nullptr;
    void *context = nullptr;
    bool stopped = false;
    bool sample_pc = false;
    size_t processor_id = 0;
    uint32_t last_svc = 0;
    uint64_t last_svc_pc = 0;
    uint64_t last_svc_lr = 0;

    void diagnose_low_address(const char *op, uint64_t vaddr, size_t size,
                              uint64_t value = 0) {
        if (vaddr >= 0x10000) return;
        char line[256];
        int n = std::snprintf(line, sizeof line,
                              "[dm] translated guest %s addr=%#llx size=%zu value=%#llx callbacks.cpu=%p\n",
                              op, static_cast<unsigned long long>(vaddr), size,
                              static_cast<unsigned long long>(value), (void *)cpu);
        if (n > 0) (void)::write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        if (!cpu) return;
        const uint64_t pc = cpu->GetPC();
        const uint64_t sp = cpu->GetSP();
        n = std::snprintf(line, sizeof line, "[dm] translated guest state pc=%#llx sp=%#llx\n",
                          static_cast<unsigned long long>(pc),
                          static_cast<unsigned long long>(sp));
        if (n > 0) (void)::write(STDERR_FILENO, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        const auto regs = cpu->GetRegisters();
        for (size_t i = 0; i < regs.size(); i++)
        {
            n = std::snprintf(line, sizeof line, "[dm] guest x%zu=%#llx\n", i,
                              static_cast<unsigned long long>(regs[i]));
            if (n > 0) (void)::write(STDERR_FILENO, line,
                                     (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        }
        const size_t dump_regs[] = {20, 22, 24};
        for (size_t i : dump_regs) {
            uint64_t words[8]{};
            mach_vm_size_t copied = 0;
            const kern_return_t kr = mach_vm_read_overwrite(
                mach_task_self(), static_cast<mach_vm_address_t>(regs[i]), sizeof words,
                reinterpret_cast<mach_vm_address_t>(words), &copied);
            if (kr != KERN_SUCCESS || copied != sizeof words) continue;
            n = std::snprintf(line, sizeof line, "[dm] guest *x%zu:", i);
            for (uint64_t word : words) {
                if (n <= 0 || static_cast<size_t>(n) >= sizeof line) break;
                n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " %#llx",
                                   static_cast<unsigned long long>(word));
            }
            if (n > 0) {
                line[sizeof line - 2] = '\n';
                line[sizeof line - 1] = '\0';
                (void)::write(STDERR_FILENO, line, strnlen(line, sizeof line));
            }
        }
    }

    std::optional<uint32_t> MemoryReadCode(uint64_t vaddr) override {
        diagnose_low_address("read-code", vaddr, sizeof(uint32_t));
        return load<uint32_t>(vaddr);
    }

    uint8_t MemoryRead8(uint64_t vaddr) override {
        diagnose_low_address("read", vaddr, sizeof(uint8_t));
        return load<uint8_t>(vaddr);
    }
    uint16_t MemoryRead16(uint64_t vaddr) override {
        diagnose_low_address("read", vaddr, sizeof(uint16_t));
        return load<uint16_t>(vaddr);
    }
    uint32_t MemoryRead32(uint64_t vaddr) override {
        diagnose_low_address("read", vaddr, sizeof(uint32_t));
        return load<uint32_t>(vaddr);
    }
    uint64_t MemoryRead64(uint64_t vaddr) override {
        diagnose_low_address("read", vaddr, sizeof(uint64_t));
        return load<uint64_t>(vaddr);
    }

    Dynarmic::A64::Vector MemoryRead128(uint64_t vaddr) override {
        diagnose_low_address("read", vaddr, sizeof(Dynarmic::A64::Vector));
        return {load<uint64_t>(vaddr), load<uint64_t>(vaddr + 8)};
    }

    void MemoryWrite8(uint64_t vaddr, uint8_t value) override { store(vaddr, value); }
    void MemoryWrite16(uint64_t vaddr, uint16_t value) override { store(vaddr, value); }
    void MemoryWrite32(uint64_t vaddr, uint32_t value) override {
        // A guest null dereference currently reaches this callback as a raw
        // host pointer because the loader maps guest memory into the process.
        // Preserve the crash, but capture the translated state before memcpy
        // faults in host code and loses the guest PC/register context.
        diagnose_low_address("write", vaddr, sizeof(value), value);
        store(vaddr, value);
    }
    void MemoryWrite64(uint64_t vaddr, uint64_t value) override {
        store(vaddr, value);
    }

    void MemoryWrite128(uint64_t vaddr, Dynarmic::A64::Vector value) override {
        store(vaddr, value[0]);
        store(vaddr + 8, value[1]);
    }

    bool MemoryWriteExclusive8(uint64_t vaddr, uint8_t value, uint8_t expected) override {
        return exclusive_store(vaddr, value, expected);
    }

    bool MemoryWriteExclusive16(uint64_t vaddr, uint16_t value, uint16_t expected) override {
        return exclusive_store(vaddr, value, expected);
    }

    bool MemoryWriteExclusive32(uint64_t vaddr, uint32_t value, uint32_t expected) override {
        return exclusive_store(vaddr, value, expected);
    }

    bool MemoryWriteExclusive64(uint64_t vaddr, uint64_t value, uint64_t expected) override {
        return exclusive_store(vaddr, value, expected);
    }

    bool MemoryWriteExclusive128(uint64_t vaddr, Dynarmic::A64::Vector value,
                                 Dynarmic::A64::Vector expected) override {
        // Dynarmic serializes the translated exclusive sequence through the
        // global monitor.  Keep the host-side pair together as well; the
        // 128-bit AArch64 callback has no portable lock-free x86_64 form.
        static std::mutex lock;
        std::lock_guard guard{lock};
        Dynarmic::A64::Vector current{load<uint64_t>(vaddr), load<uint64_t>(vaddr + 8)};
        if (current != expected) return false;
        MemoryWrite64(vaddr, value[0]);
        MemoryWrite64(vaddr + 8, value[1]);
        return true;
    }

    void InterpreterFallback(uint64_t pc, size_t instructions) override {
        uint64_t last_hle_offset = 0;
        if (last_svc >= DM_GUEST_CPU_SVC_HLE_BASE &&
            last_svc < DM_GUEST_CPU_SVC_HLE_BASE + g_nhle_stubs) {
            void *fn = g_hle_functions[last_svc - DM_GUEST_CPU_SVC_HLE_BASE];
            Dl_info image{};
            if (fn && dladdr(fn, &image) && image.dli_fbase)
                last_hle_offset = reinterpret_cast<uintptr_t>(fn) -
                                  reinterpret_cast<uintptr_t>(image.dli_fbase);
        }
        mach_vm_address_t region = pc;
        mach_vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t region_info{};
        mach_msg_type_number_t region_count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t region_result = mach_vm_region(mach_task_self(), &region, &region_size,
            VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&region_info),
            &region_count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        const bool readable = region_result == KERN_SUCCESS && region <= pc &&
            region_size >= 4 && pc - region <= region_size - 4 &&
            (region_info.protection & VM_PROT_READ);
        std::snprintf(g_last_error_detail, sizeof g_last_error_detail,
                      "Dynarmic fallback pc=%#llx insn=%#x count=%zu lr=%#llx x8=%#llx x9=%#llx x19=%#llx region=%#llx+%#llx prot=%#x last_svc=%#x fn+%#llx@%#llx lr=%#llx",
                      static_cast<unsigned long long>(pc), readable ? load<uint32_t>(pc) : 0, instructions,
                      static_cast<unsigned long long>(cpu ? cpu->GetRegister(30) : 0),
                      static_cast<unsigned long long>(cpu ? cpu->GetRegister(8) : 0),
                      static_cast<unsigned long long>(cpu ? cpu->GetRegister(9) : 0),
                      static_cast<unsigned long long>(cpu ? cpu->GetRegister(19) : 0),
                      static_cast<unsigned long long>(region),
                      static_cast<unsigned long long>(region_size),
                      region_result == KERN_SUCCESS ? region_info.protection : 0,
                      last_svc, static_cast<unsigned long long>(last_hle_offset),
                      static_cast<unsigned long long>(last_svc_pc),
                      static_cast<unsigned long long>(last_svc_lr));
        g_last_error = g_last_error_detail;
        if (cpu) cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
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
        // An exception boundary clears the local exclusive monitor.
        if (cpu) cpu->ClearExclusiveState();
        g_guest_global_monitor.ClearProcessor(processor_id);
        last_svc = number;
        last_svc_pc = state->pc;
        last_svc_lr = state->x[30];
        // Dynarmic advances PC to the instruction after SVC before calling
        // the host callback; preserve that value while the HLE runs.
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
    uint64_t GetTicksRemaining() override { return (sample_pc || thr_translated_pause_requested) ? 500'000 : std::numeric_limits<uint64_t>::max(); }
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

// A guest function called from a host HLE routine (for example, the SDK's
// StandardAllocator) may be entered thousands of times on one host thread.
// Constructing a Dynarmic JIT for every such call regenerates its x64
// fast-memory fallbacks each time. Keep one JIT per active call depth instead;
// depth-indexed contexts also make reentrant guest calls safe.
struct GuestRunContext {
    Callbacks callbacks;
    std::unique_ptr<Dynarmic::A64::Jit> cpu;
};
thread_local std::vector<std::unique_ptr<GuestRunContext>> g_guest_run_contexts;
thread_local size_t g_guest_run_depth;
thread_local std::unique_ptr<uint8_t[]> g_root_stack;

struct GuestRunScope {
    size_t depth = g_guest_run_depth++;
    ~GuestRunScope() { --g_guest_run_depth; }
};

}  // namespace

extern "C" uint8_t dm_guest_cpu_guest_stubs[];
extern "C" void dm_guest_cpu_svc_dispatch(void *, uint32_t, dm_guest_cpu_state *) __attribute__((weak));

extern "C" void dm_guest_cpu_set_pause_hooks(int (*requested)(void),
                                               void (*pause)(const dm_guest_cpu_state *)) {
    thr_translated_pause_requested = requested;
    thr_translated_pause = pause;
}

extern "C" void dm_guest_cpu_release_processor_id(uint32_t handle) {
    if (!handle) return;
    std::lock_guard guard{g_processor_lock};
    const auto it = g_processor_ids.find(handle);
    if (it == g_processor_ids.end()) return;
    const size_t id = it->second;
    g_guest_global_monitor.ClearProcessor(id);
    g_processor_ids.erase(it);
    g_processor_used[id] = false;
}

extern "C" uintptr_t dm_guest_cpu_initial_sp(void) {
    if (!g_root_stack) g_root_stack.reset(new uint8_t[kGuestCallStackSize]{});
    return reinterpret_cast<uintptr_t>(g_root_stack.get() + kGuestCallStackSize -
                                       kGuestStackArgBytes) & ~uintptr_t(0xf);
}

extern "C" int dm_guest_cpu_stack_region(uint64_t addr, uint64_t *base, uint64_t *size) {
    if (!g_root_stack) return 0;
    const uintptr_t start = reinterpret_cast<uintptr_t>(g_root_stack.get());
    if (addr < start || addr - start >= kGuestCallStackSize) return 0;
    *base = start;
    *size = kGuestCallStackSize;
    return 1;
}

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

    const dm_guest_cpu_state *parent = g_guest_run_depth
        ? g_guest_run_contexts[g_guest_run_depth - 1]->callbacks.state : nullptr;
    uintptr_t sp;
    if (parent && parent->sp) {
        // A nested SDK call belongs to the same guest thread. Keep its stack
        // contiguous and spill the suspended registers so a conservative
        // collector scanning this stack can still find the caller's roots.
        constexpr size_t rewritten_spill_bytes = 32;
        const uintptr_t saved = (parent->sp - rewritten_spill_bytes - sizeof *parent) & ~uintptr_t(0xf);
        std::memcpy(reinterpret_cast<void *>(saved), parent, sizeof *parent);
        sp = saved - kGuestStackArgBytes;
    } else {
        sp = dm_guest_cpu_initial_sp();
    }
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

    GuestRunScope scope;
    while (g_guest_run_contexts.size() <= scope.depth)
        g_guest_run_contexts.emplace_back(std::make_unique<GuestRunContext>());
    GuestRunContext &run = *g_guest_run_contexts[scope.depth];
    Callbacks &callbacks = run.callbacks;
    callbacks.state = state;
    callbacks.svc = svc;
    callbacks.context = context;
    callbacks.stopped = false;
    callbacks.last_svc = 0;
    callbacks.last_svc_pc = 0;
    callbacks.last_svc_lr = 0;
    callbacks.sample_pc = std::getenv("DM_TEST_GUEST_PC") != nullptr;

    // Dynarmic's UserConfig fields point to host storage containing the
    // register value. TPIDRRO_EL0 itself is the address of the translated TSD
    // table; pointing the field at g_guest_tsd would instead load its first
    // slot (K18), making every guest TSD access relative to zero.
    g_guest_tpidrro_el0 = reinterpret_cast<uint64_t>(g_guest_tsd);
    const size_t processor_id = current_guest_processor_id();
    if (g_last_error) return -1;
    callbacks.processor_id = processor_id;
    if (!run.cpu) {
        Dynarmic::A64::UserConfig config{};
        config.callbacks = &callbacks;
        config.global_monitor = &g_guest_global_monitor;
        config.processor_id = processor_id;
        config.enable_cycle_counting = callbacks.sample_pc || thr_translated_pause_requested;
        config.cntfrq_el0 = 19'200'000;
        config.wall_clock_cntpct = true;
        config.tpidrro_el0 = &g_guest_tpidrro_el0;
        config.tpidr_el0 = &g_guest_tpidr_el0;
        // The Intel host maps the guest address space directly at its emulated
        // addresses. Dynarmic can therefore emit direct host loads/stores for
        // mapped guest pages and fall back through its fault handler elsewhere.
        // Keep this opt-in until a representative game run confirms the A/B.
        const char *fastmem = std::getenv("DM_GUEST_CPU_FASTMEM");
        if (fastmem && std::strcmp(fastmem, "1") == 0) {
            config.fastmem_pointer = uintptr_t{0};
            config.fastmem_address_space_bits = 40; // the Mac guest AS stays below 2^40
            config.silently_mirror_fastmem = false;
        }
        run.cpu = std::make_unique<Dynarmic::A64::Jit>(config);
    }
    Dynarmic::A64::Jit *cpu = run.cpu.get();
    callbacks.cpu = cpu;

    callbacks.sync_to_cpu();
    cpu->SetRegister(30, stop_pc);
    cpu->SetPC(entry);
    // Dynarmic returns with no halt reason when a translated block reaches a
    // not-yet-compiled branch target.  A guest function is not done until it
    // reaches the synthetic SVC return, so keep dispatching those block
    // boundaries instead of treating the first one as a failed call.
    Dynarmic::HaltReason reason{};
    auto next_pc_sample = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
        reason = cpu->Run();
        if (callbacks.sample_pc && std::chrono::steady_clock::now() >= next_pc_sample) {
            std::fprintf(stderr, "[dm] DM_TEST_GUEST_PC: thread=%#x depth=%zu pc=%#llx lr=%#llx x0=%#llx last_svc=%#x svc_pc=%#llx\n",
                         thr_cur_handle ? thr_cur_handle() : 0, scope.depth,
                         static_cast<unsigned long long>(cpu->GetPC()),
                         static_cast<unsigned long long>(cpu->GetRegister(30)),
                         static_cast<unsigned long long>(cpu->GetRegister(0)),
                         callbacks.last_svc,
                         static_cast<unsigned long long>(callbacks.last_svc_pc));
            next_pc_sample = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        if (callbacks.stopped || g_last_error) break;
        if (static_cast<uint32_t>(reason) != 0) break;
        if (thr_translated_pause_requested && thr_translated_pause &&
            thr_translated_pause_requested()) {
            callbacks.sync_from_cpu();
            if (cpu) cpu->ClearExclusiveState();
            g_guest_global_monitor.ClearProcessor(processor_id);
            thr_translated_pause(state);
        }
    }
    callbacks.sync_from_cpu();
    if (cpu) cpu->ClearExclusiveState();
    g_guest_global_monitor.ClearProcessor(processor_id);

    if (!callbacks.stopped) {
        if (!g_last_error) {
            std::snprintf(g_last_error_detail, sizeof g_last_error_detail,
                          "translated guest CPU stopped before its synthetic return (reason=%#x pc=%#llx x30=%#llx)",
                          static_cast<unsigned>(reason),
                          static_cast<unsigned long long>(cpu->GetPC()),
                          static_cast<unsigned long long>(cpu->GetRegister(30)));
            g_last_error = g_last_error_detail;
        }
        return -1;
    }
    return 0;
}

extern "C" const char *dm_guest_cpu_last_error(void) {
    return g_last_error ? g_last_error : "";
}
