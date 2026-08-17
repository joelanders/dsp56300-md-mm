#include "assembler.h"
#include "dsp.h"
#include "interrupts.h"
#include "memory.h"
#include "peripherals.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace
{
	std::atomic<bool> g_measureAllocations{false};
	std::atomic<size_t> g_allocations{0};
}

void* operator new(const size_t _size)
{
	if(g_measureAllocations.load(std::memory_order_relaxed))
		g_allocations.fetch_add(1, std::memory_order_relaxed);
	if(auto* const memory = std::malloc(_size))
		return memory;
	throw std::bad_alloc();
}

void* operator new[](const size_t _size) { return ::operator new(_size); }
void operator delete(void* const _memory) noexcept { std::free(_memory); }
void operator delete[](void* const _memory) noexcept { std::free(_memory); }
void operator delete(void* const _memory, size_t) noexcept { std::free(_memory); }
void operator delete[](void* const _memory, size_t) noexcept { std::free(_memory); }

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
			require(assembled.success(), "failed to assemble bounded-JIT instruction");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(_pc + i, assembled.word[i]),
					"failed to install bounded-JIT instruction");
			return _pc + assembled.wordCount;
		}

		void begin()
		{
			require(dsp.beginBoundedJitExecution() == DSP::BoundedJitActivation::Activated,
				"failed to activate bounded JIT");
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

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

	void installNops(Fixture& _fixture, const TWord _first, const unsigned _count)
	{
		for(unsigned i = 0; i < _count; ++i)
			_fixture.install(_first + i, "nop");
	}

	void activationOwnsOneFixedBlockAndRestoresTheExactConfiguration()
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
		JitConfig perPc;
		perPc.maxInstructionsPerBlock = 9;
		perPc.dynamicFastInterrupts = true;
		original.getBlockConfig = [perPc](const TWord _pc)
		{
			return _pc == 0x222 ? std::optional<JitConfig>{perPc}
				: std::optional<JitConfig>{};
		};
		fixture.dsp.getJit().setConfig(original);

		installNops(fixture, 0x100, DSP::BoundedJitMaxInstructionsPerBlock + 3);
		fixture.dsp.setPC(0x100);
		const auto beforeOrdinary = fixture.dsp.getInstructionCounter();
		fixture.dsp.execJit();
		require(fixture.dsp.getInstructionCounter() - beforeOrdinary == 3
			&& fixture.dsp.getPC().toWord() == 0x103,
			"ordinary pre-activation block did not use the saved configuration");
		const auto ordinaryEntry = fixture.dsp.getJitEntries()[0x100];

		fixture.dsp.setPC(0x100);
		fixture.begin();
		require(fixture.dsp.getJitEntries()[0x100] != ordinaryEntry,
			"bounded activation retained an ordinary compiled block");
		require(fixture.dsp.isBoundedJitExecutionActive()
			&& !fixture.dsp.isPacedJitExecutionActive(),
			"bounded activation was exposed as the precise paced mode");
		const auto& bounded = fixture.dsp.getJit().getConfig();
		require(!bounded.linkJitBlocks
			&& bounded.maxInstructionsPerBlock == DSP::BoundedJitMaxInstructionsPerBlock
			&& bounded.maxDoIterations == DSP::BoundedJitMaxDoIterations
			&& bounded.dynamicFastInterrupts && !bounded.getBlockConfig,
			"bounded activation did not install its complete fixed configuration");
		require(fixture.dsp.beginBoundedJitExecution() == DSP::BoundedJitActivation::AlreadyActive
			&& fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::AlreadyActive
			&& !fixture.dsp.endPacedJitExecution(),
			"bounded ownership was not mutually exclusive with paced ownership");

		const auto instructionsBefore = fixture.dsp.getInstructionCounter();
		const auto cyclesBefore = fixture.dsp.getCycles();
		const auto pcBefore = fixture.dsp.getPC();
		const auto invalid = fixture.dsp.executeBoundedJitBlock(
			DSP::BoundedJitMaxInstructionsPerBlock + 1);
		require(!fixture.dsp.injectInterruptImmediate(Vba_NMI),
			"immediate interrupt bypassed bounded-JIT ownership");
		fixture.dsp.exec();
		fixture.dsp.execJit();
		fixture.dsp.execInterpreter();
		const auto mixed = fixture.dsp.executeBudgetedSlice(1);
		require(fixture.dsp.getInstructionCounter() == instructionsBefore
			&& fixture.dsp.getCycles() == cyclesBefore && fixture.dsp.getPC() == pcBefore
			&& invalid.stop == DSP::BoundedJitExecutionStop::InvalidBudget
			&& mixed.stop == DSP::BudgetedExecutionStop::ReentryDenied,
			"legacy executor bypassed bounded-JIT ownership");

		const auto block = fixture.dsp.executeBoundedJitBlock();
		require(block.stop == DSP::BoundedJitExecutionStop::BlockComplete
			&& block.instructionDelta == DSP::BoundedJitMaxInstructionsPerBlock
			&& block.cycleCounterDelta == DSP::BoundedJitMaxInstructionsPerBlock
			&& fixture.dsp.getPC().toWord() == 0x120,
			"bounded JIT did not execute exactly one fixed 32-NOP block");
		const auto boundedEntry = fixture.dsp.getJitEntries()[0x100];

		require(fixture.dsp.endBoundedJitExecution(), "failed to end bounded ownership");
		const auto& restored = fixture.dsp.getJit().getConfig();
		require(sameJitConfigScalars(restored, original)
			&& static_cast<bool>(restored.getBlockConfig)
			&& !restored.getBlockConfig(0x100).has_value()
			&& restored.getBlockConfig(0x222).has_value()
			&& sameJitConfigScalars(*restored.getBlockConfig(0x222), perPc),
			"bounded teardown did not restore every JIT scalar and callback");
		require(fixture.dsp.getJitEntries()[0x100] != boundedEntry,
			"bounded teardown retained a bounded compiled block");

		fixture.dsp.setPC(0x100);
		const auto afterRestore = fixture.dsp.getInstructionCounter();
		fixture.dsp.execJit();
		require(fixture.dsp.getInstructionCounter() - afterRestore == 3
			&& fixture.dsp.getPC().toWord() == 0x103,
			"bounded teardown did not restore ordinary three-op execution");
	}

	JitConfig boundedConfigFor(DSP& _dsp)
	{
		auto config = _dsp.getJit().getConfig();
		config.linkJitBlocks = false;
		config.maxInstructionsPerBlock = DSP::BoundedJitMaxInstructionsPerBlock;
		config.maxDoIterations = DSP::BoundedJitMaxDoIterations;
		config.dynamicFastInterrupts = true;
		config.getBlockConfig = {};
		return config;
	}

	void measuredDynamicBlocksMatchTheSameOrdinaryJitConfiguration()
	{
		{
			Fixture reference;
			Fixture bounded;
			for(auto* fixture : {&reference, &bounded})
			{
				fixture->dsp.regs().a.var = 0;
				fixture->dsp.regs().b.var = 0x00000001000000ull;
				fixture->install(0x100, "rep #$40");
				fixture->install(0x101, "add b,a");
				fixture->install(0x102, "jmp $102");
				fixture->dsp.setPC(0x100);
			}

			reference.dsp.getJit().setConfig(boundedConfigFor(reference.dsp));
			reference.dsp.execJit();

			bounded.begin();
			const auto result = bounded.dsp.executeBoundedJitBlock();
			require(result.stop == DSP::BoundedJitExecutionStop::BlockComplete
				&& result.instructionDelta == bounded.dsp.getInstructionCounter()
				&& result.cycleCounterDelta == bounded.dsp.getCycles()
				&& result.instructionDelta > DSP::BoundedJitMaxInstructionsPerBlock
				&& bounded.dsp.regs().a.var == 0x00000040000000ull,
				"bounded REP block did not report its authoritative measured overrun");
			require(bounded.dsp.getPC() == reference.dsp.getPC()
				&& bounded.dsp.getInstructionCounter() == reference.dsp.getInstructionCounter()
				&& bounded.dsp.getCycles() == reference.dsp.getCycles()
				&& bounded.dsp.regs().a.var == reference.dsp.regs().a.var
				&& bounded.dsp.getSR() == reference.dsp.getSR(),
				"bounded REP block diverged from the same ordinary JIT configuration");
			require(bounded.dsp.endBoundedJitExecution(),
				"failed to end REP bounded session");
		}

		{
			Fixture reference;
			Fixture bounded;
			for(auto* fixture : {&reference, &bounded})
			{
				fixture->dsp.regs().a.var = 0x400000;
				fixture->dsp.regs().b.var = 0x00000001000000ull;
				TWord pc = 0x100;
				pc = fixture->install(pc, "do #$2,>$107");
				pc = fixture->install(pc, "do #$2,>$106");
				pc = fixture->install(pc, "add b,a");
				pc = fixture->install(pc, "nop");
				fixture->install(pc, "nop");
				fixture->install(0x107, "jmp $107");
				fixture->dsp.setPC(0x100);
			}

			reference.dsp.getJit().setConfig(boundedConfigFor(reference.dsp));
			bounded.begin();
			for(unsigned block = 0;
				block < 32 && bounded.dsp.getPC().toWord() != 0x107; ++block)
			{
				const auto instructionsBefore = bounded.dsp.getInstructionCounter();
				const auto cyclesBefore = bounded.dsp.getCycles();
				reference.dsp.execJit();
				const auto result = bounded.dsp.executeBoundedJitBlock();
				require(result.stop == DSP::BoundedJitExecutionStop::BlockComplete
					&& result.instructionDelta
						== bounded.dsp.getInstructionCounter() - instructionsBefore
					&& result.cycleCounterDelta
						== bounded.dsp.getCycles() - cyclesBefore,
					"bounded finite-DO visit did not report its measured block delta");
				require(bounded.dsp.getPC() == reference.dsp.getPC()
					&& bounded.dsp.getInstructionCounter()
						== reference.dsp.getInstructionCounter()
					&& bounded.dsp.getCycles() == reference.dsp.getCycles()
					&& bounded.dsp.regs().a.var == reference.dsp.regs().a.var
					&& bounded.dsp.regs().sp == reference.dsp.regs().sp
					&& bounded.dsp.regs().sc == reference.dsp.regs().sc
					&& bounded.dsp.regs().la == reference.dsp.regs().la
					&& bounded.dsp.regs().lc == reference.dsp.regs().lc
					&& bounded.dsp.getSR() == reference.dsp.getSR(),
					"bounded finite-DO visit diverged from the same ordinary JIT configuration");
			}
			require(bounded.dsp.getPC().toWord() == 0x107
				&& bounded.dsp.getInstructionCounter() == 13
				&& bounded.dsp.getCycles() == 25
				&& bounded.dsp.regs().a.var == 0x0000000004400000ull,
				"bounded finite-DO visits did not reach the independent completed state");
			require(bounded.dsp.endBoundedJitExecution(),
				"failed to end finite-DO bounded session");
		}
	}

	void prewarmedOnlyModeRejectsAnUnseenBlockWithoutLosingOwnership()
	{
		Fixture fixture;
		installNops(fixture, 0x100, DSP::BoundedJitMaxInstructionsPerBlock);
		fixture.install(0x200, "nop");
		fixture.install(0x201, "jmp $200");
		fixture.dsp.setPC(0x100);
		fixture.begin();
		const auto warm = fixture.dsp.executeBoundedJitBlock();
		require(warm.stop == DSP::BoundedJitExecutionStop::BlockComplete
			&& warm.instructionDelta == DSP::BoundedJitMaxInstructionsPerBlock,
			"prewarmed-only fixture did not compile its preparation block");
		require(!fixture.dsp.setBoundedJitCompilationAllowed(false),
			"compilation closed on an unseen current program address");
		fixture.dsp.setPC(0x100);
		require(fixture.dsp.setBoundedJitCompilationAllowed(false),
			"failed to close bounded JIT compilation at a quiescent boundary");

		g_allocations.store(0, std::memory_order_relaxed);
		g_measureAllocations.store(true, std::memory_order_relaxed);
		const auto cached = fixture.dsp.executeBoundedJitBlock();
		g_measureAllocations.store(false, std::memory_order_relaxed);
		require(cached.stop == DSP::BoundedJitExecutionStop::BlockComplete
			&& cached.instructionDelta == DSP::BoundedJitMaxInstructionsPerBlock
			&& g_allocations.load(std::memory_order_relaxed) == 0,
			"prewarmed block was not executable after compilation closed");

		fixture.dsp.setPC(0x200);
		g_allocations.store(0, std::memory_order_relaxed);
		g_measureAllocations.store(true, std::memory_order_relaxed);
		const auto interpreted = fixture.dsp.executeBoundedJitBlock();
		g_measureAllocations.store(false, std::memory_order_relaxed);
		require(interpreted.stop == DSP::BoundedJitExecutionStop::BlockComplete && interpreted.instructionDelta == 1 && fixture.dsp.getPC().toWord() == 0x201 && g_allocations.load(std::memory_order_relaxed) == 0
			&& fixture.dsp.isBoundedJitExecutionActive(),
				"unseen realtime PC did not use one allocation-free interpreter boundary");

		require(fixture.dsp.setBoundedJitCompilationAllowed(true),
			"failed to reopen preparation-time compilation");
		fixture.dsp.setPC(0x200);
		const auto prepared = fixture.dsp.executeBoundedJitBlock();
		require(prepared.stop == DSP::BoundedJitExecutionStop::BlockComplete
			&& prepared.instructionDelta == 2
			&& fixture.dsp.getPC().toWord() == 0x200,
			"cache-missed block could not be prepared and retried");
		require(fixture.dsp.endBoundedJitExecution()
			&& !fixture.dsp.setBoundedJitCompilationAllowed(false),
			"prewarmed-only control escaped bounded ownership");

		Fixture interruptFixture;
		installNops(interruptFixture, 0x100,
			DSP::BoundedJitMaxInstructionsPerBlock - 1);
		interruptFixture.install(0x11f, "jmp $100");
		interruptFixture.install(Vba_NMI, "nop");
		interruptFixture.install(Vba_NMI + 1, "nop");
		interruptFixture.dsp.setPC(0x100);
		interruptFixture.begin();
		require(interruptFixture.dsp.executeBoundedJitBlock().stop
				== DSP::BoundedJitExecutionStop::BlockComplete
			&& interruptFixture.dsp.setBoundedJitCompilationAllowed(false)
			&& interruptFixture.dsp.injectInterrupt(Vba_NMI),
			"failed to prepare the unseen-vector cache-miss fixture");
		const auto vectorMiss = interruptFixture.dsp.executeBoundedJitBlock();
		require(vectorMiss.stop == DSP::BoundedJitExecutionStop::CacheMiss
			&& vectorMiss.instructionDelta == 0 && vectorMiss.cycleCounterDelta == 0
			&& interruptFixture.dsp.hasPendingInterrupts()
			&& interruptFixture.dsp.getProcessingMode() == DSP::Default,
			"unseen interrupt vector was consumed or acknowledged before compilation");
		require(interruptFixture.dsp.setBoundedJitCompilationAllowed(true),
			"failed to reopen vector preparation");
		const auto preparedVector = interruptFixture.dsp.executeBoundedJitBlock();
		require(preparedVector.stop == DSP::BoundedJitExecutionStop::BlockComplete
			&& preparedVector.instructionDelta == 2
			&& interruptFixture.dsp.hasPendingInterrupts()
			&& interruptFixture.dsp.getProcessingMode()
				== DSP::DefaultPreventInterrupt,
			"retained interrupt vector did not compile with its exact fast chronology");
		const auto vectorFollowup = interruptFixture.dsp.executeBoundedJitBlock();
		require(vectorFollowup.stop == DSP::BoundedJitExecutionStop::BlockComplete
			&& vectorFollowup.instructionDelta != 0
			&& !interruptFixture.dsp.hasPendingInterrupts()
			&& interruptFixture.dsp.getProcessingMode() == DSP::Default,
			"prepared fast vector did not acknowledge on its following source block");
		require(interruptFixture.dsp.endBoundedJitExecution(),
			"failed to end the unseen-vector bounded session");
	}

	void preparedVolatileProgramWritesUseTheAllocationFreeInterpreter()
	{
		Fixture fixture;
		const auto first = fixture.assembler.assemble("move #$1,r1");
		const auto second = fixture.assembler.assemble("move #$2,r1");
		require(first.success() && second.success() && first.wordCount == 1 && second.wordCount == 1,
				"failed to assemble volatile-program payloads");

		fixture.install(0x200, "move #$1,r1");
		fixture.install(0x201, "jmp $100");
		TWord writer = 0x100;
		writer = fixture.install(writer, "move r0,p:(r2)");
		fixture.install(writer, "jmp $200");
		const auto writerSuccessor = writer;
		fixture.dsp.regs().r[0].var = second.word[0];
		fixture.dsp.regs().r[2].var = 0x200;
		fixture.dsp.setPC(0x200);
		fixture.begin();

		require(fixture.dsp.executeBoundedJitBlock().stop == DSP::BoundedJitExecutionStop::BlockComplete,
				"failed to compile the future volatile program address");
		fixture.dsp.setPC(0x100);
		require(fixture.dsp.executeBoundedJitBlock().stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					fixture.dsp.getJit().isVolatileP(0x200),
				"preparation-time program write was not isolated as volatile");
		require(fixture.dsp.executeBoundedJitBlock().stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					fixture.dsp.getPC().toWord() == 0x200,
				"program-write successor did not reach the volatile instruction");
		require(fixture.dsp.executeBoundedJitBlock().stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					fixture.dsp.getPC().toWord() == 0x201,
				"failed to compile the isolated volatile instruction");
		require(fixture.dsp.executeBoundedJitBlock().stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					fixture.dsp.getPC().toWord() == 0x100 && fixture.dsp.setBoundedJitCompilationAllowed(false),
				"failed to close compilation after volatile-path preparation");

		fixture.dsp.regs().r[0].var = first.word[0];
		g_allocations.store(0, std::memory_order_relaxed);
		g_measureAllocations.store(true, std::memory_order_relaxed);
		const auto write = fixture.dsp.executeBoundedJitBlock();
		const auto writeReachedSuccessor = fixture.dsp.getPC().toWord() == writerSuccessor;
		const auto branch = fixture.dsp.executeBoundedJitBlock();
		const auto interpreted = fixture.dsp.executeBoundedJitBlock();
		g_measureAllocations.store(false, std::memory_order_relaxed);
		require(write.stop == DSP::BoundedJitExecutionStop::BlockComplete && writeReachedSuccessor &&
					fixture.memory.get(MemArea_P, 0x200) == first.word[0] &&
					branch.stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					interpreted.stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					interpreted.instructionDelta == 1 && fixture.dsp.getPC().toWord() == 0x201 &&
					fixture.dsp.regs().r[1].toWord() == 1 && g_allocations.load(std::memory_order_relaxed) == 0,
				"known volatile program write did not use the allocation-free interpreter");

		require(fixture.dsp.setBoundedJitCompilationAllowed(true),
				"failed to reopen compilation after a volatile program write");
		fixture.dsp.setPC(0x200);
		require(fixture.dsp.executeBoundedJitBlock().stop == DSP::BoundedJitExecutionStop::BlockComplete &&
					fixture.dsp.regs().r[1].toWord() == 1,
				"reopened preparation did not refresh the volatile native block");
		require(fixture.dsp.endBoundedJitExecution(), "failed to end volatile-program bounded session");
	}

	void waitStopAndResetRemainBounded()
	{
		{
			Fixture fixture;
			fixture.install(0x100, "wait");
			fixture.install(0x101, "move #$1,r0");
			fixture.install(0x102, "jmp $102");
			fixture.install(Vba_NMI, "nop");
			fixture.install(Vba_NMI + 1, "nop");
			fixture.dsp.setPC(0x100);
			fixture.begin();

			const auto wait = fixture.dsp.executeBoundedJitBlock();
			require(wait.stop == DSP::BoundedJitExecutionStop::Waiting
				&& wait.instructionDelta == 1 && fixture.dsp.getPC().toWord() == 0x101,
				"bounded WAIT did not park before its successor");
			const auto parked = fixture.dsp.executeBoundedJitBlock();
			require(parked.stop == DSP::BoundedJitExecutionStop::Waiting
				&& parked.instructionDelta <= DSP::BoundedJitMaxInstructionsPerBlock
				&& parked.cycleCounterDelta <= DSP::BoundedJitMaxInstructionsPerBlock
				&& fixture.dsp.regs().r[0].toWord() == 0,
				"parked bounded WAIT was not returned within one fixed idle quantum");
			const auto passiveCounterVisit = fixture.dsp.executeBoundedJitBlock(0);
			require(passiveCounterVisit.stop == DSP::BoundedJitExecutionStop::Waiting
				&& passiveCounterVisit.instructionDelta == 0
				&& passiveCounterVisit.cycleCounterDelta == 0,
				"zero-counter bounded WAIT visit invented idle progress");
			require(fixture.dsp.injectInterrupt(Vba_NMI), "failed to queue bounded WAIT wake");
			const auto wake = fixture.dsp.executeBoundedJitBlock();
			require(wake.stop == DSP::BoundedJitExecutionStop::BlockComplete
				&& wake.instructionDelta == 0 && fixture.dsp.regs().r[0].toWord() == 0,
				"bounded WAIT wake did not expose its zero-retirement control boundary");
			const auto vector = fixture.dsp.executeBoundedJitBlock();
			require(vector.stop == DSP::BoundedJitExecutionStop::BlockComplete
				&& vector.instructionDelta == 2 && fixture.dsp.regs().r[0].toWord() == 0,
				"bounded WAIT vector executed its successor in the same visit");
			const auto successor = fixture.dsp.executeBoundedJitBlock();
			require(successor.stop == DSP::BoundedJitExecutionStop::BlockComplete
				&& successor.instructionDelta == 2 && fixture.dsp.regs().r[0].toWord() == 1,
				"bounded WAIT successor did not resume in one generated block");
			require(fixture.dsp.endBoundedJitExecution(), "failed to end bounded WAIT session");
		}

		{
			Fixture fixture;
			fixture.install(0x100, "nop");
			fixture.install(0x101, "nop");
			fixture.install(0x102, "stop");
			fixture.install(0x103, "move #$1,r0");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			const auto stopped = fixture.dsp.executeBoundedJitBlock();
			require(stopped.stop == DSP::BoundedJitExecutionStop::Halted
				&& stopped.instructionDelta == 3 && fixture.dsp.getPC().toWord() == 0x102
				&& fixture.dsp.regs().r[0].toWord() == 0,
				"mid-block STOP did not terminate at its reset-only no-successor latch");
			require(fixture.dsp.executeBoundedJitBlock().stop
				== DSP::BoundedJitExecutionStop::Halted,
				"bounded STOP latch executed more source code");
			fixture.dsp.resetSW();
			require(!fixture.dsp.isBoundedJitExecutionActive()
				&& fixture.dsp.executeBoundedJitBlock().stop
					== DSP::BoundedJitExecutionStop::Inactive,
				"reset did not restore configuration and release bounded STOP ownership");
		}

		{
			Fixture fixture;
			fixture.install(0x100, "nop");
			fixture.install(0x101, "nop");
			fixture.install(0x102, "reset");
			fixture.install(0x103, "move #$1,r0");
			fixture.dsp.setPC(0x100);
			fixture.begin();
			const auto reset = fixture.dsp.executeBoundedJitBlock();
			require(reset.stop == DSP::BoundedJitExecutionStop::OwnershipLost
				&& reset.instructionDelta == 3
				&& !fixture.dsp.isBoundedJitExecutionActive()
				&& fixture.dsp.regs().r[0].toWord() == 0,
				"mid-block RESET did not terminate before deferred bounded teardown");
		}
	}

	void callbacksCannotReenterOrSilentlyChangeTheOwnedConfiguration()
	{
		Fixture fixture;
		DSP::BoundedJitExecutionResult reentry;
		bool endAccepted = true;
		const auto custom = fixture.dsp.registerInterruptFunc([&]
		{
			reentry = fixture.dsp.executeBoundedJitBlock();
			endAccepted = fixture.dsp.endBoundedJitExecution();
			auto changed = fixture.dsp.getJit().getConfig();
			changed.maxInstructionsPerBlock = 7;
			fixture.dsp.getJit().setConfig(changed);
		});
		fixture.install(0x100, "move #$1,r0");
		fixture.dsp.setPC(0x100);
		fixture.begin();
		require(fixture.dsp.injectInterrupt(custom), "failed to queue bounded callback");
		const auto outer = fixture.dsp.executeBoundedJitBlock();
		require(reentry.stop == DSP::BoundedJitExecutionStop::ReentryDenied
			&& reentry.instructionDelta == 0 && !endAccepted,
			"bounded callback reentry or live teardown was not denied");
		require(outer.stop == DSP::BoundedJitExecutionStop::OwnershipLost
			&& outer.instructionDelta == 0 && fixture.dsp.getPC().toWord() == 0x100
			&& fixture.dsp.regs().r[0].toWord() == 0,
			"callback configuration mutation was not detected before source dispatch");
		require(fixture.dsp.endBoundedJitExecution(),
			"failed to restore a configuration-lost bounded session");
	}
}

int main()
{
	try
	{
		if constexpr(dsp56k::g_jitSupported)
		{
			activationOwnsOneFixedBlockAndRestoresTheExactConfiguration();
			measuredDynamicBlocksMatchTheSameOrdinaryJitConfiguration();
			prewarmedOnlyModeRejectsAnUnseenBlockWithoutLosingOwnership();
			preparedVolatileProgramWritesUseTheAllocationFreeInterpreter();
			waitStopAndResetRemainBounded();
			callbacksCannotReenterOrSilentlyChangeTheOwnedConfiguration();
		}
		else
		{
			Fixture fixture;
			require(fixture.dsp.beginBoundedJitExecution()
				== DSP::BoundedJitActivation::Unsupported,
				"unsupported backend accepted bounded JIT activation");
		}
		std::puts("DSP56300 bounded JIT execution tests passed");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "DSP56300 bounded JIT execution test failed: %s\n", error.what());
		return 1;
	}
}
