#include "memorybuffertests.h"

#include <sstream>
#include <string>
#include <vector>

#include "memory.h"
#include "dsp56kBase/logging.h"
#include "memorybuffer.h"

namespace dsp56k
{
	namespace
	{
		constexpr TWord g_size = 0x800000;
		constexpr TWord g_bridge = 0x020000;

		void expectZero(const char* _what, const TWord* _mem, const TWord _address)
		{
			if(_mem[_address] == 0)
				return;
			std::stringstream ss;
			ss << _what << " " << HEX(_address) << " is " << HEX(_mem[_address]) << " after construction, expected 0";
			throw ss.str();
		}

		// The addresses MemoryBuffer's self test writes: X, Y and P just below the bridged address and its first three words
		void expectTestAddressesZero(const char* _path, const TWord* _x, const TWord* _y, const TWord* _p)
		{
			const std::string path(_path);
			for(const TWord a : {g_bridge - 1, g_bridge, g_bridge + 1, g_bridge + 2})
			{
				expectZero((path + " X").c_str(), _x, a);
				expectZero((path + " Y").c_str(), _y, a);
				expectZero((path + " P").c_str(), _p, a);
			}
		}
	}

	MemoryBufferTests::MemoryBufferTests()
	{
		// the MMU path directly, where the host supports it
		{
			const MemoryBuffer buffer(g_size, g_bridge, g_bridge);
			if(buffer.isValid())
				expectTestAddressesZero("MMU buffer", buffer.ptrX(), buffer.ptrY(), buffer.ptrP());
		}
		// a Memory as a machine creates it (the MMU path if it works, else the external buffer)
		{
			DefaultMemoryValidator validator;
			std::vector<TWord> external(Memory::calcMemSize(g_size, g_size, g_bridge), 0);
			Memory mem(validator, g_size, g_size, g_bridge, external.data());
			expectTestAddressesZero("Memory", mem.getMemAreaPtr(MemArea_X), mem.getMemAreaPtr(MemArea_Y), mem.getMemAreaPtr(MemArea_P));
			for(const TWord a : {g_bridge - 1, g_bridge, g_bridge + 1, g_bridge + 2})
			{
				for(const auto area : {MemArea_X, MemArea_Y, MemArea_P})
				{
					if(mem.get(area, a) != 0)
					{
						std::stringstream ss;
						ss << "Memory::get area " << area << " " << HEX(a) << " is " << HEX(mem.get(area, a)) << ", expected 0";
						throw ss.str();
					}
				}
			}
		}
	}
}
