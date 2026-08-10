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
#include <limits>
#include <stdexcept>
#include <vector>

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
			require(assembled.success(), "failed to assemble scheduling fixture");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(_pc + i, assembled.word[i]), "failed to install scheduling fixture");
			return _pc + assembled.wordCount;
		}

		uint32_t cyclesAt(const TWord _pc) const
		{
			TWord opA, opB;
			memory.getOpcode(_pc, opA, opB);
			Opcodes opcodes;
			Instruction instA, instB;
			opcodes.getInstructionTypes(opA, instA, instB);
			return calcCycles(instA, instB, _pc, opA, memory.getBridgedMemoryAddress(), 1);
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	class SerialProbe final : public Esxi
	{
	public:
		explicit SerialProbe(const DSP& _dsp) : dsp(_dsp) {}

		void execTX() override
		{
			cycleTicks.push_back(dsp.getCycles());
			instructionTicks.push_back(dsp.getInstructionCounter());
			pcs.push_back(dsp.getPC().toWord());
		}
		void execRX() override {}
		TWord hasEnabledTransmitters() const override { return 1; }
		TWord hasEnabledReceivers() const override { return 0; }

		const DSP& dsp;
		std::vector<uint64_t> cycleTicks;
		std::vector<uint64_t> instructionTicks;
		std::vector<TWord> pcs;
	};

	void configureExact(Fixture& _fixture, SerialProbe& _probe, const uint32_t _period)
	{
		auto& clock = _fixture.peripherals.getEssiClock();
		clock.setCyclesPerSample(1000);
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setExactCycleScheduling(true);
		clock.setEsaiFinePeriod(&_probe, _period);
	}

	enum class Path { Interpreter, PacedJit };

	void beginPath(Fixture& _fixture, const Path _path)
	{
		if(_path == Path::PacedJit)
			require(_fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Activated,
				"failed to activate paced JIT scheduling fixture");
	}

	void stepOne(Fixture& _fixture, const Path _path)
	{
		if(_path == Path::Interpreter)
		{
			const auto result = _fixture.dsp.executeBudgetedSlice(1);
			require(result.stop == DSP::BudgetedExecutionStop::BudgetExhausted
				&& result.instructionDelta == 1, "interpreter scheduling step did not retire once");
		}
		else
		{
			const auto result = _fixture.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::BudgetExhausted
				&& result.instructionDelta == 1, "paced-JIT scheduling step did not retire once");
		}
	}

	void instructionDeadlinesRemainTheDefault()
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0, "nop");
		fixture.install(1, "nop");
		fixture.install(2, "nop");
		auto& clock = fixture.peripherals.getEssiClock();
		clock.setCyclesPerSample(1000);
		clock.setEsaiFinePeriod(&probe, 2);

		stepOne(fixture, Path::Interpreter);
		const auto deadline = fixture.peripherals.getTargetClock();
		require(deadline == 2 && !fixture.peripherals.hasCycleDelay(),
			"default clock did not publish only its instruction deadline");
		stepOne(fixture, Path::Interpreter);
		require(fixture.peripherals.getTargetClock() == deadline && probe.cycleTicks.empty(),
			"instruction-only deadline performed an early peripheral visit");
		stepOne(fixture, Path::Interpreter);
		require(probe.instructionTicks == std::vector<uint64_t>{2},
			"instruction clock did not fire at its original boundary");
	}

	void legacyCycleClockRemainsAdvisoryUntilExplicitOptIn()
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		for(TWord pc = 0; pc < 4; ++pc)
			fixture.install(pc, "nop");
		auto& clock = fixture.peripherals.getEssiClock();
		clock.setCyclesPerSample(1000);
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setEsaiFinePeriod(&probe, 4);

		stepOne(fixture, Path::Interpreter);
		require(!clock.usesExactCycleScheduling() && !fixture.peripherals.hasCycleDelay()
			&& fixture.peripherals.getTargetClock() == 2,
			"legacy cycle clock did not retain its half-delay instruction heuristic");
		stepOne(fixture, Path::Interpreter);
		require(fixture.peripherals.getTargetClock() == 2,
			"legacy cycle-clock instruction deadline changed before becoming due");
		const auto phaseDeadline = fixture.peripherals.getTargetClock();
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		require(fixture.peripherals.getTargetClock() == phaseDeadline && !fixture.peripherals.hasCycleDelay(),
			"redundant clock-source selection discarded legacy phase");

		clock.setExactCycleScheduling(true);
		require(clock.usesExactCycleScheduling() && fixture.peripherals.hasCycleDelay()
			&& fixture.peripherals.getTargetCycleClock() == fixture.dsp.getCycles(),
			"exact-cycle opt-in did not rebase an immediate cycle-domain visit");
		stepOne(fixture, Path::Interpreter);
		require(fixture.peripherals.getTargetCycleClock() == 6,
			"exact-cycle opt-in did not publish its full cycle delay");
	}

	void frameSyncShortcutsAreScopedToLegacyUnpacedJit()
	{
		{
			Fixture fixture;
			SerialProbe probe(fixture.dsp);
			fixture.install(0, "nop");
			auto& clock = fixture.peripherals.getEssiClock();
			clock.setCyclesPerSample(1000);
			clock.setClockSource(EsxiClock::ClockSource::Cycles);
			stepOne(fixture, Path::Interpreter);
			require(clock.getRemainingInstructionsForFrameSync() > 0,
				"legacy cycle mode unexpectedly disabled its frame-sync shortcut");
			clock.setExactCycleScheduling(true);
			require(clock.getRemainingInstructionsForFrameSync() == 0,
				"exact cycle mode exposed a 1:1 frame-sync fast-forward");
		}

		auto installSpin = [](Fixture& _fixture)
		{
			const auto end = _fixture.install(0, "jset #$2,x:<<$ffffb7,$0");
			require(end == 2, "frame-sync spin fixture was not a two-word jump");
			_fixture.peripherals.getEssiClock().setCyclesPerSample(100);
		};

		Fixture ordinary;
		installSpin(ordinary);
		auto config = ordinary.dsp.getJit().getConfig();
		config.linkJitBlocks = false;
		config.maxInstructionsPerBlock = 1;
		ordinary.dsp.getJit().setConfig(config);
		ordinary.dsp.execJit();
		require(ordinary.dsp.getInstructionCounter() > 1 && ordinary.dsp.getPC().toWord() == 0,
			"ordinary JIT lost its legacy frame-sync fast-forward");

		Fixture paced;
		installSpin(paced);
		require(paced.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Activated,
			"failed to activate paced frame-sync fixture");
		const auto result = paced.dsp.executePacedJitSlice(1);
		require(result.stop == DSP::PacedJitExecutionStop::BudgetExhausted
			&& result.instructionDelta == 1 && paced.dsp.getPC().toWord() == 0,
			"paced JIT applied the legacy frame-sync fast-forward");
	}

	void modularDeadlinesAndZeroEffectivePeriodStayBounded()
	{
		constexpr auto max = std::numeric_limits<uint64_t>::max();
		require(IPeripherals::remainingToDeadline(max - 1, 1) == 3,
			"modular deadline did not cross uint64 wrap");
		require(IPeripherals::remainingToDeadline(2, 1) == 0,
			"modular deadline did not report an overdue wrapped target");

		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0, "wait");
		auto& clock = fixture.peripherals.getEssiClock();
		clock.setCyclesPerSample(1);
		clock.setSpeedPercent(1);
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setExactCycleScheduling(true);
		clock.setEsaiFinePeriod(&probe, 0);
		require(clock.getCyclesPerSample() == 0 && clock.getEffectiveCyclesPerSample() == 1,
			"exact zero-period normalization was not visible through introspection");
		const auto wait = fixture.dsp.executeBudgetedSlice(1);
		require(wait.stop == DSP::BudgetedExecutionStop::Waiting,
			"zero-effective-period fixture did not enter WAIT");
		const auto idle = fixture.dsp.executeBudgetedSlice(1);
		require(idle.stop == DSP::BudgetedExecutionStop::Waiting
			&& idle.instructionDelta == 1 && idle.cycleCounterDelta == 1,
			"zero effective base period caused a permanent due-now WAIT spin");
	}

	void instructionAndCycleDeadlinesKeepIndependentPrecedence()
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		require(fixture.dsp.memWriteP(0, 0x44f41b) && fixture.dsp.memWriteP(1, 0x000128),
			"failed to install deadline-precedence anchor");
		fixture.install(2, "nop");
		fixture.install(3, "nop");
		fixture.install(4, "nop");
		configureExact(fixture, probe, 5);
		stepOne(fixture, Path::Interpreter);
		require(fixture.dsp.getCycles() == 3 && fixture.peripherals.getTargetCycleClock() == 5,
			"exact cycle deadline did not survive a multi-cycle crossing");
		fixture.peripherals.setDelayCycles(1);
		stepOne(fixture, Path::Interpreter);
		stepOne(fixture, Path::Interpreter);
		require(probe.cycleTicks.empty() && fixture.dsp.getCycles() == 5
			&& fixture.peripherals.getTargetCycleClock() == 5,
			"earlier instruction deadline displaced the independent cycle deadline");
		stepOne(fixture, Path::Interpreter);
		require(probe.cycleTicks == std::vector<uint64_t>{5}
			&& fixture.peripherals.getTargetCycleClock() == 10,
			"cycle callback deadline was not refreshed from post-callback state");
	}

	void model56362SharesTheExactDeadlineBehavior()
	{
		DefaultMemoryValidator validator;
		Memory memory(validator, 0x1000);
		Peripherals56362 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp(memory, &peripherals, &noPeripherals);
		Assembler assembler;
		auto install = [&](const TWord pc, const char* source)
		{
			const auto assembled = assembler.assemble(source);
			require(assembled.success(), "failed to assemble 56362 fixture");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(pc + i, assembled.word[i]), "failed to install 56362 fixture");
		};
		SerialProbe probe(dsp);
		install(0, "nop");
		install(1, "bset #$4,a1");
		require(dsp.memWriteP(2, 0x44f41b) && dsp.memWriteP(3, 0x000128),
			"failed to install 56362 three-cycle anchor");
		install(4, "nop");
		auto& clock = peripherals.getEsaiClock();
		clock.setCyclesPerSample(1000);
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setExactCycleScheduling(true);
		clock.setEsaiFinePeriod(&probe, 3);
		for(unsigned i = 0; i < 4; ++i)
		{
			const auto result = dsp.executeBudgetedSlice(1);
			require(result.instructionDelta == 1, "56362 exact scheduling step did not retire once");
		}
		require(probe.cycleTicks == std::vector<uint64_t>({3, 6}),
			"56362 did not share 56303 exact cycle scheduling");
	}

	void ordinaryMultiOpJitTopologyIsUntouched()
	{
		if constexpr(!g_jitSupported)
			return;

		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		for(TWord pc = 0x100; pc < 0x106; ++pc)
			fixture.install(pc, "nop");
		// Keep this above the interrupt-vector area: default static fast-vector
		// compilation deliberately gives low PCs a different two-word topology.
		fixture.dsp.setPC(0x100);
		auto config = fixture.dsp.getJit().getConfig();
		config.linkJitBlocks = false;
		config.maxInstructionsPerBlock = 3;
		fixture.dsp.getJit().setConfig(config);
		auto& clock = fixture.peripherals.getEssiClock();
		clock.setCyclesPerSample(1000);
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setExactCycleScheduling(true);
		clock.setEsaiFinePeriod(&probe, 10);

		const auto before = fixture.dsp.getInstructionCounter();
		fixture.dsp.execJit();
		const auto& afterConfig = fixture.dsp.getJit().getConfig();
		require(fixture.dsp.getInstructionCounter() - before == 3
			&& fixture.dsp.getPC().toWord() == 0x103 && probe.cycleTicks.empty(),
			"exact scheduling rewrote an ordinary multi-op JIT block boundary");
		require(!afterConfig.linkJitBlocks && afterConfig.maxInstructionsPerBlock == 3,
			"exact scheduling mutated ordinary JIT configuration");
	}

	void oneTwoThreeCycleOrdering(const Path _path)
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0x100, "nop");
		fixture.install(0x101, "bset #$4,a1");
		require(fixture.dsp.memWriteP(0x102, 0x44f41b)
			&& fixture.dsp.memWriteP(0x103, 0x000128), "failed to install three-cycle anchor");
		fixture.install(0x104, "nop");
		require(fixture.cyclesAt(0x100) == 1 && fixture.cyclesAt(0x101) == 2
			&& fixture.cyclesAt(0x102) == 3, "one/two/three-cycle timing anchors changed");
		fixture.dsp.setPC(0x100);
		configureExact(fixture, probe, 3);
		beginPath(fixture, _path);

		stepOne(fixture, _path);
		stepOne(fixture, _path);
		require(probe.cycleTicks.empty(), "cycle callback ran before its retired-cycle deadline");
		stepOne(fixture, _path);
		require(probe.cycleTicks == std::vector<uint64_t>{3} && probe.pcs == std::vector<TWord>{0x102},
			"cycle callback was not ordered before the three-cycle operation");
		stepOne(fixture, _path);
		require(probe.cycleTicks == std::vector<uint64_t>({3, 6})
			&& probe.pcs == std::vector<TWord>({0x102, 0x104}),
			"cycle callback was not ordered after the three-cycle operation");
	}

	void basePeriodAlsoUsesTheExactCycleDeadline()
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0, "nop");
		fixture.install(1, "bset #$4,a1");
		require(fixture.dsp.memWriteP(2, 0x44f41b) && fixture.dsp.memWriteP(3, 0x000128),
			"failed to install base-period three-cycle anchor");
		fixture.install(4, "nop");
		auto& clock = fixture.peripherals.getEssiClock();
		clock.setCyclesPerSample(3);
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setExactCycleScheduling(true);
		clock.setEsaiFinePeriod(&probe, 0);
		for(unsigned i = 0; i < 4; ++i)
			stepOne(fixture, Path::Interpreter);
		require(probe.cycleTicks == std::vector<uint64_t>({3, 6}),
			"base-period serial clock did not use exact cycle deadlines");
	}

	void repRemainsAtomic(const Path _path)
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.dsp.regs().a.var = 0;
		fixture.dsp.regs().b.var = 0x00000001000000ull;
		fixture.install(0x100, "rep #$4");
		fixture.install(0x101, "add b,a");
		fixture.install(0x102, "nop");
		fixture.dsp.setPC(0x100);
		configureExact(fixture, probe, 2);
		beginPath(fixture, _path);

		if(_path == Path::Interpreter)
		{
			const auto result = fixture.dsp.executeBudgetedSlice(1);
			require(result.stop == DSP::BudgetedExecutionStop::AtomicBudgetOverrun,
				"interpreter REP did not report its atomic overrun");
		}
		else
		{
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun,
				"paced-JIT REP did not report its atomic overrun");
		}
		require(fixture.dsp.getCycles() == 9 && probe.cycleTicks.empty(),
			"cycle callback split an atomic REP boundary");
		stepOne(fixture, _path);
		require(probe.cycleTicks.size() == 4,
			"post-REP boundary did not catch up every elapsed fine tick");
		for(const auto tick : probe.cycleTicks)
			require(tick == 9, "REP catch-up callback observed a non-boundary cycle");
	}

	void fastVectorRemainsAtomic(const Path _path)
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0x100, "nop");
		fixture.install(0x101, "nop");
		fixture.install(0x102, "nop");
		fixture.install(Vba_NMI, "nop");
		fixture.install(Vba_NMI + 1, "nop");
		fixture.dsp.setPC(0x100);
		configureExact(fixture, probe, 2);
		beginPath(fixture, _path);
		stepOne(fixture, _path);
		require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to inject scheduling vector");

		if(_path == Path::Interpreter)
		{
			const auto result = fixture.dsp.executeBudgetedSlice(1);
			require(result.stop == DSP::BudgetedExecutionStop::AtomicBudgetOverrun,
				"interpreter vector did not report its atomic overrun");
		}
		else
		{
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun,
				"paced-JIT vector did not report its atomic overrun");
		}
		require(fixture.dsp.getCycles() == 3 && probe.cycleTicks.empty(),
			"cycle callback split a two-word fast vector");
		stepOne(fixture, _path);
		require(probe.cycleTicks.empty(), "cycle callback bypassed interrupt-prevention ordering");
		stepOne(fixture, _path);
		require(probe.cycleTicks.size() == 2 && probe.cycleTicks[0] == 4 && probe.cycleTicks[1] == 4,
			"post-vector boundary did not catch up at the restored source boundary");
	}

	void waitUsesTheExactCycleDeadline(const Path _path)
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0x100, "wait");
		fixture.install(0x101, "nop");
		fixture.dsp.setPC(0x100);
		configureExact(fixture, probe, 2);
		beginPath(fixture, _path);

		if(_path == Path::Interpreter)
		{
			const auto wait = fixture.dsp.executeBudgetedSlice(1);
			require(wait.stop == DSP::BudgetedExecutionStop::Waiting
				&& wait.instructionDelta == 0 && wait.cycleCounterDelta == 0,
				"interpreter WAIT retired before wake");
			fixture.dsp.executeBudgetedSlice(1);
			require(probe.cycleTicks.empty() && fixture.dsp.getCycles() == 1,
				"interpreter WAIT overshot its first exact idle cycle");
			fixture.dsp.executeBudgetedSlice(1);
			require(probe.cycleTicks == std::vector<uint64_t>{2} && fixture.dsp.getCycles() == 2,
				"interpreter WAIT missed its exact cycle deadline");
		}
		else
		{
			const auto wait = fixture.dsp.executePacedJitSlice(1);
			require(wait.stop == DSP::PacedJitExecutionStop::Waiting
				&& wait.instructionDelta == 1 && wait.cycleCounterDelta > 1
				&& probe.cycleTicks.empty(), "paced WAIT did not remain one atomic boundary");
			const auto cycles = fixture.dsp.getCycles();
			const auto service = fixture.dsp.executePacedJitSlice(1);
			require(service.stop == DSP::PacedJitExecutionStop::Waiting
				&& service.instructionDelta == 0 && service.cycleCounterDelta == 0
				&& !probe.cycleTicks.empty(), "paced WAIT did not service a due cycle deadline without overshoot");
			for(const auto tick : probe.cycleTicks)
				require(tick == cycles, "paced WAIT callback observed idle overshoot");
		}
	}

	void resetsRebaseExactCycleScheduling()
	{
		Fixture fixture;
		SerialProbe probe(fixture.dsp);
		fixture.install(0, "bset #$4,a1");
		fixture.install(1, "nop");
		fixture.install(2, "nop");
		configureExact(fixture, probe, 3);
		stepOne(fixture, Path::Interpreter);
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		require(!probe.cycleTicks.empty(), "reset fixture did not establish a pre-reset cycle phase");

		probe.cycleTicks.clear();
		fixture.dsp.resetSW();
		const auto swAnchor = fixture.dsp.getCycles();
		fixture.dsp.setPC(0);
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		require(probe.cycleTicks == std::vector<uint64_t>{swAnchor + 3},
			"software reset did not rebase exact cycle scheduling");

		probe.cycleTicks.clear();
		fixture.dsp.resetHW();
		require(fixture.dsp.getCycles() == 0 && fixture.peripherals.getTargetCycleClock() == 0,
			"hardware reset did not rebase the exact cycle deadline at zero");
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		fixture.dsp.executeBudgetedSlice(1);
		require(probe.cycleTicks == std::vector<uint64_t>{3},
			"hardware reset retained a pre-reset cycle anchor");
	}
}

int main()
{
	try
	{
		instructionDeadlinesRemainTheDefault();
		legacyCycleClockRemainsAdvisoryUntilExplicitOptIn();
		frameSyncShortcutsAreScopedToLegacyUnpacedJit();
		modularDeadlinesAndZeroEffectivePeriodStayBounded();
		instructionAndCycleDeadlinesKeepIndependentPrecedence();
		model56362SharesTheExactDeadlineBehavior();
		ordinaryMultiOpJitTopologyIsUntouched();
		basePeriodAlsoUsesTheExactCycleDeadline();
		for(const auto path : {Path::Interpreter, Path::PacedJit})
		{
			oneTwoThreeCycleOrdering(path);
			repRemainsAtomic(path);
			fastVectorRemainsAtomic(path);
			waitUsesTheExactCycleDeadline(path);
		}
		resetsRebaseExactCycleScheduling();
		std::puts("DSP56300 cycle-domain scheduling tests passed");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "DSP56300 cycle-domain scheduling test failed: %s\n", error.what());
		return 1;
	}
}
