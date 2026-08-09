#include "dsp.h"
#include "essi.h"
#include "memory.h"
#include "peripherals.h"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace dsp56k;

	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	struct Fixture
	{
		DefaultMemoryValidator validator;
		Memory memory{validator, 0x1000};
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp{memory, &peripherals, &noPeripherals};

		Essi& essi() { return peripherals.getEssi0(); }
	};

	constexpr TWord fineCra = 2u
		| (TWord(1) << Essi::RegCRAbits::CRA_PSR)
		| (TWord(2) << Essi::RegCRAbits::CRA_WL0);
	constexpr TWord receiveEnabled = TWord(1) << Essi::RegCRBbits::CRB_RE;

	void testCallbacksAndEmptyTransport()
	{
		Fixture fixture;
		auto& essi = fixture.essi();
		bool available = false;
		std::vector<char> events;

		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			events.push_back('r');
			_frame.resize(1);
			_frame[0].fill(0);
			_frame[0][0] = 0x123456;
		});
		essi.setRxDataAvailableCallback([&]
		{
			events.push_back('a');
			return available;
		});
		essi.setRxConsumeCallback([&]
		{
			require(!essi.getSR().test(Essi::RegSSISRbits::SSISR_RDF), "consume callback ran before RDF cleared");
			events.push_back('c');
		});

		essi.setFineLinkMode(true);
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);

		available = true;
		essi.execRX();
		require((events == std::vector<char>{'a', 'r'}), "availability was not checked before RX delivery");
		require(essi.readRX() == 0x123456, "RX value was not retained until consumption");
		require((events == std::vector<char>{'a', 'r', 'c'}), "consume callback ordering is incorrect");

		available = false;
		essi.execRX();
		require(events.size() == 4 && events.back() == 'a', "empty fast transport fabricated an RX word");
	}

	void testResetAndDisableSemantics()
	{
		Fixture fixture;
		auto& essi = fixture.essi();
		uint32_t reads = 0;
		bool available = true;
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			++reads;
			_frame.resize(1);
			_frame[0].fill(0);
		});
		essi.setRxDataAvailableCallback([&] { return available; });
		essi.setFineLinkMode(true);
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);
		essi.execRX();
		require(reads == 1, "fine transport did not deliver its first word");

		essi.reset();
		available = false;
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);
		essi.execRX();
		require(reads == 2, "reset discarded host transport wiring");
		essi.execRX();
		require(reads == 2, "reset did not restore empty fast-link suppression after bootstrap");

		essi.setFineLinkMode(false);
		essi.execRX();
		require(reads == 3, "disabling fine mode left fast empty-RX suppression active");

		essi.setFineLinkMode(true);
		essi.writeCRA(fineCra);
		essi.execRX();
		require(reads == 4, "re-enabled fine mode did not restart its initial receive phase");
	}

	void testFineClockCatchupIsBounded()
	{
		Fixture fixture;
		auto& clock = fixture.peripherals.getEssiClock();
		auto& essi = fixture.essi();
		uint32_t reads = 0;
		clock.setCyclesPerSample(200);
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			++reads;
			_frame.resize(1);
			_frame[0].fill(0);
		});
		essi.setRxDataAvailableCallback([] { return true; });
		essi.setFineLinkMode(true);
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);

		constexpr uint32_t maxTicks = 64;
		fixture.dsp.fastForward(96 * (maxTicks + 3), 96 * (maxTicks + 3));
		require(clock.exec() == 0, "overdue fine clock did not request immediate follow-up service");
		require(reads == maxTicks, "fine clock catch-up exceeded its per-service bound");
		clock.exec();
		require(reads == maxTicks + 3, "fine clock catch-up did not make deferred progress");
	}

	void testDefaultModeRemainsUngated()
	{
		Fixture fixture;
		auto& essi = fixture.peripherals.getEssi1();
		uint32_t reads = 0;
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			++reads;
			_frame.resize(1);
			_frame[0].fill(0);
		});
		essi.setRxDataAvailableCallback([] { return false; });
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);
		essi.execRX();
		essi.execRX();
		require(reads == 2, "default ESSI RX behavior changed without fine mode");
	}

	void testTerminationDetachesTransport()
	{
		Fixture fixture;
		auto& essi = fixture.essi();
		uint32_t availabilityChecks = 0;
		uint32_t reads = 0;
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame&) { ++reads; });
		essi.setRxDataAvailableCallback([&] { ++availabilityChecks; return true; });
		essi.setFineLinkMode(true);
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);
		essi.terminate();
		essi.execRX();
		require(availabilityChecks == 0 && reads == 0, "terminated ESSI retained a transport callback");
	}

	void testFineClockTicks()
	{
		Fixture fixture;
		auto& clock = fixture.peripherals.getEssiClock();
		auto& essi = fixture.essi();
		uint32_t reads = 0;
		clock.setCyclesPerSample(200);
		essi.setReadRxCallback([&](uint64_t&, Audio::RxFrame& _frame)
		{
			++reads;
			_frame.resize(1);
			_frame[0].fill(0);
		});
		essi.setRxDataAvailableCallback([] { return true; });
		essi.setFineLinkMode(true);
		essi.writeCRA(fineCra);
		essi.writeCRB(receiveEnabled);

		for(uint32_t i = 0; i < 96; ++i)
			fixture.dsp.execInterpreter();
		require(fixture.dsp.getInstructionCounter() == 96, "interpreter did not advance the expected number of instructions");
		require(reads == 0, "fine clock ticked before the CRA-derived period");
		fixture.dsp.execInterpreter();
		require(fixture.dsp.getInstructionCounter() == 97, "interpreter did not cross the fine-clock service boundary");
		require(reads == 1, "fine clock did not tick at the CRA-derived period");
	}
}

int main()
{
	try
	{
		testCallbacksAndEmptyTransport();
		testResetAndDisableSemantics();
		testDefaultModeRemainsUngated();
		testTerminationDetachesTransport();
		testFineClockTicks();
		testFineClockCatchupIsBounded();
	}
	catch(const std::exception& error)
	{
		std::fputs(error.what(), stderr);
		return 1;
	}
	return 0;
}
