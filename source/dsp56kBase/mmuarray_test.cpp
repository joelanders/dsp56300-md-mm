#include "mmuarray.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
	[[noreturn]] void fail(const char* const _message)
	{
		std::fprintf(stderr, "mmuArrayTest: %s\n", _message);
		std::exit(1);
	}

	void expect(const bool _condition, const char* const _message)
	{
		if(!_condition)
			fail(_message);
	}
}

int main()
{
	constexpr uint32_t defaultValue = 0x12345678;
	dsp56k::MmuArray<uint32_t> values;
	values.init(33, defaultValue, 8);

	// The tested growth policy is used only by the non-MMU path. A host with a
	// working MMU mapping already reserves the complete range during init.
	if(values.usesMMU())
	{
		std::puts("mmuArrayTest: SKIP fallback checks (MMU mode active)");
		return 0;
	}

	expect(values.size() == 0, "fallback array allocated eagerly");
	expect(values.ensureBlockForIndex(2), "initial fallback growth was not reported");
	expect(values.size() == 8, "initial fallback growth was not block-aligned");
	expect(values[2] == defaultValue && values[7] == defaultValue,
		"fill function did not initialize the complete fallback block");

	values[2] = 42;
	expect(!values.ensureBlockForIndex(6), "existing fallback block grew again");
	expect(values[2] == 42, "existing fallback value was not preserved");

	expect(values.ensureBlockForIndex(9), "second fallback block growth was not reported");
	expect(values.size() == 16, "second fallback growth was not block-aligned");
	expect(values[2] == 42 && values[9] == defaultValue && values[15] == defaultValue,
		"fallback growth did not preserve or fill values");

	expect(values.ensureBlockForIndex(32), "final fallback growth was not reported");
	expect(values.size() == 33, "fallback growth exceeded the declared maximum");
	expect(values[32] == defaultValue, "final partial block was not initialized");

	std::puts("mmuArrayTest: PASS");
	return 0;
}
