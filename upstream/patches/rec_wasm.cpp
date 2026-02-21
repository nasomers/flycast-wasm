// rec_wasm.cpp — WASM JIT backend for Flycast SH4 dynarec
//
// Phase 1: Skeleton implementing Sh4Dynarec interface with interpreter fallback.
// Blocks are decoded into SHIL IR (proving the pipeline works) but execution
// falls back to per-instruction interpretation. Phase 2+ will emit WASM bytecode.
//
// This file is compiled when FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC
// (set in build.h for __EMSCRIPTEN__).

#include "build.h"

#if FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC

#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "oslib/virtmem.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Cycle cost per interpreted instruction (approximate)
static constexpr int CYCLES_PER_INSTRUCTION = 1;

// Forward declarations from driver.cpp
DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc);

class WasmDynarec : public Sh4Dynarec
{
public:
	WasmDynarec()
	{
		sh4Dynarec = this;
	}

	void init(Sh4Context& ctx, Sh4CodeBuffer& buf) override
	{
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.log('[rec_wasm] WasmDynarec::init() ENTERED, this=' + $0); }, (int)(uintptr_t)this);
#endif
		sh4ctx = &ctx;
		codeBuffer = &buf;
#ifdef __EMSCRIPTEN__
		EM_ASM({ console.log('[rec_wasm] WASM JIT backend initialized (Phase 1: interpreter fallback)'); });
#endif
	}

	void compile(RuntimeBlockInfo* block, bool smc_checks, bool optimise) override
	{
		// Phase 1: The block's SHIL IR has already been decoded by block->Setup()
		// (called in driver.cpp before compile()). We just need to set a valid
		// code entry in the code buffer so the block manager can register it.
		//
		// We write a 4-byte dummy marker. The mainloop doesn't call these entries
		// as function pointers — it uses interpreter fallback instead.

		block->code = (DynarecCodeEntryPtr)codeBuffer->get();

		// Advance the buffer to give this block a unique address
		// (needed for block manager lookup and FPCB table)
		u32 spaceNeeded = 4;
		if (codeBuffer->getFreeSpace() >= spaceNeeded)
			codeBuffer->advance(spaceNeeded);
	}

	void mainloop(void* cntx) override
	{
		// Phase 1: Pure interpreter dispatch loop.
		//
		// Matches Sh4Interpreter::Run() exactly — no FPCB lookups (FPCB only
		// covers 32MB of address space, SH4 boot PC is 0xA0000000).
		// Phase 3 will add FPCB dispatch with proper address masking.

		// CRITICAL: Branch instructions with delay slots call executeDelaySlot()
		// which dereferences Sh4Interpreter::Instance. In the JIT path,
		// Sh4Recompiler::Instance is set (in constructor), but the base class
		// Sh4Interpreter::Instance is never set — Sh4Recompiler shadows it.
		// Since Sh4Recompiler IS-A Sh4Interpreter, we can just assign it.
		Sh4Interpreter::Instance = Sh4Recompiler::Instance;

#ifdef __EMSCRIPTEN__
		static int mainloop_count = 0;
		mainloop_count++;
		if (mainloop_count <= 5 || (mainloop_count % 10) == 0) {
			EM_ASM({ console.log('[rec_wasm] Entering mainloop #' + $0); }, mainloop_count);
		}
#endif

		u32 insn_count = 0;
		u32 timeslice_count = 0;
		bool exited_via_exception = false;

		try {
			do {
				try {
					do {
						u32 addr = sh4ctx->pc;
						sh4ctx->pc = addr + 2;
						u16 op = IReadMem16(addr);

						// Check for FPU disabled exception
						if (sh4ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
							throw SH4ThrownException(addr, Sh4Ex_FpuDisabled);

						OpPtr[op](sh4ctx, op);
						sh4ctx->cycle_counter -= CYCLES_PER_INSTRUCTION;
						insn_count++;

#ifdef __EMSCRIPTEN__
						// Minimal diagnostics — log milestones every 2M instructions (first mainloop)
						if (mainloop_count == 1 && (insn_count % 2000000) == 0) {
							EM_ASM({ console.log('[rec_wasm] PC@' + $0 + 'M: 0x' + ($1>>>0).toString(16).padStart(8,'0')); },
								insn_count / 1000000, addr);
						}
#endif

					} while (sh4ctx->cycle_counter > 0);

					// Time slice expired — process interrupts and system events
					sh4ctx->cycle_counter += SH4_TIMESLICE;
					timeslice_count++;
					UpdateSystem_INTC();

				} catch (const SH4ThrownException& ex) {
					Do_Exception(ex.epc, ex.expEvn);
					sh4ctx->cycle_counter += 5;  // exception drain cycles
				}
			} while (sh4ctx->CpuRunning);

		} catch (...) {
			exited_via_exception = true;
#ifdef __EMSCRIPTEN__
			EM_ASM({ console.log('[rec_wasm] WARNING: mainloop exited via catch(...) — exception swallowed!'); });
#endif
		}

		sh4ctx->CpuRunning = false;

#ifdef __EMSCRIPTEN__
		if (mainloop_count <= 5 || (mainloop_count % 10) == 0) {
			EM_ASM({ console.log('[rec_wasm] Exited mainloop #' + $0 + ': insns=' + $1 + ', timeslices=' + $2 + ', exception=' + $3); },
				mainloop_count, insn_count, timeslice_count, exited_via_exception ? 1 : 0);
		}
#endif
	}

	void handleException(host_context_t& context) override
	{
		// Phase 1: No native code, so no host exception handling needed.
		// In native backends, this rewrites the host PC to jump to the
		// exception handler. In our case, C++ exceptions handle this.
	}

	bool rewrite(host_context_t& context, void* faultAddress) override
	{
		// Phase 1: No fast memory accesses to rewrite.
		return false;
	}

	void reset() override
	{
		// Called when the code buffer is cleared.
		// Phase 1: nothing to do (no generated code to invalidate).
		// Phase 3: will need to invalidate compiled WASM module cache.
	}

	// Canonical callback interface — used by shil_canonical.h to generate
	// calls to default op implementations when the backend can't emit native code.
	// Phase 1: Not used (we interpret SH4 directly, not SHIL ops).
	// Phase 2: Will be used for ops we can't emit WASM for.
	void canonStart(const shil_opcode* op) override {}
	void canonParam(const shil_opcode* op, const shil_param* par, CanonicalParamType tp) override {}
	void canonCall(const shil_opcode* op, void* function) override {}
	void canonFinish(const shil_opcode* op) override {}

private:
	Sh4Context* sh4ctx = nullptr;
	Sh4CodeBuffer* codeBuffer = nullptr;
};

static WasmDynarec instance;

// Explicit init function callable from driver.cpp to ensure the
// static WasmDynarec instance is linked in and sh4Dynarec is set.
// Archive linkers may strip translation units with no referenced symbols,
// so this gives driver.cpp something to call.
extern "C" void wasm_dynarec_init()
{
	if (!sh4Dynarec)
		sh4Dynarec = &instance;
}

#endif // FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_GENERIC
