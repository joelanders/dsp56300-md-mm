#include "gpio.h"
#include "essi.h"
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

	void portModelsInputOutputAndEssiSelection()
	{
		EssiPort port;
		port.hostWrite(0x15);
		require(port.dspRead() == 0x15, "latched host GPIO input was not readable");

		TWord externalPins = 0x2a;
		port.setHostInputSource([&] { return externalPins; });

		require(port.getControl() == 0, "ESSI port control did not reset to GPIO mode");
		require(port.getDirection() == 0, "ESSI port direction did not reset to input");
		require(port.dspRead() == externalPins, "GPIO inputs did not reflect the external pin source");

		port.setDirection(0x09);
		port.dspWrite(0x3f);
		require(port.dspRead() == 0x2b, "GPIO data read did not combine input pins and output latches");
		require(port.hostRead() == 0x09, "host-side GPIO read did not expose output pins");

		port.setControl(0x01);
		require(port.getControl() == 0x01, "ESSI control register was not retained");
		require(port.dspRead() == 0x2a, "ESSI-selected pin remained visible through GPIO data");
		require(port.hostRead() == 0x08, "ESSI-selected pin remained visible as a GPIO output");

		port.setControl(0xffffff);
		port.setDirection(0xffffff);
		require(port.getControl() == EssiPort::PinMask, "reserved control bits did not read as zero");
		require(port.getDirection() == EssiPort::PinMask, "reserved direction bits did not read as zero");
		require(port.dspRead() == 0, "all ESSI-selected pins remained visible as GPIO");

		port.reset();
		require(port.getControl() == 0 && port.getDirection() == 0,
			"port reset did not clear PCR and PRR");
		port.setDirection(0x01);
		require((port.dspRead() & 0x01) == 0, "port reset did not clear the PDR output latch");
		externalPins = 0xffffff;
		port.setDirection(0);
		require(port.dspRead() == EssiPort::PinMask, "reserved data bits did not read as zero");
	}

	void peripheralRegistersDispatchAndResetIndependently()
	{
		Peripherals56303 peripherals;
		TWord portCInput = 0x15;
		TWord portDInput = 0x2a;
		peripherals.getPortC().setHostInputSource([&] { return portCInput; });
		peripherals.getPortD().setHostInputSource([&] { return portDInput; });

		require(peripherals.read(Essi::ESSI_PDRC, Nop) == portCInput,
			"Port C data register did not dispatch to its GPIO pins");
		require(peripherals.read(Essi::ESSI_PDRD, Nop) == portDInput,
			"Port D data register did not dispatch to its GPIO pins");

		peripherals.write(Essi::ESSI_PRRC, 0x03);
		peripherals.write(Essi::ESSI_PDRC, 0x01);
		peripherals.write(Essi::ESSI_PCRC, 0x02);
		require(peripherals.read(Essi::ESSI_PRRC, Nop) == 0x03,
			"Port C direction register was not retained");
		require(peripherals.read(Essi::ESSI_PCRC, Nop) == 0x02,
			"Port C control register was not retained");
		require(peripherals.read(Essi::ESSI_PDRC, Nop) == 0x15,
			"Port C data register returned the wrong input/output pin mixture");
		require(peripherals.read(Essi::ESSI_PRRD, Nop) == 0
			&& peripherals.read(Essi::ESSI_PCRD, Nop) == 0
			&& peripherals.read(Essi::ESSI_PDRD, Nop) == portDInput,
			"Port C register writes affected Port D");

		peripherals.write(Essi::ESSI_PRRD, 0xffffff);
		peripherals.write(Essi::ESSI_PCRD, 0xffffff);
		require(peripherals.read(Essi::ESSI_PRRD, Nop) == EssiPort::PinMask
			&& peripherals.read(Essi::ESSI_PCRD, Nop) == EssiPort::PinMask,
			"Port D exposed reserved register bits");

		peripherals.reset();
		require(peripherals.read(Essi::ESSI_PRRC, Nop) == 0
			&& peripherals.read(Essi::ESSI_PCRC, Nop) == 0
			&& peripherals.read(Essi::ESSI_PRRD, Nop) == 0
			&& peripherals.read(Essi::ESSI_PCRD, Nop) == 0,
			"peripheral reset did not restore both ESSI GPIO ports");
		peripherals.write(Essi::ESSI_PRRC, 0x01);
		require((peripherals.read(Essi::ESSI_PDRC, Nop) & 0x01) == 0,
			"peripheral reset did not clear the Port C output latch");
		require(peripherals.read(Essi::ESSI_PDRD, Nop) == portDInput,
			"peripheral reset discarded external Port D wiring");
	}
}

int main()
{
	try
	{
		portModelsInputOutputAndEssiSelection();
		peripheralRegistersDispatchAndResetIndependently();
		std::puts("DSP56303 ESSI GPIO tests passed");
		return 0;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "DSP56303 ESSI GPIO test failed: %s\n", e.what());
		return 1;
	}
}
