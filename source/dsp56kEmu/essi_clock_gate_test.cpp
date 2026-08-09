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
		essi.terminate();
		require(essi.clockTickEnabled() && gateCalls == 2,
			"ESSI termination retained a callback into external wiring");
	}
}

int main()
{
	try
	{
		baseClockGateControlsOneWholeSerialTick();
		fineClockGateControlsOneWholeSerialTick();
		essiGateLifecyclePreservesResetAndDetachesOnTerminate();
		std::puts("DSP56303 ESSI clock-gate tests passed");
		return 0;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "DSP56303 ESSI clock-gate test failed: %s\n", e.what());
		return 1;
	}
}
