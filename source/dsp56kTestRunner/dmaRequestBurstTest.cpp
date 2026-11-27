#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/externalbusdevice.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

// Dma::execRequests and Dma::previewRequests against the same transfer served one request at a time through
// the serial port, and EsxiClock's one-shot cycle alarm.

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
		Memory memory{validator, 0x10000};
		Peripherals56303 x;
		PeripheralsNop y;
		DSP dsp{memory, &x, &y};
	};

	constexpr TWord g_channel = 4;
	constexpr TWord g_destination = 0x400;

	std::vector<TWord> makeWords(const uint32_t _count)
	{
		std::vector<TWord> words;
		for(uint32_t i = 0; i < _count; ++i)
			words.push_back(0x100000 + i * 0x010101);
		return words;
	}

	// A receive channel on ESSI0: one word per request. Transfer mode 1 clears the enable bit at the end of the block,
	// mode 5 keeps it
	TWord receiveControl(const TWord _destinationMode, const bool _interrupt, const TWord _transferMode = 1)
	{
		return (4u << DmaChannel::Dam0) | (_destinationMode << DmaChannel::Dam3)
			| (static_cast<TWord>(DmaChannel::RequestSource::Essi0ReceiveData) << DmaChannel::Drs0)
			| (_transferMode << DmaChannel::Dtm0) | (_interrupt ? (1u << DmaChannel::Die) : 0u) | (1u << DmaChannel::De);
	}

	void armReceive(Fixture& _f, const TWord _destinationMode, const TWord _counter, const bool _interrupt, const TWord _transferMode = 1)
	{
		auto& rx = _f.x.getEssi0();
		auto& dma = _f.x.getDMA();
		rx.writeCRA(3u << Essi::CRA_WL0);
		rx.writeCRB((1u << Essi::CRB_RE) | (1u << Essi::CRB_MOD));
		dma.setDOR(2, 0x21);		// two-dimensional destination: lines 0x40 apart for a line of 0x20 words
		dma.setDSR(g_channel, Essi::ESSI0_RX);
		dma.setDDR(g_channel, g_destination);
		dma.setDCO(g_channel, _counter);
		dma.setDCR(g_channel, receiveControl(_destinationMode, _interrupt, _transferMode));
	}

	std::vector<TWord> dump(const Fixture& _f)
	{
		std::vector<TWord> result;
		for(TWord a = g_destination; a < g_destination + 0x200; ++a)
			result.push_back(_f.memory.get(MemArea_X, a));
		return result;
	}

	// _destinationMode 2: two-dimensional with DOR2, _counter is lines-1 and words-1. 5: post-increment
	void receive(const TWord _destinationMode, const TWord _counter, const uint32_t _count)
	{
		// five more than the block: the calls below that ask for more than the block read no further than this
		const auto words = makeWords(_count + 5);

		// the reference: every word arrives through the receiver and raises its own request
		Fixture reference;
		armReceive(reference, _destinationMode, _counter, false);
		{
			auto& rx = reference.x.getEssi0();
			uint32_t next = 0;
			rx.setReadRxCallback([&](uint64_t& _index, Audio::RxFrame& _frame)
			{
				_frame.resize(1);
				_frame[0] = Audio::RxSlot{words[next++]};
				++_index;
			});
			for(uint32_t i = 0; i < _count; ++i)
				rx.execRX();
			require(next == _count, "the reference did not take every word");
		}
		auto& referenceDma = reference.x.getDMA();
		require(!(referenceDma.getDCR(g_channel) & (1u << DmaChannel::De)), "the reference transfer did not end");

		Fixture f;
		armReceive(f, _destinationMode, _counter, true);
		auto& dma = f.x.getDMA();
		const auto ddr = dma.getDDR(g_channel), dco = dma.getDCO(g_channel), dcr = dma.getDCR(g_channel);

		// preview: the words are where the channel is going to put them, the channel has not moved
		require(dma.previewRequests(g_channel, _count, words.data()) == _count, "preview did not store every word");
		require(dump(f) == dump(reference), "preview stored the words at other addresses than the transfer");
		require(dma.getDDR(g_channel) == ddr && dma.getDCO(g_channel) == dco && dma.getDCR(g_channel) == dcr,
			"preview changed the channel");
		require(!f.dsp.hasPendingInterrupts(), "preview raised an interrupt");

		// a preview of more words than the block holds stops at its end
		require(dma.previewRequests(g_channel, _count + 5, words.data()) == _count, "preview ran past the end of the block");
		require(dma.getDDR(g_channel) == ddr && dma.getDCO(g_channel) == dco, "preview past the end changed the channel");

		// the transfer itself: the same memory and the same channel as after the requests one by one
		require(dma.execRequests(g_channel, _count + 5, words.data()) == _count, "the burst did not stop at the end of the block");
		require(dump(f) == dump(reference), "the burst stored other words than the requests one by one");
		require(dma.getDDR(g_channel) == referenceDma.getDDR(g_channel), "destination address differs after the burst");
		require(dma.getDCO(g_channel) == referenceDma.getDCO(g_channel), "counter differs after the burst");
		require(!(dma.getDCR(g_channel) & (1u << DmaChannel::De)), "the burst did not clear the enable bit");
		require(f.dsp.hasPendingInterrupts(), "the burst did not raise the channel's interrupt");

		// nothing more is served once the channel is off
		require(dma.execRequests(g_channel, 4, words.data()) == 0, "a disabled channel served a request");
		require(dma.previewRequests(g_channel, 4, words.data()) == 0, "a disabled channel stored a preview");
	}

	// A channel that moves a line or a block per request, or one in a reserved mode, is not served: one call would be
	// more than one word, or the transfer is not defined
	void refused(const TWord _transferMode, const TWord _sourceMode, const TWord _destinationMode, const bool _threeD = false)
	{
		Fixture f;
		auto& rx = f.x.getEssi0();
		auto& dma = f.x.getDMA();
		rx.writeCRA(3u << Essi::CRA_WL0);
		rx.writeCRB((1u << Essi::CRB_RE) | (1u << Essi::CRB_MOD));
		dma.setDOR(2, 0x21);
		dma.setDSR(g_channel, _sourceMode == 4 ? static_cast<TWord>(Essi::ESSI0_RX) : 0x600);
		dma.setDDR(g_channel, g_destination);
		dma.setDCO(g_channel, (3u << 12) | 31u);
		dma.setDCR(g_channel, (_sourceMode << DmaChannel::Dam0) | (_destinationMode << DmaChannel::Dam3)
			| (static_cast<TWord>(DmaChannel::RequestSource::Essi0ReceiveData) << DmaChannel::Drs0)
			| (_transferMode << DmaChannel::Dtm0) | (1u << DmaChannel::Die) | (1u << DmaChannel::De)
			| (_threeD ? (1u << DmaChannel::D3d) : 0u));
		require(!dma.servesRequests(g_channel), "a channel that moves more than a word per request, or in a reserved mode, is reported as served");

		// eight words only: a line would be 32
		const auto words = makeWords(8);
		const auto before = dump(f);
		const auto dsr = dma.getDSR(g_channel), ddr = dma.getDDR(g_channel), dco = dma.getDCO(g_channel), dcr = dma.getDCR(g_channel);
		require(dma.previewRequests(g_channel, 8, words.data()) == 0, "a preview was stored on a channel that is not served");
		require(dma.execRequests(g_channel, 8, words.data()) == 0, "a burst was served on a channel that moves more than a word per request");
		require(dma.execRequests(g_channel, 8) == 0, "a burst without words was served on such a channel");
		require(dump(f) == before, "a refused burst changed memory");
		require(dma.getDSR(g_channel) == dsr && dma.getDDR(g_channel) == ddr && dma.getDCO(g_channel) == dco
			&& dma.getDCR(g_channel) == dcr, "a refused burst changed the channel");
		require(!f.dsp.hasPendingInterrupts(), "a refused burst raised an interrupt");
	}

	// a transmit channel run to its end without clocking the port: memory -> TX0
	void transmit()
	{
		Fixture f;
		auto& tx = f.x.getEssi0();
		auto& dma = f.x.getDMA();
		tx.writeCRA(3u << Essi::CRA_WL0);
		tx.writeCRB((1u << Essi::CRB_TE0) | (1u << Essi::CRB_MOD));
		constexpr TWord source = 0x500;
		constexpr uint32_t count = 48;
		const auto words = makeWords(count);
		for(uint32_t i = 0; i < count; ++i)
			f.memory.set(MemArea_X, source + i, words[i]);
		// the register holds a word: the channel is armed and has to wait for a request
		tx.writeTX(0, 0x7f0000);
		dma.setDSR(1, source);
		dma.setDDR(1, Essi::ESSI0_TX0);
		dma.setDCO(1, count - 1);
		dma.setDCR(1, (5u << DmaChannel::Dam0) | (4u << DmaChannel::Dam3)
			| (static_cast<TWord>(DmaChannel::RequestSource::Essi0TransmitData) << DmaChannel::Drs0)
			| (1u << DmaChannel::Dtm0) | (1u << DmaChannel::De));
		require(dma.getDSR(1) == source, "the channel moved a word without a request");

		// a preview into a peripheral register would hand it the words twice: not stored
		require(dma.previewRequests(1, count, words.data()) == 0, "a preview was stored into a peripheral register");
		require(tx.readTX(0) == 0x7f0000 && dma.getDSR(1) == source, "a refused preview changed the port or the channel");

		require(dma.execRequests(1, count) == count, "the burst did not move every word");
		require(dma.getDSR(1) == source + count, "source address wrong after the burst");
		require(!(dma.getDCR(1) & (1u << DmaChannel::De)), "the burst did not clear the enable bit");
		require(tx.readTX(0) == words.back(), "the transmit register does not hold the last word");
	}

	// Transfer mode 5 keeps the channel enabled at the end of its block. A burst still ends there, after the block's
	// transfer done, and the next burst carries on into the next block as the requests one by one do
	void receiveKeepsEnabled()
	{
		constexpr uint32_t block = 32;
		const auto words = makeWords(2 * block + 8);

		Fixture reference;
		armReceive(reference, 5, block - 1, false, 5);
		auto& referenceDma = reference.x.getDMA();
		std::vector<TWord> afterFirst;
		TWord ddrFirst = 0, dcoFirst = 0;
		{
			auto& rx = reference.x.getEssi0();
			uint32_t next = 0;
			rx.setReadRxCallback([&](uint64_t& _index, Audio::RxFrame& _frame)
			{
				_frame.resize(1);
				_frame[0] = Audio::RxSlot{words[next++]};
				++_index;
			});
			for(uint32_t i = 0; i < 2 * block; ++i)
			{
				rx.execRX();
				if(next == block)
				{
					afterFirst = dump(reference);
					ddrFirst = referenceDma.getDDR(g_channel);
					dcoFirst = referenceDma.getDCO(g_channel);
				}
			}
			require(next == 2 * block, "the reference did not take every word");
		}
		require(referenceDma.getDCR(g_channel) & (1u << DmaChannel::De), "the reference channel did not stay enabled");

		Fixture f;
		armReceive(f, 5, block - 1, true, 5);
		auto& dma = f.x.getDMA();
		require(dma.servesRequests(g_channel), "a word channel that stays enabled is not served");

		require(dma.previewRequests(g_channel, block + 8, words.data()) == block, "the preview did not stop at the end of the block");

		require(dma.execRequests(g_channel, block + 8, words.data()) == block, "the burst ran past the end of the block");
		require(dump(f) == afterFirst, "the first block differs from the requests one by one");
		require(dma.getDDR(g_channel) == ddrFirst && dma.getDCO(g_channel) == dcoFirst, "the channel differs after the first block");
		require(dma.getDCR(g_channel) & (1u << DmaChannel::De), "the burst cleared the enable bit of a channel that stays enabled");
		require(f.dsp.hasPendingInterrupts(), "the burst did not raise the block's interrupt");

		require(dma.execRequests(g_channel, block + 8, words.data() + block) == block, "the second burst did not move the second block");
		require(dump(f) == dump(reference), "the second block differs from the requests one by one");
		require(dma.getDDR(g_channel) == referenceDma.getDDR(g_channel) && dma.getDCO(g_channel) == referenceDma.getDCO(g_channel),
			"the channel differs after the second block");
	}

	// A memory to memory (or memory to port) channel against the same transfer served one request at a time through
	// Dma::trigger
	struct MemoryChannel
	{
		TWord dcr;
		TWord dco;
		TWord destination;
		bool peripheralDestination = false;
	};

	void memoryBurst(const MemoryChannel& _c, const char* _name)
	{
		constexpr TWord channel = 2;
		constexpr TWord source = 0x2000;
		constexpr TWord sourceWords = 0x800;
		constexpr auto request = DmaChannel::RequestSource::Essi1TransmitData;
		const auto words = makeWords(sourceWords);

		auto arm = [&](Fixture& _f)
		{
			for(TWord i = 0; i < sourceWords; ++i)
				_f.memory.set(MemArea_X, source + i, words[i]);
			auto& dma = _f.x.getDMA();
			dma.setDOR(1, 0x11);		// two-dimensional: lines of 16 words 0x20 apart
			dma.setDSR(channel, source);
			dma.setDDR(channel, _c.destination);
			dma.setDCO(channel, _c.dco);
			dma.setDCR(channel, _c.dcr | (static_cast<TWord>(request) << DmaChannel::Drs0) | (1u << DmaChannel::De));
		};

		auto memoryDump = [](const Fixture& _f)
		{
			std::vector<TWord> result;
			for(TWord a = 0x800; a < 0x1000; ++a)
				result.push_back(_f.memory.get(MemArea_X, a));
			return result;
		};

		Fixture reference;
		arm(reference);
		auto& referenceDma = reference.x.getDMA();
		uint32_t requests = 0;
		while((referenceDma.getDCR(channel) & (1u << DmaChannel::De)) && requests < 4096)
		{
			referenceDma.trigger(request);
			++requests;
		}
		require(requests > 1 && requests < 4096, _name);

		Fixture f;
		arm(f);
		auto& dma = f.x.getDMA();
		require(dma.servesRequests(channel), _name);
		require(dma.wordsLeftInBlock(channel) == requests, _name);
		require(dma.execRequests(channel, 5) == 5, _name);
		require(dma.wordsLeftInBlock(channel) == requests - 5, _name);
		require(dma.execRequests(channel, requests) == requests - 5, _name);
		require(dma.wordsLeftInBlock(channel) == 0, _name);
		require(memoryDump(f) == memoryDump(reference), _name);
		require(dma.getDSR(channel) == referenceDma.getDSR(channel) && dma.getDDR(channel) == referenceDma.getDDR(channel)
			&& dma.getDCO(channel) == referenceDma.getDCO(channel), _name);
		require(!(dma.getDCR(channel) & (1u << DmaChannel::De)), _name);
		if(_c.peripheralDestination)
			require(f.x.getEssi0().readTX(0) == reference.x.getEssi0().readTX(0), _name);
	}

	void memoryBursts()
	{
		// two-dimensional source (DOR1) into post-increment memory: 4 lines of 16 words
		memoryBurst({(1u << DmaChannel::Dam0) | (5u << DmaChannel::Dam3) | (1u << DmaChannel::Dtm0), (3u << 12) | 15u, 0x800},
			"a two-dimensional source burst differs from the requests one by one");

		// the same source into the serial transmit register: the shape of the Monomachine's 128-word link send
		memoryBurst({(1u << DmaChannel::Dam0) | (4u << DmaChannel::Dam3) | (1u << DmaChannel::Dtm0), (3u << 12) | 15u, Essi::ESSI0_TX0, true},
			"a two-dimensional source burst into the transmit register differs from the requests one by one");

		// three-dimensional destination (DAM[2] = 1), counter mode 01, post-increment source (DAM[5-3] = 5): one word per
		// request, although DAM[2-0] = 5 would read as post-increment on both sides outside three-dimensional mode
		memoryBurst({(((5u << 3) | (1u << 2) | 1u) << DmaChannel::Dam0) | (1u << DmaChannel::D3d) | (1u << DmaChannel::Dtm0),
				(1u << 12) | 15u, 0x800},
			"a three-dimensional burst differs from the requests one by one");
	}

	// A preview in the middle of a block, across the end of a line, leaves the working counters as they were: the
	// rest of the block is then served as without the preview
	void partialPreview(const TWord _destinationMode, const TWord _counter, const uint32_t _count)
	{
		constexpr uint32_t first = 10;
		const auto words = makeWords(_count);

		Fixture reference;
		armReceive(reference, _destinationMode, _counter, true);
		auto& referenceDma = reference.x.getDMA();
		require(referenceDma.wordsLeftInBlock(g_channel) == _count, "a channel just armed does not have its whole block left");
		require(referenceDma.execRequests(g_channel, first, words.data()) == first, "the first part of a block was not served");
		require(referenceDma.wordsLeftInBlock(g_channel) == _count - first, "the words left in the block do not count down");
		require(referenceDma.execRequests(g_channel, _count, words.data() + first) == _count - first, "the rest of a block was not served");
		require(referenceDma.wordsLeftInBlock(g_channel) == 0, "a channel at the end of its block has words left");

		Fixture f;
		armReceive(f, _destinationMode, _counter, true);
		auto& dma = f.x.getDMA();
		require(dma.execRequests(g_channel, first, words.data()) == first, "the first part of a block was not served");
		const auto dsr = dma.getDSR(g_channel), ddr = dma.getDDR(g_channel), dco = dma.getDCO(g_channel);

		// further than the end of the first line of a two-dimensional block (32 words)
		const auto previewCount = std::min<uint32_t>(40, _count - first);
		require(dma.previewRequests(g_channel, previewCount, words.data() + first) == previewCount, "a preview in the middle of a block stored the wrong number of words");
		require(dma.getDSR(g_channel) == dsr && dma.getDDR(g_channel) == ddr && dma.getDCO(g_channel) == dco
			&& dma.wordsLeftInBlock(g_channel) == _count - first, "a preview in the middle of a block changed the channel");
		require(dma.execRequests(g_channel, _count, words.data() + first) == _count - first, "after a preview in the middle of a block the rest was served short or long");
		require(dump(f) == dump(reference), "after a preview in the middle of a block memory differs");
		require(dma.getDSR(g_channel) == referenceDma.getDSR(g_channel) && dma.getDDR(g_channel) == referenceDma.getDDR(g_channel)
			&& dma.getDCO(g_channel) == referenceDma.getDCO(g_channel), "after a preview in the middle of a block the channel differs");
	}

	// A preview stops before a word for a device on the external bus, which would see it now and again when the
	// transfer is served
	void previewExternalBus()
	{
		struct Bus final : IExternalBusDevice
		{
			uint32_t writes = 0;
			TWord read(TWord) override { return 0; }
			void write(TWord, TWord) override { ++writes; }
		};

		constexpr TWord busBegin = 0x8000;
		Fixture f;
		Bus bus;
		f.dsp.setExternalBusDevice(&bus);
		auto config = f.dsp.getJit().getConfig();
		config.externalBusBegin = busBegin;
		config.externalBusEnd = busBegin + 0x1000;
		f.dsp.getJit().setConfig(config);

		// a post-increment destination that reaches the bus after three words
		auto& rx = f.x.getEssi0();
		auto& dma = f.x.getDMA();
		rx.writeCRA(3u << Essi::CRA_WL0);
		rx.writeCRB((1u << Essi::CRB_RE) | (1u << Essi::CRB_MOD));
		dma.setDSR(g_channel, Essi::ESSI0_RX);
		dma.setDDR(g_channel, busBegin - 3);
		dma.setDCO(g_channel, 15);
		dma.setDCR(g_channel, receiveControl(5, false));

		const auto words = makeWords(16);
		require(dma.previewRequests(g_channel, 16, words.data()) == 3, "a preview did not stop before the external bus");
		require(bus.writes == 0, "a preview wrote to a device on the external bus");
		for(TWord i = 0; i < 3; ++i)
			require(f.memory.get(MemArea_X, busBegin - 3 + i) == words[i], "a preview did not store the words before the external bus");
		require(dma.getDDR(g_channel) == busBegin - 3 && dma.wordsLeftInBlock(g_channel) == 16, "a preview that stopped at the external bus changed the channel");
	}

	// The alarm on a clock counting DSP cycles with exact deadlines, as the Machinedrum and Monomachine run it
	void cycleAlarmExact()
	{
		Fixture f;
		auto& clock = f.x.getEssiClock();
		clock.setClockSource(EsxiClock::ClockSource::Cycles);
		clock.setCyclesPerSample(1152);
		clock.setExactCycleDeadlineEnabled(true);
		clock.setExactBaseTickDeadline(true);
		unsigned fired = 0;
		clock.setCycleAlarmCallback([&] { ++fired; });

		f.x.exec();
		const auto start = clock.getDspInstructionCounter();
		require(start == f.dsp.getCycles(), "the clock does not count cycles");

		clock.setCycleAlarm(start + 100);
		f.x.exec();
		require(fired == 0 && clock.getNextCycleDeadline() == 100, "the alarm is not the exact cycle deadline");

		f.dsp.fastForward(40, 99);
		f.x.exec();
		require(fired == 0 && clock.getNextCycleDeadline() == 1, "the alarm fired a cycle early or its deadline is not exact");

		f.dsp.fastForward(1, 1);
		f.x.exec();
		require(fired == 1 && !clock.hasCycleAlarm(), "the alarm did not fire at its cycle");

		// a disabled clock has no deadline of its own: the alarm still is one, also when it is further away than the
		// last deadline of the enabled clock, and once it has fired no deadline is left
		require(clock.getNextCycleDeadline() < 3000, "test setup: the enabled clock's last deadline is not below the alarm's");
		clock.setEnabled(false);
		clock.setCycleAlarm(clock.getDspInstructionCounter() + 3000);
		f.x.exec();
		require(fired == 1 && clock.getNextCycleDeadline() == 3000, "the alarm of a disabled clock is not its deadline");
		f.dsp.fastForward(1000, 2998);
		f.x.exec();
		require(fired == 1 && clock.getNextCycleDeadline() == 2, "the alarm of a disabled clock is not an exact deadline");
		f.dsp.fastForward(1, 2);
		f.x.exec();
		require(fired == 2, "the alarm of a disabled clock did not fire at its cycle");
		require(clock.getNextCycleDeadline() == std::numeric_limits<uint32_t>::max(), "a disabled clock kept a deadline after its alarm fired");
		clock.setEnabled(true);

		// the RESET instruction (DSP::resetSW) resets the peripherals but not the counter: the alarm stays
		clock.setCycleAlarm(clock.getDspInstructionCounter() + 50);
		f.dsp.resetSW();
		require(clock.hasCycleAlarm(), "the RESET instruction cleared the alarm");
		f.dsp.fastForward(20, 50);
		f.x.exec();
		require(fired == 3, "the alarm did not fire after the RESET instruction");

		// a hardware reset starts the counter over and clears the alarm, also once the counter has passed the alarm's
		// count again
		const auto alarmCount = clock.getDspInstructionCounter() + 50;
		clock.setCycleAlarm(alarmCount);
		f.dsp.resetHW();
		require(!clock.hasCycleAlarm(), "a reset did not clear the alarm");
		f.dsp.fastForward(10, 10);
		f.x.exec();
		f.dsp.fastForward(1000, static_cast<uint32_t>(alarmCount + 100));
		f.x.exec();
		require(fired == 3 && !clock.hasCycleAlarm(), "an alarm set before a reset fired after it");
	}

	void cycleAlarm()
	{
		Fixture f;
		auto& clock = f.x.getEssiClock();
		unsigned fired = 0;
		clock.setCycleAlarmCallback([&] { ++fired; });

		// without an alarm the clock is as before
		f.x.exec();
		require(fired == 0 && !clock.hasCycleAlarm(), "an alarm fired that was never set");

		const auto start = clock.getDspInstructionCounter();
		clock.setCycleAlarm(start + 1000);
		require(clock.hasCycleAlarm(), "the alarm is not armed");
		const auto delay = f.x.exec();
		require(fired == 0, "the alarm fired early");
		require(delay <= 1000, "the peripherals were not asked back in time for the alarm");

		f.dsp.fastForward(999, 999);
		f.x.exec();
		require(fired == 0, "the alarm fired one count early");

		f.dsp.fastForward(1, 1);
		f.x.exec();
		require(fired == 1 && !clock.hasCycleAlarm(), "the alarm did not fire at its count");

		f.dsp.fastForward(5000, 5000);
		f.x.exec();
		require(fired == 1, "a one-shot alarm fired twice");

		// the callback may set the next alarm, an alarm in the past fires on the next pass
		clock.setCycleAlarmCallback([&]
		{
			if(++fired == 2)
				clock.setCycleAlarm(clock.getDspInstructionCounter());
		});
		clock.setCycleAlarm(0);
		f.x.exec();
		require(fired == 2 && clock.hasCycleAlarm(), "the callback could not set the next alarm");
		f.x.exec();
		require(fired == 3 && !clock.hasCycleAlarm(), "the alarm set by the callback did not fire");

		clock.setCycleAlarm(clock.getDspInstructionCounter() + 10);
		clock.clearCycleAlarm();
		f.dsp.fastForward(100, 100);
		f.x.exec();
		require(fired == 3, "a cleared alarm fired");
	}
}

int main() try
{
	receive(2, (3u << 12) | 31u, 128);		// four lines of 32 words, lines 0x40 apart
	receive(5, 47, 48);						// 48 words, one after the other
	refused(2, 4, 2);						// a line per request
	refused(0, 4, 2);						// the block per request
	refused(1, 5, 5);						// word mode, but post-increment on both sides is moved as a block
	refused(1, 6, 5);						// a reserved source address mode
	refused(1, 1 | (1u << 2), 6, true);		// three-dimensional, the other side in a reserved address mode
	refused(1, 3 | (1u << 2), 5, true);		// three-dimensional, the reserved counter mode
	receiveKeepsEnabled();
	partialPreview(2, (3u << 12) | 31u, 128);
	partialPreview(5, 47, 48);
	previewExternalBus();
	memoryBursts();
	transmit();
	cycleAlarm();
	cycleAlarmExact();
	std::cout << "PASS: DMA request bursts, previews, refused transfer modes and the serial clock's cycle alarm\n";
	return 0;
}
catch(const std::exception& e)
{
	std::cerr << "FAIL: " << e.what() << '\n';
	return 1;
}
