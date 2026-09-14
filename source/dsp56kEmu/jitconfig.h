#pragma once

#include <cstdint>
#include <optional>
#include <functional>

#include "types.h"

namespace dsp56k
{
	struct JitConfig
	{
		bool aguSupportBitreverse = false;
		bool aguSupportMultipleWrapModulo = true;
		bool cacheSingleOpBlocks = true;
		bool linkJitBlocks = true;
		bool splitOpsByNops = false;
		bool dynamicPeripheralAddressing = false;

		uint32_t maxInstructionsPerBlock = 0;
		bool memoryWritesCallCpp = false;
		// Use live storage for immediate peripheral reads that explicitly support
		// it. Disable for differential checks against the ordinary read handler.
		bool inlinePeripheralReads = true;
		// ARM64-only local CCR sequence reductions. Keep an unoptimized path for
		// differential checks of flags, lazy updates and conditional execution.
		bool optimizeCcrSequences = true;

		// 16 bit compatibility mode for AGU operations are not supported by default, set to true if needed
		bool support16BitSCMode = false;

		// maximum number of iterations of a do loop before the Jit block is exited (and later re-entered), giving a time slice for interrupts/peripherals
		uint32_t maxDoIterations = 0;
		// Combine bookkeeping only within the existing bounded slice of a complete
		// one-/two-NOP body. Keep the switch for exact return-boundary comparisons.
		bool combineNopLoopIterations = true;

		// needs to be true if there is code that executes code in interrupt regions as regular jumps
		bool dynamicFastInterrupts = false;

		// asmjit can validate the generate code, usually not needed
		bool asmjitDiagnostics = false;

		// enable JIT optimizer (dead code elimination + constant folding)
		bool enableOptimizer = true;

		// x86-64 only: Will issue int3() = breakpoint interrupt if a memory address is detected that points to peripherals but DPA is disabled
		bool debugDynamicPeripheralAddressing = false;

		// retrieves a JitConfig for a specific PC. If null, the global default config is used
		std::function<std::optional<JitConfig>(TWord)> getBlockConfig;
	};
}
