#include "dsp56kEmu/dsp.h"

#include <array>
#include <iostream>
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

	// A receive channel on ESSI0: one word per request, enable cleared at the end of the block
	TWord receiveControl(const TWord _destinationMode, const bool _interrupt)
	{
		return (4u << DmaChannel::Dam0) | (_destinationMode << DmaChannel::Dam3)
			| (static_cast<TWord>(DmaChannel::RequestSource::Essi0ReceiveData) << DmaChannel::Drs0)
			| (1u << DmaChannel::Dtm0) | (_interrupt ? (1u << DmaChannel::Die) : 0u) | (1u << DmaChannel::De);
	}

	void armReceive(Fixture& _f, const TWord _destinationMode, const TWord _counter, const bool _interrupt)
	{
		auto& rx = _f.x.getEssi0();
		auto& dma = _f.x.getDMA();
		rx.writeCRA(3u << Essi::CRA_WL0);
		rx.writeCRB((1u << Essi::CRB_RE) | (1u << Essi::CRB_MOD));
		dma.setDOR(2, 0x21);		// two-dimensional destination: lines 0x40 apart for a line of 0x20 words
		dma.setDSR(g_channel, Essi::ESSI0_RX);
		dma.setDDR(g_channel, g_destination);
		dma.setDCO(g_channel, _counter);
		dma.setDCR(g_channel, receiveControl(_destinationMode, _interrupt));
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
		const auto words = makeWords(_count);

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

	// A channel that moves a line or a block per request is not served: one call would be more than one word
	void refused(const TWord _transferMode, const TWord _sourceMode, const TWord _destinationMode)
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
			| (_transferMode << DmaChannel::Dtm0) | (1u << DmaChannel::Die) | (1u << DmaChannel::De));

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

		require(dma.execRequests(1, count) == count, "the burst did not move every word");
		require(dma.getDSR(1) == source + count, "source address wrong after the burst");
		require(!(dma.getDCR(1) & (1u << DmaChannel::De)), "the burst did not clear the enable bit");
		require(tx.readTX(0) == words.back(), "the transmit register does not hold the last word");
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
	transmit();
	cycleAlarm();
	std::cout << "PASS: DMA request bursts, previews, refused transfer modes and the serial clock's cycle alarm\n";
	return 0;
}
catch(const std::exception& e)
{
	std::cerr << "FAIL: " << e.what() << '\n';
	return 1;
}
