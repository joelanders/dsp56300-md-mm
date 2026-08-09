#include "assembler.h"
#include "dsp.h"
#include "interrupts.h"
#include "memory.h"
#include "peripherals.h"

#include <cstdio>
#include <stdexcept>

namespace
{
	using namespace dsp56k;

	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	class Fixture final
	{
	public:
		Fixture()
			: memory(validator, 0x1000, 0x1000, 0x0800)
			, dsp(memory, &peripherals, &noPeripherals)
		{
			auto config = dsp.getJit().getConfig();
			config.dynamicFastInterrupts = true;
			config.linkJitBlocks = false;
			config.maxInstructionsPerBlock = 1;
			dsp.getJit().setConfig(config);
		}

		TWord install(const TWord _pc, const char* const _instruction)
		{
			const auto result = assembler.assemble(_instruction);
			require(result.success(), "failed to assemble focused instruction");
			require(dsp.memWriteP(_pc, result.word[0]), "failed to write focused opcode");
			if(result.wordCount > 1)
				require(dsp.memWriteP(_pc + 1, result.word[1]),
					"failed to write focused extension word");
			return _pc + result.wordCount;
		}

		DefaultMemoryValidator validator;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		Memory memory;
		DSP dsp;
		Assembler assembler;
	};

	void normalFlowInVectorAreaUsesOrdinaryJsrReturnAddress()
	{
		Fixture fixture;
		constexpr TWord entry = 0x24;
		const auto returnPc = fixture.install(entry, "jsr $200");
		fixture.install(returnPc, "nop");
		fixture.install(0x200, "rts");

		fixture.dsp.setPC(entry);
		for(unsigned i = 0; i < 8 && fixture.dsp.getPC().toWord() != returnPc; ++i)
			fixture.dsp.execJit();

		require(fixture.dsp.getPC().toWord() == returnPc,
			"normal vector-area JSR did not return to its following instruction");
		require(fixture.dsp.regs().sp.toWord() == 0,
			"normal vector-area JSR left the hardware stack unbalanced");
	}

	void fastInterruptJsrPreservesInterruptedProgramCounter()
	{
		Fixture fixture;
		constexpr TWord interruptedPc = 0x400;
		fixture.install(Vba_NMI, "jsset #$0,a1,$300");
		fixture.install(0x300, "rti");
		fixture.install(interruptedPc, "move #$1,r0");
		fixture.dsp.regs().a.var = 0x00000001000000ull;
		fixture.dsp.setPC(interruptedPc);

		require(fixture.dsp.injectInterruptImmediate(Vba_NMI),
			"focused non-maskable interrupt was rejected");
		require(fixture.dsp.getPC().toWord() == interruptedPc + 1,
			"fast-interrupt return did not resume through the interrupted instruction");
		require(fixture.dsp.regs().r[0].toWord() == 1,
			"the interrupted instruction was not executed during immediate interrupt completion");
		require(fixture.dsp.regs().sp.toWord() == 0,
			"fast-interrupt RTI left the hardware stack unbalanced");
	}

	void unchangedZeroSuccessorHasAJitDispatchEntry()
	{
		Fixture fixture;
		constexpr TWord entry = 0x100;
		fixture.install(entry, "move #$1,r0");
		require(fixture.memory.get(MemArea_P, entry + 1) == 0,
			"zero-successor regression requires untouched default P memory");
		fixture.dsp.setPC(entry);
		fixture.dsp.execJit();
		const auto compiledEntry = fixture.dsp.getJitEntries()[entry];
		require(compiledEntry != nullptr, "first JIT instruction did not publish a compiled entry");
		fixture.dsp.execJit();
		require(fixture.dsp.getPC().toWord() == entry + 2 && fixture.dsp.regs().r[0].toWord() == 1,
			"unchanged zero successor was not dispatched as a JIT instruction");
		require(fixture.dsp.getJitEntries()[entry] == compiledEntry,
			"dispatch-capacity growth invalidated the already compiled predecessor entry");
	}

	void unchangedZeroFastVectorHasAJitDispatchEntry()
	{
		Fixture fixture;
		constexpr TWord interruptedPc = 0x400;
		fixture.install(interruptedPc, "move #$1,r0");
		require(fixture.memory.get(MemArea_P, Vba_NMI) == 0 && fixture.memory.get(MemArea_P, Vba_NMI + 1) == 0,
			"zero-vector regression requires untouched default P memory");
		fixture.dsp.setPC(interruptedPc);
		require(fixture.dsp.injectInterruptImmediate(Vba_NMI), "unchanged zero NMI was rejected");
		require(fixture.dsp.getPC().toWord() == interruptedPc + 1 && fixture.dsp.regs().r[0].toWord() == 1,
			"unchanged zero fast vector did not dispatch and resume its interrupted source");
	}
}

int main()
{
	try
	{
		if constexpr(dsp56k::g_jitSupported)
		{
			normalFlowInVectorAreaUsesOrdinaryJsrReturnAddress();
			fastInterruptJsrPreservesInterruptedProgramCounter();
			unchangedZeroSuccessorHasAJitDispatchEntry();
			unchangedZeroFastVectorHasAJitDispatchEntry();
		}
	}
	catch(const std::exception& _error)
	{
		std::fputs(_error.what(), stderr);
		return 1;
	}
	return 0;
}
