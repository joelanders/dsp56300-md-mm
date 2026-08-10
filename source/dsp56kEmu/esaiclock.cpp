#include "esaiclock.h"

#include <algorithm>
#include <ios>

#include "dsp.h"
#include "dsp56kBase/logging.h"

#include "peripherals.h"

namespace dsp56k
{
	EsxiClock::EsxiClock(IPeripherals& _peripherals)
		: m_periph(_peripherals)
	{
	}

	uint32_t EsxiClock::exec() noexcept
	{
		const auto now = *m_dspInstructionCounter;
		const auto period = getEffectiveCyclesPerSample();
		auto finish = [this](const uint32_t _delay)
		{
			if(usesExactCycleScheduling())
			{
				m_nextCycleDelay = _delay;
				return IPeripherals::MaxDelayCycles;
			}
			m_periph.disableCycleDelay();
			return _delay >> static_cast<uint32_t>(m_clockSource);
		};
		const auto diff = now - m_lastClock;
		uint32_t fineDelay = period;

		if(m_hasFineEsais)
		{
			for(auto& entry : m_esais)
			{
				if(!entry.finePeriod)
					continue;
				uint32_t ticks = 0;
				while(ticks < MaxFineTicksPerExec && now - entry.fineLastClock >= entry.finePeriod)
				{
					++ticks;
					entry.fineLastClock += entry.finePeriod;
					const auto transmitEnabled = entry.esai->hasEnabledTransmitters();
					const auto receiveEnabled = entry.esai->hasEnabledReceivers();
					if(!(transmitEnabled || receiveEnabled) || !entry.esai->clockTickEnabled())
						continue;
					if(transmitEnabled)
						entry.esai->execTX();
					if(receiveEnabled)
						entry.esai->execRX();
				}
				if(now - entry.fineLastClock >= entry.finePeriod)
					fineDelay = 0;
				else
				{
					const auto remaining = static_cast<uint32_t>(entry.finePeriod - (now - entry.fineLastClock));
					fineDelay = std::min(fineDelay, remaining);
				}
			}
		}

		if(diff < period)
			return finish(std::min(fineDelay, static_cast<uint32_t>(period - diff)));

		m_lastClock += period;

		auto advanceClock = [](Clock& _c)
		{
			if (++_c.counter > static_cast<int32_t>(_c.divider))
			{
				_c.counter = 0;
				return true;
			}
			return false;
		};

		std::array<Esxi*, MaxEsais> processTx;
		uint32_t txCount = 0;
		std::array<Esxi*, MaxEsais> processRx;
		uint32_t rxCount = 0;

		for (auto& e : m_esais)
		{
			if(e.finePeriod)
				continue;
			const auto transmitEnabled = e.esai->hasEnabledTransmitters();
			const auto receiveEnabled = e.esai->hasEnabledReceivers();
			if(!(transmitEnabled || receiveEnabled) || !e.esai->clockTickEnabled())
				continue;
			if(transmitEnabled && advanceClock(e.tx))
				processTx[txCount++] = e.esai;

			if(receiveEnabled && advanceClock(e.rx))
				processRx[rxCount++] = e.esai;
		}

		for(size_t i=0; i<txCount; ++i) processTx[i]->execTX();
		for(size_t i=0; i<rxCount; ++i) processRx[i]->execRX();

		if(diff >= (period<<1))
			return finish(0);

		const auto delay = static_cast<uint32_t>((period << 1) - diff);

		return finish(std::min(fineDelay, delay));
	}

	void EsxiClock::refreshExactCycleDeadline()
	{
		if(usesExactCycleScheduling())
			m_periph.resetCycleDelay(m_periph.getDSP().getCycles(), m_nextCycleDelay);
	}

	void EsxiClock::setPCTL(const TWord _val)
	{
		if(m_pctl == _val)
			return;

		m_pctl = _val;

		updateCyclesPerSample();
	}

	void EsxiClock::setSamplerate(const uint32_t _samplerate)
	{
		if (m_samplerate == _samplerate)
			return;

		m_samplerate = _samplerate;

		updateCyclesPerSample();
	}

	void EsxiClock::setCyclesPerSample(const uint32_t _cyclesPerSample)
	{
		if(m_fixedCyclesPerSample == _cyclesPerSample)
			return;

		m_fixedCyclesPerSample = _cyclesPerSample;

		updateCyclesPerSample();
	}

	void EsxiClock::setExternalClockFrequency(const uint32_t _freq)
	{
		if(_freq == m_externalClockFrequency)
			return;

		m_externalClockFrequency = _freq;
		updateCyclesPerSample();
	}

	void EsxiClock::setDSP(const DSP* _dsp)
	{
		setClockSource(_dsp, ClockSource::Instructions);
	}

	void EsxiClock::setClockSource(const ClockSource _clockSource)
	{
		setClockSource(&m_periph.getDSP(), _clockSource);
	}

	void EsxiClock::restartClock()
	{
		m_lastClock = *m_dspInstructionCounter;
		for(auto& entry : m_esais)
			entry.fineLastClock = m_lastClock;
		requestProcessing();
	}

	TWord EsxiClock::getRemainingInstructionsForFrameSync() const
	{
		// The JIT shortcut advances instructions and cycles 1:1. It is not an
		// exact representation of a multi-cycle instruction stream.
		if(usesExactCycleScheduling())
			return 0;

		constexpr TWord offset = 1;

		const auto cyclesSinceWrite = getDspInstructionCounter() - getLastClock();

		if (cyclesSinceWrite >= getCyclesPerSample() - offset)
			return 0;

		const auto periphCycles = getPeripherals().getDSP().getRemainingPeripheralsCycles();

		if(periphCycles < offset)
			return 0;

		const auto diff = getCyclesPerSample() - cyclesSinceWrite - offset;

		return std::min(static_cast<uint32_t>(diff), periphCycles - offset);
	}

	void EsxiClock::setClockSource(const DSP* _dsp, const ClockSource _clockSource)
	{
		const uint64_t* nextCounter = nullptr;
		switch (_clockSource)
		{
		case ClockSource::Instructions:
			nextCounter = &_dsp->getInstructionCounter();
			break;
		case ClockSource::Cycles:
			nextCounter = &_dsp->getCycles();
			break;
		}
		if(m_dspInstructionCounter == nextCounter && m_clockSource == _clockSource)
			return;
		m_dspInstructionCounter = nextCounter;
		m_clockSource = _clockSource;
		// Counters from different domains cannot share phase anchors or deadlines.
		restartClock();
	}

	void EsxiClock::setExactCycleScheduling(const bool _enabled)
	{
		if(m_exactCycleScheduling == _enabled)
			return;
		m_exactCycleScheduling = _enabled;
		// Do not carry advisory-domain phase or deadlines into or out of exact mode.
		restartClock();
	}

	void EsxiClock::requestProcessing()
	{
		if(!m_periph.hasDSP())
			return;
		if(usesExactCycleScheduling())
			m_periph.resetCycleDelay(*m_dspInstructionCounter, 0);
		else
			m_periph.disableCycleDelay();
		m_periph.setDelayCycles(0);
	}

	void EsxiClock::updateCyclesPerSample()
	{
		uint32_t cyclesPerSample = m_fixedCyclesPerSample;

		if(!cyclesPerSample)
		{
			if(!m_pctl)
				return;

			const auto pd = ((m_pctl >> 20) & 15) + 1;
			const auto df = 1 << ((m_pctl >> 12) & 3);
			const auto mf = (m_pctl & 0xfff) + 1;

			m_speedHz = static_cast<uint64_t>(m_externalClockFrequency) * static_cast<uint64_t>(mf) / (static_cast<uint64_t>(pd) * static_cast<uint64_t>(df));

			if (m_samplerate)
				cyclesPerSample = static_cast<uint32_t>((m_speedHz / m_samplerate) >> 1);	// 2 samples = 1 frame (stereo)
			else
				cyclesPerSample = mf * 128 / pd;			// The ratio between external clock and sample period simplifies to this.

			cyclesPerSample *= m_speedPercent;
			cyclesPerSample /= 100;

			// A more full expression would be m_cyclesPerSample = dsp_frequency / samplerate, where
			// dsp_frequency = m_extClock * mf / pd and samplerate = m_extClock/256

			const auto speedMhz = static_cast<double>(m_speedHz) / 1000000.0f * m_speedPercent / 100.0f;
			LOG("Clock speed changed to: " << speedMhz << " Mhz, EXTAL=" << m_externalClockFrequency << " Hz, PCTL=" << HEX(m_pctl) << ", mf=" << HEX(mf) << ", pd=" << HEX(pd) << ", df=" << HEX(df) << " => cycles per sample=" << std::dec << cyclesPerSample << ", predefined samplerate=" << m_samplerate);
		}
		else
		{
			cyclesPerSample *= m_speedPercent;
			cyclesPerSample /= 100;
		}

		m_cyclesPerSample = cyclesPerSample;
		m_lastClock = *m_dspInstructionCounter;
		requestProcessing();
	}
	void EsxiClock::setEsaiDivider(Esxi* _esai, const TWord _dividerTX, const TWord _dividerRX)
	{
		bool found = false;

		for (auto& entry : m_esais)
		{
			if(entry.esai == _esai)
			{
				if(entry.tx.divider == _dividerTX && entry.rx.divider == _dividerRX)
					return;

				entry.tx.divider = _dividerTX;
				entry.rx.divider = _dividerRX;
				found = true;
				break;
			}
		}

		if(!found)
		{
			m_esais.emplace_back(EsaiEntry{ _esai, {_dividerTX}, {_dividerRX} });
		}

		if(m_periph.hasDSP())
			requestProcessing();
	}

	void EsxiClock::setEsaiFinePeriod(Esxi* _esai, const uint32_t _finePeriod)
	{
		// Fine periods share the selected counter with the base clock. exec() is
		// called at instruction boundaries, so this only provides advisory timing.
		const auto now = m_dspInstructionCounter ? *m_dspInstructionCounter : 0;
		bool found = false;
		for(auto& entry : m_esais)
		{
			if(entry.esai != _esai)
				continue;
			entry.finePeriod = _finePeriod;
			entry.fineLastClock = now;
			found = true;
			break;
		}
		if(!found)
		{
			m_esais.emplace_back(EsaiEntry{_esai, {}, {}, _finePeriod, now});
		}
		m_hasFineEsais = false;
		for(const auto& entry : m_esais)
			m_hasFineEsais = m_hasFineEsais || entry.finePeriod != 0;
		if(m_periph.hasDSP())
			requestProcessing();
	}

	bool EsxiClock::setEsaiCounter(const Esxi* _esai, const int _counterTX, const int _counterRX)
	{
		for (auto& esai : m_esais)
		{
			if(esai.esai == _esai)
			{
				esai.tx.counter = _counterTX;
				esai.rx.counter = _counterRX;
				return true;
			}
		}
		return false;
	}

	bool EsxiClock::setSpeedPercent(const uint32_t _percent)
	{
		if(m_speedPercent == _percent)
			return true;

		if(_percent == 0)
			return false;

		m_speedPercent = _percent;
		updateCyclesPerSample();
		return true;
	}
	
	// ______________________
	//

	EsaiClock::EsaiClock(Peripherals56362& _peripherals) : EsxiClock(_peripherals)
	{
	}

	template<bool ExpectedResult>
	TWord EsaiClock::getRemainingInstructionsForTransmitFrameSync() const
	{
		if (static_cast<bool>(static_cast<Esai*>(getEsais().front().esai)->getTransmitFrameSync()) == ExpectedResult)
		{
			// already reached the desired value
			return 0;
		}

		return getRemainingInstructionsForFrameSync();
	}

	template<bool ExpectedResult>
	TWord EsaiClock::getRemainingInstructionsForReceiveFrameSync() const
	{
		if (static_cast<bool>(static_cast<Esai*>(getEsais().front().esai)->getReceiveFrameSync()) == ExpectedResult)
		{
			// already reached the desired value
			return 0;
		}

		return getRemainingInstructionsForFrameSync();
	}

	EssiClock::EssiClock(Peripherals56303& _peripherals) : EsxiClock(_peripherals)
	{
	}

	template<bool ExpectedResult>
	TWord EssiClock::getRemainingInstructionsForTransmitFrameSync(uint32_t _esaiIndex) const
	{
		if (static_cast<Essi*>(getEsais()[_esaiIndex].esai)->getTransmitFrameSync() == ExpectedResult)
		{
			// already reached the desired value
			return 0;
		}
		return getRemainingInstructionsForFrameSync();
	}

	template<bool ExpectedResult>
	TWord EssiClock::getRemainingInstructionsForReceiveFrameSync(uint32_t _esaiIndex) const
	{
		if (static_cast<Essi*>(getEsais()[_esaiIndex].esai)->getReceiveFrameSync() == ExpectedResult)
		{
			// already reached the desired value
			return 0;
		}
		return getRemainingInstructionsForFrameSync();
	}

	template TWord EssiClock::getRemainingInstructionsForTransmitFrameSync<true>(uint32_t) const;
	template TWord EssiClock::getRemainingInstructionsForTransmitFrameSync<false>(uint32_t) const;
	template TWord EssiClock::getRemainingInstructionsForReceiveFrameSync<true>(uint32_t) const;
	template TWord EssiClock::getRemainingInstructionsForReceiveFrameSync<false>(uint32_t) const;

	template TWord EsaiClock::getRemainingInstructionsForTransmitFrameSync<true>() const;
	template TWord EsaiClock::getRemainingInstructionsForTransmitFrameSync<false>() const;
	template TWord EsaiClock::getRemainingInstructionsForReceiveFrameSync<true>() const;
	template TWord EsaiClock::getRemainingInstructionsForReceiveFrameSync<false>() const;
}
