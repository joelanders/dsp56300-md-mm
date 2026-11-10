// Differential test: writes to X/Y halves (x0, x1, y0, y1) or long moves into X/Y must survive a later read of the
// whole 48-bit register in the same JIT block. Every program is run on the interpreter and on the JIT under several
// block configurations, and A, B, X and Y must match.

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace dsp56k;

	constexpr TWord g_start = 0x100;
	constexpr TWord g_halt = 0x300;

	struct Fixture final
	{
		explicit Fixture(const std::function<void(JitConfig&)>& _configure)
			: memory(validator, 0x1000)
			, dsp(memory, &peripherals, &noPeripherals)
		{
			if(_configure)
			{
				auto config = dsp.getJit().getConfig();
				_configure(config);
				dsp.getJit().setConfig(config);
			}
		}

		void install(TWord _pc, const std::vector<std::string>& _lines)
		{
			for(const auto& line : _lines)
			{
				const auto assembled = assembler.assemble(line.c_str());
				if(!assembled.success())
					throw std::runtime_error("failed to assemble: " + line);
				for(uint32_t i = 0; i < assembled.wordCount; ++i)
					dsp.memWriteP(_pc++, assembled.word[i]);
			}
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	struct State
	{
		uint64_t a, b, x, y;
		bool halted;
	};

	struct Program
	{
		const char* name;
		std::vector<std::string> lines;	// the trailing "jmp $300" is appended
	};

	std::vector<Program> programs()
	{
		return {
			{"L-load X, add x,a", {"move l:<$10,x", "add x,a"}},
			{"x0/x1 immediates, add x,a", {"move #>$444444,x0", "move #>$555555,x1", "add x,a"}},
			{"L-load Y, add y,a", {"move l:<$10,y", "add y,a"}},
			{"x0 only, add x,a", {"move #>$444444,x0", "add x,a"}},
			{"y1 only, sub y,a", {"move #>$444444,y1", "sub y,a"}},
			{"x0 only, move x,l:", {"move #>$444444,x0", "move x,l:<$20"}},
			{"x0 then merge then x1 rewritten", {"move #>$444444,x0", "add x,a", "move #>$555555,x1"}},
			// DO loop: x1/x0 written, X read, then a 3-pass loop body that reads X again
			{"DO loop after merge", {"move #>$444444,x1", "move #>$123456,x0", "add x,b",
				"do #3,$109",	// two-word moves at $100/$102, add at $104, do at $105, body $107-$108
				"add x,a", "nop"}},
			// b = 0 at entry, so after tst b Z=1: ifeq is taken, ifne is not
			{"IFcc taken (x0, add x,a ifeq)", {"tst b", "move #>$444444,x0", "add x,a ifeq"}},
			{"IFcc not taken (x0, add x,a ifne)", {"tst b", "move #>$444444,x0", "add x,a ifne"}},
			{"IFcc not taken (y1, add y,a ifne)", {"tst b", "move #>$444444,y1", "add y,a ifne"}},
			{"IFcc not taken, both halves (x0+x1, add x,a ifne)", {"tst b", "move #>$444444,x0", "move #>$555555,x1", "add x,a ifne"}},
		};
	}

	State run(const Program& _program, const std::function<void(JitConfig&)>& _configure, bool _useJit)
	{
		Fixture f(_configure);

		auto lines = _program.lines;
		lines.emplace_back("jmp $300");
		f.install(g_start, lines);
		f.install(g_halt, {"jmp $300"});

		f.dsp.memWrite(MemArea_X, 0x10, 0xaaaaaa);
		f.dsp.memWrite(MemArea_Y, 0x10, 0xbbbbbb);

		auto& r = f.dsp.regs();
		r.x.var = 0x222222111111ull;
		r.y.var = 0x777777666666ull;
		r.a.var = 0;
		r.b.var = 0;
		f.dsp.setPC(g_start);

		bool halted = false;
		for(int i = 0; i < 1000; ++i)
		{
			if(f.dsp.getPC().toWord() == g_halt)
			{
				halted = true;
				break;
			}
			if(_useJit)
				f.dsp.execJit();
			else
				f.dsp.execInterpreter();
		}

		return {static_cast<uint64_t>(r.a.var), static_cast<uint64_t>(r.b.var),
			static_cast<uint64_t>(r.x.var), static_cast<uint64_t>(r.y.var), halted};
	}

	void print(const char* _label, const State& _s)
	{
		std::fprintf(stderr, "      %-6s A=%014llx B=%014llx X=%012llx Y=%012llx%s\n", _label,
			static_cast<unsigned long long>(_s.a), static_cast<unsigned long long>(_s.b),
			static_cast<unsigned long long>(_s.x), static_cast<unsigned long long>(_s.y),
			_s.halted ? "" : " (did not reach halt)");
	}
}

int main()
{
	struct Config
	{
		const char* name;
		std::function<void(JitConfig&)> apply;
	};

	const std::vector<Config> configs = {
		{"default", nullptr},
		{"no linking", [](JitConfig& c) { c.linkJitBlocks = false; }},
		{"optimizer off", [](JitConfig& c) { c.enableOptimizer = false; }},
		{"max 1", [](JitConfig& c) { c.maxInstructionsPerBlock = 1; }},
		{"max 2", [](JitConfig& c) { c.maxInstructionsPerBlock = 2; }},
		{"max 3", [](JitConfig& c) { c.maxInstructionsPerBlock = 3; }},
		{"max 4", [](JitConfig& c) { c.maxInstructionsPerBlock = 4; }},
		{"max 32", [](JitConfig& c) { c.maxInstructionsPerBlock = 32; }},
		{"maxDoIterations 1", [](JitConfig& c) { c.maxDoIterations = 1; }},
	};

	int runs = 0;
	int failures = 0;

	try
	{
		for(const auto& program : programs())
		{
			const auto expected = run(program, nullptr, false);
			if(!expected.halted)
			{
				std::fprintf(stderr, "interpreter did not halt: %s\n", program.name);
				return 2;
			}

			for(const auto& config : configs)
			{
				// Known separate crash: on this DSP version the JIT executes an illegal instruction for a DO loop with
				// maxDoIterations 1, with or without the X/Y fix. Skipped until that is fixed.
				if(std::string(program.name) == "DO loop after merge" && std::string(config.name) == "maxDoIterations 1")
					continue;
				++runs;
				const auto actual = run(program, config.apply, true);
				const bool same = actual.halted && actual.a == expected.a && actual.b == expected.b
					&& actual.x == expected.x && actual.y == expected.y;
				if(!same)
				{
					++failures;
					std::fprintf(stderr, "FAIL  %-50s [%s]\n", program.name, config.name);
					print("interp", expected);
					print("jit", actual);
				}
			}
		}
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}

	std::printf("%d of %d JIT runs differ from the interpreter\n", failures, runs);
	return failures == 0 ? 0 : 1;
}
