#include "dspBootCode.h"

#include "dsp.h"

namespace dsp56k
{
	DspBoot::DspBoot(DSP& _dsp) : m_dsp(_dsp)
	{
		// H(D)I08 interface needs to be enabled after boot, set it up now already
		auto enableHdi08 = [](HDI08& _hdi08)
		{
			_hdi08.writePortControlRegister(1<<HDI08::HPCR_HEN);
		};

		if(auto* periph56362 = dynamic_cast<Peripherals56362*>(m_dsp.getPeriph(0)))
			enableHdi08(periph56362->getHDI08());
		else if(auto* periph56303 = dynamic_cast<Peripherals56303*>(m_dsp.getPeriph(0)))
			enableHdi08(periph56303->getHI08());
	}

	void DspBoot::reset()
	{
		m_state = State::Length;
		m_length = 0;
		m_initialPC = 0;
		m_remaining = 0;
		m_address = 0;
	}

	bool DspBoot::hdiWriteTX(const TWord& _val)
	{
		// DSP Boot Code emulation. The following bytes are sent to the DSP via HDI08:
		//
		// * Length = number of words to be received
		// * Address = target address where words will be written to
		// * 'Length' number of words
		//
		// After completion, DSP jumps to 'Address' to execute the code it just received

		switch (m_state)
		{
		case State::Length:
			// A zero-length stream would enter Data with m_remaining == 0,
			// underflow on the first word, and never complete.  Leave the
			// protocol in a terminal error state so the caller must explicitly
			// reset before retrying a malformed upload.
			if(_val == 0)
			{
				m_state = State::Error;
				return false;
			}

			m_remaining = _val;
			m_length = _val;
			m_state = State::Address;
			return false;
		case State::Address:
		{
			// Validate the complete range before changing DSP registers or
			// accepting any data.  Use subtraction after the lower-bound check
			// instead of address + length, which could overflow TWord.
			const auto pSize = m_dsp.memory().size(MemArea_P);
			if(_val >= pSize || m_length > pSize - _val)
			{
				m_state = State::Error;
				return false;
			}

			// Validate every destination before changing DSP registers.  This
			// keeps rejecting validators and translated ranges from leaving a
			// partially accepted boot stream.
			for(TWord i = 0; i < m_length; ++i)
			{
				if(!m_dsp.memory().canWrite(MemArea_P, _val + i))
				{
					m_state = State::Error;
					return false;
				}
			}

			m_address = _val;
			m_initialPC = _val;

			LOG("DSP Boot: " << m_remaining << " words, initial PC " << HEX(m_address));

			// r0 is used as counter, r1 holds the initial PC
			m_dsp.regs().r[0].var = static_cast<int32_t>(m_address + m_remaining);
			m_dsp.regs().r[1].var = static_cast<int32_t>(m_address);

			m_dsp.regs().sr.var &= ~0xff;	// ccr is cleared before jump to r1
			m_dsp.setPC(m_address);			// jmp (r1)

			m_state = State::Data;
			return false;
		}
		case State::Data:
			if(!m_dsp.memory().set(MemArea_P, m_address, _val))
			{
				// A validator failure is terminal for this stream.  Do not keep
				// consuming words after the caller has been told that the write
				// was not accepted; reset() is the explicit recovery operation.
				m_state = State::Error;
				return false;
			}
			m_dsp.getJit().notifyProgramMemWrite(m_address);
			++m_address;
			if(0 == --m_remaining)
			{
				LOG("DSP Boot: finished");
				m_state = State::Finished;
				return true;
			}
			return false;
		case State::Finished:
			// Completion is an edge, not a persistent level.  The normal host
			// bridge replaces its boot callback after the edge; post-finish
			// words are ignored until reset() starts a new stream.
			return false;
		case State::Error:
			return false;
		}
		assert(false && "invalid state");
		return false;
	}
}
