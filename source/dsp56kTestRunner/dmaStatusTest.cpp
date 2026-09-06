// Public DSP56300FM table 10-10: enable pipeline and DMA completion status.
// ROM-free synthetic requests; no active serial ports or interrupt handlers.
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <iostream>
#include <memory>

namespace
{
	struct Fixture
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Peripherals56303 peripherals;
		dsp56k::PeripheralsNop unused;
		dsp56k::Memory memory{validator, 0x10000, 0x10000, 0x8000};
		dsp56k::DSP dsp{memory, &peripherals, &unused};
	};
}

int main()
{
	bool failed = false;
	for(unsigned channel = 0; channel < 6; ++channel)
	{
		auto fixture = std::make_unique<Fixture>();
		auto& dsp = fixture->dsp;
		auto& dma = fixture->peripherals.getDMA();
		auto config = dsp.getJit().getConfig();
		config.maxInstructionsPerBlock = 1;
		config.linkJitBlocks = false;
		dsp.getJit().setConfig(config);
		for(unsigned pc = 0x100; pc < 0x200; ++pc) dsp.memWriteP(pc, 0); // NOP
		dsp.setPC(0x100);
		dsp.regs().sr.var = 0x300;
		const auto reset = dma.getDSTR() & 0x3f;
		dma.setDSR(channel, 0x1000);
		dma.setDDR(channel, 0x2000);
		fixture->memory.set(dsp56k::MemArea_X, 0x1000, 0x123456);
		dma.setDCO(channel, 3);
		// External IRQA, word/request/clear-DE mode; no external edge is raised.
		const auto control = (44u << dsp56k::DmaChannel::Dam0)
			| (1u << dsp56k::DmaChannel::Dtm0) | (1u << dsp56k::DmaChannel::De);
		dma.setDCR(channel, control);
		const auto start = dsp.getCycles();
		const auto check = [&](bool ok, const char* phase)
		{
			if(!ok) std::cerr << "DMA channel " << channel << " failed " << phase << '\n';
			failed |= !ok;
		};
		const auto advance = [&](unsigned cycles)
		{
			dsp.execUntilCycles(dsp.getCycles() + cycles);
			// Sample after peripheral dispatch at the ending instruction boundary.
			dma.exec();
		};
		advance(2);
		check((dma.getDSTR() & 0x3f) == 0x3f, "enable delay before third instruction");
		advance(1);
		check((dma.getDSTR() & 0x3f) == (0x3fu & ~(1u << channel)), "enable delay at third instruction");
		// Deliberately beyond the documented three-instruction pipeline delay.
		dsp.execUntilCycles(start + 16);
		const auto pending = dma.getDSTR() & 0x3f;
		const auto expected = 0x3fu & ~(1u << channel);
		const bool enabled = (dma.getDCR(channel) & (1u << dsp56k::DmaChannel::De)) != 0;
		const bool untouched = dma.getDSR(channel) == 0x1000 && dma.getDDR(channel) == 0x2000
			&& dma.getDCO(channel) == 3;
		std::cout << "DMA status channel " << channel << " reset " << reset
			<< " enabled " << enabled << " untouched " << untouched << " after-cycles "
			<< dsp.getCycles() - start << " done-mask " << pending << " expected " << expected << '\n';
		failed |= reset != 0x3f || !enabled || !untouched || pending != expected;
		for(unsigned word = 0; word < 4; ++word)
		{
			check(dma.trigger(dsp56k::DmaChannel::RequestSource::ExternalIRQA), "request routed");
			check(bool(dma.getDSTR() & (1u << channel)) == (word == 3), "word/block completion");
		}
		check(!(dma.getDCR(channel) & (1u << dsp56k::DmaChannel::De)), "completion clears DE");
		check(dma.getDDR(channel) == 0x2004, "four words transferred");
		for(unsigned word = 0; word < 4; ++word)
			check(fixture->memory.get(dsp56k::MemArea_X, 0x2000 + word) == 0x123456,
				"transferred data preserved");

		// Disable during the deferred clear, then re-enable. The old deadline
		// must not clear status in the new generation before its own delay.
		dma.setDCR(channel, control);
		advance(1);
		dma.setDCR(channel, control & ~(1u << dsp56k::DmaChannel::De));
		check(dma.getDSTR() & (1u << channel), "disable waiting channel");
		dma.setDCR(channel, control);
		advance(2);
		check(dma.getDSTR() & (1u << channel), "re-enable replaces old deadline");
		advance(1);
		check(!(dma.getDSTR() & (1u << channel)), "re-enable new deadline");
		dma.setDCR(channel, control & ~(1u << dsp56k::DmaChannel::De));
		advance(4);
		check(dma.getDSTR() & (1u << channel), "disabled channel stays done");

		// A one-word block completing before the deferred clear must remain done.
		dma.setDCO(channel, 0);
		dma.setDCR(channel, control);
		dma.trigger(dsp56k::DmaChannel::RequestSource::ExternalIRQA);
		advance(4);
		check(dma.getDSTR() & (1u << channel), "early completion cancels delayed clear");
		dma.setDCR(channel, control);
		// No explicit DMA dispatch here: the peripheral wake must work even
		// though a waiting request channel does not own the DACT slot.
		dsp.execUntilCycles(dsp.getCycles() + 16);
		check(!(dma.getDSTR() & (1u << channel)), "normal dispatcher clears waiting status");
		dma.setDCR(channel, control & ~(1u << dsp56k::DmaChannel::De));

		// Continuous word mode stays enabled across block completion, so DTD
		// cannot be computed as inverse DE. The next request starts a new block.
		dma.setDCO(channel, 1);
		const auto continuous = control | (4u << dsp56k::DmaChannel::Dtm0);
		dma.setDCR(channel, continuous);
		advance(4);
		for(unsigned word = 0; word < 4; ++word)
		{
			dma.trigger(dsp56k::DmaChannel::RequestSource::ExternalIRQA);
			check(bool(dma.getDSTR() & (1u << channel)) == bool(word & 1), "continuous block status");
			check(dma.getDCR(channel) & (1u << dsp56k::DmaChannel::De), "continuous mode retains DE");
		}
		dma.setDCR(channel, continuous & ~(1u << dsp56k::DmaChannel::De));

		// A DE-triggered block owns a deferred transfer. Disabling the channel
		// must not falsely announce completion before that accepted work runs.
		dma.setDCO(channel, 7);
		const auto block = (45u << dsp56k::DmaChannel::Dam0)
			| (3u << dsp56k::DmaChannel::Dtm0) | (1u << dsp56k::DmaChannel::De);
		dma.setDCR(channel, block);
		const auto blockDestination = dma.getDDR(channel);
		advance(4);
		check(!(dma.getDSTR() & (1u << channel)), "delayed block busy");
		dma.setDCR(channel, block & ~(1u << dsp56k::DmaChannel::De));
		check(!(dma.getDSTR() & (1u << channel)), "accepted block remains pending after disable");
		advance(16);
		check(dma.getDSTR() & (1u << channel), "accepted block completes after disable");
		check(dma.getDDR(channel) == blockDestination + 8, "accepted block transfers all words");
	}
	if(failed) std::cerr << "DMA transfer-done status does not reflect enabled waiting channels\n";
	return failed ? 1 : 0;
}
