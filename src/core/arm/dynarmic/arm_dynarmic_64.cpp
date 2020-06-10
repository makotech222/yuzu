// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cinttypes>
#include <memory>
#include <dynarmic/A64/a64.h>
#include <dynarmic/A64/config.h>
#include "common/logging/log.h"
#include "common/page_table.h"
#include "core/arm/cpu_interrupt_handler.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_util.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/scheduler.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/settings.h"

namespace Core {

using Vector = Dynarmic::A64::Vector;

class DynarmicCallbacks64 : public Dynarmic::A64::UserCallbacks {
public:
    explicit DynarmicCallbacks64(ARM_Dynarmic_64& parent) : parent(parent) {}

    u8 MemoryRead8(u64 vaddr) override {
        return parent.system.Memory().Read8(vaddr);
    }
    u16 MemoryRead16(u64 vaddr) override {
        return parent.system.Memory().Read16(vaddr);
    }
    u32 MemoryRead32(u64 vaddr) override {
        return parent.system.Memory().Read32(vaddr);
    }
    u64 MemoryRead64(u64 vaddr) override {
        return parent.system.Memory().Read64(vaddr);
    }
    Vector MemoryRead128(u64 vaddr) override {
        auto& memory = parent.system.Memory();
        return {memory.Read64(vaddr), memory.Read64(vaddr + 8)};
    }

    void MemoryWrite8(u64 vaddr, u8 value) override {
        parent.system.Memory().Write8(vaddr, value);
    }
    void MemoryWrite16(u64 vaddr, u16 value) override {
        parent.system.Memory().Write16(vaddr, value);
    }
    void MemoryWrite32(u64 vaddr, u32 value) override {
        parent.system.Memory().Write32(vaddr, value);
    }
    void MemoryWrite64(u64 vaddr, u64 value) override {
        parent.system.Memory().Write64(vaddr, value);
    }
    void MemoryWrite128(u64 vaddr, Vector value) override {
        auto& memory = parent.system.Memory();
        memory.Write64(vaddr, value[0]);
        memory.Write64(vaddr + 8, value[1]);
    }

    bool MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) override {
        return parent.system.Memory().WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) override {
        return parent.system.Memory().WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) override {
        return parent.system.Memory().WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) override {
        return parent.system.Memory().WriteExclusive64(vaddr, value, expected);
    }
    bool MemoryWriteExclusive128(u64 vaddr, Vector value, Vector expected) override {
        return parent.system.Memory().WriteExclusive128(vaddr, value, expected);
    }

    void InterpreterFallback(u64 pc, std::size_t num_instructions) override {
        LOG_INFO(Core_ARM, "Unicorn fallback @ 0x{:X} for {} instructions (instr = {:08X})", pc,
                 num_instructions, MemoryReadCode(pc));

        ARM_Interface::ThreadContext64 ctx;
        parent.SaveContext(ctx);
        parent.inner_unicorn.LoadContext(ctx);
        parent.inner_unicorn.ExecuteInstructions(num_instructions);
        parent.inner_unicorn.SaveContext(ctx);
        parent.LoadContext(ctx);
        num_interpreted_instructions += num_instructions;
    }

    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override {
        switch (exception) {
        case Dynarmic::A64::Exception::WaitForInterrupt:
        case Dynarmic::A64::Exception::WaitForEvent:
        case Dynarmic::A64::Exception::SendEvent:
        case Dynarmic::A64::Exception::SendEventLocal:
        case Dynarmic::A64::Exception::Yield:
            return;
        case Dynarmic::A64::Exception::Breakpoint:
            if (GDBStub::IsServerEnabled()) {
                parent.jit->HaltExecution();
                parent.SetPC(pc);
                Kernel::Thread* const thread = parent.system.CurrentScheduler().GetCurrentThread();
                parent.SaveContext(thread->GetContext64());
                GDBStub::Break();
                GDBStub::SendTrap(thread, 5);
                return;
            }
            [[fallthrough]];
        default:
            ASSERT_MSG(false, "ExceptionRaised(exception = {}, pc = {:X})",
                       static_cast<std::size_t>(exception), pc);
        }
    }

    void CallSVC(u32 swi) override {
        Kernel::Svc::Call(parent.system, swi);
    }

    void AddTicks(u64 ticks) override {
        if (parent.uses_wall_clock) {
            return;
        }
        // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
        // rough approximation of the amount of executed ticks in the system, it may be thrown off
        // if not all cores are doing a similar amount of work. Instead of doing this, we should
        // device a way so that timing is consistent across all cores without increasing the ticks 4
        // times.
        u64 amortized_ticks =
            (ticks - num_interpreted_instructions) / Core::Hardware::NUM_CPU_CORES;
        // Always execute at least one tick.
        amortized_ticks = std::max<u64>(amortized_ticks, 1);

        parent.system.CoreTiming().AddTicks(amortized_ticks);
        num_interpreted_instructions = 0;
    }

    u64 GetTicksRemaining() override {
        if (parent.uses_wall_clock) {
            if (!parent.interrupt_handlers[parent.core_index].IsInterrupted()) {
                return 1000U;
            }
            return 0U;
        }
        return std::max<s64>(parent.system.CoreTiming().GetDowncount(), 0);
    }

    u64 GetCNTPCT() override {
        return parent.system.CoreTiming().GetClockTicks();
    }

    ARM_Dynarmic_64& parent;
    std::size_t num_interpreted_instructions = 0;
    u64 tpidrro_el0 = 0;
    u64 tpidr_el0 = 0;
};

std::shared_ptr<Dynarmic::A64::Jit> ARM_Dynarmic_64::MakeJit(Common::PageTable& page_table,
                                                             std::size_t address_space_bits) const {
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = cb.get();

    // Memory
    config.page_table = reinterpret_cast<void**>(page_table.pointers.data());
    config.page_table_address_space_bits = address_space_bits;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;

    // Multi-process state
    config.processor_id = core_index;
    config.global_monitor = &exclusive_monitor.monitor;

    // System registers
    config.tpidrro_el0 = &cb->tpidrro_el0;
    config.tpidr_el0 = &cb->tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.cntfrq_el0 = Hardware::CNTFREQ;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Optimizations
    if (Settings::values.disable_cpu_opt) {
        config.enable_optimizations = false;
        config.enable_fast_dispatch = false;
    }

    // Timing
    config.wall_clock_cntpct = uses_wall_clock;

    return std::make_shared<Dynarmic::A64::Jit>(config);
}

void ARM_Dynarmic_64::Run() {
    jit->Run();
}

void ARM_Dynarmic_64::Step() {
    cb->InterpreterFallback(jit->GetPC(), 1);
}

ARM_Dynarmic_64::ARM_Dynarmic_64(System& system, CPUInterrupts& interrupt_handlers,
                                 bool uses_wall_clock, ExclusiveMonitor& exclusive_monitor,
                                 std::size_t core_index)
    : ARM_Interface{system, interrupt_handlers, uses_wall_clock},
      cb(std::make_unique<DynarmicCallbacks64>(*this)), inner_unicorn{system, interrupt_handlers,
                                                                      uses_wall_clock,
                                                                      ARM_Unicorn::Arch::AArch64,
                                                                      core_index},
      core_index{core_index}, exclusive_monitor{
                                  dynamic_cast<DynarmicExclusiveMonitor&>(exclusive_monitor)} {}

ARM_Dynarmic_64::~ARM_Dynarmic_64() = default;

void ARM_Dynarmic_64::SetPC(u64 pc) {
    jit->SetPC(pc);
}

u64 ARM_Dynarmic_64::GetPC() const {
    return jit->GetPC();
}

u64 ARM_Dynarmic_64::GetReg(int index) const {
    return jit->GetRegister(index);
}

void ARM_Dynarmic_64::SetReg(int index, u64 value) {
    jit->SetRegister(index, value);
}

u128 ARM_Dynarmic_64::GetVectorReg(int index) const {
    return jit->GetVector(index);
}

void ARM_Dynarmic_64::SetVectorReg(int index, u128 value) {
    jit->SetVector(index, value);
}

u32 ARM_Dynarmic_64::GetPSTATE() const {
    return jit->GetPstate();
}

void ARM_Dynarmic_64::SetPSTATE(u32 pstate) {
    jit->SetPstate(pstate);
}

u64 ARM_Dynarmic_64::GetTlsAddress() const {
    return cb->tpidrro_el0;
}

void ARM_Dynarmic_64::SetTlsAddress(VAddr address) {
    cb->tpidrro_el0 = address;
}

u64 ARM_Dynarmic_64::GetTPIDR_EL0() const {
    return cb->tpidr_el0;
}

void ARM_Dynarmic_64::SetTPIDR_EL0(u64 value) {
    cb->tpidr_el0 = value;
}

void ARM_Dynarmic_64::ChangeProcessorId(std::size_t new_core_id) {
    jit->ChangeProcessorID(new_core_id);
}

void ARM_Dynarmic_64::SaveContext(ThreadContext64& ctx) {
    ctx.cpu_registers = jit->GetRegisters();
    ctx.sp = jit->GetSP();
    ctx.pc = jit->GetPC();
    ctx.pstate = jit->GetPstate();
    ctx.vector_registers = jit->GetVectors();
    ctx.fpcr = jit->GetFpcr();
    ctx.fpsr = jit->GetFpsr();
    ctx.tpidr = cb->tpidr_el0;
}

void ARM_Dynarmic_64::LoadContext(const ThreadContext64& ctx) {
    jit->SetRegisters(ctx.cpu_registers);
    jit->SetSP(ctx.sp);
    jit->SetPC(ctx.pc);
    jit->SetPstate(ctx.pstate);
    jit->SetVectors(ctx.vector_registers);
    jit->SetFpcr(ctx.fpcr);
    jit->SetFpsr(ctx.fpsr);
    SetTPIDR_EL0(ctx.tpidr);
}

void ARM_Dynarmic_64::PrepareReschedule() {
    jit->HaltExecution();
}

void ARM_Dynarmic_64::ClearInstructionCache() {
    if (!jit) {
        return;
    }
    jit->ClearCache();
}

void ARM_Dynarmic_64::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

void ARM_Dynarmic_64::PageTableChanged(Common::PageTable& page_table,
                                       std::size_t new_address_space_size_in_bits) {
    auto key = std::make_pair(&page_table, new_address_space_size_in_bits);
    auto iter = jit_cache.find(key);
    if (iter != jit_cache.end()) {
        jit = iter->second;
        return;
    }
    jit = MakeJit(page_table, new_address_space_size_in_bits);
    jit_cache.emplace(key, jit);
}

DynarmicExclusiveMonitor::DynarmicExclusiveMonitor(Memory::Memory& memory, std::size_t core_count)
    : monitor(core_count), memory{memory} {}

DynarmicExclusiveMonitor::~DynarmicExclusiveMonitor() = default;

u8 DynarmicExclusiveMonitor::ExclusiveRead8(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u8>(core_index, addr, [&]() -> u8 { return memory.Read8(addr); });
}

u16 DynarmicExclusiveMonitor::ExclusiveRead16(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u16>(core_index, addr, [&]() -> u16 { return memory.Read16(addr); });
}

u32 DynarmicExclusiveMonitor::ExclusiveRead32(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u32>(core_index, addr, [&]() -> u32 { return memory.Read32(addr); });
}

u64 DynarmicExclusiveMonitor::ExclusiveRead64(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u64>(core_index, addr, [&]() -> u64 { return memory.Read64(addr); });
}

u128 DynarmicExclusiveMonitor::ExclusiveRead128(std::size_t core_index, VAddr addr) {
    return monitor.ReadAndMark<u128>(core_index, addr, [&]() -> u128 {
        u128 result;
        result[0] = memory.Read64(addr);
        result[1] = memory.Read64(addr + 8);
        return result;
    });
}

void DynarmicExclusiveMonitor::ClearExclusive() {
    monitor.Clear();
}

bool DynarmicExclusiveMonitor::ExclusiveWrite8(std::size_t core_index, VAddr vaddr, u8 value) {
    return monitor.DoExclusiveOperation<u8>(core_index, vaddr, [&](u8 expected) -> bool {
        return memory.WriteExclusive8(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite16(std::size_t core_index, VAddr vaddr, u16 value) {
    return monitor.DoExclusiveOperation<u16>(core_index, vaddr, [&](u16 expected) -> bool {
        return memory.WriteExclusive16(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite32(std::size_t core_index, VAddr vaddr, u32 value) {
    return monitor.DoExclusiveOperation<u32>(core_index, vaddr, [&](u32 expected) -> bool {
        return memory.WriteExclusive32(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite64(std::size_t core_index, VAddr vaddr, u64 value) {
    return monitor.DoExclusiveOperation<u64>(core_index, vaddr, [&](u64 expected) -> bool {
        return memory.WriteExclusive64(vaddr, value, expected);
    });
}

bool DynarmicExclusiveMonitor::ExclusiveWrite128(std::size_t core_index, VAddr vaddr, u128 value) {
    return monitor.DoExclusiveOperation<u128>(core_index, vaddr, [&](u128 expected) -> bool {
        return memory.WriteExclusive128(vaddr, value, expected);
    });
}

} // namespace Core
