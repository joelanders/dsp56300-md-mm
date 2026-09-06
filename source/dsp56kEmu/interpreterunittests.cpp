#include "interpreterunittests.h"

#include "agu.h"
#include "dsp.h"
#include "memory.h"

namespace dsp56k
{
	InterpreterUnitTests::InterpreterUnitTests()
	{
		testCCCC();
		testSubr();
		testCycleAccounting();
		testCooperativeDo();
		testMemoryDoCounts();
		testDeferredCCR();
		
		runAllTests();
	}

	void InterpreterUnitTests::testDeferredCCR()
	{
		// Cache contract: every subset of E/U/N can be replaced independently.
		// These no-scaling results have E/U/N respectively all clear or all set.
		dsp.setALU(false, TReg56(0x00400000000000ll));
		dsp.setALU(true, TReg56(0xff000000000000ll));
		constexpr uint32_t flags = CCR_E | CCR_U | CCR_N;
		constexpr TWord preserved = CCR_S | CCR_L | CCR_Z | CCR_V | CCR_C;
		for(const bool firstSet : {false, true})
		for(uint32_t oldSubset = 0; oldSubset < 8; ++oldSubset)
		for(uint32_t newSubset = 0; newSubset < 8; ++newSubset)
		{
			const auto oldMask = oldSubset << CCRB_N;
			const auto newMask = newSubset << CCRB_N;
			const auto oldFlags = firstSet ? flags : 0u;
			const auto newFlags = firstSet ? 0u : flags;
			const auto initial = preserved | newFlags;
			dsp.setSR(initial);
			dsp.setCCRDirty(firstSet, firstSet ? dsp.regs().b : dsp.regs().a, oldMask);
			dsp.setCCRDirty(!firstSet, firstSet ? dsp.regs().a : dsp.regs().b, newMask);
			const auto expected = (initial & ~(oldMask | newMask))
				| (oldFlags & oldMask & ~newMask) | (newFlags & newMask);
			verify(dsp.getSR().var == expected);
			verify(dsp.ccrCache.dirty == 0);
			verify(dsp.getSR().var == expected); // repeated reads are idempotent
		}

		// Cover the mask-set, mask-clear and bit-value write entry points.
		for(const bool pendingSet : {false, true})
		for(uint32_t subset = 1; subset < 8; ++subset)
		for(const bool explicitSet : {false, true})
		{
			const auto mask = static_cast<CCRMask>(subset << CCRB_N);
			dsp.setSR(preserved);
			dsp.setCCRDirty(pendingSet, pendingSet ? dsp.regs().b : dsp.regs().a, flags);
			if(explicitSet)
				dsp.sr_set(mask);
			else
				dsp.sr_clear(mask);
			const auto expected = preserved | ((pendingSet ? flags : 0u) & ~mask)
				| (explicitSet ? static_cast<uint32_t>(mask) : 0u);
			verify(dsp.ccrCache.dirty == (flags & ~mask));
			verify(dsp.getSR().var == expected);
		}
		for(const bool pendingSet : {false, true})
		for(const auto bit : {CCRB_E, CCRB_U, CCRB_N})
		for(const bool explicitSet : {false, true})
		{
			const auto mask = 1u << bit;
			dsp.setSR(preserved);
			dsp.setCCRDirty(pendingSet, pendingSet ? dsp.regs().b : dsp.regs().a, flags);
			dsp.sr_toggle(bit, Bit(explicitSet));
			const auto expected = preserved | ((pendingSet ? flags : 0u) & ~mask)
				| (explicitSet ? mask : 0u);
			verify(dsp.ccrCache.dirty == (flags & ~mask));
			verify(dsp.getSR().var == expected);
		}
	}

	void InterpreterUnitTests::testCycleAccounting()
	{
		if constexpr(g_useJIT)
		{
			// Normal JIT builds must not pay for the interpreter-only per-PC cache.
			verify(dsp.m_opcodeCycleCache.empty());
			return;
		}

		verify(dsp.m_opcodeCycleCache.size() == dsp.memory().sizeP());

		// A cached instruction cost is used on execution and invalidated by P writes.
		dsp.resetHW();
		execOpcode(assembler.assemble("nop").word[0], 0, false, 0x100);
		verify(dsp.getCycles() == 1);
		verify(dsp.m_opcodeCycleCache[0x100] == 1);

		const auto andi = assembler.assemble("andi #$33,mr");
		verify(andi.success());
		dsp.memWriteP(0x100, andi.word[0]);
		if(andi.wordCount > 1)
			dsp.memWriteP(0x101, andi.word[1]);
		verify(dsp.m_opcodeCycleCache[0x100] == 0);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getCycles() == 4);
		verify(dsp.m_opcodeCycleCache[0x100] == 3);

		// REP executes its own instruction plus the repeated body inside one interpreter step.
		dsp.resetHW();
		TWord pc = 0x100;
		pc = emitToMemory("rep #$4", pc);
		emitToMemory("nop", pc);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getCycles() == 9); // REP (5) + four NOPs (1 each)

		// DO setup must return before its body so the host scheduler can run.
		dsp.resetHW();
		pc = 0x100;
		pc = emitToMemory("do #$5,>$104", pc);
		pc = emitToMemory("nop", pc);
		emitToMemory("nop", pc);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getPC().toWord() == 0x102);
		verify(dsp.getCycles() == 5);
		execUntil(0x104);
		verify(dsp.getCycles() == 15); // DO (5) + five two-NOP iterations
	}

	void InterpreterUnitTests::testCooperativeDo()
	{
		// Independently assembled host-polling loop. X:$20 stands for a value
		// supplied by the host between DSP steps; no firmware image is involved.
		dsp.resetHW();
		dsp.memory().set(MemArea_X, 0x20, 0);
		emitToMemory("do #$2,>$108", 0x100);
		emitToMemory("move x:>$20,x0", 0x102);
		emitToMemory("brclr #0,x0,>$fffffe", 0x104);
		emitToMemory("nop", 0x106);
		emitToMemory("nop", 0x107);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getPC().toWord() == 0x102);
		for(unsigned step = 0; step < 8; ++step)
			dsp.execInterpreter();
		verify(dsp.getPC().toWord() == 0x102);
		verify(dsp.regs().lc.toWord() == 2);
		dsp.memory().set(MemArea_X, 0x20, 1);
		execUntil(0x108);
		verify(!dsp.sr_test_noCache(SR_LF));
		verify(dsp.regs().sc.toWord() == 0);

		// Nested loops restore the outer LA/LC, then the caller's original state.
		dsp.resetHW();
		dsp.setALU(false, TReg56(0));
		dsp.setALU(true, TReg56(0));
		dsp.regs().la.var = 0x321;
		dsp.regs().lc.var = 7;
		emitToMemory("do #$2,>$10a", 0x100);
		emitToMemory("do #$3,>$106", 0x102);
		emitToMemory("inc a", 0x104);
		emitToMemory("nop", 0x105);
		emitToMemory("inc b", 0x106);
		for(TWord pc = 0x107; pc < 0x10a; ++pc)
			emitToMemory("nop", pc);
		dsp.setPC(0x100);
		execUntil(0x10a);
		verify(dsp.aluA().var == 6 && dsp.aluB().var == 2);
		verify(dsp.regs().la.var == 0x321 && dsp.regs().lc.var == 7);
		verify(dsp.regs().sc.var == 0 && !dsp.sr_test_noCache(SR_LF));

		// ENDDO terminates early without jumping over the remaining instructions.
		dsp.resetHW();
		dsp.setALU(false, TReg56(0));
		dsp.setALU(true, TReg56(0));
		emitToMemory("do #$5,>$106", 0x100);
		emitToMemory("inc a", 0x102);
		emitToMemory("enddo", 0x103);
		emitToMemory("nop", 0x104);
		emitToMemory("inc b", 0x105);
		dsp.setPC(0x100);
		execUntil(0x106);
		verify(dsp.aluA().var == 1 && dsp.aluB().var == 1);
		verify(dsp.regs().sc.var == 0 && !dsp.sr_test_noCache(SR_LF));

		// A zero count in native (non-SC) mode skips the body without stacking.
		dsp.resetHW();
		dsp.setALU(false, TReg56(0));
		emitToMemory("do #$0,>$104", 0x100);
		emitToMemory("inc a", 0x102);
		emitToMemory("nop", 0x103);
		dsp.setPC(0x100);
		dsp.execInterpreter();
		verify(dsp.getPC().toWord() == 0x104 && dsp.aluA().var == 0);
		verify(dsp.regs().sc.var == 0 && !dsp.sr_test_noCache(SR_LF));
	}

	void InterpreterUnitTests::testMemoryDoCounts()
	{
		// Encode standard ISA forms directly: the test assembler does not support
		// memory-source DO/DOR. No words here come from a firmware image.
		for(const bool relative : {false, true})
			for(const bool indirect : {false, true})
				for(const bool y : {false, true})
					for(const TWord count : {0u, 3u})
					{
						dsp.resetHW();
						dsp.setALU(false, TReg56(0));
						dsp.regs().r[0].var = 0x20;
						dsp.memory().set(y ? MemArea_Y : MemArea_X, 0x20, count);
						dsp.memory().set(y ? MemArea_X : MemArea_Y, 0x20, count + 7);
						// DO aa: 0000011000aaaaaa0S000000, aa=$20.
						// DO ea: 0000011001MMMRRR0S000000, MMM=011 (R0)+.
						const TWord opcode = (indirect ? 0x065800 : 0x062000)
							| (relative ? 0x10 : 0) | (y ? 0x40 : 0);
						emitToMemory(opcode, relative ? 3 : 0x103, 0x100);
						emitToMemory("inc a", 0x102);
						emitToMemory("nop", 0x103);
						dsp.setPC(0x100);
						dsp.execInterpreter();
						verify(dsp.getPC().toWord() == (count ? 0x102 : 0x104));
						if(count)
							verify(dsp.regs().lc.var == count);
						verify(dsp.regs().r[0].var == (indirect ? 0x21 : 0x20));
						execUntil(0x104);
						verify(dsp.aluA().var == count);
						verify(dsp.regs().sc.var == 0 && !dsp.sr_test_noCache(SR_LF));
					}
	}

	void InterpreterUnitTests::execOpcode(uint32_t _op0, uint32_t _op1, const bool _reset, TWord _pc)
	{
		if(_reset)
			dsp.resetHW();
		dsp.clearOpcodeCache();
		dsp.mem.set(MemArea_P, _pc, _op0);
		dsp.mem.set(MemArea_P, _pc + 1, _op1);
		dsp.setPC(_pc);

		// Execute only the instruction, bypassing interrupt handling which
		// is designed for a running DSP, not single-step unit tests.
		dsp.pcCurrentInstruction = _pc;
		const auto op = dsp.fetchPC();
		dsp.execOp(op);
	}

	void InterpreterUnitTests::testSubr()
	{
		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00600000000000)));
		dsp.setALU(true , TReg56(static_cast<TReg56::MyType>(0x00020000000000)));

		emit("subr b,a");
		verify(dsp.aluA().var == 0x002e0000000000);
		verify(!dsp.sr_test(CCR_C));
		verify(!dsp.sr_test(CCR_V));
	}

	void InterpreterUnitTests::testCCCC()
	{
		constexpr auto T=true;
		constexpr auto F=false;

		//                            <  <= =  >= >  != 
		testCCCC(0xff000000000000, 0, T, T, F, F, F, T);
		testCCCC(0x00ff0000000000, 0, F, F, F, T, T, T);
		testCCCC(0x00000000000000, 0, F, T, T, T ,F ,F);
	}

	void InterpreterUnitTests::testCCCC(const int64_t _value, const int64_t _compareValue, const bool _lt, bool _le, bool _eq, bool _ge, bool _gt, bool _neq)
	{
		dsp.resetHW();
		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(_value)));
		dsp.alu_cmp(false, TReg56(_compareValue), false);
		char sr[16]{};
		dsp.sr_debug(sr);
		verify(_lt == (dsp.decode_cccc(CCCC_LessThan) != 0));
		verify(_le == (dsp.decode_cccc(CCCC_LessEqual) != 0));
		verify(_eq == (dsp.decode_cccc(CCCC_Equal) != 0));
		verify(_ge == (dsp.decode_cccc(CCCC_GreaterEqual) != 0));
		verify(_gt == (dsp.decode_cccc(CCCC_GreaterThan) != 0));
		verify(_neq == (dsp.decode_cccc(CCCC_NotEqual) != 0));	
	}

	void InterpreterUnitTests::runTest(const std::function<void()>& _build, const std::function<void()>& _verify)
	{
		_build();
		_verify();
	}

	void InterpreterUnitTests::emit(TWord _opA, TWord _opB, TWord _pc)
	{
		execOpcode(_opA, _opB, false, _pc);
	}

}
