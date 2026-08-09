#include "dsp.h"
#include "dspBootCode.h"
#include "memory.h"
#include "peripherals.h"

#include <cstdio>
#include <stdexcept>

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
		Memory memory{validator, 8};
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp{memory, &peripherals, &noPeripherals};
		DspBoot boot{dsp};
	};

	struct RejectingValidator final : IMemoryValidator
	{
		bool memValidateAccess(const EMemArea _area, const TWord _addr,
			const bool _write) const override
		{
			return _area != MemArea_P || !_write || _addr != 2;
		}
	};

	void testValidUploadAndSingleCompletionEdge()
	{
		Fixture fixture;

		require(!fixture.boot.finished(), "boot unexpectedly started finished");
		require(!fixture.boot.failed(), "boot unexpectedly started failed");
		require(!fixture.boot.hdiWriteTX(2), "length word reported completion");
		require(!fixture.boot.hdiWriteTX(3), "address word reported completion");
		require(!fixture.boot.hdiWriteTX(0x123456), "first data word reported completion");
		require(fixture.boot.hdiWriteTX(0xabcdef), "last data word did not report completion");
		require(fixture.boot.finished(), "valid upload did not finish");
		require(!fixture.boot.failed(), "valid upload entered the error state");
		require(fixture.boot.getLength() == 2, "boot length was not retained");
		require(fixture.boot.getInitialPC() == 3, "boot initial PC was not retained");
		require(fixture.dsp.getPC().toWord() == 3, "boot did not set the initial PC");
		require(fixture.memory.get(MemArea_P, 3) == 0x123456, "first data word was not written");
		require(fixture.memory.get(MemArea_P, 4) == 0xabcdef, "last data word was not written");

		// The bridge consumes the completion edge and switches away from DspBoot.
		// A stray post-finish word must not retrigger completion or write memory.
		require(!fixture.boot.hdiWriteTX(0x654321), "post-finish write retriggered completion");
		require(fixture.memory.get(MemArea_P, 3) == 0x123456, "post-finish write changed memory");
	}

	void testMalformedRangesAreRejectedWithoutPartialWrites()
	{
		Fixture fixture;

		// Zero length must not enter Data (where the old implementation
		// underflowed its remaining-word counter and never completed).
		require(!fixture.boot.hdiWriteTX(0), "zero-length upload was accepted");
		require(!fixture.boot.finished(), "zero-length upload finished");
		require(fixture.boot.failed(), "zero-length upload was not terminal");

		fixture.boot.reset();

		// The final P word plus length 2 is invalid.  The range is rejected
		// before PC/register changes or data writes.
		const auto invalidAddress = fixture.memory.size(MemArea_P) - 1;
		require(!fixture.boot.hdiWriteTX(2), "invalid-range length was accepted");
		require(!fixture.boot.hdiWriteTX(invalidAddress), "invalid-range address was accepted");
		require(!fixture.boot.finished(), "invalid-range upload finished");
		require(fixture.boot.failed(), "invalid-range upload was not terminal");
		require(fixture.dsp.getPC().toWord() == 0, "invalid range changed the PC");
		require(fixture.memory.get(MemArea_P, invalidAddress) == 0, "invalid range partially wrote memory");

		// An address near the TWord limit must be rejected without address +
		// length arithmetic overflowing.
		fixture.boot.reset();
		require(!fixture.boot.hdiWriteTX(1), "overflow-test length was accepted");
		require(!fixture.boot.hdiWriteTX(0xffffffffu), "overflow-test address was accepted");
		require(!fixture.boot.finished(), "overflow-test upload finished");
		require(fixture.boot.failed(), "overflow-test upload was not terminal");
	}

	void testMemoryValidationWithoutMmu()
	{
		Fixture fixture;
		require(!fixture.memory.hasMmuSupport(), "small fixture unexpectedly used the MMU");
		require(fixture.memory.canWrite(MemArea_P, 1), "valid non-MMU write was rejected");
		require(fixture.memory.set(MemArea_P, 1, 0x123456), "non-MMU write failed");
		require(fixture.memory.get(MemArea_P, 1) == 0x123456, "non-MMU readback failed");
		require(!fixture.memory.canWrite(MemArea_P, fixture.memory.size(MemArea_P)),
			"out-of-range non-MMU write was accepted");

		RejectingValidator validator;
		Memory memory{validator, 8};
		require(!memory.canWrite(MemArea_P, 2), "rejecting validator was bypassed");
	}

	void testRejectingValidatorLeavesBootUntouched()
	{
		RejectingValidator validator;
		Memory memory{validator, 8};
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp{memory, &peripherals, &noPeripherals};
		DspBoot boot{dsp};
		const auto initialPC = dsp.getPC().toWord();
		const auto initialR0 = dsp.regs().r[0].var;
		const auto initialR1 = dsp.regs().r[1].var;
		const auto initialData = memory.get(MemArea_P, 2);

		require(!boot.hdiWriteTX(2), "rejecting upload length reported completion");
		require(!boot.hdiWriteTX(2), "rejecting upload address reported completion");
		require(boot.failed(), "rejecting validator did not make upload terminal");
		require(!boot.finished(), "rejecting validator marked upload finished");
		require(dsp.getPC().toWord() == initialPC, "rejecting validator changed the PC");
		require(dsp.regs().r[0].var == initialR0 && dsp.regs().r[1].var == initialR1,
			"rejecting validator changed DSP boot registers");
		require(memory.get(MemArea_P, 2) == initialData, "rejecting validator partially wrote memory");
	}

	void testResetRestartsProtocolWithoutErasingMemory()
	{
		Fixture fixture;

		require(!fixture.boot.hdiWriteTX(2), "partial length reported completion");
		require(!fixture.boot.hdiWriteTX(5), "partial address reported completion");
		require(!fixture.boot.hdiWriteTX(0xabcdef), "partial data reported completion");
		fixture.boot.reset();

		require(!fixture.boot.finished(), "reset left boot finished");
		require(!fixture.boot.failed(), "reset left boot failed");
		require(fixture.boot.getLength() == 0, "reset left stale boot length");
		require(fixture.boot.getInitialPC() == 0, "reset left stale initial PC");
		require(fixture.memory.get(MemArea_P, 5) == 0xabcdef, "protocol reset erased DSP memory");

		require(!fixture.boot.hdiWriteTX(1), "reset length reported completion");
		require(!fixture.boot.hdiWriteTX(1), "reset address reported completion");
		require(fixture.boot.hdiWriteTX(0x123456), "upload after reset did not finish");
		require(fixture.boot.finished(), "upload after reset is not finished");
		require(fixture.memory.get(MemArea_P, 1) == 0x123456, "upload after reset was not written");
	}
}

int main()
{
	try
	{
		testValidUploadAndSingleCompletionEdge();
		testMalformedRangesAreRejectedWithoutPartialWrites();
		testMemoryValidationWithoutMmu();
		testRejectingValidatorLeavesBootUntouched();
		testResetRestartsProtocolWithoutErasingMemory();
	}
	catch(const std::exception& error)
	{
		std::fputs(error.what(), stderr);
		return 1;
	}
	return 0;
}
