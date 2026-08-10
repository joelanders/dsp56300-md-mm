#pragma once

#include <vector>
#include <functional>
#include <atomic>

#include "opcodetypes.h"
#include "types.h"
#include "dsp56kBase/ringbuffer.h"
#include "utils.h"
#include "dma.h"

namespace dsp56k
{
	class IPeripherals;
	class Disassembler;

	class HDI08
	{
	public:
		explicit HDI08(IPeripherals& _peripheral);

		enum Addresses
		{
			HCR		= 0xFFFFC2,					// Host Control Register (HCR)
			HSR		= 0xFFFFC3,					// Host Status Register (HSR)
			HPCR	= 0xFFFFC4,					// Host Port Control Register (HPCR)
			HBAR	= 0xFFFFC5,					// Host Base Address Register (HBAR)
			HORX	= 0xFFFFC6,					// Host Receive Register (HORX)
			HOTX	= 0xFFFFC7,					// Host Transmit Register (HOTX)
			HDDR	= 0xFFFFC8,					// Host Data Direction Register (HDDR)
			HDR		= 0xFFFFC9					// Host Data Register (HDR)
		};

		enum HostStatusRegisterBits
		{
			HSR_HRDF,						// Receive Data Full
			HSR_HTDE,						// Transmit Data Empty
			HSR_HCP,						// Host Command Pending
			HSR_HF0,						// Host Flag 0
			HSR_HF1,						// Host Flag 1
			HSR_DMA = 7,					// DMA Status
		};

		enum HostPortControlRegisterBits
		{
			HPCR_HEN = 6,					// HostEnable
		};
		
		enum HostControlRegisterBits
		{
			HCR_HRIE,					// Host Receive Interrupt Enable
			HCR_HTIE,					// Host Transmit Interrupt Enable
			HCR_HCIE,					// Host Command Interrupt Enable
			HCR_HF2,					// HCR Host Flags 2,3 (HF2,HF3) Bits 3-4
			HCR_HF3,
			HCR_HDM0,					// HCR Host DMA Mode Control Bits (HDM0, HDM1, HDM2) Bits 5-7
			HCR_HDM1,
			HCR_HDM2,
		};

		using CallbackTx = std::function<void()>;
		using CallbackRx = std::function<void()>;
		using CallbackHostStateChanged = std::function<void()>;

		enum class HostTxRetention
		{
			Legacy,
			SingleRegister,
			BoundedFifo,
		};

		enum class HostCommandAdmission
		{
			Accepted,
			Disabled,
			Busy,
			InterruptQueueFull,
			InvalidVector,
		};

		TWord readStatusRegister();

		TWord readControlRegister() const
		{
			// HF2/HF3 live here and are written by the DSP thread; the UC thread polls this during boot.
			return m_hcr.load(std::memory_order_acquire);
		}

		TWord readPortControlRegister() const
		{
			return m_hpcr;
		}

		void writeControlRegister(TWord _val);

		void writeStatusRegister(const TWord _val);

		void writePortControlRegister(const TWord _val);

		bool hasTX() const
		{
			return !m_dataTX.empty();
		}
		TWord readTX();
		bool tryReadTX(TWord& _word) noexcept;
		void writeTX(TWord _val);
		size_t txWordsAvailable() const { return m_dataTX.size(); }
		// Quiescent owner reset. As with readTX(), HTDE is recomputed by the
		// next peripheral service after this wakes it.
		void clearTX();

		uint32_t exec() noexcept;

		TWord readRX(Instruction _inst);

		void writeRX(const std::vector<TWord>& _data)		{ writeRX(_data.data(), _data.size()); }
		void writeRX(const TWord* _data, size_t _count);
		bool tryWriteRX(TWord _word) noexcept;
		void clearRX();
		size_t rxWordsAvailable() const { return m_dataRX.size(); }
		size_t rxWordsFree() const { return m_dataRX.remaining(); }

		const auto& rxData() const { return m_dataRX; }
		const auto& txData() const { return m_dataTX; }

		bool hasRXData() const {return !m_dataRX.empty();}

		void setPendingHostFlags01(uint32_t _pendingHostFlags)
		{
			m_pendingHostFlags01.store(static_cast<int32_t>(_pendingHostFlags), std::memory_order_release);
		}

		bool hasPendingHostFlags01() const
		{
			return m_pendingHostFlags01.load(std::memory_order_acquire) >= 0;
		}

		void setHostFlags(uint8_t _flag0, uint8_t _flag1);
		void setHostFlagsWithWait(uint8_t _flag0, uint8_t _flag1);
		bool needsToWaitForHostFlags(uint8_t _flag0, uint8_t _flag1) const;
		void waitUntilBufferEmpty() const;

		void reset();

		bool dataRXFull() const;

		void terminate();

		TWord readHDR() const;
		void writeHDR(TWord _val);

		TWord readHDDR() const;
		void writeHDDR(TWord _val);

		void setTransmitDataAlwaysEmpty(bool _alwaysEmpty)
		{
			m_transmitDataAlwaysEmpty = _alwaysEmpty;
		}

		// One-time, quiescent opt-in host retention policy. The default Legacy state
		// preserves pre-existing behavior; bounded modes never wait at capacity.
		// BoundedFifo retains the ring's SPSC contract. SingleRegister overwrite and
		// host reads must instead be serialized by one execution owner.
		bool configureHostTxRetention(HostTxRetention _retention, size_t _limit = 1) noexcept;
		HostTxRetention hostTxRetention() const { return m_hostTxRetention; }
		size_t hostTxAdmissionLimit() const { return m_hostTxAdmissionLimit; }
		size_t hostTxOverflowCount() const
		{
			return m_hostTxOverflowCount.load(std::memory_order_acquire);
		}
		bool hostTxBackpressured() const
		{
			return m_hostTxRetention == HostTxRetention::BoundedFifo
				&& m_dataTX.size() >= m_hostTxAdmissionLimit;
		}

		// Opt-in, single-owner native host-command seam. A command raises HCP,
		// prevents serviceable mainline HORX consumption, and enters through the
		// ordinary external-interrupt FIFO. HCP clears at exact vector dispatch.
		// Admission, HCR writes, DSP execution, cancellation, and reset are serialized
		// by one execution owner. Disabling while pending fails.
		bool setNativeHostCommandsEnabled(bool _enabled);
		bool nativeHostCommandsEnabled() const { return m_nativeHostCommands; }
		HostCommandAdmission tryWriteHostCommand(TWord _vba) noexcept;
		// Quiescent reset helper. Removes this command by its exact DSP queue token,
		// even after it moved from the external FIFO into the core pending FIFO.
		bool cancelPendingHostCommand() noexcept;
		bool hostCommandPending() const
		{
			return m_hostCommandPending;
		}
		// Single-owner monotonic observation seam. This advances only when the exact
		// tagged native command reaches vector dispatch, never on cancellation or
		// reset, so an owner can distinguish service from teardown.
		uint64_t hostCommandDispatchGeneration() const
		{
			return m_hostCommandDispatchGeneration;
		}

		static void setSymbols(Disassembler& _disasm);

		void injectTXInterrupt();

		bool txInterruptEnabled() const
		{
			return dsp56k::bittest<TWord, HCR_HTIE>(m_hcr.load(std::memory_order_acquire));
		}

		bool rxInterruptEnabled() const
		{
			return dsp56k::bittest<TWord, HCR_HRIE>(m_hcr.load(std::memory_order_acquire));
		}

		void setWriteTxCallback(const CallbackTx& _callback)
		{
			m_callbackTx = _callback;
		}

		void setRXRateLimit(uint32_t _rateLimit)
		{
			m_rxRateLimit = _rateLimit;
		}

		void setReadRxCallback(const CallbackRx& _callback)
		{
			m_callbackRx = _callback;

			if(!m_callbackRx)
				m_callbackRx = [] {};
		}

		// Synchronous wake notification only. The callback must not re-enter HCR,
		// command, reset, or transport configuration; owners inspect/service state
		// after the current architectural boundary has completed.
		void setHostStateChangedCallback(const CallbackHostStateChanged& _callback)
		{
			m_callbackHostStateChanged = _callback;

			if(!m_callbackHostStateChanged)
				m_callbackHostStateChanged = [] {};
		}

	private:
		bool hostCommandHoldActive() const;
		bool publishPendingHostCommand() noexcept;
		bool unpublishPendingHostCommand() noexcept;
		void onInterruptServiced(TWord _vba, uint64_t _token) noexcept;
		TWord popTX();
		bool dmaTriggerReceive() const;
		bool dmaTriggerTransmit() const;
		bool hasDmaReceiveTrigger() const;

		TWord m_hsr = 0;
		std::atomic<TWord> m_hcr{0};		// HF2/HF3: DSP thread writes, UC thread reads (boot HF3 handshake)
		TWord m_hpcr = 0;
		RingBuffer<TWord, 8192, true> m_dataRX;
		RingBuffer<TWord, 8192, true> m_dataTX;
		IPeripherals& m_periph;
		std::atomic<uint32_t> m_pendingTXInterrupts;
		uint64_t m_lastRXClock = 0;
		TWord m_hdr = 0;
		TWord m_hddr = 0;
		bool m_transmitDataAlwaysEmpty = true;
		HostTxRetention m_hostTxRetention = HostTxRetention::Legacy;
		size_t m_hostTxAdmissionLimit = 8192;
		std::atomic<size_t> m_hostTxOverflowCount{0};
		CallbackTx m_callbackTx;
		CallbackRx m_callbackRx = [] {};
		CallbackHostStateChanged m_callbackHostStateChanged = [] {};
		uint32_t m_rxRateLimit;		// minimum number of instructions between two RX interrupts
		bool m_waitServeRXInterrupt = false;
		std::atomic<int32_t> m_pendingHostFlags01{-1};	// pulse latch: UC/queue thread sets, DSP thread reads+clears
		bool m_nativeHostCommands = false;
		bool m_hostCommandPending = false;
		bool m_hostCommandPublished = false;
		mutable bool m_hostCommandPayloadHold = false;
		TWord m_hostCommandVba = 0;
		uint64_t m_hostCommandToken = 0;
		uint64_t m_hostCommandDispatchGeneration = 0;
		TWord m_lastRXValue = 0;

		DmaChannel::RequestSource m_dmaReqSourceReceive;
		DmaChannel::RequestSource m_dmaReqSourceTransmit;
		Dma* m_dma;
	};
}
