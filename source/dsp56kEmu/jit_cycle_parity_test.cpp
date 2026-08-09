#include "assembler.h"
#include "dsp.h"
#include "esaiclock.h"
#include "esxi.h"
#include "interrupts.h"
#include "memory.h"
#include "opcodecycles.h"
#include "opcodes.h"
#include "peripherals.h"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace
{
	using namespace dsp56k;

	void require(const bool value, const char* const message)
	{
		if(!value)
			throw std::runtime_error(message);
	}

	struct Fixture final
	{
		Fixture()
			: memory(validator, 0x1000)
			, dsp(memory, &peripherals, &noPeripherals)
		{
			// Configure before the first JIT entry: cached blocks do not acquire a
			// later configuration change. One ordinary-op block and no child links
			// are the J0 boundary contract, not a production pacing API. REP and
			// hardware fast vectors remain architecturally atomic.
			auto config = dsp.getJit().getConfig();
			config.dynamicFastInterrupts = true;
			config.linkJitBlocks = false;
			config.maxInstructionsPerBlock = 1;
			dsp.getJit().setConfig(config);
		}

		TWord install(const TWord pc, const char* const source)
		{
			const auto assembled = assembler.assemble(source);
			require(assembled.success(), "failed to assemble J0 instruction");
			for(uint32_t i = 0; i < assembled.wordCount; ++i)
				require(dsp.memWriteP(pc + i, assembled.word[i]), "failed to install J0 instruction");
			return pc + assembled.wordCount;
		}

		DefaultMemoryValidator validator;
		Memory memory;
		Peripherals56303 peripherals;
		PeripheralsNop noPeripherals;
		DSP dsp;
		Assembler assembler;
	};

	struct Snapshot
	{
		uint64_t instructions;
		uint64_t cycles;
		std::array<TWord, 8> r;
		std::array<TWord, 8> n;
		std::array<TWord, 8> m;
		uint64_t a;
		uint64_t b;
		uint64_t x;
		uint64_t y;
		TWord pc;
		TWord la;
		TWord lc;
		TWord sp;
		TWord sc;
		TWord sr;
		TWord omr;
		TWord vba;
		std::array<uint64_t, 16> ss;
		DSP::ProcessingMode processingMode;
		DSP::TInterruptFunc interruptFunc;
	};

	Snapshot snapshot(const DSP& dsp)
	{
		Snapshot result{dsp.getInstructionCounter(), dsp.getCycles(), {}, {}, {},
			static_cast<uint64_t>(dsp.regs().a.var), static_cast<uint64_t>(dsp.regs().b.var),
			static_cast<uint64_t>(dsp.regs().x.var), static_cast<uint64_t>(dsp.regs().y.var),
			dsp.regs().pc.toWord(), dsp.regs().la.toWord(), dsp.regs().lc.toWord(),
			dsp.regs().sp.toWord(), dsp.regs().sc.toWord(), dsp.getSR().toWord(),
			dsp.regs().omr.toWord(), dsp.regs().vba.toWord(), {}, dsp.getProcessingMode(),
			dsp.getInterruptFunc()};
		for(size_t i = 0; i < result.r.size(); ++i)
		{
			result.r[i] = dsp.regs().r[i].toWord();
			result.n[i] = dsp.regs().n[i].toWord();
			result.m[i] = dsp.regs().m[i].toWord();
		}
		for(size_t i = 0; i < result.ss.size(); ++i)
			result.ss[i] = static_cast<uint64_t>(dsp.regs().ss[i].var);
		return result;
	}

	void requireSame(const DSP& interpreter, const DSP& jit, const char* const message)
	{
		const auto a = snapshot(interpreter);
		const auto b = snapshot(jit);
		const auto same = a.instructions == b.instructions && a.cycles == b.cycles && a.r == b.r && a.n == b.n && a.m == b.m
			&& a.a == b.a && a.b == b.b && a.x == b.x && a.y == b.y && a.pc == b.pc && a.la == b.la
			&& a.lc == b.lc && a.sp == b.sp && a.sc == b.sc && a.sr == b.sr && a.omr == b.omr
			&& a.vba == b.vba && a.ss == b.ss && a.processingMode == b.processingMode
			&& a.interruptFunc == b.interruptFunc;
		if(!same)
		{
			std::fprintf(stderr,
				"J0 parity mismatch: interpreter{ins=%llu cyc=%llu pc=%06x r0=%06x a=%014llx sr=%06x mode=%u} "
				"jit{ins=%llu cyc=%llu pc=%06x r0=%06x a=%014llx sr=%06x mode=%u}\n",
				static_cast<unsigned long long>(a.instructions), static_cast<unsigned long long>(a.cycles), a.pc, a.r[0],
				static_cast<unsigned long long>(a.a), a.sr, static_cast<unsigned>(a.processingMode),
				static_cast<unsigned long long>(b.instructions), static_cast<unsigned long long>(b.cycles), b.pc, b.r[0],
				static_cast<unsigned long long>(b.a), b.sr, static_cast<unsigned>(b.processingMode));
		}
		require(same, message);
	}

	uint32_t independentCycles(const Fixture& fixture, const TWord pc)
	{
		TWord opA, opB;
		fixture.memory.getOpcode(pc, opA, opB);
		Opcodes opcodes;
		Instruction instA, instB;
		opcodes.getInstructionTypes(opA, instA, instB);
		return calcCycles(instA, instB, pc, opA, fixture.memory.getBridgedMemoryAddress(), 1);
	}

	void stepTogether(Fixture& interpreter, Fixture& jit, const char* const message)
	{
		interpreter.dsp.execInterpreter();
		jit.dsp.execJit();
		requireSame(interpreter.dsp, jit.dsp, message);
	}

	void normalAndParallelFixedCostsMatch()
	{
		Fixture interpreter;
		Fixture jit;
		for(auto* fixture : {&interpreter, &jit})
		{
			fixture->install(0x100, "add #$1,a");
			require(fixture->dsp.memWriteP(0x101, 0x44f41b) && fixture->dsp.memWriteP(0x102, 0x000128),
				"failed to install long-immediate parallel anchor");
			fixture->install(0x103, "nop");
			fixture->dsp.setPC(0x100);
		}
		require(independentCycles(interpreter, 0x100) == 1, "normal fixed cycle anchor changed");
		require(independentCycles(interpreter, 0x101) == 3, "parallel fixed cycle anchor changed");
		require(independentCycles(interpreter, 0x103) == 1, "NOP fixed cycle anchor changed");
		stepTogether(interpreter, jit, "JIT normal one-boundary cycle parity failed");
		stepTogether(interpreter, jit, "JIT long-immediate parallel one-boundary cycle parity failed");
		stepTogether(interpreter, jit, "JIT NOP one-boundary cycle parity failed");
	}

	void finiteNestedDoMatches()
	{
		Fixture interpreter;
		Fixture jit;
		for(auto* fixture : {&interpreter, &jit})
		{
			fixture->dsp.regs().a.var = 0x400000;
			fixture->dsp.regs().b.var = 0x00000001000000ull;
			TWord pc = 0x100;
			pc = fixture->install(pc, "do #$2,>$107");
			pc = fixture->install(pc, "do #$2,>$106");
			pc = fixture->install(pc, "add b,a");
			pc = fixture->install(pc, "nop");
			fixture->install(pc, "nop");
			fixture->dsp.setPC(0x100);
		}
		auto runToCompletedTarget = [](DSP& dsp, const bool interpreterPath)
		{
			for(unsigned i = 0; i < 32 && dsp.getPC().toWord() != 0x107; ++i)
			{
				if(interpreterPath)
					dsp.execInterpreter();
				else
					dsp.execJit();
			}
			require(dsp.getPC().toWord() == 0x107, "finite nested DO did not reach its completed target");
		};
		runToCompletedTarget(interpreter.dsp, true);
		runToCompletedTarget(jit.dsp, false);
		requireSame(interpreter.dsp, jit.dsp, "JIT finite nested DO completed-state parity failed");
		require(interpreter.dsp.getInstructionCounter() == 13 && interpreter.dsp.getCycles() == 25,
			"finite nested DO fixed instruction/cycle totals changed");
		require(interpreter.dsp.regs().a.var == 0x0000000004400000ull,
			"finite nested DO fixed accumulator result changed");
		require(interpreter.dsp.regs().sp.toWord() == 0 && interpreter.dsp.regs().sc.toWord() == 0,
			"nested DO did not restore its system-stack depth");
		require((interpreter.dsp.regs().sr.toWord() & SR_LF) == 0,
			"nested DO did not clear the loop flag after its final boundary");
	}

	void repNormalAndDivMatch()
	{
		for(const char* const body : {"add b,a", "div y0,a"})
		{
			Fixture interpreter;
			Fixture jit;
			for(auto* fixture : {&interpreter, &jit})
			{
				fixture->dsp.regs().a.var = 0x00000040000000ull;
				fixture->dsp.regs().b.var = 0x00000001000000ull;
				fixture->dsp.regs().y.var = 0x400000;
				fixture->install(0x100, "rep #$4");
				fixture->install(0x101, body);
				fixture->dsp.setPC(0x100);
			}
			stepTogether(interpreter, jit, "JIT immediate REP cycle parity failed");
		}

		Fixture interpreter;
		Fixture jit;
		for(auto* fixture : {&interpreter, &jit})
		{
			fixture->dsp.regs().a.var = 0;
			fixture->dsp.regs().b.var = 0x00000001000000ull;
			fixture->dsp.regs().x.var = 4;
			fixture->install(0x100, "rep x0");
			fixture->install(0x101, "add b,a");
			fixture->dsp.setPC(0x100);
		}
		stepTogether(interpreter, jit, "JIT dynamic REP cycle parity failed");
	}

	void fastAndLongVectorsStayJitRegressions()
	{
		// execJit() runs m_interruptFunc first and then unconditionally dispatches
		// the current PC. A vector or DefaultPrevent transition can therefore share
		// one outer call with the restored source instruction.
		{
			Fixture jit;
			jit.install(Vba_NMI, "nop");
			jit.install(Vba_NMI + 1, "nop");
			jit.install(0x100, "move #$1,r0");
			jit.install(0x101, "nop");
			jit.dsp.setPC(0x100);
			require(jit.dsp.regs().r[0].toWord() == 0, "fast vector source register precondition failed");
			require(jit.dsp.injectInterrupt(Vba_NMI), "failed to queue fast vector");
			jit.dsp.execJit();
			require(jit.dsp.getPC().toWord() == 0x101 && jit.dsp.regs().r[0].toWord() == 1
				&& jit.dsp.getProcessingMode() == DSP::DefaultPreventInterrupt,
				"fast vector did not execute the restored source under interrupt prevention");
			jit.dsp.execJit();
			require(jit.dsp.getPC().toWord() == 0x102 && jit.dsp.regs().r[0].toWord() == 1
				&& jit.dsp.getProcessingMode() == DSP::Default,
				"fast vector prevent transition did not retire the successor once");
			jit.dsp.execJit();
			require(jit.dsp.getPC().toWord() == 0x103 && jit.dsp.regs().r[0].toWord() == 1
				&& jit.dsp.getProcessingMode() == DSP::Default,
				"fast vector successor did not remain an ordinary JIT boundary");
		}

		auto runLongVector = [](const bool jsrInSecondSlot)
		{
			Fixture jit;
			if(jsrInSecondSlot)
			{
				jit.install(Vba_NMI, "nop");
				jit.install(Vba_NMI + 1, "jsr $300");
			}
			else
			{
				jit.install(Vba_NMI, "jsset #$0,a1,$300");
				jit.dsp.regs().a.var = 0x00000001000000ull;
			}
			jit.install(0x300, "rti");
			jit.install(0x400, "move #$1,r0");
			jit.install(0x401, "nop");
			jit.dsp.setPC(0x400);
			require(jit.dsp.injectInterrupt(Vba_NMI), "failed to queue long vector");
			jit.dsp.execJit();
			require(jit.dsp.getPC().toWord() == 0x400 && jit.dsp.regs().r[0].toWord() == 0
				&& jit.dsp.regs().sp.toWord() == 0 && jit.dsp.getProcessingMode() == DSP::DefaultPreventInterrupt,
				"long vector did not execute RTI and restore its source under interrupt prevention");
			jit.dsp.execJit();
			require(jit.dsp.getPC().toWord() == 0x401 && jit.dsp.regs().r[0].toWord() == 1
				&& jit.dsp.regs().sp.toWord() == 0 && jit.dsp.getProcessingMode() == DSP::Default,
				"long vector prevent transition did not retire the restored source once");
			jit.dsp.execJit();
			require(jit.dsp.getPC().toWord() == 0x402 && jit.dsp.regs().r[0].toWord() == 1
				&& jit.dsp.regs().sp.toWord() == 0 && jit.dsp.getProcessingMode() == DSP::Default,
				"long vector successor did not remain an ordinary JIT boundary");
		};
		runLongVector(false);
		runLongVector(true);
	}

	class WakeProbe final : public Esxi
	{
	public:
		explicit WakeProbe(DSP& dsp) : m_dsp(dsp) {}
		void execTX() override { require(m_dsp.injectInterrupt(Vba_NMI), "failed to inject WAIT wake"); }
		void execRX() override {}
		TWord hasEnabledTransmitters() const override { return 1; }
		TWord hasEnabledReceivers() const override { return 0; }

	private:
		DSP& m_dsp;
	};

	void waitWakeStaysJitRegression()
	{
		Fixture jit;
		WakeProbe jitWake(jit.dsp);
		jit.install(0x100, "wait");
		jit.install(0x101, "move #$1,r0");
		jit.install(0x102, "nop");
		jit.install(Vba_NMI, "nop");
		jit.install(Vba_NMI + 1, "nop");
		auto& clock = jit.peripherals.getEssiClock();
		clock.setCyclesPerSample(1);
		clock.setEsaiDivider(&jitWake, 0);
		jit.dsp.setPC(0x100);
		jit.dsp.execJit();
		require(jit.dsp.getPC().toWord() == 0x101 && jit.dsp.regs().r[0].toWord() == 0
			&& jit.dsp.getProcessingMode() == DSP::Default,
			"WAIT advanced its successor before the wake interrupt was dispatched");
		jit.dsp.execJit();
		require(jit.dsp.getPC().toWord() == 0x102 && jit.dsp.regs().r[0].toWord() == 1
			&& jit.dsp.getProcessingMode() == DSP::DefaultPreventInterrupt,
			"WAIT wake vector did not execute its restored source under interrupt prevention");
		jit.dsp.execJit();
		require(jit.dsp.getPC().toWord() == 0x103 && jit.dsp.regs().r[0].toWord() == 1
			&& jit.dsp.getProcessingMode() == DSP::Default,
			"WAIT source successor did not remain an ordinary JIT boundary");
	}

	void reportDynamicRepZeroJ2Hold()
	{
		// DSP56300 Family Manual Rev. 5, §13 REP (p. 13-160) defines raw zero
		// as 65,536 repetitions with LC restored. All raw-zero REP forms are held:
		// the interpreter underflows its 24-bit LC, dynamic JIT branches around
		// zero, and immediate JIT needs LC/accounting adjudication. Keep this out
		// of J0's green nonzero matrix: J2 must add a bounded 65,536-repeat oracle.
		std::fputs("DSP56300 J0 diagnostic: all REP zero forms must repeat 65536 times (J2 hold)\n", stderr);
	}

}

int main()
{
	try
	{
		if constexpr(dsp56k::g_jitSupported)
		{
			normalAndParallelFixedCostsMatch();
			finiteNestedDoMatches();
			repNormalAndDivMatch();
			fastAndLongVectorsStayJitRegressions();
			waitWakeStaysJitRegression();
			reportDynamicRepZeroJ2Hold();
		}
		else
		{
			std::fputs("DSP56300 JIT cycle parity skipped: JIT unsupported on this backend\n", stderr);
		}
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "DSP56300 JIT cycle parity test failed: %s\n", error.what());
		return 1;
	}
	return 0;
}
