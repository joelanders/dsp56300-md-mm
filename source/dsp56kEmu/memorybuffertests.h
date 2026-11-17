#pragma once

namespace dsp56k
{
	// The memory a DSP starts with: zero at every address, with the MMU based buffer (MemoryBuffer) as without it.
	// Throws a std::string on a failure.
	class MemoryBufferTests
	{
	public:
		MemoryBufferTests();
	};
}
