#include "assembler.h"
#include "dsp.h"
#include "esaiclock.h"
#include "essi.h"
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

	class SerialProbe final : public Esxi
	{
	public:
		void execTX() override { ++transmitTicks; }
		void execRX() override { ++receiveTicks; }
		TWord hasEnabledTransmitters() const override { return 1; }
		TWord hasEnabledReceivers() const override { return 1; }
		bool clockTickEnabled() const override
		{
			++gateCalls;
			return gateOpen;
		}

		bool gateOpen = false;
		mutable unsigned gateCalls = 0;
		unsigned transmitTicks = 0;
		unsigned receiveTicks = 0;
	};

	struct ClockFixture
	{
		ClockFixture()
			: memory(validator, 0x1000)
			, dsp(memory, &peripheralsX, &peripheralsY)
			, clock(peripheralsX)
		{
			const auto nop = assembler.assemble("nop");
			require(nop.success() && nop.wordCount == 1, "failed to assemble clock-test NOP");
			for(TWord pc = 0; pc < 8; ++pc)
				require(dsp.memWriteP(pc, nop.word[0]), "failed to install clock-test NOP");
			clock.setDSP(&dsp);
			clock.setCyclesPerSample(1);
		}

		void tick()
		{
			dsp.execInterpreter();
			clock.exec();
		}

		DefaultMemoryValidator validator;
		Memory memory;
		PeripheralsNop peripheralsX;
		PeripheralsNop peripheralsY;
		DSP dsp;
		EsxiClock clock;
		Assembler assembler;
	};

	struct EssiFixture
	{
		EssiFixture()
			: memory(validator, 0x1000)
			, dsp(memory, &peripherals, &noPeripherals)
		{
			peripherals.getEssiClock().setCyclesPerSample(1);
		}

		void advanceClock(const uint32_t _instructions = 1)
		{
			dsp.fastForward(_instructions, _instructions);
			peripherals.getEssiClock().exec();
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
	};

	constexpr TWord transmit0 = TWord(1) << Essi::RegCRBbits::CRB_TE0;
	constexpr TWord receive = TWord(1) << Essi::RegCRBbits::CRB_RE;

	void fillOutputRing(Audio& _audio)
	{
		Audio::TxFrame empty;
		auto& outputs = _audio.getAudioOutputs();
		while(!outputs.full())
			outputs.push_back(empty);
	}

	void baseClockGateControlsOneWholeSerialTick()
	{
		ClockFixture fixture;
		SerialProbe serial;
		fixture.clock.setEsaiDivider(&serial, 1);

		fixture.tick();
		require(serial.gateCalls == 1, "closed base clock gate was not polled exactly once");
		require(serial.transmitTicks == 0 && serial.receiveTicks == 0,
			"closed base clock gate advanced part of a serial slot");

		serial.gateOpen = true;
		fixture.tick();
		require(serial.gateCalls == 2, "open base clock gate was not polled once for the slot");
		require(serial.transmitTicks == 0 && serial.receiveTicks == 0,
			"closed clock time incorrectly advanced the serial divider");

		fixture.tick();
		require(serial.gateCalls == 3, "base clock gate poll count drifted");
		require(serial.transmitTicks == 1 && serial.receiveTicks == 1,
			"open base clock did not advance TX and RX atomically");
	}

	void fineClockGateControlsOneWholeSerialTick()
	{
		ClockFixture fixture;
		SerialProbe serial;
		fixture.clock.setEsaiFinePeriod(&serial, 1);

		fixture.tick();
		require(serial.gateCalls == 1 && serial.transmitTicks == 0 && serial.receiveTicks == 0,
			"closed fine clock gate advanced a serial slot");

		serial.gateOpen = true;
		fixture.tick();
		require(serial.gateCalls == 2, "fine clock gate was polled more than once per slot");
		require(serial.transmitTicks == 1 && serial.receiveTicks == 1,
			"open fine clock did not advance TX and RX atomically");
	}

	void essiGateLifecyclePreservesResetAndDetachesOnTerminate()
	{
		Peripherals56303 peripherals;
		auto& essi = peripherals.getEssi0();
		unsigned gateCalls = 0;
		essi.setClockGate([&]
		{
			++gateCalls;
			return false;
		});

		require(!essi.clockTickEnabled() && gateCalls == 1,
			"ESSI did not consult its installed clock gate");
		essi.reset();
		require(!essi.clockTickEnabled() && gateCalls == 2,
			"ESSI reset discarded external clock wiring");
		fillOutputRing(essi);
		require(!essi.canAcceptTxFrame(), "full default ring did not close admission");
		essi.terminate();
		require(essi.clockTickEnabled() && gateCalls == 2,
			"ESSI termination retained a callback into external wiring");
		require(essi.canAcceptTxFrame(), "terminated ESSI retained blocking ring admission");
	}

	void defaultRingHoldsOnlyTheCompletingTransmitSlot()
	{
		EssiFixture fixture;
		auto& essi = fixture.peripherals.getEssi0();
		auto& outputs = essi.getAudioOutputs();
		unsigned callbacks = 0;
		essi.setCallback([&](Audio*) { ++callbacks; });
		essi.writeCRA(TWord(1) << Essi::RegCRAbits::CRA_DC0); // two slots per frame
		fillOutputRing(essi);
		require(!essi.canAcceptTxFrame(), "full default TX ring still reported capacity");

		essi.writeTX(0, 0x123456);
		essi.writeCRB(transmit0); // direct priming is the first, non-completing slot
		require(outputs.full(), "intermediate TX slot published a frame");

		essi.writeTX(0, 0x654321);
		const auto heldStatus = essi.readSR();
		essi.execTX();
		require(outputs.full(), "blocked completing slot changed TX ring occupancy");
		require(essi.readSR() == heldStatus, "blocked completing slot changed ESSI status");
		require(callbacks == 0, "blocked completing slot invoked the output callback");
		essi.reset();
		require(outputs.full() && !essi.canAcceptTxFrame(),
			"ESSI reset discarded queued output or changed admission state");

		outputs.pop_front();
		require(essi.canAcceptTxFrame(), "draining one TX frame did not release admission");
		essi.execTX();
		require(outputs.full(), "released completing slot did not publish exactly one frame");
		require(callbacks == 1, "released completing slot did not invoke one output callback");
		const auto& completed = outputs[outputs.size() - 1];
		require(completed.size() == 2, "released TX frame has the wrong slot count");
		require(completed[0][0] == 0x123456 && completed[1][0] == 0x654321,
			"held TX frame lost or reordered its slots");
	}

	void baseClockCapacityGateIsAtomicAcrossTransmitAndReceive()
	{
		EssiFixture fixture;
		auto& essi = fixture.peripherals.getEssi0();
		auto& outputs = essi.getAudioOutputs();
		unsigned reads = 0;
		unsigned writes = 0;
		essi.setCallback([&](Audio*) { ++writes; });
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			++reads;
			_frame.resize(1);
			_frame[0].fill(0);
		});
		fillOutputRing(essi);
		essi.writeTX(0, 0x112233);
		essi.writeCRB(transmit0 | receive);

		fixture.advanceClock();
		require(outputs.full() && reads == 0 && writes == 0,
			"full TX ring advanced part of a base-clock serial slot");

		outputs.pop_front();
		fixture.advanceClock();
		require(outputs.full() && reads == 1 && writes == 1,
			"released base-clock slot did not advance TX and RX exactly once");
		const auto& completed = outputs[outputs.size() - 1];
		require(completed.size() == 1 && completed[0][0] == 0x112233,
			"base-clock release published the wrong TX frame");

		fixture.advanceClock();
		require(reads == 1, "full ring did not hold the following base-clock RX slot");
	}

	void fineClockCapacityGateAdvancesNoSerialStateWhileBlocked()
	{
		EssiFixture fixture;
		auto& clock = fixture.peripherals.getEssiClock();
		auto& essi = fixture.peripherals.getEssi0();
		auto& outputs = essi.getAudioOutputs();
		unsigned reads = 0;
		unsigned writes = 0;
		essi.setCallback([&](Audio*) { ++writes; });
		clock.setCyclesPerSample(200);
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			++reads;
			_frame.resize(1);
			_frame[0].fill(0);
		});
		fillOutputRing(essi);
		essi.writeTX(0, 0x445566);
		essi.writeCRB(transmit0 | receive);
		clock.setEsaiFinePeriod(&essi, 1);

		fixture.advanceClock();
		require(outputs.full() && reads == 0 && writes == 0,
			"full TX ring advanced part of a fine-clock serial slot");

		outputs.pop_front();
		fixture.advanceClock();
		require(outputs.full() && reads == 1 && writes == 1,
			"released fine-clock slot did not advance TX and RX exactly once");
		fixture.advanceClock();
		require(reads == 1, "fine clock fabricated catch-up serial work while blocked");
	}

	void customTransmitSinkOwnsItsAdmissionContract()
	{
		EssiFixture fixture;
		auto& essi = fixture.peripherals.getEssi0();
		fillOutputRing(essi);
		unsigned writes = 0;
		essi.setWriteTxCallback([&](uint64_t&, const Audio::TxFrame& _frame)
		{
			++writes;
			require(_frame.size() == 1, "custom sink received a malformed TX frame");
		});
		require(essi.canAcceptTxFrame(), "stale default-ring occupancy blocked a custom sink");
		essi.writeTX(0, 0xabcdef);
		essi.writeCRB(transmit0);
		require(writes == 1, "custom TX sink did not receive direct priming output");

		essi.reset();
		essi.writeCRB(0);
		essi.writeTX(0, 0x123456);
		essi.writeCRB(transmit0);
		require(writes == 2, "ESSI reset discarded the custom TX sink");
		essi.terminate();
		require(essi.canAcceptTxFrame(), "terminated audio retained a blocking admission state");
	}
}

int main()
{
	try
	{
		baseClockGateControlsOneWholeSerialTick();
		fineClockGateControlsOneWholeSerialTick();
		essiGateLifecyclePreservesResetAndDetachesOnTerminate();
		defaultRingHoldsOnlyTheCompletingTransmitSlot();
		baseClockCapacityGateIsAtomicAcrossTransmitAndReceive();
		fineClockCapacityGateAdvancesNoSerialStateWhileBlocked();
		customTransmitSinkOwnsItsAdmissionContract();
		std::puts("DSP56303 ESSI clock-gate tests passed");
		return 0;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "DSP56303 ESSI clock-gate test failed: %s\n", e.what());
		return 1;
	}
}
