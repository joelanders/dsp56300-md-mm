#pragma once

#include <functional>
#include <vector>

#include "types.h"

namespace dsp56k
{
	class Peripherals56303;
	class Peripherals56362;
	class Esxi;
	class IPeripherals;
	class DSP;

	class EsxiClock
	{
	public:
		enum class ClockSource
		{
			Instructions,
			Cycles
		};

		static constexpr uint32_t MaxEsais = 2;

		EsxiClock(IPeripherals& _peripherals);
		uint32_t exec() noexcept;

		void setPCTL(TWord _val);
		TWord getPCTL() const { return m_pctl; }

		void setSamplerate(uint32_t _samplerate);
		void setCyclesPerSample(uint32_t _cyclesPerSample);
		void setExternalClockFrequency(uint32_t _freq);
		void setEnabled(const bool _enabled)
		{
			if(m_enabled == _enabled)
				return;
			m_enabled = _enabled;
			if(m_enabled && m_dspInstructionCounter)
				restartClock();
		}
		bool isEnabled() const { return m_enabled; }

		void setEsaiDivider(Esxi* _esai, TWord _divider)
		{
			setEsaiDivider(_esai, _divider, _divider);
		}
		void setEsaiDivider(Esxi* _esai, TWord _dividerTX, TWord _dividerRX);
		// A nonzero fine period schedules an ESSI independently of the audio base tick. Zero keeps the base-plus-divider clock
		void setEsaiFinePeriod(Esxi* _esai, uint32_t _finePeriod);
		void setExactCycleDeadlineEnabled(bool _enabled) { m_exactCycleDeadlineEnabled = _enabled; }

		/*	The base tick as an exact cycle deadline even when no port runs on the fine schedule. The instruction
			count that a cycle delay converts to is an estimate; with a fine port its deadlines come often enough to
			hide that, without one the base tick is served a slot or more late whenever the cycles per instruction
			change, and two slots then run back to back.
		*/
		void setExactBaseTickDeadline(bool _enabled) { m_exactBaseTickDeadline = _enabled; }

		// Where a port on the fine schedule stands: the clock value of its last slot and its slot period.
		// False if the port is not on the fine schedule.
		bool getEsaiFineSchedule(const Esxi* _esai, uint64_t& _lastClock, uint32_t& _period) const;
		bool usesExactCycleDeadline() const;
		uint32_t getNextCycleDeadline() const { return m_nextCycleDeadline; }
		bool shiftEsaiFineAnchor(const Esxi* _esai, int64_t _cycles);	// one-time fine-phase step
		bool setEsaiCounter(const Esxi* _esai, int _counter)
		{
			return setEsaiCounter(_esai, _counter, _counter);
		}
		bool setEsaiCounter(const Esxi* _esai, int _counterTX, int _counterRX);

		bool setSpeedPercent(uint32_t _percent = 100);

		auto getClockSource() const			{ return m_clockSource; }
		auto getCyclesPerSample() const		{ return m_cyclesPerSample; }
		auto getExternalClockFrequency() const { return m_externalClockFrequency; }
		auto getSpeedInHz() const			{ return m_speedHz; }
		auto getSpeedPercent() const		{ return m_speedPercent; }

		void setDSP(const DSP* _dsp);
		void setClockSource(ClockSource _clockSource);

		void restartClock();

		TWord getRemainingInstructionsForFrameSync() const;

		// the instructions until the next slot, none if the flag the DSP spins on already has the value it waits for
		template<bool ExpectedResult> TWord getRemainingInstructionsUntil(const bool _flag) const
		{
			return _flag == ExpectedResult ? 0 : getRemainingInstructionsForFrameSync();
		}

		// Called on the DSP thread once per slot of the serial clock, after every transmitter and receiver has been
		// served. For a board that derives a signal from the serial clock, a sample clock that is a fixed number of
		// slots long for example, and needs it on the exact slot
		using TickCallback = std::function<void()>;
		void setTickCallback(TickCallback _callback) { m_tickCallback = std::move(_callback); }

		auto getDspInstructionCounter() const { return *m_dspInstructionCounter; }
		auto getLastClock() const { return m_lastClock; }

	protected:
		const auto& getEsais() const { return m_esais; }
		const auto& getPeripherals() const { return m_periph; }

	private:
		uint32_t instructionsForCycles(uint32_t _cycles) noexcept;
		uint32_t scheduleDelay(uint32_t _cycles) noexcept;	// records the cycle deadline, returns it in instructions
		void setClockSource(const DSP* _dsp, ClockSource _clockSource);

		void updateCyclesPerSample();

		const uint64_t* m_dspInstructionCounter = nullptr;
		uint64_t m_lastClock = 0;
		uint32_t m_nextCycleDeadline = 0;
		uint32_t m_cyclesPerSample = 2133;				// estimated cycles per sample before calculated

		ClockSource m_clockSource = ClockSource::Instructions;

		// measured instructions per cycle, to convert a cycle delay into the instruction count the DSP paces by
		static constexpr uint32_t g_ratioShift = 16;			// fixed point fraction of the reciprocal
		static constexpr uint32_t g_ratioWindow = 16;			// instructions to measure over before it is updated
		uint64_t m_ratioInstructions = 0;
		uint64_t m_ratioCycles = 0;
		uint32_t m_instructionsPerCycle = (1u << g_ratioShift) / 2;

		IPeripherals& m_periph;

		uint32_t m_fixedCyclesPerSample = 0;
		uint32_t m_samplerate = 0;
		TWord m_pctl = 0;
		uint32_t m_externalClockFrequency = 12000000;	// Hz

		uint64_t m_speedHz = 0;							// DSP clock speed in Hertz
		uint32_t m_speedPercent = 100;					// 100% = regular operation, overclock/underclock otherwise
		bool m_enabled = true;

		struct Clock
		{
			uint32_t divider = 0;
			int32_t counter = 0;
		};

		struct EsaiEntry
		{
			Esxi* esai = nullptr;

			Clock tx;
			Clock rx;

			uint64_t requestTime = 0;					// while a request of it is pending: when the slot that raised it came

			// Nonzero selects the independent fine schedule, zero uses the base tick
			uint32_t finePeriod = 0;
			uint64_t fineLastClock = 0;
		};

		std::vector<EsaiEntry> m_esais;

		// Cached "any entry has a nonzero finePeriod", keeps exec() free of the fine-schedule loop when it is unused
		bool m_hasFineEsais = false;
		bool m_exactCycleDeadlineEnabled = false;
		bool m_exactBaseTickDeadline = false;

		TickCallback m_tickCallback;
	};

	class EsaiClock : public EsxiClock
	{
	public:
		EsaiClock(Peripherals56362& _peripherals);
	};

	class EssiClock : public EsxiClock
	{
	public:
		EssiClock(Peripherals56303& _peripherals);
	};
}
