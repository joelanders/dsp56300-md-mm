#include "assembler.h"
#include "dsp.h"
#include "esaiclock.h"
#include "esxi.h"
#include "interrupts.h"
#include "memory.h"
#include "opcodecycles.h"
#include "opcodes.h"
#include "peripherals.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace
{
	using namespace dsp56k;

	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	struct Fixture final
	{
		Fixture()
			: memory(validator, 0x1000)
			, dsp(memory, &peripherals, &noPeripherals)
		{
		}

		TWord install(TWord _pc, const char* const _source)
		{
			const auto assembled = assembler.assemble(_source);
			require(assembled.success(), "failed to assemble focused instruction");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(_pc + i, assembled.word[i]), "failed to install focused instruction");
			return _pc + assembled.wordCount;
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	uint32_t independentCycleCost(const Fixture& _fixture, const TWord _pc)
	{
		TWord opA, opB;
		_fixture.memory.getOpcode(_pc, opA, opB);
		Opcodes opcodes;
		Instruction instA, instB;
		opcodes.getInstructionTypes(opA, instA, instB);
		return calcCycles(instA, instB, _pc, opA,
			_fixture.memory.getBridgedMemoryAddress(), 1);
	}

	class SerialProbe final : public Esxi
	{
	public:
		void execTX() override { ++transmitTicks; }
		void execRX() override { ++receiveTicks; }
		TWord hasEnabledTransmitters() const override { return 1; }
		TWord hasEnabledReceivers() const override { return 1; }

		unsigned transmitTicks = 0;
		unsigned receiveTicks = 0;
	};

	void zeroBudgetDoesNoWork()
	{
		Fixture fixture;
		fixture.install(0, "nop");
		const auto result = fixture.dsp.executeBudgetedSlice(0);
		require(result.instructionDelta == 0 && result.cycleCounterDelta == 0,
			"zero budget advanced a counter");
		require(fixture.dsp.getPC().toWord() == 0, "zero budget fetched an opcode");
	}

	void finiteAndNestedDoResumeAtInstructionBoundaries()
	{
		Fixture fixture;
		fixture.dsp.regs().a.var = 0;
		fixture.dsp.regs().b.var = 0x00000001000000ull;

		TWord pc = 0;
		pc = fixture.install(pc, "do #$2,>$7");
		pc = fixture.install(pc, "do #$2,>$6");
		pc = fixture.install(pc, "add b,a");
		pc = fixture.install(pc, "nop");
		pc = fixture.install(pc, "nop");
		fixture.install(pc, "nop");

		for(unsigned guard = 0; guard < 32 && fixture.dsp.getPC().toWord() != 7; ++guard)
		{
			const auto result = fixture.dsp.executeBudgetedSlice(1);
			require(result.instructionDelta == 1, "finite DO did not yield at one instruction boundary");
		}

		require(fixture.dsp.getPC().toWord() == 7, "nested DO did not resume to its successor");
		require(fixture.dsp.regs().a.var == 0x00000004000000ull,
			"nested DO did not retain its architectural loop state across slices");
		require(fixture.dsp.regs().sp.toWord() == 0, "nested DO left the hardware stack unbalanced");
	}

	void repCompletesAtomicallyAndReportsOverrun()
	{
		Fixture fixture;
		fixture.dsp.regs().a.var = 0;
		fixture.dsp.regs().b.var = 0x00000001000000ull;
		fixture.install(0, "rep #$4");
		fixture.install(1, "add b,a");
		const auto expectedCycles = independentCycleCost(fixture, 0)
			+ 4 * independentCycleCost(fixture, 1);
		require(independentCycleCost(fixture, 0) == 5 && independentCycleCost(fixture, 1) == 1
			&& expectedCycles == 9,
			"REP fixed timing vector no longer matches the DSP56300 cycle table");

		const auto result = fixture.dsp.executeBudgetedSlice(1);
		require(result.stop == DSP::BudgetedExecutionStop::AtomicBudgetOverrun,
			"REP budget overrun was not reported as atomic");
		require(result.instructionDelta > 1, "REP was interrupted at the requested budget");
		require(fixture.dsp.regs().a.var == 0x00000004000000ull,
			"REP did not execute its complete repetition count");
		require(fixture.dsp.getPC().toWord() == 2, "REP did not consume exactly its repeated operation");
		require(fixture.dsp.getCycles() == expectedCycles,
			"REP did not retire its setup and every repeated body through calcCycles");
	}

	void interpreterRetiresIndependentNormalAndParallelCycleCosts()
	{
		Fixture fixture;
		fixture.install(0, "add #$1,a");
		fixture.install(1, "nop");
		const auto normalCycles = independentCycleCost(fixture, 0);
		const auto nopCycles = independentCycleCost(fixture, 1);
		require(normalCycles == 1 && nopCycles == 1,
			"normal fixed timing vectors no longer match the DSP56300 cycle table");

		fixture.dsp.execInterpreter();
		require(fixture.dsp.getCycles() == normalCycles,
			"normal interpreter operation did not retire its independent calcCycles cost");
		fixture.dsp.execInterpreter();
		require(fixture.dsp.getCycles() == normalCycles + nopCycles,
			"interpreter cycle retirement did not preserve normal operation chronology");

		Fixture parallel;
		// CLR B || MOVE #>$128,X0. The parallel decoder resolves this as
		// Clr + Movex_ea. Its MMM=110/RRR=100 is immediate data, so the
		// documented table cost is Movex_ea base 1 + lab 1 + lim 1 = 3;
		// CLR's unspecified entry defaults to 1 and the parallel max is 3.
		constexpr TWord parallelOp = 0x44f41b;
		constexpr TWord parallelExtension = 0x000128;
		require(parallel.dsp.memWriteP(0, parallelOp), "failed to install parallel timing opcode");
		require(parallel.dsp.memWriteP(1, parallelExtension), "failed to install parallel timing extension");
		Opcodes parallelOpcodes;
		Instruction parallelA, parallelB;
		parallelOpcodes.getInstructionTypes(parallelOp, parallelA, parallelB);
		require(parallelA == Clr && parallelB == Movex_ea,
			"parallel fixed timing vector no longer decodes as CLR plus MOVEX immediate");
		const auto parallelCycles = independentCycleCost(parallel, 0);
		require(parallelCycles == 3,
			"parallel fixed timing vector no longer matches the DSP56300 cycle table");
		parallel.dsp.execInterpreter();
		require(parallel.dsp.getCycles() == parallelCycles,
			"parallel interpreter operation did not use calcCycles max(A, B)");
	}

	void finiteNestedDoHasTheSameRetiredCyclesWhenSliced()
	{
		Fixture continuous;
		Fixture sliced;
		for(auto* fixture : {&continuous, &sliced})
		{
			fixture->dsp.regs().a.var = 0;
			fixture->dsp.regs().b.var = 0x00000001000000ull;
			TWord pc = 0;
			pc = fixture->install(pc, "do #$2,>$6");
			pc = fixture->install(pc, "do #$2,>$5");
			pc = fixture->install(pc, "add b,a");
			fixture->install(pc, "nop");
		}

		continuous.dsp.execInterpreter();
		for(unsigned guard = 0; guard < 16 && sliced.dsp.getPC().toWord() != 6; ++guard)
			sliced.dsp.executeBudgetedSlice(1);
		require(sliced.dsp.getPC().toWord() == 6
			&& sliced.dsp.getCycles() == continuous.dsp.getCycles()
			&& sliced.dsp.getInstructionCounter() == continuous.dsp.getInstructionCounter(),
			"finite nested DO did not retire identically in sliced and unbounded interpreter paths");
	}

	void zeroCountDoAndContinuousInterpreterStayConsistent()
	{
		{
			Fixture fixture;
			fixture.install(0, "do #$0,>$4");
			fixture.install(2, "move #$1,r0");
			fixture.install(3, "nop");
			fixture.install(4, "move #$2,r0");
			fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.regs().r[0].toWord() == 0 && fixture.dsp.regs().sp.toWord() == 0,
				"zero-count DO entered its body or changed loop stack state");
			fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.regs().r[0].toWord() == 2,
				"zero-count DO did not skip directly to its architectural successor");
		}

		Fixture continuous;
		Fixture sliced;
		for(auto* fixture : {&continuous, &sliced})
		{
			fixture->dsp.regs().a.var = 0;
			fixture->dsp.regs().b.var = 0x00000001000000ull;
			fixture->install(0, "add b,a");
			fixture->install(1, "add b,a");
		}
		continuous.dsp.execInterpreter();
		continuous.dsp.execInterpreter();
		sliced.dsp.executeBudgetedSlice(1);
		sliced.dsp.executeBudgetedSlice(1);
		require(continuous.dsp.regs().a.var == sliced.dsp.regs().a.var
			&& continuous.dsp.getPC() == sliced.dsp.getPC()
			&& continuous.dsp.getInstructionCounter() == sliced.dsp.getInstructionCounter()
			&& continuous.dsp.getCycles() == sliced.dsp.getCycles(),
			"two ordinary interpreter boundaries diverged from sliced execution");
	}

	void longInterruptCanReturnIntoAnActiveFiniteDo()
	{
		Fixture fixture;
		fixture.dsp.regs().a.var = 0;
		fixture.dsp.regs().b.var = 0x00000001000000ull;
		fixture.install(0, "do #$2,>$4");
		fixture.install(2, "add b,a");
		fixture.install(3, "nop");
		fixture.install(4, "nop");
		fixture.install(Vba_NMI, "jsset #$0,a1,$300");
		fixture.install(0x300, "rti");
		fixture.dsp.regs().a.var = 0x00000001000000ull;

		fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue finite-DO interrupt");
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		for(unsigned guard = 0; guard < 16 && fixture.dsp.getPC().toWord() != 4; ++guard)
			fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.getPC().toWord() == 4 && fixture.dsp.regs().a.var == 0x00000003000000ull
			&& fixture.dsp.regs().sp.toWord() == 0,
			"long interrupt RTI did not restore active finite-DO state");
	}

	void fastAndLongInterruptsKeepTheirArchitecturalBoundaries()
	{
		{
			Fixture fixture;
			fixture.install(Vba_NMI, "nop");
			fixture.install(Vba_NMI + 1, "nop");
			fixture.install(0x100, "move #$1,r0");
			fixture.dsp.setPC(0x100);
			require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue NMI");
			const auto vectorCycles = independentCycleCost(fixture, Vba_NMI)
				+ independentCycleCost(fixture, Vba_NMI + 1);

			const auto vector = fixture.dsp.executeBudgetedSlice(1);
			require(vector.stop == DSP::BudgetedExecutionStop::AtomicBudgetOverrun && vector.instructionDelta == 2,
				"fast vector was not kept as one atomic two-word transaction");
			require(vector.cycleCounterDelta == vectorCycles,
				"fast vector did not retire both vector words through calcCycles");
			require(fixture.dsp.regs().r[0].toWord() == 0 && fixture.dsp.getPC().toWord() == 0x100,
				"fast vector executed an interrupted-program successor");

			fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.regs().r[0].toWord() == 1, "fast vector did not resume the interrupted program");
		}

		{
			Fixture fixture;
			fixture.install(Vba_NMI, "jsset #$0,a1,$300");
			fixture.install(0x300, "rti");
			fixture.install(0x400, "move #$1,r0");
			fixture.dsp.regs().a.var = 0x00000001000000ull;
			fixture.dsp.setPC(0x400);
			require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue long NMI");

			const auto vector = fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.getProcessingMode() == DSP::LongInterrupt,
				"long vector did not enter long interrupt mode");
			require(vector.cycleCounterDelta == independentCycleCost(fixture, Vba_NMI),
				"long vector did not retire its vector operation through calcCycles");
			const auto rti = fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.getProcessingMode() == DSP::DefaultPreventInterrupt,
				"long interrupt RTI did not restore the regular dispatch boundary");
			require(rti.cycleCounterDelta == independentCycleCost(fixture, 0x300),
				"RTI did not retire through calcCycles after a long vector");
			fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.regs().r[0].toWord() == 1 && fixture.dsp.regs().sp.toWord() == 0,
				"long interrupt RTI did not resume with a balanced stack");
		}

		{
			Fixture fixture;
			fixture.install(Vba_NMI, "nop");
			fixture.install(Vba_NMI + 1, "jsr $300");
			fixture.install(0x300, "rti");
			fixture.install(0x400, "move #$1,r0");
			fixture.dsp.setPC(0x400);
			require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue slot-two long NMI");

			const auto vector = fixture.dsp.executeBudgetedSlice(2);
			require(fixture.dsp.getProcessingMode() == DSP::LongInterrupt,
				"slot-two vector JSR did not enter long interrupt mode");
			require(vector.cycleCounterDelta == independentCycleCost(fixture, Vba_NMI)
				+ independentCycleCost(fixture, Vba_NMI + 1),
				"slot-two long vector did not retire both vector words through calcCycles");
			fixture.dsp.executeBudgetedSlice(1);
			fixture.dsp.executeBudgetedSlice(1);
			require(fixture.dsp.regs().r[0].toWord() == 1 && fixture.dsp.regs().sp.toWord() == 0,
				"slot-two long interrupt did not return through the normal interpreter path");
		}
	}

	void maskedInterruptStillUsesTheNormalPeripheralDispatcher()
	{
		Fixture fixture;
		SerialProbe serial;
		fixture.install(0, "nop");
		fixture.install(1, "nop");
		fixture.peripherals.getEssiClock().setCyclesPerSample(1);
		fixture.peripherals.getEssiClock().setEsaiDivider(&serial, 0);
		fixture.dsp.regs().sr.var |= SR_I0 | SR_I1;
		require(fixture.dsp.injectInterrupt(Vba_DMAchannel0), "failed to queue focused masked interrupt");

		fixture.dsp.executeBudgetedSlice(1);
		require(serial.transmitTicks == 0 && serial.receiveTicks == 0,
			"budgeted dispatcher advanced a peripheral before its deadline");
		fixture.dsp.executeBudgetedSlice(1);
		require(serial.transmitTicks == 1 && serial.receiveTicks == 1,
			"budgeted masked interrupt stalled peripheral progress");
		require(fixture.dsp.getPC().toWord() == 2 && fixture.dsp.getProcessingMode() == DSP::Default,
			"budgeted masked interrupt changed normal instruction dispatch");
	}

	void waitYieldsWithoutExecutingItsSuccessor()
	{
		Fixture fixture;
		fixture.install(0, "wait");
		fixture.install(1, "move #$1,r0");
		fixture.install(Vba_NMI, "nop");
		fixture.install(Vba_NMI + 1, "nop");

		const auto wait = fixture.dsp.executeBudgetedSlice(1);
		require(wait.stop == DSP::BudgetedExecutionStop::Waiting && wait.instructionDelta == 0 && wait.cycleCounterDelta == 0,
			"WAIT completed its instruction accounting before a wake request");
		require(fixture.dsp.getPC().toWord() == 1 && fixture.dsp.regs().r[0].toWord() == 0,
			"WAIT executed its successor before an interrupt");
		const auto parkedZero = fixture.dsp.executeBudgetedSlice(0);
		require(parkedZero.stop == DSP::BudgetedExecutionStop::Waiting
			&& parkedZero.instructionDelta == 0 && parkedZero.cycleCounterDelta == 0
			&& fixture.dsp.getPC().toWord() == 1 && fixture.dsp.regs().r[0].toWord() == 0,
			"zero-budget parked WAIT did not report its state without executing work");

		const auto idle = fixture.dsp.executeBudgetedSlice(1);
		require(idle.stop == DSP::BudgetedExecutionStop::Waiting && idle.instructionDelta == 1 && idle.cycleCounterDelta == 1,
			"WAIT idle visit did not perform one bounded virtual-time advance");
		require(fixture.dsp.regs().r[0].toWord() == 0, "WAIT idle visit executed a successor");

		require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue WAIT wake interrupt");
		const auto wake = fixture.dsp.executeBudgetedSlice(1);
		require(wake.instructionDelta == 1 && wake.cycleCounterDelta == independentCycleCost(fixture, 0)
			&& fixture.dsp.regs().r[0].toWord() == 0,
			"WAIT wake did not complete WAIT before interrupt dispatch");
		fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.regs().r[0].toWord() == 0, "WAIT wake executed a successor in the vector boundary");
		fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.regs().r[0].toWord() == 1, "WAIT did not resume after the wake vector");
	}

	void waitAtDoEndDefersLoopCompletionUntilWake()
	{
		Fixture fixture;
		fixture.install(0, "do #$1,>$3");
		fixture.install(2, "wait");
		fixture.install(3, "move #$1,r0");
		fixture.install(Vba_NMI, "nop");
		fixture.install(Vba_NMI + 1, "nop");

		fixture.dsp.executeBudgetedSlice(1);
		const auto beforeWait = fixture.dsp.getInstructionCounter();
		constexpr uint32_t waitCycles = 10;
		require(independentCycleCost(fixture, 2) == waitCycles,
			"WAIT fixed timing vector no longer matches the DSP56300 cycle table");
		const auto waiting = fixture.dsp.executeBudgetedSlice(1);
		require(waiting.instructionDelta == 0 && waiting.cycleCounterDelta == 0
			&& fixture.dsp.regs().sp.toWord() != 0,
			"WAIT at DO end completed its loop before a wake request");

		require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue DO WAIT wake interrupt");
		const auto wake = fixture.dsp.executeBudgetedSlice(1);
		require(wake.instructionDelta == 1 && wake.cycleCounterDelta == waitCycles
			&& fixture.dsp.getInstructionCounter() == beforeWait + 1 && fixture.dsp.getPC().toWord() == 3,
			"WAIT wake did not perform deferred DO-end bookkeeping");
		require(fixture.dsp.regs().sp.toWord() == 0 && fixture.dsp.regs().lc.toWord() == 0
			&& (fixture.dsp.regs().sr.toWord() & SR_LF) == 0 && fixture.dsp.regs().r[0].toWord() == 0,
			"WAIT wake left DO stack state or executed its successor early");
	}

	void waitOnlyWakesForTheServiceableFifoFront()
	{
		Fixture fixture;
		bool customDelivered = false;
		const auto custom = fixture.dsp.registerInterruptFunc([&] { customDelivered = true; });
		fixture.install(0, "wait");
		fixture.install(1, "nop");
		fixture.dsp.regs().sr.var |= SR_I0 | SR_I1;
		require(fixture.dsp.injectInterrupt(Vba_DMAchannel0), "failed to queue masked FIFO head");
		require(fixture.dsp.injectInterrupt(custom), "failed to queue custom FIFO tail");

		fixture.dsp.executeBudgetedSlice(1);
		const auto blocked = fixture.dsp.executeBudgetedSlice(1);
		require(blocked.stop == DSP::BudgetedExecutionStop::Waiting && !customDelivered,
			"masked FIFO head incorrectly woke WAIT or bypassed its custom tail");
		require(fixture.dsp.getPC().toWord() == 1,
			"masked FIFO head allowed WAIT successor execution");

		fixture.dsp.regs().sr.var &= ~(SR_I0 | SR_I1);
		const auto wake = fixture.dsp.executeBudgetedSlice(1);
		require(wake.instructionDelta == 1 && !customDelivered,
			"WAIT did not complete behind the now-serviceable FIFO head");

		Fixture customFirst;
		bool customFirstDelivered = false;
		const auto customFirstVector = customFirst.dsp.registerInterruptFunc([&] { customFirstDelivered = true; });
		customFirst.install(0, "wait");
		customFirst.install(1, "nop");
		customFirst.dsp.executeBudgetedSlice(1);
		require(customFirst.dsp.injectInterrupt(customFirstVector), "failed to queue serviceable custom FIFO head");
		customFirst.dsp.executeBudgetedSlice(1);
		customFirst.dsp.executeBudgetedSlice(1);
		require(customFirstDelivered,
			"serviceable custom FIFO head did not wake and dispatch WAIT");
	}

	void mixedEntryAndReentryFailClosedUntilReset()
	{
		{
			Fixture fixture;
			fixture.install(0, "wait");
			fixture.install(1, "move #$1,r0");
			fixture.dsp.executeBudgetedSlice(1);
			fixture.dsp.exec();
			fixture.dsp.execInterpreter();
			fixture.dsp.execJit();
			require(fixture.dsp.regs().r[0].toWord() == 0 && fixture.dsp.getPC().toWord() == 1,
				"legacy execution entry bypassed a budgeted WAIT latch");
			fixture.dsp.resetSW();
			fixture.dsp.execInterpreter();
			require(fixture.dsp.regs().r[0].toWord() == 1,
				"reset did not release the fail-closed budgeted WAIT latch");
		}

		{
			Fixture fixture;
			DSP::BudgetedExecutionResult reentry;
			const auto custom = fixture.dsp.registerInterruptFunc([&]
			{
				reentry = fixture.dsp.executeBudgetedSlice(1);
			});
			fixture.install(0, "nop");
			require(fixture.dsp.injectInterrupt(custom), "failed to queue reentry custom interrupt");
			fixture.dsp.executeBudgetedSlice(1);
			require(reentry.stop == DSP::BudgetedExecutionStop::ReentryDenied && reentry.instructionDelta == 0,
				"budgeted callback reentry was not denied without state mutation");
		}
	}

	void stopIsExplicitlyHaltedUntilReset()
	{
		Fixture fixture;
		fixture.install(0, "stop");
		fixture.install(1, "move #$1,r0");
		fixture.install(Vba_NMI, "nop");
		fixture.install(Vba_NMI + 1, "nop");

		const auto stopped = fixture.dsp.executeBudgetedSlice(1);
		require(stopped.stop == DSP::BudgetedExecutionStop::Halted && stopped.instructionDelta == 1,
			"budgeted STOP did not report one explicit halted boundary");
		require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue STOP interrupt");
		const auto stillStopped = fixture.dsp.executeBudgetedSlice(8);
		require(stillStopped.stop == DSP::BudgetedExecutionStop::Halted && stillStopped.instructionDelta == 0,
			"external interrupt woke budgeted STOP without a reset contract");
		fixture.dsp.resetSW();
		fixture.dsp.setPC(1);
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.regs().r[0].toWord() == 1, "reset did not release budgeted STOP");
	}

	void ordinaryDefaultInterpreterRemainsAvailableOutsideBudgetedMode()
	{
		Fixture fixture;
		fixture.install(0, "nop");
		fixture.install(1, "nop");
		fixture.dsp.execInterpreter();
		require(fixture.dsp.getPC().toWord() != 0,
			"ordinary default interpreter was blocked without a budgeted latch");
	}

	void ordinaryDefaultJitRetainsProgressAndCountersOutsideBudgetedMode()
	{
		if constexpr(!g_jitSupported)
			return;

		Fixture fixture;
		auto config = fixture.dsp.getJit().getConfig();
		config.dynamicFastInterrupts = true;
		config.linkJitBlocks = false;
		config.maxInstructionsPerBlock = 1;
		fixture.dsp.getJit().setConfig(config);

		constexpr TWord entry = 0x24;
		const auto returnPc = fixture.install(entry, "jsr $200");
		fixture.install(returnPc, "nop");
		fixture.install(0x200, "move #$55,r0");
		fixture.install(0x201, "rts");
		fixture.dsp.setPC(entry);

		const auto instructionsBefore = fixture.dsp.getInstructionCounter();
		fixture.dsp.exec();
		require(fixture.dsp.getPC().toWord() == 0x200 && fixture.dsp.regs().sp.toWord() != 0,
			"ordinary default DSP::exec did not use the JIT path to execute JSR");
		require(fixture.dsp.getInstructionCounter() > instructionsBefore,
			"ordinary default JIT execution did not retain instruction-counter progress");

		fixture.dsp.exec();
		require(fixture.dsp.getPC().toWord() == 0x201 && fixture.dsp.regs().r[0].toWord() == 0x55,
			"ordinary default JIT execution did not retain register-state progression");

		fixture.dsp.exec();
		require(fixture.dsp.getPC().toWord() == returnPc && fixture.dsp.regs().sp.toWord() == 0,
			"ordinary default JIT execution did not retain RTS state progression");
	}

	void cycleClockObservesOnlyRetiredInterpreterCycles()
	{
		Fixture continuous;
		Fixture sliced;
		SerialProbe continuousSerial;
		SerialProbe slicedSerial;
		for(auto [fixture, serial] : {std::pair{&continuous, &continuousSerial}, std::pair{&sliced, &slicedSerial}})
		{
			fixture->install(0, "add #$1,a");
			fixture->install(1, "nop");
			auto& clock = fixture->peripherals.getEssiClock();
			clock.setClockSource(EsxiClock::ClockSource::Cycles);
			clock.setCyclesPerSample(1);
			clock.setEsaiFinePeriod(serial, 1);
		}

		continuous.dsp.execInterpreter();
		sliced.dsp.executeBudgetedSlice(1);
		require(continuousSerial.transmitTicks == 0 && slicedSerial.transmitTicks == 0,
			"cycle-clock callback observed an instruction before retirement");
		continuous.dsp.execInterpreter();
		sliced.dsp.executeBudgetedSlice(1);
		require(continuousSerial.transmitTicks == 1 && continuousSerial.receiveTicks == 1
			&& slicedSerial.transmitTicks == 1 && slicedSerial.receiveTicks == 1
			&& continuous.dsp.getCycles() == sliced.dsp.getCycles(),
			"cycle-clock callback order diverged between unbounded and sliced interpreter execution");
	}
}

int main()
{
	try
	{
		zeroBudgetDoesNoWork();
		finiteAndNestedDoResumeAtInstructionBoundaries();
		repCompletesAtomicallyAndReportsOverrun();
		interpreterRetiresIndependentNormalAndParallelCycleCosts();
		finiteNestedDoHasTheSameRetiredCyclesWhenSliced();
		zeroCountDoAndContinuousInterpreterStayConsistent();
		longInterruptCanReturnIntoAnActiveFiniteDo();
		fastAndLongInterruptsKeepTheirArchitecturalBoundaries();
		maskedInterruptStillUsesTheNormalPeripheralDispatcher();
		waitYieldsWithoutExecutingItsSuccessor();
		waitAtDoEndDefersLoopCompletionUntilWake();
		waitOnlyWakesForTheServiceableFifoFront();
		mixedEntryAndReentryFailClosedUntilReset();
		stopIsExplicitlyHaltedUntilReset();
		ordinaryDefaultInterpreterRemainsAvailableOutsideBudgetedMode();
		ordinaryDefaultJitRetainsProgressAndCountersOutsideBudgetedMode();
		cycleClockObservesOnlyRetiredInterpreterCycles();
		std::puts("DSP56300 budgeted execution tests passed");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "DSP56300 budgeted execution test failed: %s\n", error.what());
		return 1;
	}
}
