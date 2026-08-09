#include "assembler.h"
#include "dsp.h"
#include "esaiclock.h"
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

	void maskedInterruptKeepsPeripheralsAdvancing()
	{
		DefaultMemoryValidator validator;
		Memory memory{validator, 0x1000};
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp{memory, &peripherals, &noPeripherals};
		SerialProbe serial;
		Assembler assembler;
		const auto nop = assembler.assemble("nop");
		require(nop.success() && nop.wordCount == 1, "failed to assemble focused NOP");
		for(TWord pc = 0; pc < 4; ++pc)
			require(dsp.memWriteP(pc, nop.word[0]), "failed to install focused NOP");

		peripherals.getEssiClock().setCyclesPerSample(1);
		peripherals.getEssiClock().setEsaiDivider(&serial, 0);

		// Level-two DMA remains pending while the core mask is level three.
		dsp.regs().sr.var |= SR_I0 | SR_I1;
		require(dsp.injectInterrupt(Vba_DMAchannel0), "failed to queue focused DMA interrupt");
		require(dsp.getProcessingMode() == DSP::Default,
			"queuing a masked interrupt changed processing mode");

		// The pending request remains installed at ordinary instruction
		// boundaries. The first NOP reaches the serial deadline; the second
		// boundary must run the clock before executing its NOP.
		dsp.execInterpreter();
		require(serial.transmitTicks == 0 && serial.receiveTicks == 0,
			"peripheral clock advanced before its first deadline");
		dsp.execInterpreter();
		require(serial.transmitTicks == 1 && serial.receiveTicks == 1,
			"masked interrupt stalled the peripheral clock");
		require(dsp.getPC().toWord() == 2,
			"masked interrupt prevented ordinary instruction execution");
		require(dsp.getProcessingMode() == DSP::Default,
			"masked interrupt was serviced instead of remaining queued");

		// The request remains pending, but each later deadline must still run.
		dsp.execInterpreter();
		require(serial.transmitTicks == 2 && serial.receiveTicks == 2,
			"masked interrupt did not recheck peripherals on a later boundary");
		require(dsp.getPC().toWord() == 3,
			"rechecked masked interrupt prevented instruction execution");
		require(dsp.getProcessingMode() == DSP::Default,
			"rechecked masked interrupt changed processing mode");
	}
}

int main()
{
	try
	{
		maskedInterruptKeepsPeripheralsAdvancing();
		std::puts("DSP56300 masked-interrupt peripheral tests passed");
		return 0;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "DSP56300 masked-interrupt peripheral test failed: %s\n", e.what());
		return 1;
	}
}
