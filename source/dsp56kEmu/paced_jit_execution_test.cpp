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

	struct Fixture final
	{
		Fixture()
			: memory(validator, 0x1000)
			, dsp(memory, &peripherals, &noPeripherals)
		{
		}

		TWord install(const TWord _pc, const char* const _source)
		{
			const auto assembled = assembler.assemble(_source);
			require(assembled.success(), "failed to assemble paced-JIT instruction");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(_pc + i, assembled.word[i]), "failed to install paced-JIT instruction");
			return _pc + assembled.wordCount;
		}

		void begin()
		{
			require(dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Activated,
				"failed to activate paced JIT");
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	void requireOne(const DSP::PacedJitExecutionResult& _result, const char* const _message)
	{
		require(_result.stop == DSP::PacedJitExecutionStop::BudgetExhausted
			&& _result.instructionDelta == 1, _message);
	}

	bool sameJitConfigScalars(const JitConfig& _a, const JitConfig& _b)
	{
		return _a.aguSupportBitreverse == _b.aguSupportBitreverse
			&& _a.aguSupportMultipleWrapModulo == _b.aguSupportMultipleWrapModulo
			&& _a.cacheSingleOpBlocks == _b.cacheSingleOpBlocks
			&& _a.linkJitBlocks == _b.linkJitBlocks
			&& _a.splitOpsByNops == _b.splitOpsByNops
			&& _a.dynamicPeripheralAddressing == _b.dynamicPeripheralAddressing
			&& _a.maxInstructionsPerBlock == _b.maxInstructionsPerBlock
			&& _a.memoryWritesCallCpp == _b.memoryWritesCallCpp
			&& _a.support16BitSCMode == _b.support16BitSCMode
			&& _a.maxDoIterations == _b.maxDoIterations
			&& _a.dynamicFastInterrupts == _b.dynamicFastInterrupts
			&& _a.asmjitDiagnostics == _b.asmjitDiagnostics
			&& _a.enableOptimizer == _b.enableOptimizer
			&& _a.debugDynamicPeripheralAddressing == _b.debugDynamicPeripheralAddressing;
	}

	void activationOwnsAndRestoresTheExactJitConfiguration()
	{
		Fixture fixture;
		auto original = fixture.dsp.getJit().getConfig();
		original.aguSupportBitreverse = true;
		original.aguSupportMultipleWrapModulo = false;
		original.cacheSingleOpBlocks = false;
		original.linkJitBlocks = false;
		original.splitOpsByNops = true;
		original.dynamicPeripheralAddressing = true;
		original.maxInstructionsPerBlock = 3;
		original.memoryWritesCallCpp = true;
		original.support16BitSCMode = true;
		original.maxDoIterations = 8;
		original.dynamicFastInterrupts = false;
		original.asmjitDiagnostics = true;
		original.enableOptimizer = false;
		original.debugDynamicPeripheralAddressing = true;
		JitConfig callbackConfig;
		callbackConfig.maxInstructionsPerBlock = 9;
		callbackConfig.dynamicFastInterrupts = true;
		original.getBlockConfig = [callbackConfig](const TWord _pc)
		{
			return _pc == 0x222 ? std::optional<JitConfig>{callbackConfig}
				: std::optional<JitConfig>{};
		};
		fixture.dsp.getJit().setConfig(original);

		fixture.install(0x100, "nop");
		fixture.install(0x101, "nop");
		fixture.install(0x102, "nop");
		fixture.install(0x103, "nop");
		fixture.dsp.setPC(0x100);
		const auto beforeFirstOrdinary = fixture.dsp.getInstructionCounter();
		fixture.dsp.execJit();
		require(fixture.dsp.getInstructionCounter() - beforeFirstOrdinary == 3
			&& fixture.dsp.getPC().toWord() == 0x103,
			"ordinary pre-activation block did not compile with the saved configuration");
		const auto ordinaryEntry = fixture.dsp.getJitEntries()[0x100];
		fixture.dsp.setPC(0x100);
		fixture.begin();
		require(fixture.dsp.getJitEntries()[0x100] != ordinaryEntry,
			"paced activation retained a compiled ordinary block");

		const auto& paced = fixture.dsp.getJit().getConfig();
		require(!paced.linkJitBlocks
			&& paced.maxInstructionsPerBlock == 1 && paced.maxDoIterations == 1
			&& paced.dynamicFastInterrupts && !paced.getBlockConfig,
			"paced activation did not install its complete fixed configuration");
		require(fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::AlreadyActive,
			"nested paced activation was not denied");

		const auto instructionsBefore = fixture.dsp.getInstructionCounter();
		fixture.dsp.exec();
		fixture.dsp.execJit();
		fixture.dsp.execInterpreter();
		const auto mixed = fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.getInstructionCounter() == instructionsBefore
			&& fixture.dsp.getPC().toWord() == 0x100
			&& mixed.stop == DSP::BudgetedExecutionStop::ReentryDenied,
			"legacy entry bypassed paced-JIT ownership");

		const auto zero = fixture.dsp.executePacedJitSlice(0);
		const auto invalid = fixture.dsp.executePacedJitSlice(2);
		require(zero.instructionDelta == 0 && zero.cycleCounterDelta == 0
			&& invalid.stop == DSP::PacedJitExecutionStop::InvalidBudget
			&& fixture.dsp.getPC().toWord() == 0x100,
			"zero or invalid paced budget performed work");
		requireOne(fixture.dsp.executePacedJitSlice(1), "paced one-op configuration did not retire one boundary");
		const auto pacedEntry = fixture.dsp.getJitEntries()[0x100];

		require(fixture.dsp.endPacedJitExecution(), "failed to end paced-JIT ownership");
		const auto& restored = fixture.dsp.getJit().getConfig();
		require(sameJitConfigScalars(restored, original)
			&& static_cast<bool>(restored.getBlockConfig)
			&& !restored.getBlockConfig(0x100).has_value()
			&& restored.getBlockConfig(0x222).has_value()
			&& sameJitConfigScalars(*restored.getBlockConfig(0x222), callbackConfig),
			"paced teardown did not restore every JIT scalar and callback behavior");
		require(fixture.dsp.getJitEntries()[0x100] != pacedEntry,
			"paced teardown retained a compiled paced block");

		fixture.dsp.setPC(0x100);
		const auto beforeOrdinary = fixture.dsp.getInstructionCounter();
		fixture.dsp.execJit();
		require(fixture.dsp.getInstructionCounter() - beforeOrdinary == 3
			&& fixture.dsp.getPC().toWord() == 0x103,
			"paced teardown retained a cached one-op block under restored ordinary configuration");
	}

	void takenAndNotTakenConditionalBranchesRetireOneBoundary()
	{
		for(const bool taken : {false, true})
		{
			Fixture fixture;
			fixture.dsp.regs().a.var = taken ? 0 : 0x00000001000000ull;
			fixture.install(0x100, "tst a");
			// Bcc_xxxx stores a signed displacement, not an absolute address.
			// $101 + $7 therefore selects the installed $108 target.
			fixture.install(0x101, "beq >$7");
			fixture.install(0x103, "move #$11,r0");
			fixture.install(0x108, "move #$22,r0");
			TWord branchWord, displacement;
			fixture.memory.getOpcode(0x101, branchWord, displacement);
			require(branchWord != 0 && displacement == 7,
				"conditional branch fixture did not encode displacement seven");
			fixture.dsp.setPC(0x100);
			fixture.begin();

			requireOne(fixture.dsp.executePacedJitSlice(1), "conditional setup did not retire once");
			requireOne(fixture.dsp.executePacedJitSlice(1),
				taken ? "taken conditional branch overran its boundary" : "not-taken conditional branch overran its boundary");
			const auto expectedPC = static_cast<TWord>(taken ? 0x108 : 0x103);
			if(fixture.dsp.getPC().toWord() != expectedPC)
			{
				std::fprintf(stderr,
					"paced conditional mismatch: taken=%u expected=%06x actual=%06x sr=%06x\n",
					static_cast<unsigned>(taken), expectedPC, fixture.dsp.getPC().toWord(),
					fixture.dsp.getSR().toWord());
			}
			require(fixture.dsp.getPC().toWord() == expectedPC,
				"conditional branch selected the wrong paced successor");
			requireOne(fixture.dsp.executePacedJitSlice(1), "conditional successor did not retire once");
			require(fixture.dsp.regs().r[0].toWord() == (taken ? 0x22 : 0x11),
				"conditional successor state was incorrect");
			require(fixture.dsp.endPacedJitExecution(), "failed to end conditional paced session");
		}
	}

	void finiteNestedDoProvesEveryPacedCallAndCompletedState()
	{
		Fixture reference;
		Fixture paced;
		for(auto* fixture : {&reference, &paced})
		{
			fixture->dsp.regs().a.var = 0x400000;
			fixture->dsp.regs().b.var = 0x00000001000000ull;
			TWord pc = 0x100;
			pc = fixture->install(pc, "do #$2,>$107");
			pc = fixture->install(pc, "do #$2,>$106");
			pc = fixture->install(pc, "add b,a");
			pc = fixture->install(pc, "nop");
			fixture->install(pc, "nop");
			fixture->dsp.setPC(0x100);
		}

		for(unsigned guard = 0; guard < 32 && reference.dsp.getPC().toWord() != 0x107; ++guard)
			reference.dsp.execInterpreter();
		require(reference.dsp.getPC().toWord() == 0x107, "reference nested DO did not terminate");

		paced.begin();
		unsigned calls = 0;
		for(; calls < 32 && paced.dsp.getPC().toWord() != 0x107; ++calls)
		{
			const auto result = paced.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::BudgetExhausted
				&& result.instructionDelta == 1,
				"finite or nested DO retired more or less than one instruction in a paced call");
		}
		require(paced.dsp.getPC().toWord() == 0x107 && calls == 13,
			"paced nested DO did not expose all thirteen instruction boundaries");
		require(paced.dsp.getInstructionCounter() == reference.dsp.getInstructionCounter()
			&& paced.dsp.getCycles() == reference.dsp.getCycles()
			&& paced.dsp.regs().a.var == reference.dsp.regs().a.var
			&& paced.dsp.regs().sp == reference.dsp.regs().sp
			&& paced.dsp.regs().sc == reference.dsp.regs().sc
			&& paced.dsp.regs().la == reference.dsp.regs().la
			&& paced.dsp.regs().lc == reference.dsp.regs().lc
			&& paced.dsp.getSR() == reference.dsp.getSR(),
			"paced nested DO completed state or counters diverged from the interpreter");
		require(paced.dsp.endPacedJitExecution(), "failed to end nested-DO paced session");
	}

	void repAndFastVectorReportOnlyTheirMeasuredAtomicOverruns()
	{
		{
			Fixture fixture;
			fixture.dsp.regs().a.var = 0;
			fixture.dsp.regs().b.var = 0x00000001000000ull;
			fixture.install(0x100, "rep #$4");
			fixture.install(0x101, "add b,a");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun
				&& result.instructionDelta == 5
				&& fixture.dsp.regs().a.var == 0x00000004000000ull
				&& fixture.dsp.getPC().toWord() == 0x102,
				"paced REP did not report its complete measured atomic transaction");
			require(fixture.dsp.endPacedJitExecution(), "failed to end REP paced session");
		}

		{
			Fixture fixture;
			fixture.dsp.regs().a.var = 0;
			fixture.dsp.regs().b.var = 0x00000001000000ull;
			fixture.dsp.regs().x.var = 4;
			fixture.install(0x100, "rep x0");
			fixture.install(0x101, "add b,a");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun
				&& result.instructionDelta == 5
				&& fixture.dsp.regs().a.var == 0x00000004000000ull
				&& fixture.dsp.getPC().toWord() == 0x102,
				"paced register REP was not classified as a measured atomic transaction");
			require(fixture.dsp.endPacedJitExecution(), "failed to end register-REP paced session");
		}

		{
			Fixture fixture;
			fixture.install(Vba_NMI, "nop");
			fixture.install(Vba_NMI + 1, "nop");
			fixture.install(0x100, "move #$1,r0");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue paced fast vector");
			const auto vector = fixture.dsp.executePacedJitSlice(1);
			require(vector.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun
				&& vector.instructionDelta == 2
				&& fixture.dsp.getPC().toWord() == 0x100
				&& fixture.dsp.regs().r[0].toWord() == 0,
				"paced fast vector was not isolated from its interrupted source");
			requireOne(fixture.dsp.executePacedJitSlice(1),
				"paced interrupt-prevention transition did not expose one restored source boundary");
			require(fixture.dsp.regs().r[0].toWord() == 1,
				"paced fast vector did not resume its source");
			require(fixture.dsp.endPacedJitExecution(), "failed to end vector paced session");
		}
	}

	void waitStopAndResetUseBoundedSessionLatches()
	{
		{
			Fixture fixture;
			fixture.install(0x100, "wait");
			fixture.install(0x101, "move #$1,r0");
			fixture.install(Vba_NMI, "nop");
			fixture.install(Vba_NMI + 1, "nop");
			fixture.dsp.setPC(0x100);
			fixture.begin();

			const auto wait = fixture.dsp.executePacedJitSlice(1);
			require(wait.stop == DSP::PacedJitExecutionStop::Waiting
				&& wait.instructionDelta == 1 && fixture.dsp.getPC().toWord() == 0x101,
				"paced WAIT did not retire once and latch before its successor");
			const auto parked = fixture.dsp.executePacedJitSlice(0);
			require(parked.stop == DSP::PacedJitExecutionStop::Waiting
				&& parked.instructionDelta == 0 && fixture.dsp.regs().r[0].toWord() == 0,
				"zero-budget paced WAIT was not passive");
			const auto cyclesBeforeProbe = fixture.dsp.getCycles();
			const auto zeroAdvance = fixture.dsp.executePacedJitSlice(1, 0);
			require(zeroAdvance.stop == DSP::PacedJitExecutionStop::Waiting && zeroAdvance.instructionDelta == 0 &&
						zeroAdvance.cycleCounterDelta == 0 && fixture.dsp.getCycles() == cyclesBeforeProbe,
					"zero-advance paced WAIT probe invented counter progress");
			require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue paced WAIT wake");
			const auto wake = fixture.dsp.executePacedJitSlice(1);
			require(wake.stop == DSP::PacedJitExecutionStop::BudgetExhausted
				&& wake.instructionDelta == 0 && fixture.dsp.regs().r[0].toWord() == 0,
				"paced WAIT wake did not expose a zero-retirement wake boundary");
			const auto vector = fixture.dsp.executePacedJitSlice(1);
			require(vector.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun
				&& vector.instructionDelta == 2 && fixture.dsp.regs().r[0].toWord() == 0,
				"paced WAIT wake vector executed its successor");
			requireOne(fixture.dsp.executePacedJitSlice(1), "paced WAIT did not resume its successor once");
			require(fixture.dsp.regs().r[0].toWord() == 1, "paced WAIT successor state was incorrect");
			require(fixture.dsp.endPacedJitExecution(), "failed to end WAIT paced session");
		}

		{
			Fixture fixture;
			fixture.install(0x100, "stop");
			fixture.install(0x101, "move #$1,r0");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			const auto stopped = fixture.dsp.executePacedJitSlice(1);
			require(stopped.stop == DSP::PacedJitExecutionStop::Halted
				&& stopped.instructionDelta == 1 && fixture.dsp.getPC().toWord() == 0x100,
				"paced STOP did not enter a reset-only no-successor latch");
			require(fixture.dsp.executePacedJitSlice(1).stop == DSP::PacedJitExecutionStop::Halted,
				"paced STOP latch executed more code");
			fixture.dsp.resetSW();
			require(!fixture.dsp.isPacedJitExecutionActive(),
				"reset did not restore configuration and end paced STOP ownership");
		}

		{
			Fixture fixture;
			fixture.install(0x100, "reset");
			fixture.install(0x101, "nop");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			const auto reset = fixture.dsp.executePacedJitSlice(1);
			require(reset.stop == DSP::PacedJitExecutionStop::OwnershipLost
				&& reset.instructionDelta == 1 && !fixture.dsp.isPacedJitExecutionActive(),
				"RESET inside paced JIT did not defer safe teardown until block return");
		}

		{
			Fixture fixture;
			auto original = fixture.dsp.getJit().getConfig();
			original.maxInstructionsPerBlock = 5;
			original.linkJitBlocks = false;
			fixture.dsp.getJit().setConfig(original);
			fixture.install(0x100, "nop");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			requireOne(fixture.dsp.executePacedJitSlice(1),
				"hardware-reset fixture did not create a paced block");
			fixture.dsp.resetHW();
			require(!fixture.dsp.isPacedJitExecutionActive()
				&& sameJitConfigScalars(fixture.dsp.getJit().getConfig(), original)
				&& fixture.dsp.getInstructionCounter() == 0
				&& fixture.dsp.getCycles() == 0 && fixture.dsp.getPC().toWord() == 0,
				"hardware reset did not restore ordinary JIT configuration and core reset state");
			require(fixture.dsp.executePacedJitSlice(1).stop == DSP::PacedJitExecutionStop::Inactive,
				"hardware reset left paced ownership callable");
		}
	}

	void immediateInterruptInjectionCannotBypassPacedOwnership()
	{
		Fixture fixture;
		fixture.install(Vba_NMI, "nop");
		fixture.install(Vba_NMI + 1, "nop");
		fixture.install(0x100, "move #$1,r0");
		fixture.dsp.setPC(0x100);
		fixture.begin();

		const auto pc = fixture.dsp.getPC();
		const auto instructions = fixture.dsp.getInstructionCounter();
		const auto cycles = fixture.dsp.getCycles();
		const auto mode = fixture.dsp.getProcessingMode();
		const auto sp = fixture.dsp.regs().sp;
		require(!fixture.dsp.injectInterruptImmediate(Vba_NMI),
			"immediate interrupt bypassed paced-JIT ownership");
		require(fixture.dsp.getPC() == pc
			&& fixture.dsp.getInstructionCounter() == instructions
			&& fixture.dsp.getCycles() == cycles
			&& fixture.dsp.getProcessingMode() == mode
			&& fixture.dsp.regs().sp == sp,
			"rejected immediate interrupt mutated paced core state");
		requireOne(fixture.dsp.executePacedJitSlice(1),
			"rejected immediate interrupt damaged subsequent paced dispatch");
		require(fixture.dsp.regs().r[0].toWord() == 1,
			"paced source did not run after rejected immediate interrupt");
		require(fixture.dsp.endPacedJitExecution(), "failed to end immediate-interrupt paced session");
	}

	void callbackReentryAndConfigurationMutationFailClosed()
	{
		{
			Fixture fixture;
			DSP::PacedJitExecutionResult reentry;
			bool endAccepted = true;
			const auto custom = fixture.dsp.registerInterruptFunc([&]
			{
				reentry = fixture.dsp.executePacedJitSlice(1);
				endAccepted = fixture.dsp.endPacedJitExecution();
				fixture.dsp.exec();
			});
			fixture.install(0x100, "nop");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			require(fixture.dsp.injectInterrupt(custom), "failed to queue paced reentry callback");
			const auto outer = fixture.dsp.executePacedJitSlice(1);
			requireOne(outer, "paced custom callback did not return to exactly one normal boundary");
			require(reentry.stop == DSP::PacedJitExecutionStop::ReentryDenied
				&& reentry.instructionDelta == 0 && !endAccepted,
				"paced callback reentry or teardown was not denied");
			require(fixture.dsp.endPacedJitExecution(), "failed to end reentry paced session");
		}

		{
			Fixture fixture;
			const auto custom = fixture.dsp.registerInterruptFunc([&]
			{
				auto changed = fixture.dsp.getJit().getConfig();
				changed.maxInstructionsPerBlock = 7;
				fixture.dsp.getJit().setConfig(changed);
			});
			fixture.install(0x100, "move #$1,r0");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			require(fixture.dsp.injectInterrupt(custom), "failed to queue paced configuration mutation");
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::OwnershipLost
				&& result.instructionDelta == 0 && fixture.dsp.getPC().toWord() == 0x100
				&& fixture.dsp.regs().r[0].toWord() == 0,
				"configuration mutation was not detected before JIT dispatch");
			fixture.dsp.exec();
			require(fixture.dsp.regs().r[0].toWord() == 0,
				"legacy entry escaped a configuration-lost paced session");
			require(fixture.dsp.endPacedJitExecution(), "failed to restore mutated paced session");
			require(!fixture.dsp.isPacedJitExecutionActive(),
				"configuration-lost teardown did not restore ordinary JIT ownership");
		}
	}
}

int main()
{
	try
	{
		if constexpr(dsp56k::g_jitSupported)
		{
			activationOwnsAndRestoresTheExactJitConfiguration();
			takenAndNotTakenConditionalBranchesRetireOneBoundary();
			finiteNestedDoProvesEveryPacedCallAndCompletedState();
			repAndFastVectorReportOnlyTheirMeasuredAtomicOverruns();
			waitStopAndResetUseBoundedSessionLatches();
			immediateInterruptInjectionCannotBypassPacedOwnership();
			callbackReentryAndConfigurationMutationFailClosed();
		}
		else
		{
			Fixture fixture;
			require(fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Unsupported,
				"unsupported backend accepted paced JIT activation");
		}
		std::puts("DSP56300 paced JIT execution tests passed");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "DSP56300 paced JIT execution test failed: %s\n", error.what());
		return 1;
	}
}
