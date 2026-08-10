#include "assembler.h"
#include "dsp.h"
#include "hdi08.h"
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

		TWord install(TWord _pc, const char* const _source)
		{
			const auto assembled = assembler.assemble(_source);
			require(assembled.success(), "failed to assemble HI08 fixture");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(_pc + i, assembled.word[i]),
					"failed to install HI08 fixture");
			return _pc + assembled.wordCount;
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	HDI08& hostPort(Peripherals56303& _peripherals) { return _peripherals.getHI08(); }
	HDI08& hostPort(Peripherals56362& _peripherals) { return _peripherals.getHDI08(); }

	template<typename TPeripherals>
	void verifyNonblockingQueues()
	{
		DefaultMemoryValidator validator;
		Memory memory{validator, 0x1000};
		TPeripherals peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp{memory, &peripherals, &noPeripherals};
		auto& hdi = hostPort(peripherals);

		TWord word = 0xabcdef;
		require(!hdi.tryReadTX(word) && word == 0xabcdef,
			"empty nonblocking HOTX read mutated output");
		for(size_t i = 0; i < 8192; ++i)
			require(hdi.tryWriteRX(static_cast<TWord>(i)), "HORX rejected before capacity");
		require(hdi.rxWordsAvailable() == 8192 && hdi.rxWordsFree() == 0
			&& !hdi.tryWriteRX(0x123456) && hdi.rxWordsAvailable() == 8192,
			"full HORX admission was not an exact no-op");
		for(size_t i = 0; i < 8192; ++i)
			require(hdi.readRX(Move_Nop) == static_cast<TWord>(i),
				"nonblocking HORX admission changed FIFO order");
	}

	void txRetentionIsExplicitAndBounded()
	{
		Fixture legacy;
		auto& legacyHdi = legacy.peripherals.getHI08();
		legacyHdi.writeTX(1);
		legacyHdi.writeTX(2);
		require(legacyHdi.hostTxRetention() == HDI08::HostTxRetention::Legacy
			&& legacyHdi.txWordsAvailable() == 2
			&& legacyHdi.hostTxOverflowCount() == 0,
			"default HOTX behavior changed");
		TWord word = 0;
		require(legacyHdi.tryReadTX(word) && word == 1
			&& legacyHdi.tryReadTX(word) && word == 2
			&& !legacyHdi.tryReadTX(word), "legacy HOTX FIFO was not preserved");

		Fixture single;
		auto& singleHdi = single.peripherals.getHI08();
		require(singleHdi.configureHostTxRetention(HDI08::HostTxRetention::SingleRegister),
			"single-register configuration failed");
		require(singleHdi.hostTxAdmissionLimit() == 1,
			"single-register policy did not expose its exact one-word retention limit");
		singleHdi.writeTX(0x111111);
		singleHdi.writeTX(0x222222);
		require(singleHdi.txWordsAvailable() == 1
			&& singleHdi.hostTxOverflowCount() == 1
			&& singleHdi.tryReadTX(word) && word == 0x222222,
			"single-register HOTX did not replace/count exactly");
		singleHdi.writePortControlRegister(TWord{1} << HDI08::HPCR_HEN);
		singleHdi.writeTX(0x333333);
		require((singleHdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HTDE)) == 0,
			"retained HOTX did not lower HTDE");
		singleHdi.clearTX();
		require(singleHdi.txWordsAvailable() == 0
			&& (singleHdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HTDE)) == 0,
			"clearTX did not preserve deferred HTDE service semantics");
		(void)singleHdi.exec();
		require((singleHdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HTDE)) != 0,
			"peripheral service did not recompute HTDE after clearTX");

		Fixture bounded;
		auto& boundedHdi = bounded.peripherals.getHI08();
		require(!boundedHdi.configureHostTxRetention(HDI08::HostTxRetention::BoundedFifo, 0)
			&& boundedHdi.configureHostTxRetention(HDI08::HostTxRetention::BoundedFifo, 2),
			"bounded HOTX accepted an invalid limit or rejected a valid one");
		boundedHdi.writeTX(3);
		boundedHdi.writeTX(4);
		boundedHdi.writeTX(5);
		require(boundedHdi.hostTxBackpressured() && boundedHdi.txWordsAvailable() == 2
			&& boundedHdi.hostTxOverflowCount() == 1,
			"bounded HOTX did not reject/count at its exact limit");
		require(!boundedHdi.configureHostTxRetention(HDI08::HostTxRetention::SingleRegister),
			"one-time HOTX policy unexpectedly changed after activation");
		require(boundedHdi.tryReadTX(word) && word == 3
			&& boundedHdi.tryReadTX(word) && word == 4,
			"bounded HOTX did not preserve accepted FIFO words");
	}

	void serializedOwnerControlsNativeCommandDispatchOrdering()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord Data = 0x123456;
		constexpr TWord Vector = 0x0c;
		constexpr TWord Hcp = TWord{1} << HDI08::HSR_HCP;
		constexpr TWord Hrdf = TWord{1} << HDI08::HSR_HRDF;

		require(hdi.setNativeHostCommandsEnabled(true), "native command enable failed");
		require(hdi.tryWriteRX(Data), "command data admission failed");
		DSP::ExternalInterruptToken equalVectorToken = 0;
		require(fixture.dsp.tryInjectExternalInterrupt(Vector, equalVectorToken),
			"equal-vector predecessor setup failed");
		require(hdi.tryWriteHostCommand(Vector) == HDI08::HostCommandAdmission::Accepted
			&& (hdi.readStatusRegister() & (Hcp | Hrdf)) == (Hcp | Hrdf)
			&& fixture.dsp.hasPendingExternalInterrupts(),
			"HCIE-off command dispatched or hid HRDF");
		require(hdi.tryWriteHostCommand(Vector + 2) == HDI08::HostCommandAdmission::Busy,
			"second hardware command was accepted while HCP was pending");

		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(fixture.dsp.hasPendingExternalInterrupts()
			&& (hdi.readStatusRegister() & Hcp) != 0
			&& (hdi.readStatusRegister() & Hrdf) == 0,
			"HCIE transition did not publish command and hold mainline HRDF");
		require(hdi.readRX(Move_Nop) == 0 && hdi.rxWordsAvailable() == 1,
			"held mainline HORX read consumed command data");

		const auto predecessorEnd = fixture.install(Vector, "movep x:<<$ffffc6,y:>$6");
		fixture.install(predecessorEnd, "nop");
		fixture.install(0x100, "bra $100");
		fixture.dsp.setPC(0x100);
		fixture.dsp.processExternalInterrupts();
		fixture.dsp.execInterpreter();
		require(hdi.hostCommandPending() && hdi.rxWordsAvailable() == 1
			&& fixture.memory.get(MemArea_Y, 6) == 0,
			"equal-vector predecessor cleared HCP or consumed tagged command data");
		const auto vectorEnd = fixture.install(Vector, "movep x:<<$ffffc6,y:>$1");
		fixture.install(vectorEnd, "nop");
		fixture.dsp.execInterpreter();
		fixture.dsp.execInterpreter();
		require(!hdi.hostCommandPending() && (hdi.readStatusRegister() & Hcp) == 0
			&& fixture.memory.get(MemArea_Y, 1) == Data && hdi.rxWordsAvailable() == 0,
			"native command did not clear HCP and deliver HORX at actual dispatch");
		require(hdi.setNativeHostCommandsEnabled(false), "native command disable failed");
	}

	void nativeCommandObserverOwnershipIsExclusive()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		int externalOwner = 0;
		require(fixture.dsp.setInterruptServicedCallback(&externalOwner,
			[](TWord, DSP::ExternalInterruptToken) {}),
			"external dispatch observer setup failed");
		require(!hdi.setNativeHostCommandsEnabled(true)
			&& !hdi.nativeHostCommandsEnabled(),
			"native command mode stole another dispatch observer's ownership");
		require(fixture.dsp.clearInterruptServicedCallback(&externalOwner)
			&& hdi.setNativeHostCommandsEnabled(true),
			"native command mode did not acquire released dispatch ownership");
		require(!fixture.dsp.clearInterruptServicedCallback(&externalOwner)
			&& hdi.setNativeHostCommandsEnabled(false),
			"dispatch observer accepted a mismatched release owner");
	}

	void longPredecessorCannotStealLatchedCommandPayload()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord CommandVector = Vba_IRQA;
		constexpr TWord Data = 0x345678;
		fixture.dsp.regs().sr.var &= ~(SR_I0 | SR_I1);
		require(hdi.setNativeHostCommandsEnabled(true), "long-predecessor enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		fixture.dsp.injectExternalInterrupt(Vba_NMI);
		require(hdi.tryWriteRX(Data)
			&& hdi.tryWriteHostCommand(CommandVector) == HDI08::HostCommandAdmission::Accepted,
			"long-predecessor command setup failed");

		fixture.install(Vba_NMI, "jsr $200");
		const auto routineEnd = fixture.install(0x200, "movep x:<<$ffffc6,y:>$9");
		fixture.install(routineEnd, "rti");
		const auto commandEnd = fixture.install(CommandVector, "movep x:<<$ffffc6,y:>$a");
		fixture.install(commandEnd, "nop");
		fixture.install(0x100, "bra $100");
		fixture.dsp.setPC(0x100);
		fixture.dsp.processExternalInterrupts();
		fixture.dsp.execInterpreter();
		require(hdi.hostCommandPending() && hdi.rxWordsAvailable() == 1
			&& fixture.memory.get(MemArea_Y, 9) == 0,
			"older long interrupt consumed the latched host-command payload");
		for(size_t i = 0; i < 16 && hdi.hostCommandPending(); ++i)
			fixture.dsp.execInterpreter();
		require(!hdi.hostCommandPending() && hdi.rxWordsAvailable() == 0
			&& fixture.memory.get(MemArea_Y, 0x0a) == Data,
			"tagged command did not receive its payload after the long predecessor");
	}

	void commandAdmittedInsideLongInterruptReservesItsPayload()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord CommandVector = Vba_IRQA;
		constexpr TWord Data = 0x56789a;
		fixture.dsp.regs().sr.var &= ~(SR_I0 | SR_I1);
		require(hdi.setNativeHostCommandsEnabled(true), "mid-long enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		fixture.dsp.injectExternalInterrupt(Vba_NMI);
		fixture.install(Vba_NMI, "jsr $200");
		fixture.install(0x200, "nop");
		const auto readEnd = fixture.install(0x201, "movep x:<<$ffffc6,y:>$b");
		fixture.install(readEnd, "rti");
		const auto commandEnd = fixture.install(CommandVector, "movep x:<<$ffffc6,y:>$c");
		fixture.install(commandEnd, "nop");
		fixture.install(0x100, "bra $100");
		fixture.dsp.setPC(0x100);
		fixture.dsp.processExternalInterrupts();
		fixture.dsp.execInterpreter();
		require(fixture.dsp.getProcessingMode() == DSP::LongInterrupt,
			"adversarial predecessor did not enter a long interrupt");

		// Model the temporary raised IPL of the in-flight long interrupt; RTI will
		// restore the unmasked SR that its JSR already stacked.
		fixture.dsp.regs().sr.var |= SR_I0 | SR_I1;
		require(hdi.tryWriteRX(Data)
			&& hdi.tryWriteHostCommand(CommandVector) == HDI08::HostCommandAdmission::Accepted,
			"mid-long command admission failed");
		fixture.dsp.execInterpreter();
		require(hdi.hostCommandPending() && hdi.rxWordsAvailable() == 1
			&& fixture.memory.get(MemArea_Y, 0x0b) == 0,
			"long handler stole payload admitted under its temporary IPL");
		for(size_t i = 0; i < 16 && hdi.hostCommandPending(); ++i)
			fixture.dsp.execInterpreter();
		require(!hdi.hostCommandPending() && hdi.rxWordsAvailable() == 0
			&& fixture.memory.get(MemArea_Y, 0x0c) == Data,
			"mid-long tagged command did not consume payload after RTI");
	}

	void nativeCommandRejectsInvalidArchitecturalVectors()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		require(hdi.setNativeHostCommandsEnabled(true), "invalid-vector enable failed");
		require(hdi.tryWriteHostCommand(0x0d) == HDI08::HostCommandAdmission::InvalidVector
			&& hdi.tryWriteHostCommand(Vba_End) == HDI08::HostCommandAdmission::InvalidVector
			&& !hdi.hostCommandPending() && !fixture.dsp.hasPendingExternalInterrupts()
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HCP)) == 0,
			"invalid odd/custom-space vector mutated native command state");
	}

	void unpublishedCommandIgnoresLegacySameVectorDispatch()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord Vector = 0x0c;
		require(hdi.setNativeHostCommandsEnabled(true), "unpublished fixture enable failed");
		require(hdi.tryWriteHostCommand(Vector) == HDI08::HostCommandAdmission::Accepted,
			"HCIE-off unpublished command setup failed");
		fixture.dsp.injectExternalInterrupt(Vector);
		fixture.install(Vector, "nop");
		fixture.install(Vector + 1, "nop");
		fixture.install(0x100, "bra $100");
		fixture.dsp.setPC(0x100);
		fixture.dsp.processExternalInterrupts();
		fixture.dsp.execInterpreter();
		require(hdi.hostCommandPending()
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HCP)) != 0,
			"legacy token-zero same-vector dispatch cleared unpublished HCP");
		require(hdi.cancelPendingHostCommand(), "unpublished fixture did not cancel");
	}

	void maskedCommandDoesNotHoldAndCancellationIsExact()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		require(hdi.setNativeHostCommandsEnabled(true), "masked fixture enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		fixture.dsp.regs().sr.var |= SR_I0 | SR_I1;
		require(hdi.tryWriteRX(0x654321)
			&& hdi.tryWriteHostCommand(Vba_IRQA) == HDI08::HostCommandAdmission::Accepted,
			"masked command setup failed");
		require((hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HRDF)) != 0
			&& hdi.readRX(Move_Nop) == 0x654321,
			"masked command incorrectly held mainline HORX");

		fixture.dsp.processExternalInterrupts();
		fixture.dsp.resetSW();
		require(!hdi.hostCommandPending() && hdi.nativeHostCommandsEnabled(),
			"DSP software reset did not cancel the command or preserve native mode");
		fixture.dsp.regs().sr.var &= ~(SR_I0 | SR_I1);
		const auto vectorEnd = fixture.install(Vba_IRQA, "move #$7,y:$2");
		fixture.install(vectorEnd, "nop");
		fixture.install(0x100, "bra $100");
		fixture.dsp.setPC(0x100);
		for(size_t i = 0; i < 4; ++i)
			fixture.dsp.execInterpreter();
		require(fixture.memory.get(MemArea_Y, 2) == 0,
			"cancelled host-command vector dispatched stale work");

		constexpr TWord NewVector = 0x0c;
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(hdi.tryWriteRX(0x2468ac)
			&& hdi.tryWriteHostCommand(NewVector) == HDI08::HostCommandAdmission::Accepted,
			"native command transport did not survive DSP software reset");
		const auto newVectorEnd = fixture.install(NewVector, "movep x:<<$ffffc6,y:>$4");
		fixture.install(newVectorEnd, "nop");
		fixture.dsp.processExternalInterrupts();
		fixture.dsp.execInterpreter();
		require(!hdi.hostCommandPending() && fixture.memory.get(MemArea_Y, 4) == 0x2468ac,
			"post-reset native command did not dispatch through the surviving observer");
	}

	void hcieTransitionRetriesWithoutLosingHcp()
	{
		if constexpr(!g_jitSupported)
			return;

		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord CommandVector = 0x0c;
		fixture.install(0x100, "wait");
		fixture.install(0x101, "move #$1,r0");
		fixture.install(0x20, "nop");
		fixture.install(0x21, "nop");
		const auto commandEnd = fixture.install(CommandVector, "movep x:<<$ffffc6,y:>$8");
		fixture.install(commandEnd, "nop");
		fixture.dsp.setPC(0x100);
		require(hdi.setNativeHostCommandsEnabled(true), "retry fixture enable failed");
		require(fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Activated,
			"retry fixture paced activation failed");
		const auto waiting = fixture.dsp.executePacedJitSlice(1);
		require(waiting.stop == DSP::PacedJitExecutionStop::Waiting,
			"retry fixture did not park in WAIT");
		for(size_t i = 0; i < 32; ++i)
		{
			DSP::ExternalInterruptToken token = 0;
			require(fixture.dsp.tryInjectExternalInterrupt(0x20, token),
				"retry fixture external interrupt FIFO filled early");
		}
		require(hdi.tryWriteRX(0x112233)
			&& hdi.tryWriteHostCommand(CommandVector) == HDI08::HostCommandAdmission::Accepted,
			"HCIE-off command was rejected by an unrelated full interrupt FIFO");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(hdi.hostCommandPending()
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HCP)) != 0
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HRDF)) == 0,
			"failed HCIE publication lost HCP or exposed mainline HRDF");
		const auto wake = fixture.dsp.executePacedJitSlice(1);
		require(wake.stop == DSP::PacedJitExecutionStop::BudgetExhausted
			&& wake.instructionDelta == 0 && hdi.hostCommandPending(),
			"full-FIFO retry did not wake through an ordinary paced owner boundary");
		for(size_t i = 0; i < 160 && hdi.hostCommandPending(); ++i)
		{
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop != DSP::PacedJitExecutionStop::OwnershipLost
				&& result.stop != DSP::PacedJitExecutionStop::BoundaryViolation,
				"bounded publication retry lost paced ownership");
		}
		require(!hdi.hostCommandPending() && hdi.rxWordsAvailable() == 0
			&& fixture.memory.get(MemArea_Y, 8) == 0x112233,
			"pending HCIE command stranded after external FIFO capacity returned");
		require(fixture.dsp.endPacedJitExecution(), "retry fixture paced teardown failed");
	}

	void hcieDisableUnpublishesButRetainsTheCommand()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		require(hdi.setNativeHostCommandsEnabled(true), "disable fixture enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(hdi.tryWriteRX(0x102030)
			&& hdi.tryWriteHostCommand(0x0c) == HDI08::HostCommandAdmission::Accepted
			&& fixture.dsp.hasPendingExternalInterrupts(),
			"disable fixture command setup failed");
		hdi.writeControlRegister(0);
		require(hdi.hostCommandPending() && !fixture.dsp.hasPendingExternalInterrupts()
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HRDF)) != 0,
			"HCIE disable did not unpublish the command and expose mainline HRDF");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(hdi.hostCommandPending() && fixture.dsp.hasPendingExternalInterrupts()
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HRDF)) == 0,
			"HCIE re-enable did not republish and hold the retained command");
		require(hdi.cancelPendingHostCommand(), "republished disable fixture did not cancel");
	}

	void fullInterruptQueueRejectsWithoutHcp()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		require(hdi.setNativeHostCommandsEnabled(true), "full-queue fixture enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		for(size_t i = 0; i < 32; ++i)
		{
			DSP::ExternalInterruptToken token = 0;
			require(fixture.dsp.tryInjectExternalInterrupt(0x20, token) && token != 0,
				"external interrupt FIFO filled early");
		}
		require(hdi.tryWriteHostCommand(0x0c)
				== HDI08::HostCommandAdmission::InterruptQueueFull
			&& !hdi.hostCommandPending()
			&& (hdi.readStatusRegister() & (TWord{1} << HDI08::HSR_HCP)) == 0,
			"full command admission partially raised HCP");
	}

	void rejectedDataFreeCommandWakesPacedOwnerForExplicitRetry()
	{
		if constexpr(!g_jitSupported)
			return;

		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord CommandVector = 0x0c;
		fixture.install(0x100, "wait");
		fixture.install(0x101, "nop");
		fixture.install(0x20, "nop");
		fixture.install(0x21, "nop");
		fixture.install(CommandVector, "nop");
		fixture.install(CommandVector + 1, "nop");
		fixture.dsp.setPC(0x100);
		require(hdi.setNativeHostCommandsEnabled(true), "data-free retry enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Activated
			&& fixture.dsp.executePacedJitSlice(1).stop == DSP::PacedJitExecutionStop::Waiting,
			"data-free retry fixture did not park in WAIT");
		for(size_t i = 0; i < 32; ++i)
		{
			DSP::ExternalInterruptToken token = 0;
			require(fixture.dsp.tryInjectExternalInterrupt(0x20, token),
				"data-free retry FIFO filled early");
		}
		require(hdi.tryWriteHostCommand(CommandVector)
				== HDI08::HostCommandAdmission::InterruptQueueFull
			&& !hdi.hostCommandPending(),
			"full data-free command admission partially mutated HCP");
		const auto wake = fixture.dsp.executePacedJitSlice(1);
		require(wake.stop == DSP::PacedJitExecutionStop::BudgetExhausted
			&& wake.instructionDelta == 0 && !fixture.dsp.hasPendingExternalInterrupts(),
			"rejected data-free command did not wake/drain through an owner boundary");
		require(hdi.tryWriteHostCommand(CommandVector) == HDI08::HostCommandAdmission::Accepted,
			"explicit data-free command retry did not use returned FIFO capacity");
		for(size_t i = 0; i < 160 && hdi.hostCommandPending(); ++i)
		{
			const auto result = fixture.dsp.executePacedJitSlice(1);
			require(result.stop != DSP::PacedJitExecutionStop::OwnershipLost
				&& result.stop != DSP::PacedJitExecutionStop::BoundaryViolation,
				"explicit data-free retry lost paced ownership");
		}
		require(!hdi.hostCommandPending(),
			"accepted data-free retry did not reach exact tagged dispatch");
		require(fixture.dsp.endPacedJitExecution(), "data-free retry teardown failed");
	}

	void saturatedCoreRetainsExternalCommandForLaterService()
	{
		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		for(size_t i = 0; i < 1024; ++i)
			require(fixture.dsp.injectInterrupt(Vba_IRQA),
				"core pending interrupt FIFO filled early");
		require(hdi.setNativeHostCommandsEnabled(true), "saturated-core enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(hdi.tryWriteHostCommand(0x0c) == HDI08::HostCommandAdmission::Accepted,
			"saturated core rejected available external admission");
		fixture.dsp.processExternalInterrupts();
		require(hdi.hostCommandPending() && fixture.dsp.hasPendingExternalInterrupts(),
			"saturated core consumed or lost the external host command");
		fixture.dsp.regs().sr.var &= ~(SR_I0 | SR_I1);
		fixture.install(Vba_IRQA, "nop");
		fixture.install(Vba_IRQA + 1, "nop");
		fixture.dsp.execInterpreter();
		fixture.dsp.processExternalInterrupts();
		require(hdi.hostCommandPending() && !fixture.dsp.hasPendingExternalInterrupts(),
			"staged external command did not resume after core capacity returned");
		require(hdi.cancelPendingHostCommand(),
			"resumed saturated-core command did not cancel");
	}

	void nativeCommandWakesPacedWaitAtItsDispatchBoundary()
	{
		if constexpr(!g_jitSupported)
			return;

		Fixture fixture;
		auto& hdi = fixture.peripherals.getHI08();
		constexpr TWord Vector = 0x0c;
		fixture.install(0x100, "wait");
		fixture.install(0x101, "move #$1,r0");
		const auto vectorEnd = fixture.install(Vector, "movep x:<<$ffffc6,y:>$3");
		fixture.install(vectorEnd, "nop");
		fixture.dsp.setPC(0x100);
		require(hdi.setNativeHostCommandsEnabled(true), "paced-WAIT native enable failed");
		hdi.writeControlRegister(TWord{1} << HDI08::HCR_HCIE);
		require(fixture.dsp.beginPacedJitExecution() == DSP::PacedJitActivation::Activated,
			"paced-WAIT activation failed");
		const auto waiting = fixture.dsp.executePacedJitSlice(1);
		require(waiting.stop == DSP::PacedJitExecutionStop::Waiting
			&& fixture.dsp.regs().r[0].toWord() == 0,
			"paced WAIT did not park before host-command admission");
		require(hdi.tryWriteRX(0x456789)
			&& hdi.tryWriteHostCommand(Vector) == HDI08::HostCommandAdmission::Accepted,
			"paced-WAIT host command setup failed");
		const auto wake = fixture.dsp.executePacedJitSlice(1);
		require(wake.stop == DSP::PacedJitExecutionStop::BudgetExhausted
			&& wake.instructionDelta == 0 && hdi.hostCommandPending(),
			"host command did not expose the parked paced-WAIT wake boundary");
		const auto vector = fixture.dsp.executePacedJitSlice(1);
		require((vector.stop == DSP::PacedJitExecutionStop::BudgetExhausted
				|| vector.stop == DSP::PacedJitExecutionStop::AtomicBudgetOverrun)
			&& vector.instructionDelta >= 1
			&& !hdi.hostCommandPending() && hdi.rxWordsAvailable() == 0
			&& fixture.memory.get(MemArea_Y, 3) == 0x456789
			&& fixture.dsp.regs().r[0].toWord() == 0,
			"host command did not dispatch/deliver at the paced vector boundary");
		require(fixture.dsp.endPacedJitExecution(), "paced-WAIT teardown failed");
	}

}

int main()
{
	try
	{
		verifyNonblockingQueues<dsp56k::Peripherals56303>();
		verifyNonblockingQueues<dsp56k::Peripherals56362>();
		txRetentionIsExplicitAndBounded();
		serializedOwnerControlsNativeCommandDispatchOrdering();
		nativeCommandObserverOwnershipIsExclusive();
		longPredecessorCannotStealLatchedCommandPayload();
		commandAdmittedInsideLongInterruptReservesItsPayload();
		nativeCommandRejectsInvalidArchitecturalVectors();
		unpublishedCommandIgnoresLegacySameVectorDispatch();
		maskedCommandDoesNotHoldAndCancellationIsExact();
		hcieTransitionRetriesWithoutLosingHcp();
		hcieDisableUnpublishesButRetainsTheCommand();
		fullInterruptQueueRejectsWithoutHcp();
		rejectedDataFreeCommandWakesPacedOwnerForExplicitRetry();
		saturatedCoreRetainsExternalCommandForLaterService();
		nativeCommandWakesPacedWaitAtItsDispatchBoundary();
		std::puts("DSP56300 HI08 runtime transport tests passed");
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "DSP56300 HI08 runtime transport test failed: %s\n", _error.what());
		return 1;
	}
}
