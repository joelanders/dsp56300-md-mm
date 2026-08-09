#include "assembler.h"
#include "dsp.h"
#include "memory.h"
#include "opcodecycles.h"
#include "opcodes.h"
#include "peripherals.h"

#include <cstdio>
#include <stdexcept>

namespace
{
	using namespace dsp56k;

	constexpr TWord kEntry = 0x100;
	constexpr TWord kInitialLc = 0x005a5a;
	constexpr uint64_t kBodyExecutions = 0x10000;
	constexpr uint64_t kAddResult = uint64_t{1} << 40;

	void require(const bool condition, const char* const message)
	{
		if(!condition)
			throw std::runtime_error(message);
	}

	struct Fixture final
	{
		Fixture()
			: memory(validator, 0x1000)
		, dsp(memory, &peripherals, &noPeripherals)
		{
			// REP remains architecturally atomic, but stop the JIT after this one
			// translated REP operation rather than consuming default-zero memory.
			auto config = dsp.getJit().getConfig();
			config.linkJitBlocks = false;
			config.maxInstructionsPerBlock = 1;
			dsp.getJit().setConfig(config);
		}

		TWord install(const TWord pc, const char* const source)
		{
			const auto assembled = assembler.assemble(source);
			require(assembled.success(), "failed to assemble REP-zero fixture instruction");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(pc + i, assembled.word[i]), "failed to install REP-zero fixture instruction");
			return pc + assembled.wordCount;
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	struct State
	{
		uint64_t instructions;
		uint64_t cycleCounter;
		uint64_t accumulator;
		TWord pc;
		TWord lc;
		TWord sr;
	};

	State state(const DSP& dsp)
	{
		return {dsp.getInstructionCounter(), dsp.getCycles(), static_cast<uint64_t>(dsp.regs().a.var),
			dsp.getPC().toWord(), dsp.regs().lc.toWord(), dsp.getSR().toWord()};
	}

	void requireSame(const State& left, const State& right, const char* const message)
	{
		require(left.instructions == right.instructions && left.cycleCounter == right.cycleCounter && left.accumulator == right.accumulator
			&& left.pc == right.pc && left.lc == right.lc && left.sr == right.sr, message);
	}

	uint32_t bodyCycles(const Fixture& fixture, const TWord body)
	{
		TWord opA, opB;
		fixture.memory.getOpcode(body, opA, opB);
		Opcodes opcodes;
		Instruction instA, instB;
		opcodes.getInstructionTypes(opA, instA, instB);
		return calcCycles(instA, instB, body, opA, fixture.memory.getBridgedMemoryAddress(), 1);
	}

	struct AddProgram
	{
		TWord body;
		TWord successor;
	};

	AddProgram installAddFixture(Fixture& fixture, const char* const rep)
	{
		// The manual defines a raw REP count of zero as 65,536 executions. Keep
		// the body independent of control flow and use its table-proven one-cycle
		// cost; raw-zero REP decode timing is intentionally not made an absolute
		// cycle claim here.
		fixture.dsp.regs().a.var = 0;
		fixture.dsp.regs().b.var = 0x00000001000000ull;
		fixture.dsp.regs().x.var = 0;
		fixture.dsp.regs().lc.var = kInitialLc;
		const auto body = fixture.install(kEntry, rep);
		const auto successor = fixture.install(body, "add b,a");
		fixture.dsp.setPC(kEntry);
		require(bodyCycles(fixture, body) == 1, "REP-zero body must remain a one-cycle non-flow operation");
		return {body, successor};
	}

	void requireAddResult(const DSP& dsp, const AddProgram& program, const char* const message)
	{
		require(dsp.regs().a.var == kAddResult, message);
		require(dsp.getPC().toWord() == program.successor, "REP-zero did not advance to the body successor");
		require(dsp.regs().lc.toWord() == kInitialLc, "REP-zero did not restore LC");
		require(dsp.getInstructionCounter() == kBodyExecutions + 1,
			"REP-zero did not retire exactly 65,536 bodies plus REP");
	}

	void immediateAndRegisterZeroMatchInterpreterAndOneBoundaryJit()
	{
		for(const char* const rep : {"rep #$0", "rep x0"})
		{
			Fixture interpreter;
			const auto interpreterProgram = installAddFixture(interpreter, rep);
			interpreter.dsp.execInterpreter();
			requireAddResult(interpreter.dsp, interpreterProgram,
				"interpreter REP-zero add result was not exactly 65,536 bodies");

			if constexpr(g_jitSupported)
			{
				Fixture jit;
				const auto jitProgram = installAddFixture(jit, rep);
				jit.dsp.exec();
				requireAddResult(jit.dsp, jitProgram,
					"one-boundary JIT REP-zero add result was not exactly 65,536 bodies");
				requireSame(state(interpreter.dsp), state(jit.dsp),
					"interpreter/one-boundary-JIT REP-zero architectural state diverged");
			}
		}
	}

	void budgetedInterpreterZeroIsAtomicAndMatchesCompletedState()
	{
		Fixture completed;
		installAddFixture(completed, "rep #$0");
		completed.dsp.execInterpreter();

		Fixture budgeted;
		const auto budgetedProgram = installAddFixture(budgeted, "rep #$0");
		const auto result = budgeted.dsp.executeBudgetedSlice(1);
		require(result.stop == DSP::BudgetedExecutionStop::AtomicBudgetOverrun,
			"budgeted REP-zero did not report atomic budget overrun");
		require(result.instructionDelta == kBodyExecutions + 1,
			"budgeted REP-zero did not report its full atomic instruction count");
		requireAddResult(budgeted.dsp, budgetedProgram,
			"budgeted REP-zero did not complete all 65,536 bodies");
		requireSame(state(completed.dsp), state(budgeted.dsp),
			"budgeted REP-zero final state diverged from the completed interpreter state");
	}

	void immediateZeroNopAndDivUseTheNormalizedSpecialPaths()
	{
		if constexpr(!g_jitSupported)
			return;

		{
			Fixture fixture;
			fixture.dsp.regs().lc.var = kInitialLc;
			const auto body = fixture.install(kEntry, "rep #$0");
			require(fixture.memory.get(MemArea_P, body) == 0,
				"REP-zero NOP regression requires the default zero opcode body");
			fixture.dsp.setPC(kEntry);
			fixture.dsp.exec();
			require(fixture.dsp.getPC().toWord() == body + 1 && fixture.dsp.regs().lc.toWord() == kInitialLc,
				"immediate REP-zero NOP did not restore PC and LC");
			require(fixture.dsp.getInstructionCounter() == kBodyExecutions + 1,
				"immediate REP-zero NOP did not use the normalized repetition count");
		}

		{
			Fixture fixture;
			fixture.dsp.regs().a.var = 0x00000040000000ull;
			fixture.dsp.regs().y.var = 0x400000;
			fixture.dsp.regs().lc.var = kInitialLc;
			const auto body = fixture.install(kEntry, "rep #$0");
			const auto successor = fixture.install(body, "div y0,a");
			fixture.dsp.setPC(kEntry);
			fixture.dsp.exec();
			require(fixture.dsp.getPC().toWord() == successor && fixture.dsp.regs().lc.toWord() == kInitialLc,
				"immediate REP-zero DIV did not restore PC and LC");
			require(fixture.dsp.getInstructionCounter() == kBodyExecutions + 1,
				"immediate REP-zero DIV did not use the normalized repetition count");
		}
	}
}

int main()
{
	try
	{
		immediateAndRegisterZeroMatchInterpreterAndOneBoundaryJit();
		budgetedInterpreterZeroIsAtomicAndMatchesCompletedState();
		immediateZeroNopAndDivUseTheNormalizedSpecialPaths();
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "DSP56300 REP-zero test failed: %s\n", error.what());
		return 1;
	}
	return 0;
}
