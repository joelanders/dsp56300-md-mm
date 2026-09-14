#include <iostream>
#include "jitdspregpool.h"
#include "jitunittests.h"

#include "jitasmjithelpers.h"
#include "jitblock.h"
#include "jitblockruntimedata.h"
#include "jitemitter.h"
#include "jithelper.h"
#include "jitops.h"

namespace dsp56k
{
	namespace
	{
		// The CCR emitters take an accumulator, so the test has to hand them one in whatever representation
		// the JIT currently uses. Left-aligned puts the 56-bit value in bits 63..8.
		constexpr uint64_t aluTestValue(const uint64_t _v)
		{
			return g_leftAlignedAlu ? (_v << 8) : _v;
		}
	}

	static constexpr bool g_useDspMode = true;

	JitUnittests::JitUnittests(bool _logging/* = true*/)
	: m_checks({})
	, m_logging(_logging)
	{
		runTest(&JitUnittests::conversion_build, &JitUnittests::conversion_verify);
		runTest(&JitUnittests::signextend_build, &JitUnittests::signextend_verify);

		runTest(&JitUnittests::ccr_u_build, &JitUnittests::ccr_u_verify);
		runtimeUnnormalizedFlag();
		runTest(&JitUnittests::ccr_e_build, &JitUnittests::ccr_e_verify);
		runTest(&JitUnittests::ccr_n_build, &JitUnittests::ccr_n_verify);
		runTest(&JitUnittests::ccr_s_build, &JitUnittests::ccr_s_verify);

		runTest(&JitUnittests::agu_build, &JitUnittests::agu_verify);
		runTest(&JitUnittests::agu_modulo_build, &JitUnittests::agu_modulo_verify);
		runTest(&JitUnittests::agu_modulo2_build, &JitUnittests::agu_modulo2_verify);

		runTest(&JitUnittests::transferSaturation_build, &JitUnittests::transferSaturation_verify);
		transferSaturation48();

		{
			constexpr auto T=true;
			constexpr auto F=false;

			//                            <  <= =  >= >  != 
			testCCCC(0xff000000000000, 0, T, T, F, F, F, T);
			testCCCC(0x00ff0000000000, 0, F, F, F, T, T, T);
			testCCCC(0x00000000000000, 0, F, T, T, T ,F ,F);
		}

		decode_dddddd_write();
		decode_dddddd_read();

		runTest(&JitUnittests::getSS_build, &JitUnittests::getSS_verify);
		runTest(&JitUnittests::getSetRegs_build, &JitUnittests::getSetRegs_verify);

		runAllTests();

		jitDiv();
		rep_div();

		parallelMoveXY();
		boundedDispatch();
		loopStateWriteback();
		nopLoopSlices();
		memoryBaseEntries();
		peripheralDmaReads();
		ccrSequences();
	}

	void JitUnittests::runtimeUnnormalizedFlag()
	{
		// DSP56300FM Rev. 5, Table 5-1: U compares bits 47/46, 48/47 or
		// 46/45 according to scaling mode. Cover all patterns of bits 48..45.
		for(const unsigned scaling : {0u, 1u, 2u})
		for(uint64_t bits = 0; bits < 16; ++bits)
		{
			const uint64_t input = bits << 45;
			const unsigned lowBit = scaling == 1 ? 47 : scaling == 2 ? 45 : 46;
			const auto pair = (input >> lowBit) & 3;
			const bool expectedU = pair == 0 || pair == 3;
			runTest([&]()
			{
				dsp.setSR((scaling << 10) | (expectedU ? 0u : 0x10u));
				dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(input)));
				emit("tst a");
				// Require the runtime-SR fallback; runTest flushes deferred flags
				// after this builder returns, without a compile-time mode.
				verify(block->getMode() == nullptr);
			}, [&]()
			{
				if(dsp.sr_val(CCRB_U) != expectedU)
					LOG("Runtime U scaling=" << scaling << " bits=" << bits
						<< " U=" << dsp.sr_val(CCRB_U) << " expected=" << expectedU);
				verify(dsp.sr_val(CCRB_U) == expectedU);
				verify(dsp.aluA() == input);
				verify((dsp.getSR().var & 0xc00) == (scaling << 10));
			});
		}
		dsp.setSR(0);
	}

	JitUnittests::~JitUnittests()
	{
		m_rt.reset(asmjit::ResetPolicy::kHard);
	}

	void JitUnittests::runTest(void( JitUnittests::* _build)(), void( JitUnittests::* _verify)())
	{
		runTest([&]()
		{
			(this->*_build)();
		},[&]()
		{
			(this->*_verify)();
		});
	}

	void JitUnittests::runTest(const std::function<void()>& _build, const std::function<void()>& _verify)
	{
		AsmJitErrorHandler errorHandler;
		asmjit::CodeHolder code;
		AsmJitLogger logger;
		logger.addFlags(asmjit::FormatFlags::kHexImms | /*asmjit::FormatFlags::kHexOffsets |*/ asmjit::FormatFlags::kMachineCode);

#ifdef HAVE_ARM64
		constexpr auto arch = asmjit::Arch::kAArch64;
#else
		constexpr auto arch = asmjit::Arch::kX64;
#endif

		const auto foreignArch = m_rt.environment().arch() != arch;

		if(foreignArch)
			code.init(asmjit::Environment(arch));
		else
			code.init(m_rt.environment());

		if(m_logging)
			code.setLogger(&logger);
		code.setErrorHandler(&errorHandler);

		JitEmitter m_asm(&code);

		m_asm.addDiagnosticOptions(asmjit::DiagnosticOptions::kValidateIntermediate);
		m_asm.addDiagnosticOptions(asmjit::DiagnosticOptions::kValidateAssembler);

		if(m_logging)
			LOG("Creating test code");

		JitRuntimeData rtData;

		{
			JitConfig config;
			config.dynamicPeripheralAddressing = true;
			config.aguSupportBitreverse = true;
			config.optimizeCcrSequences = m_optimizeCcrSequences;

			JitBlock b(m_asm, dsp, rtData, std::move(config));
			JitBlockRuntimeData rt;
			JitOps o(b, rt);

			block = &b;
			ops = &o;

			m_asm.nop();

			errorHandler.setBlock(&rt);

			PushAllUsed pusher(b);

			m_asm.mov(regDspPtr, asmjit::Imm(&dsp.regs()));
			_build();

			block = nullptr;
			ops = nullptr;

			o.updateDirtyCCR();

			pusher.end();
		}

		m_asm.ret();

		m_asm.finalize();
		m_lastCodeSize = code.codeSize();

		TJitFunc func;
		const auto err = m_rt.add(&func, &code);
		if(err)
		{
			const auto* const errString = asmjit::DebugUtils::errorAsString(err);
			std::stringstream ss;
			ss << "JIT failed: " << err << " - " << errString;
			const std::string msg(ss.str());
			LOG(msg);
			throw std::runtime_error(msg);
		}

		if(!foreignArch)
		{
			if(m_logging)
				LOG("Running test code");

			dsp.getJit().getTrampoline().execOne(&dsp.regs(), 0xbadbc, func);
//			func(&dsp.getJit(), 0xbadbc);

			if(m_logging)
				LOG("Verifying test code");

			_verify();
		}
		else
		{
			LOG("Run & Verify of code for foreign arch skipped");
		}

		m_rt.release(&func);
	}

	void JitUnittests::ccrSequences()
	{
#ifdef HAVE_ARM64
		const auto oldLogging = m_logging;
		m_logging = false;
		size_t checks = 0;
		std::array<size_t, 2> cleanBranchBytes{};
		constexpr std::array<ConditionCode, 4> conditions = {
			CCCC_GreaterEqual, CCCC_LessThan, CCCC_Normalized, CCCC_NotNormalized};
		constexpr std::array<asmjit::arm::CondCode, 4> hostFlags = {
			asmjit::arm::CondCode::kZero, asmjit::arm::CondCode::kCS,
			asmjit::arm::CondCode::kMI, asmjit::arm::CondCode::kVS};

		for(const bool optimized : {false, true})
		{
			m_optimizeCcrSequences = optimized;
			std::cout << "CCR mask/NZCV checks, optimized=" << optimized << std::endl;
			// Include every CCR mask, not just masks encodable as logical immediates.
			// The scratch-register fallback and mask zero must remain valid too.
			for(unsigned mask = 0; mask < 256; ++mask)
				for(const TWord initial : {0u, 0xffffffu, 0x030055u, 0x0302aau})
				{
					dsp.setSR(initial);
					runTest([&]()
					{
						const RegGP r(*block);
						// Exercise zero, negative/carry and signed-overflow NZCV patterns.
						const uint64_t value = initial == 0 ? 1 : initial == 0xffffff ? 0 :
							initial == 0x030055 ? 0x8000000000000000ull : 0xffffffffffffffffull;
						block->asm_().mov(r64(r), asmjit::Imm(value));
						block->asm_().cmp(r64(r), asmjit::Imm(1));
						for(size_t i = 0; i < hostFlags.size(); ++i)
						{
							block->asm_().cset(r64(r), hostFlags[i]);
							block->mem().mov(m_checks[i], r64(r));
						}
						{ JitOps::CcrBatchUpdate batch(*ops, static_cast<CCRMask>(mask)); }
						for(size_t i = 0; i < hostFlags.size(); ++i)
						{
							block->asm_().cset(r64(r), hostFlags[i]);
							block->mem().mov(m_checks[i + 4], r64(r));
						}
					}, [&]()
					{
						verify(dsp.getSR().var == (initial & ~mask));
						for(size_t i = 0; i < hostFlags.size(); ++i)
							verify(m_checks[i] == m_checks[i + 4]);
					});
					++checks;
				}

			// BFI accepts an unmasked source for nonsticky destinations only.
			std::cout << "CCR bit-copy checks, optimized=" << optimized << std::endl;
			// Cover every source bit and destination, with deliberately dirty upper bits.
			for(unsigned sourceBit = 0; sourceBit < 64; ++sourceBit)
				for(unsigned destinationBit = 0; destinationBit < 8; ++destinationBit)
					for(const uint64_t source : {0ull, 0xffffffffffffffffull,
						0xaaaaaaaaaaaaaaaaull, 0x5555555555555555ull})
					{
						const TWord initial = 0x030000 | (source & 0xff);
						const TWord bit = ((source >> sourceBit) & 1) << destinationBit;
						const bool sticky = destinationBit == CCRB_L || destinationBit == CCRB_S;
						const TWord expected = sticky ? (initial | bit) :
							((initial & ~(1u << destinationBit)) | bit);
						dsp.setSR(initial);
						runTest([&]()
						{
							const RegGP r(*block);
							block->asm_().mov(r64(r), asmjit::Imm(source));
							ops->copyBitToCCR(r, sourceBit, static_cast<CCRBit>(destinationBit));
						}, [&]() { verify(dsp.getSR().var == expected); });
						++checks;
					}

			std::cout << "CCR clean-condition checks, optimized=" << optimized << std::endl;
			for(unsigned scaling = 0; scaling < 4; ++scaling)
				for(unsigned ccr = 0; ccr < 256; ++ccr)
					for(const auto condition : conditions)
					{
						const TWord initial = 0x030000 | (scaling << SRB_S0) | ccr;
						const bool nvEqual = bool(ccr & CCR_N) == bool(ccr & CCR_V);
						const bool normalized = (ccr & (CCR_U | CCR_E | CCR_Z)) == 0;
						const bool expected = condition == CCCC_GreaterEqual ? nvEqual :
							condition == CCCC_LessThan ? !nvEqual :
							condition == CCCC_Normalized ? normalized : !normalized;
						dsp.setSR(initial);
						runTest([&]()
						{
							const RegGP r(*block);
							ops->decode_cccc(r, condition);
							block->mem().mov(m_checks[0], r.get());
						}, [&]()
						{
							cleanBranchBytes[optimized ? 1 : 0] += m_lastCodeSize;
							verify(m_checks[0] == (expected ? 1u : 0u));
							verify(dsp.getSR().var == initial);
						});
						++checks;
					}
		}

		// Differentially exercise each subset of pending lazy flags. Preserve the
		// old update order, including V's sticky-L update, across scaling modes.
		constexpr std::array<CCRMask, 5> dirtyFlags = {CCR_N, CCR_V, CCR_U, CCR_E, CCR_Z};
		std::cout << "CCR lazy-condition differential checks" << std::endl;
		for(unsigned scaling = 0; scaling < 4; ++scaling)
			for(unsigned subset = 0; subset < 32; ++subset)
				for(const uint64_t value : {0ull, 1ull, 0xffffffffffffffffull,
					0x003fffffffffffull, 0x00400000000000ull, 0x007fffffffffffull,
					0x00800000000000ull, 0x00ffffffffffffull, 0x0100000000000000ull,
					0x7fffffffffffffull, 0x80000000000000ull, 0xff800000000000ull})
					for(const auto condition : conditions)
					{
						CCRMask mask = CCR_None;
						for(size_t i = 0; i < dirtyFlags.size(); ++i)
							if(subset & (1u << i))
								mask = static_cast<CCRMask>(mask | dirtyFlags[i]);
						std::array<uint64_t, 2> reference{};
						for(const bool optimized : {false, true})
						{
							m_optimizeCcrSequences = optimized;
							dsp.setSR(0x030000 | (scaling << SRB_S0) | (subset & 1 ? 0xff : 0));
							runTest([&]()
							{
								JitDspMode mode;
								mode.initialize(dsp);
								block->setMode(&mode);
								{
									const RegGP input(*block);
									block->asm_().mov(r64(input), asmjit::Imm(aluTestValue(value)));
									ops->ccr_dirty(0, r64(input), mask);
								}
								// Like a real branch, decode without retaining an unrelated
								// temporary: the old lazy NR path needs all four GP temps.
								const auto cc = ops->decode_cccc(condition);
								{
									const RegGP result(*block);
									block->asm_().cset(r64(result), cc);
									block->mem().mov(m_checks[0], r64(result));
								}
								ops->updateDirtyCCR();
								block->setMode(nullptr);
							}, [&]()
							{
								const std::array<uint64_t, 2> actual = {m_checks[0], static_cast<uint64_t>(dsp.getSR().var)};
								if(optimized)
								{
									verify(actual == reference);
								}
								else
								{
									reference = actual;
								}
							});
							++checks;
						}
					}
		verify(cleanBranchBytes[1] < cleanBranchBytes[0]);
		m_optimizeCcrSequences = true;
		m_logging = oldLogging;
		LOG("ARM64 CCR sequences: " << checks << " cases; clean condition fixture bytes "
			<< cleanBranchBytes[0] << " -> " << cleanBranchBytes[1]);
#endif
	}

	void JitUnittests::nop(size_t _count) const
	{
		for(size_t i=0; i<_count; ++i)
			block->asm_().nop();
	}

	void JitUnittests::memoryBaseEntries()
	{
		const auto oldConfig = dsp.getJit().getConfig();
		auto config = oldConfig;
		config.enableOptimizer = false;
		config.linkJitBlocks = false;
		config.dynamicPeripheralAddressing = true;
		config.maxInstructionsPerBlock = 2;
		config.getBlockConfig = {};
		dsp.getJit().setConfig(config);

		// Exercise both independent internal X/Y and external X/Y/P aliases.
		for(const TWord addressBase : {TWord(0), dsp.memory().getBridgedMemoryAddress()})
		for(const unsigned blockCount : {8u, 24u, 64u})
		{
			// This fixture's bridged backing is larger than its guest P window.
			// The fallback C++ translator intentionally rejects those external P
			// addresses, so its entry-path checks use the internal ranges only.
			if(addressBase && !dsp.memory().hasMmuSupport()) continue;
			std::vector<uint64_t> reference;
			uint64_t targetCycles = 0;
			for(const unsigned entry : {0u, 1u, 2u})
			{
				dsp.getJit().destroyAllBlocks();
				dsp.resetHW();
				dsp.setSR(0x30000);
				dsp.regs().la.var = 0x654321;
				dsp.regs().lc.var = 0;
				dsp.regs().r[0].var = addressBase + 0x100;
				dsp.regs().r[1].var = addressBase + 0x200;
				dsp.regs().r[2].var = addressBase + 0x300;
				dsp.regs().r[3].var = addressBase + 0x400;
				dsp.regs().x.var = dsp.regs().y.var = 0;
				for(unsigned i = 0; i < 128; ++i)
				{
					dsp.memory().set(MemArea_X, addressBase + 0x100 + i, 0x123400 + i);
					dsp.memory().set(MemArea_Y, addressBase + 0x200 + i, 0);
					dsp.memory().set(MemArea_Y, addressBase + 0x300 + i, 0x654300 + i);
					dsp.memory().set(MemArea_X, addressBase + 0x400 + i, 0);
				}
				TWord pc = 0x400;
				pc = emitToMemory("move x:(r0)+,x0", pc);
				pc = emitToMemory("move x0,y:(r1)+", pc);
				// The peripheral read calls C++; invariant bases must survive that call.
				pc = emitToMemory("movep x:<<$ffffc5,y1", pc);
				pc = emitToMemory("move y:(r2)+,y0", pc);
				pc = emitToMemory("move y0,x:(r3)+", pc);
				emitToMemory("bra >$400", pc);
				dsp.setPC(0x400);

				if(entry == 0) for(unsigned i = 0; i < blockCount; ++i) dsp.execJit();
				else if(entry == 1) dsp.getJit().getTrampoline().exec(&dsp, blockCount);
				else dsp.execUntilCycles(targetCycles);

				std::vector<uint64_t> result{dsp.getInstructionCounter(), dsp.getCycles()};
				for(const auto value : {dsp.regs().x.var, dsp.regs().y.var}) result.push_back(value);
				result.push_back(dsp.getPC().var);
				for(unsigned i = 0; i < 4; ++i) result.push_back(dsp.regs().r[i].var);
				const auto writtenY = dsp.regs().r[1].var - addressBase - 0x200;
				const auto writtenX = dsp.regs().r[3].var - addressBase - 0x400;
				for(unsigned i = 0; i < 128; ++i)
				{
					const auto y = dsp.memory().get(MemArea_Y, addressBase + 0x200 + i);
					const auto x = dsp.memory().get(MemArea_X, addressBase + 0x400 + i);
					verify(y == (i < writtenY ? 0x123400 + i : 0));
					verify(x == (i < writtenX ? 0x654300 + i : 0));
					if(addressBase)
					{
						// Inspect the allocated P backing, not the smaller guest P
						// API window in this unit-test Memory configuration.
						verify(dsp.memory().getMemAreaPtr(MemArea_P)[addressBase + 0x200 + i] == y);
						verify(dsp.memory().getMemAreaPtr(MemArea_P)[addressBase + 0x400 + i] == x);
					}
					result.push_back(y);
					result.push_back(x);
				}
				if(entry == 0) { reference = result; targetCycles = dsp.getCycles(); }
				else verify(result == reference);
			}
		}
		dsp.getJit().destroyAllBlocks();
		dsp.getJit().setConfig(oldConfig);
		std::cout << "Memory base tests: exact internal X/Y and external X/Y/P aliases across all three trampoline entries passed." << std::endl;
	}

	void JitUnittests::peripheralDmaReads()
	{
		unsigned comparisons = 0;
		for(const bool incrementSource : {false, true})
		for(const bool optimizer : {false, true})
		for(const bool dynamic : {false, true})
		for(const unsigned entry : {0u, 1u, 2u})
		{
			std::vector<std::vector<uint64_t>> reference;
			for(const bool direct : {false, true})
			{
				// Each arm owns different peripheral objects. Native addresses must
				// belong to that DSP, never another instance or a retired one.
				DefaultMemoryValidator validator;
				Memory memory(validator, 0x4000);
				Peripherals56303 px;
				PeripheralsNop py; // the supported 56303 X / NOP Y product configuration
				DSP cpu(memory, &px, &py);
				auto config = cpu.getJit().getConfig();
				config.enableOptimizer = optimizer;
				config.inlinePeripheralReads = direct;
				config.linkJitBlocks = false;
				config.dynamicPeripheralAddressing = true;
				config.maxInstructionsPerBlock = 1;
				cpu.getJit().setConfig(config);
				cpu.setSR(0x30000);
				px.resetDelayCycles(0, 1);
				py.resetDelayCycles(0, 1);
				px.clearCycleDeadline();
				py.clearCycleDeadline();

				for(const TWord address : {TWord(HDI08::HORX), TWord(Essi::ESSI0_RX),
					TWord(Essi::ESSI1_RX), TWord(Essi::ESSI_PDRC), TWord(XIO_DSTR), TWord(XIO_IDR)})
					verify(px.readAsPtr(address, Movep_ppea) == nullptr);

				auto emitLocal = [&](const std::string& text, TWord pc)
				{
					const auto encoded = assembler.assemble(text.c_str());
					verify(encoded.success());
					for(unsigned i = 0; i < encoded.wordCount; ++i) cpu.memWriteP(pc++, encoded.word[i]);
					return pc;
				};
				std::array<TWord, 24> endPC{};
				for(unsigned index = 0; index < 24; ++index)
				{
					const TWord address = XIO_DCR5 + index;
					std::stringstream operand;
					operand << "<<$" << std::hex << address;
					TWord pc = 0x200 + index * 16;
					pc = emitLocal(dynamic ? "move x:(r0),x0" : "movep x:" + operand.str() + ",x0", pc);
					pc = emitLocal(dynamic ? "move y:(r0),y0" : "movep y:" + operand.str() + ",y0", pc);
					// The counted trampoline requires groups of eight entries.
					for(unsigned i = 0; i < 6; ++i) pc = emitLocal("nop", pc);
					endPC[index] = pc;
				}

				size_t snapshotIndex = 0;
				const TWord values[] = {0u, 0x7fffffu, 0x800000u, 0xffffffu, 0x345678u};
				std::array<const JitBlockInfo*, 24> cached{};
				for(unsigned phase = 0; phase < 10; ++phase)
				{
					if(phase == 4)
					{
						cpu.resetHW();
						cpu.setSR(0x30000);
						px.resetDelayCycles(0, 1);
						py.resetDelayCycles(0, 1);
					}
					if(phase < 5)
					{
						for(unsigned index = 0; index < 24; ++index)
						{
							const TWord address = XIO_DCR5 + index;
							const TWord mask = index % 4 == 0 ? 0x7fffff : 0xffffff; // leave DE disabled
							px.write(address, values[phase] & mask);
						}
					}
					else if(phase == 5)
					{
						{
							auto& dma = px.getDMA();
							const TWord source = 0x800;
							for(unsigned i = 0; i < 4; ++i) memory.set(MemArea_X, source + i, 0x765400 + i);
							dma.setDCR(0, 0);
							dma.setDSR(0, source);
							dma.setDDR(0, source + 0x100);
							dma.setDCO(0, 3);
							dma.setDCR(0, ((incrementSource ? 5u : 4u) << DmaChannel::Dam0)
								| ((incrementSource ? 4u : 5u) << DmaChannel::Dam3)
								| (1u << DmaChannel::Dtm0) | (1u << DmaChannel::De));
						}
					}
					else
					{
						// Autonomous word requests modify already-compiled live reads.
						{
							auto& dma = px.getDMA();
							verify(dma.trigger(DmaChannel::RequestSource::ExternalIRQA));
							const TWord source = 0x800;
							verify(dma.getDSR(0) == source + (incrementSource ? phase - 5 : 0));
							verify(dma.getDDR(0) == source + 0x100 + (incrementSource ? 0 : phase - 5));
							verify(memory.get(MemArea_X, source + 0x100 + (incrementSource ? 0 : phase - 6))
								== 0x765400 + (incrementSource ? phase - 6 : 0));
						}
					}

					for(unsigned index = 0; index < 24; ++index)
					{
						const TWord address = XIO_DCR5 + index;
						const TWord pc = 0x200 + index * 16;
						const auto* xp = px.readAsPtr(address, Movep_ppea);
						verify(xp);
						verify(*xp == px.read(address, Movep_ppea));
						cpu.regs().r[0].var = address;
						cpu.setPC(pc);
						for(unsigned step = 0; step < 8; step += entry == 1 ? 8 : 1)
						{
							if(entry == 0) cpu.execJit();
							else if(entry == 1) cpu.getJit().getTrampoline().exec(&cpu, 8);
							else cpu.execUntilCycles(cpu.getCycles() + 1);
							const auto& r = cpu.regs();
							std::vector<uint64_t> state;
							for(const int64_t value : {r.x.var, r.y.var, r.a.var, r.b.var})
								state.push_back(static_cast<uint64_t>(value));
							for(const auto value : std::array<int64_t, 6>{r.pc.var, r.sr.var, r.la.var, r.lc.var, r.sp.var, r.sc.var})
								state.push_back(static_cast<uint64_t>(value));
							for(const auto value : {cpu.getInstructionCounter(), cpu.getCycles(), px.getTargetClock(), py.getTargetClock()})
								state.push_back(value);
							state.push_back(px.getDMA().getDSTR());
							if(!direct) reference.push_back(state);
							else { verify(state == reference.at(snapshotIndex)); ++comparisons; }
							++snapshotIndex;
						}
						verify(cpu.getPC().var == endPC[index]);
						verify(cpu.x0().var == *xp && cpu.y0().var == 0);
						const auto* info = cpu.getJit().getBlockInfo(pc);
						verify(info);
						if(phase == 0 || phase == 4) cached[index] = info;
						else verify(info == cached[index]);
					}
				}
			}
		}
		std::cout << "DMA peripheral reads: " << comparisons
			<< " exact return-boundary comparisons, live X DMA/Y NOP, cached blocks, reset and DMA requests passed." << std::endl;
	}

	void JitUnittests::loopStateWriteback()
	{
		const auto oldConfig = dsp.getJit().getConfig();
		auto config = oldConfig;
		config.enableOptimizer = false; // exercise the emitter, not dead-code cleanup
		config.linkJitBlocks = false;
		config.maxInstructionsPerBlock = 0;
		config.getBlockConfig = {};

		auto snapshot = [&]()
		{
			dsp.getSR(); // materialize the interpreter's lazy CCR before comparison
			const auto& r = dsp.regs();
			std::vector<int64_t> values{r.x.var, r.y.var, r.a.var, r.b.var,
				r.pc.var, r.sr.var, r.omr.var, r.la.var, r.lc.var, r.sp.var, r.sc.var};
			for(unsigned i = 0; i < 8; ++i)
				for(const auto value : std::array<int64_t, 5>{r.r[i].var, r.n[i].var, r.m[i].var, r.mMask[i], r.mModulo[i]})
					values.push_back(value);
			for(const auto& value : r.ss) values.push_back(value.var);
			values.push_back(dsp.getInstructionCounter());
			return values;
		};

		for(const unsigned body : {0u, 1u, 2u, 3u, 4u, 5u})
		for(const TWord count : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 17u, 257u})
#ifdef NDEBUG
		for(const TWord stack : {0u, 14u})
#else
		// The interpreter deliberately asserts instead of wrapping its stack.
		// Leave room for the nested body's four pushes in assertion-enabled runs.
		// Release still checks interpreter wrap; nopLoopSlices checks JIT wrap
		// against the unfused emitter in both configurations.
		for(const TWord stack : {0u, 11u})
#endif
		{
			const TWord after = body == 0 ? 0x203 : body == 1 ? 0x204 : 0x210;
			const auto setup = [&]()
			{
				dsp.resetHW();
				dsp.setSR(0x30014 | (stack ? SR_LF : 0));
				dsp.regs().la.var = 0x654321;
				dsp.regs().lc.var = 0x123456;
				dsp.regs().sp.var = stack;
				dsp.regs().sc.var = stack;
				for(unsigned i = 0; i < 16; ++i) dsp.regs().ss[i].var = 0x123456654321ull + i;
				dsp.setALU(false, TReg56(int64_t(0)));
				dsp.setALU(true, TReg56(int64_t(0x1000000)));
				dsp.x0(count);
				dsp.y0(0);
				dsp.x1(0);
				dsp.y1(0);
				std::stringstream doOp;
				doOp << "do x0,>$" << std::hex << after;
				TWord pc = emitToMemory(doOp.str().c_str(), 0x200);
				verify(pc == 0x202);
				if(body == 2) pc = emitToMemory("add b,a", pc);
				if(body == 3) pc = emitToMemory("move #>$20f,la", pc);
				if(body == 4) pc = emitToMemory("ori #$40,ccr", pc);
				if(body == 5) pc = emitToMemory("do #$3,>$208", pc);
				while(pc < after) pc = emitToMemory("nop", pc);
				verify(pc == after);
				dsp.setPC(0x200);
			};

			std::vector<int64_t> reference;
			uint64_t referenceCycles = 0;
			for(const unsigned limit : {0u, 1u, 4u, 8u})
			{
				config.maxDoIterations = limit;
				dsp.getJit().destroyAllBlocks();
				dsp.getJit().setConfig(config);
				setup();
				unsigned steps = 0;
				while(dsp.getPC().var != after && ++steps < 100000)
				{
					const bool singleIteration = limit == 1 && body <= 1 && dsp.getPC().var == 0x202;
					const auto beforeLC = dsp.regs().lc.var;
					const auto beforeInstructions = dsp.getInstructionCounter();
					dsp.execJit();
					if(singleIteration)
					{
						verify(dsp.getInstructionCounter() - beforeInstructions == after - 0x202);
						if(beforeLC > 1) verify(dsp.regs().lc.var == beforeLC - 1);
					}
					if(dsp.regs().sp.var == stack + 2 && dsp.getPC().var != after)
					{
						verify(dsp.regs().la.var == after - 1);
						verify(dsp.regs().lc.var >= 1 && dsp.regs().lc.var <= count);
						verify(dsp.getSR().var & SR_LF);
					}
				}
				verify(steps < 100000);
				verify(dsp.regs().la.var == 0x654321);
				verify(dsp.regs().lc.var == 0x123456);
				verify(dsp.regs().sp.var == stack && dsp.regs().sc.var == stack);
				if(reference.empty()) { reference = snapshot(); referenceCycles = dsp.getCycles(); }
				else { verify(snapshot() == reference); verify(dsp.getCycles() == referenceCycles); }

				// A cached loop-ending block can also run outside an active DO loop.
				if(body <= 1 && count)
				{
					dsp.setSR(0x30014);
					dsp.setPC(0x202);
					dsp.execJit();
					verify(dsp.getPC().var == after);
					verify(dsp.regs().la.var == 0x654321);
					verify(dsp.regs().lc.var == 0x123456);
					verify(dsp.getSR().var == 0x30014);
					verify(dsp.regs().sp.var == stack && dsp.regs().sc.var == stack);
				}
			}
			setup();
			dsp.execInterpreter(); // the interpreter executes a whole DO internally
			verify(dsp.getPC().var == after);
			verify(snapshot() == reference);
		}
		dsp.getJit().destroyAllBlocks();
		dsp.getJit().setConfig(oldConfig);
		std::cout << "Loop write-back tests: 528 cases, four slice limits, exact interpreter state passed." << std::endl;
	}

	void JitUnittests::nopLoopSlices()
	{
		const auto oldConfig = dsp.getJit().getConfig();
		auto config = oldConfig;
		config.enableOptimizer = false;
		config.linkJitBlocks = false;
		config.maxInstructionsPerBlock = 0;
		config.getBlockConfig = {};
		unsigned cases = 0;
		auto snapshot = [&]()
		{
			dsp.getSR();
			const auto& r = dsp.regs();
			std::vector<int64_t> values{r.x.var, r.y.var, r.a.var, r.b.var,
				r.pc.var, r.sr.var, r.omr.var, r.la.var, r.lc.var, r.sp.var, r.sc.var};
			for(unsigned i = 0; i < 8; ++i)
				for(const auto value : std::array<int64_t, 5>{r.r[i].var, r.n[i].var, r.m[i].var, r.mMask[i], r.mModulo[i]})
					values.push_back(value);
			for(const auto& value : r.ss) values.push_back(value.var);
			values.push_back(dsp.getInstructionCounter());
			values.push_back(dsp.getCycles());
			values.push_back(peripheralsX.getTargetClock());
			values.push_back(peripheralsY.getTargetClock());
			return values;
		};
		auto setup = [&](unsigned bodyWords, TWord count, TWord stack, bool nested)
		{
			dsp.resetHW();
			// resetHW does not reset the scheduling deadlines in IPeripherals.
			// Start both arms with identical, active near-term deadlines.
			peripheralsX.resetDelayCycles(0, 7);
			peripheralsY.resetDelayCycles(0, 11);
			peripheralsX.clearCycleDeadline();
			peripheralsY.clearCycleDeadline();
			peripheralsX.setCycleDeadline(5);
			peripheralsY.setCycleDeadline(9);
			dsp.setSR(0x30014 | (stack ? SR_LF : 0));
			dsp.regs().la.var = 0x654321;
			dsp.regs().lc.var = 0x123456;
			dsp.regs().sp.var = dsp.regs().sc.var = stack;
			for(unsigned i = 0; i < 16; ++i) dsp.regs().ss[i].var = 0x123456654321ull + i;
			dsp.x0(count);
			const TWord body = nested ? 0x404 : 0x402;
			const TWord after = body + bodyWords;
			std::stringstream inner, outer;
			inner << "do x0,>$" << std::hex << after;
			outer << "do #$4,>$" << std::hex << after + 1;
			TWord pc = 0x400;
			if(nested) pc = emitToMemory(outer.str().c_str(), pc);
			pc = emitToMemory(inner.str().c_str(), pc);
			verify(pc == body);
			for(unsigned i = 0; i < bodyWords; ++i) pc = emitToMemory("nop", pc);
			if(nested) pc = emitToMemory("nop", pc);
			dsp.setPC(0x400);
			return pc;
		};

		for(const unsigned bodyWords : {1u, 2u})
		for(const unsigned limit : {2u, 4u, 8u, 16u})
		for(const TWord count : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 17u, 255u, 256u, 257u})
		for(const TWord stack : {0u, 14u})
		for(const bool nested : {false, true})
		{
			std::vector<std::vector<int64_t>> reference;
			for(const bool combined : {false, true})
			{
				dsp.getJit().destroyAllBlocks();
				config.maxDoIterations = limit;
				config.combineNopLoopIterations = combined;
				dsp.getJit().setConfig(config);
				const auto after = setup(bodyWords, count, stack, nested);
				std::vector<std::vector<int64_t>> trace;
				while(dsp.getPC().var != after && trace.size() < 10000)
				{
					dsp.execJit();
					trace.push_back(snapshot());
				}
				verify(trace.size() < 10000);
				if(count)
				{
					// Reuse a previously compiled ending body with LF clear.
					dsp.setSR(0x30014);
					dsp.setPC(nested ? 0x404 : 0x402);
					dsp.execJit();
					trace.push_back(snapshot());
				}
				if(!combined) reference = trace;
				else
				{
					if(trace != reference)
					{
						std::cerr << "NOP slice mismatch: words=" << bodyWords << " limit=" << limit
							<< " count=" << count << " stack=" << stack << " nested=" << nested
							<< " returns=" << reference.size() << "/" << trace.size() << std::endl;
						for(size_t step = 0; step < std::min(reference.size(), trace.size()); ++step)
						{
							if(reference[step] == trace[step]) continue;
							for(size_t field = 0; field < reference[step].size(); ++field)
								if(reference[step][field] != trace[step][field])
									std::cerr << "  return " << step << " field " << field << ": "
										<< reference[step][field] << " -> " << trace[step][field] << std::endl;
							break;
						}
					}
					verify(trace == reference);
				}
			}
			++cases;
		}

		// Direct cached-body entries include LC=0/1 and large 24-bit counts;
		// compare one return so the latter do not create million-step tests.
		for(const unsigned bodyWords : {1u, 2u})
		for(const unsigned limit : {2u, 4u, 8u, 16u})
		for(const TWord count : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 0x7fffffu, 0x800000u, 0xffffffu})
		for(const TWord stack : {0u, 14u})
		for(const bool loopFlag : {false, true})
		{
			std::vector<int64_t> reference;
			for(const bool combined : {false, true})
			{
				dsp.getJit().destroyAllBlocks();
				config.maxDoIterations = limit;
				config.combineNopLoopIterations = combined;
				dsp.getJit().setConfig(config);
				setup(bodyWords, 3, stack, false);
				dsp.execJit();
				dsp.regs().lc.var = count;
				dsp.setSR(0x30014 | (loopFlag ? SR_LF : 0));
				dsp.execJit();
				if(!combined) reference = snapshot();
				else verify(snapshot() == reference);
			}
			++cases;
		}
		dsp.getJit().destroyAllBlocks();
		dsp.getJit().setConfig(oldConfig);
		std::cout << "NOP slice tests: " << cases << " paired cases with exact state/counters at every return passed." << std::endl;
	}

	void JitUnittests::boundedDispatch()
	{
		constexpr TWord loopPC = 0x100;
		TWord pc = loopPC;
		pc = emitToMemory("nop", pc);
		pc = emitToMemory("nop", pc);
		emitToMemory("bra >$100", pc);

		const auto run = [this](const bool _bounded)
		{
			dsp.resetHW();
			dsp.setPC(loopPC);
			peripheralsX.resetDelayCycles(0, IPeripherals::MaxDelayCycles);
			peripheralsX.setCycleDeadline(7);

			constexpr uint64_t targetCycles = 257;
			if(_bounded)
				dsp.execUntilCycles(targetCycles);
			else
			{
				do
					dsp.execJit();
				while(dsp.getCycles() < targetCycles);
			}

			return std::array<uint64_t, 4>{
				dsp.getPC().toWord(), dsp.getInstructionCounter(), dsp.getCycles(),
				peripheralsX.getTargetClock()};
		};

		const auto reference = run(false);
		const auto bounded = run(true);
		verify(bounded == reference);
		verify(bounded[2] >= 257);

		constexpr TWord highPC = 0x70000;
		dsp.resetHW();
		emitToMemory("jmp (r0)", loopPC);
		emitToMemory("jmp (r0)", highPC);
		dsp.regs().r[0].var = highPC;
		dsp.setPC(loopPC);
		const bool needsGrowth = dsp.getJitEntriesSize() <= highPC;
		dsp.execUntilCycles(32);
		verify(dsp.getPC().toWord() == highPC);
		verify(dsp.getCycles() >= 32);
		if(needsGrowth)
			verify(dsp.getJitEntriesSize() > highPC);
	}

	void JitUnittests::conversion_build()
	{
		block->asm_().bind(block->asm_().newNamedLabel("test_conv"));

		dsp.regs().x.var = 0xffeedd112233;
		dsp.regs().y.var = 0x445566fedcba;

		const RegGP r(*block);

		ops->XY0to56(r, 0);		block->mem().mov(m_checks[0], r);
		ops->XY1to56(r, 0);		block->mem().mov(m_checks[1], r);
		ops->XY0to56(r, 1);		block->mem().mov(m_checks[2], r);
		ops->XY1to56(r, 1);		block->mem().mov(m_checks[3], r);
	}

	void JitUnittests::conversion_verify()
	{
		// XY*to56 feeds ALU arithmetic, so the result follows the ALU representation
		verify(m_checks[0] == aluTestValue(0x0000112233000000));
		verify(m_checks[1] == aluTestValue(0x00ffffeedd000000));
		verify(m_checks[2] == aluTestValue(0x00fffedcba000000));
		verify(m_checks[3] == aluTestValue(0x0000445566000000));
	}

	void JitUnittests::signextend_build()
	{
		m_checks[0] = 0xabcdef;
		m_checks[1] = 0x123456;
		m_checks[2] = 0xabcdef123456;
		m_checks[3] = 0x123456abcdef;
		m_checks[4] = 0xab123456abcdef;
		m_checks[5] = 0x12123456abcdef;

		const DSPReg regLA(*block, PoolReg::DspLA, true, false);
		const DSPReg regLC(*block, PoolReg::DspLC, true, false);
		const DSPReg regSR(*block, PoolReg::DspSR, true, false);
		const DSPReg regA(*block, PoolReg::DspA, true, false);
		const RegGP ra(*block);
		const RegGP rb(*block);

		block->mem().mov(regLA, m_checks[0]);
		block->mem().mov(regLC, m_checks[1]);
		block->mem().mov(regSR, m_checks[2]);
		block->mem().mov(regA, m_checks[3]);
		block->mem().mov(ra, m_checks[4]);
		block->mem().mov(rb, m_checks[5]);

		ops->signextend24to56(r64(regLA.get()));
		ops->signextend24to56(r64(regLC.get()));
		ops->signextend48to56(r64(regSR.get()));
		ops->signextend48to56(r64(regA.get()));
		ops->signextend56to64(ra);
		ops->signextend56to64(rb);

		block->mem().mov(m_checks[0], regLA);
		block->mem().mov(m_checks[1], regLC);
		block->mem().mov(m_checks[2], regSR);
		block->mem().mov(m_checks[3], regA);
		block->mem().mov(m_checks[4], ra);
		block->mem().mov(m_checks[5], rb);
	}

	void JitUnittests::signextend_verify()
	{
		verify(m_checks[0] == 0xffffffffabcdef);
		verify(m_checks[1] == 0x00000000123456);

		verify(m_checks[2] == 0xffabcdef123456);
		verify(m_checks[3] == 0x00123456abcdef);

		verify(m_checks[4] == 0xffab123456abcdef);
		verify(m_checks[5] == 0x0012123456abcdef);
	}

	void JitUnittests::ccr_u_build()
	{
		dsp.regs().sr.var = 0;

		m_checks[0] = 0xee012233445566;
		m_checks[1] = 0xee412233445566;
		m_checks[2] = 0xee812233445566;
		m_checks[3] = 0xeec12233445566;

		for(auto i=0; i<4; ++i)
		{
			RegGP r(*block);

			block->asm_().mov(r, asmjit::Imm(aluTestValue(m_checks[i])));
			ops->ccr_u_update(r);
			block->mem().mov(m_checks[i], block->regs().getSR(JitDspRegs::Read));
		}
	}

	void JitUnittests::ccr_u_verify()
	{
		verify((m_checks[0] & CCR_U));
		verify(!(m_checks[1] & CCR_U));
		verify(!(m_checks[2] & CCR_U));
		verify((m_checks[3] & CCR_U));
	}

	void JitUnittests::ccr_e_build()
	{
		dsp.regs().sr.var = 0;

		m_checks[0] = 0xff812233445566;
		m_checks[1] = 0xff712233445566;
		m_checks[2] = 0x00712233445566;
		m_checks[3] = 0x00812233445566;

		for(auto i=0; i<4; ++i)
		{
			RegGP r(*block);

			block->asm_().mov(r, asmjit::Imm(aluTestValue(m_checks[i])));
			ops->ccr_e_update(r);
			block->mem().mov(m_checks[i], block->regs().getSR(JitDspRegs::Read));

		}
	}

	void JitUnittests::ccr_e_verify()
	{
		verify(!(m_checks[0] & CCR_E));
		verify((m_checks[1] & CCR_E));
		verify(!(m_checks[2] & CCR_E));
		verify((m_checks[3] & CCR_E));
	}

	void JitUnittests::ccr_n_build()
	{
		m_checks[0] = 0xff812233445566;
		m_checks[1] = 0x7f812233445566;

		for(auto i=0; i<2; ++i)
		{
			RegGP r(*block);

			block->asm_().mov(r, asmjit::Imm(aluTestValue(m_checks[i])));
			ops->ccr_n_update_by55(r);
			block->mem().mov(m_checks[i], block->regs().getSR(JitDspRegs::Read));
		}
	}

	void JitUnittests::ccr_n_verify()
	{
		verify((m_checks[0] & CCR_N));
		verify(!(m_checks[1] & CCR_N));
	}

	void JitUnittests::ccr_s_build()
	{
		m_checks[0] = 0xff0fffffffffff;
		m_checks[1] = 0xff2fffffffffff;
		m_checks[2] = 0xff4fffffffffff;
		m_checks[3] = 0xff6fffffffffff;

		dsp.regs().sr.var = 0;

		for(auto i=0; i<4; ++i)
		{
			RegGP r(*block);

			block->asm_().mov(r, asmjit::Imm(aluTestValue(m_checks[i])));
			block->asm_().clr(block->regs().getSR(JitDspRegs::Write));
			ops->ccr_s_update(r);
			block->mem().mov(m_checks[i], block->regs().getSR(JitDspRegs::Read));
		}
	}

	void JitUnittests::ccr_s_verify()
	{
		verify(m_checks[0] == 0);
		verify(m_checks[1] == CCR_S);
		verify(m_checks[2] == CCR_S);
		verify(m_checks[3] == 0);
	}

	void JitUnittests::agu_build()
	{
		dsp.regs().r[0].var = 0x1000;
		dsp.regs().n[0].var = 0x10;
		dsp.set_m(0, 0xffffff);

		uint32_t ci=0;

		DspValue temp(*block);

		{
			auto r = ops->updateAddressRegister(MMM_Rn, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}

		{
			auto r = ops->updateAddressRegister(MMM_RnPlus, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}

		{
			auto r = ops->updateAddressRegister(MMM_RnMinus, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}

		{
			auto r = ops->updateAddressRegister(MMM_RnPlusNn, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}

		{
			auto r = ops->updateAddressRegister(MMM_RnMinusNn, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}

		{
			auto r = ops->updateAddressRegister(MMM_RnPlusNnNoUpdate, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}

		{
			auto r = ops->updateAddressRegister(MMM_MinusRn, 0);
			block->mem().mov(m_checks[ci++], r.get());
			r.release();
			r = block->regs().getR(0);
			block->mem().mov(m_checks[ci++], r.get());
		}
	}

	void JitUnittests::agu_verify()
	{
		verify(m_checks[0 ] == 0x1000);	verify(m_checks[1 ] == 0x1000);
		verify(m_checks[2 ] == 0x1000);	verify(m_checks[3 ] == 0x1001);
		verify(m_checks[4 ] == 0x1001);	verify(m_checks[5 ] == 0x1000);
		verify(m_checks[6 ] == 0x1000);	verify(m_checks[7 ] == 0x1010);
		verify(m_checks[8 ] == 0x1010);	verify(m_checks[9 ] == 0x1000);
		verify(m_checks[10] == 0x1010);	verify(m_checks[11] == 0x1000);
		verify(m_checks[12] == 0x0fff);	verify(m_checks[13] == 0x0fff);
	}

	void JitUnittests::agu_modulo_build()
	{
		dsp.regs().r[0].var = 0x100;
		dsp.regs().n[0].var = 0x200;
		dsp.set_m(0, 0xfff);

		DspValue temp(*block);

		for(size_t i=0; i<8; ++i)
		{
			m_checks[i] = 0;
			ops->updateAddressRegister(MMM_RnPlusNn, 0);
			block->regs().getR(temp, 0);
			block->mem().mov(m_checks[i], temp.get());
			temp.release();
		}
	}

	void JitUnittests::agu_modulo_verify()
	{
		verify(m_checks[0] == 0x300);
		verify(m_checks[1] == 0x500);
		verify(m_checks[2] == 0x700);
		verify(m_checks[3] == 0x900);
		verify(m_checks[4] == 0xb00);
		verify(m_checks[5] == 0xd00);
		verify(m_checks[6] == 0xf00);
		verify(m_checks[7] == 0x100);
	}

	void JitUnittests::agu_modulo2_build()
	{
		dsp.regs().r[0].var = 0x70;
		dsp.regs().n[0].var = 0x20;
		dsp.set_m(0, 0x100);

		DspValue temp(*block);

		for(size_t i=0; i<8; ++i)
		{
			ops->updateAddressRegister(MMM_RnMinusNn, 0);
			block->regs().getR(temp, 0);
			block->mem().mov(m_checks[i], temp.get());
			temp.release();
		}
	}

	void JitUnittests::agu_modulo2_verify()
	{
		verify(m_checks[0] == 0x50);
		verify(m_checks[1] == 0x30);
		verify(m_checks[2] == 0x10);
		verify(m_checks[3] == 0xf1);
		verify(m_checks[4] == 0xd1);
		verify(m_checks[5] == 0xb1);
		verify(m_checks[6] == 0x91);
		verify(m_checks[7] == 0x71);
	}

	void JitUnittests::transferSaturation_build()
	{
		const RegGP temp(*block);

		block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x00ff700000555555)));
		ops->transferSaturation24(temp, temp);
		block->mem().mov(m_checks[0], temp);

		block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x00008abbcc555555)));
		ops->transferSaturation24(temp, temp);
		block->mem().mov(m_checks[1], temp);

		block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x0000334455667788)));
		ops->transferSaturation24(temp, temp);
		block->mem().mov(m_checks[2], temp);
	}

	void JitUnittests::transferSaturation_verify()
	{
		verify(m_checks[0] == 0x800000);
		verify(m_checks[1] == 0x7fffff);
		verify(m_checks[2] == 0x334455);
	}

	void JitUnittests::transferSaturation48()
	{
		runTest([&]()
		{
			const RegGP temp(*block);

			block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x00ff700000555555)));
			ops->transferSaturation48(temp, temp);
			block->mem().mov(m_checks[0], temp);

			block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x00008abbcc555555)));
			ops->transferSaturation48(temp, temp);
			block->mem().mov(m_checks[1], temp);

			block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x0000334455667788)));
			ops->transferSaturation48(temp, temp);
			block->mem().mov(m_checks[2], temp);

			block->asm_().mov(temp, asmjit::Imm(aluTestValue(0x00fffefefefefefe)));
			ops->transferSaturation48(temp, temp);
			block->mem().mov(m_checks[3], temp);
		}, [&]()
		{
			verify(m_checks[0] == 0x800000000000);
			verify(m_checks[1] == 0x7fffffffffff);
			verify(m_checks[2] == 0x334455667788);
			verify(m_checks[3] == 0xfefefefefefe);
		}
		);
	}

	void JitUnittests::testCCCC(const int64_t _value, const int64_t _compareValue, const bool _lt, bool _le, bool _eq, bool _ge, bool _gt, bool _neq)
	{
		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(_value)));
		runTest([&]()
		{
			const RegGP r(*block);
			block->asm_().mov(r, asmjit::Imm(_compareValue));
			ops->alu_cmp(0, r, false);

			ops->decode_cccc(r.get(), CCCC_LessThan);			block->mem().mov(m_checks[0], r.get());
			ops->decode_cccc(r.get(), CCCC_LessEqual);			block->mem().mov(m_checks[1], r.get());
			ops->decode_cccc(r.get(), CCCC_Equal);				block->mem().mov(m_checks[2], r.get());
			ops->decode_cccc(r.get(), CCCC_GreaterEqual);		block->mem().mov(m_checks[3], r.get());
			ops->decode_cccc(r.get(), CCCC_GreaterThan);		block->mem().mov(m_checks[4], r.get());
			ops->decode_cccc(r.get(), CCCC_NotEqual);			block->mem().mov(m_checks[5], r.get());
		}, [&]()
		{
			verify(_lt == (dsp.decode_cccc(CCCC_LessThan) != 0));
			verify(_le == (dsp.decode_cccc(CCCC_LessEqual) != 0));
			verify(_eq == (dsp.decode_cccc(CCCC_Equal) != 0));
			verify(_ge == (dsp.decode_cccc(CCCC_GreaterEqual) != 0));
			verify(_gt == (dsp.decode_cccc(CCCC_GreaterThan) != 0));
			verify(_neq == (dsp.decode_cccc(CCCC_NotEqual) != 0));	

			verify(_lt  == (m_checks[0] != 0));
			verify(_le  == (m_checks[1] != 0));
			verify(_eq  == (m_checks[2] != 0));
			verify(_ge  == (m_checks[3] != 0));
			verify(_gt  == (m_checks[4] != 0));
			verify(_neq == (m_checks[5] != 0));	
		}
		);
	}

	void JitUnittests::decode_dddddd_write()
	{
		runTest([&]()
		{
			m_checks.fill(0);

			DspValue r(*block);

			for(int i=0; i<8; ++i)
			{
				const TWord inc = i * 0x10000;

				emit(0x60f400 + inc, 0x110000 * (i+1));		// move #$110000,ri
				block->regs().getR(r, i);
				block->mem().mov(m_checks[i], r32(r.get()));

				emit(0x70f400 + inc, 0x001100 * (i+1));		// move #$001100,ni
				block->regs().getN(r, i);
				block->mem().mov(m_checks[i+8], r32(r.get()));

				emit(0x05f420 + i, 0x000011 * (i+1));		// move #$000011,mi
				block->regs().getM(r, i);
				block->mem().mov(m_checks[i+16], r32(r.get()));
			}
		}, [&]()
		{
			for(size_t i=0; i<8; ++i)
			{
				verify(m_checks[i   ] == 0x110000 * (i+1));
				verify(m_checks[i+8 ] == 0x001100 * (i+1));
				verify(m_checks[i+16] == 0x000011 * (i+1));
			}
		});

		runTest([&]()
		{
			m_checks.fill(0);

			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0)));
			dsp.setALU(true , TReg56(static_cast<TReg56::MyType>(0)));

			int i=0;
			const RegGP r(*block);

			emit("move #$111111,a0");
			emit("move #$222222,b0");

			block->regs().getALU(r, 0);	block->mem().mov(m_checks[i++], r.get());
			block->regs().getALU(r, 1);	block->mem().mov(m_checks[i++], r.get());

			emit("move #$111111,a1");
			emit("move #$222222,b1");

			block->regs().getALU(r, 0);	block->mem().mov(m_checks[i++], r.get());
			block->regs().getALU(r, 1);	block->mem().mov(m_checks[i++], r.get());

			emit("move #$111111,a");
			emit("move #$222222,b");

			block->regs().getALU(r, 0);	block->mem().mov(m_checks[i++], r.get());
			block->regs().getALU(r, 1);	block->mem().mov(m_checks[i++], r.get());

			emit("move #$111111,a2");
			emit("move #$222222,b2");

			block->regs().getALU(r, 0);	block->mem().mov(m_checks[i++], r.get());
			block->regs().getALU(r, 1);	block->mem().mov(m_checks[i], r.get());
		}, [&]()
		{
			int i=0;
			// these read the raw accumulator, so the expected value follows the current representation
			verify(m_checks[i++] == aluTestValue(0x00000000111111));
			verify(m_checks[i++] == aluTestValue(0x00000000222222));
			verify(m_checks[i++] == aluTestValue(0x00111111111111));
			verify(m_checks[i++] == aluTestValue(0x00222222222222));
			verify(m_checks[i++] == aluTestValue(0x00111111000000));
			verify(m_checks[i++] == aluTestValue(0x00222222000000));
			verify(m_checks[i++] == aluTestValue(0x11111111000000));
			verify(m_checks[i++] == aluTestValue(0x22222222000000));
		});

		runTest([&]()
		{
			m_checks.fill(0);
			dsp.x0(0xaaaaaa);
			dsp.x1(0xbbbbbb);
			dsp.y0(0xcccccc);
			dsp.y1(0xdddddd);

			int i=0;
			const RegGP r(*block);

			emit("move #$111111,x0");
			block->regs().getXY(r, 0);
			block->mem().mov(m_checks[i++], r.get());

			emit("move #$222222,x1");
			block->regs().getXY(r, 0);
			block->mem().mov(m_checks[i++], r.get());

			emit("move #$333333,y0");
			block->regs().getXY(r, 1);
			block->mem().mov(m_checks[i++], r.get());

			emit("move #$444444,y1");
			block->regs().getXY(r, 1);
			block->mem().mov(m_checks[i], r.get());

		}, [&]()
		{
			int i=0;
			verify(m_checks[i++] == 0xbbbbbb111111);
			verify(m_checks[i++] == 0x222222111111);
			verify(m_checks[i++] == 0xdddddd333333);
			verify(m_checks[i++] == 0x444444333333);
		});
	}

	void JitUnittests::decode_dddddd_read()
	{
		runTest([&]()
		{
			m_checks.fill(0);

			DspValue r(*block);

			for(int i=0; i<8; ++i)
			{
				const TWord inc = i * 0x10000;

				dsp.regs().r[i].var = (i+1) * 0x110000;
				dsp.regs().n[i].var = (i+1) * 0x001100;
				dsp.set_m(i, (i+1) * 0x000011);

				emit(0x600500 + inc);						// asm move ri,x:$5
				block->mem().readDspMemory(r, MemArea_X, 0x5);
				block->mem().mov(m_checks[i], r32(r.get()));

				emit(0x700500 + inc);						// asm move ni,x:$5
				block->mem().readDspMemory(r, MemArea_X, 0x5);
				block->mem().mov(m_checks[i+8], r32(r.get()));

				emit(0x050520 + i);							// asm move mi,x:$5
				block->mem().readDspMemory(r, MemArea_X, 0x5);
				block->mem().mov(m_checks[i+16], r32(r.get()));
			}
		}, [&]()
		{
			for(size_t i=0; i<8; ++i)
			{
				verify(m_checks[i   ] == 0x110000 * (i+1));
				verify(m_checks[i+8 ] == 0x001100 * (i+1));
				verify(m_checks[i+16] == 0x000011 * (i+1));
			}
		});

		runTest([&]()
		{
			m_checks.fill(0);
			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x11112233445566)));
			dsp.setALU(true , TReg56(static_cast<TReg56::MyType>(0xff5566778899aa)));

			int i=0;
			DspValue r(*block);

			emit("move a,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move b,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move a0,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move a1,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move a2,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move b0,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move b1,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));

			emit("move b2,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i], r32(r.get()));
		}, [&]()
		{
			int i=0;
			verify(m_checks[i++] == 0x7fffff); // a
			verify(m_checks[i++] == 0x800000); // b
			verify(m_checks[i++] == 0x445566); // a0
			verify(m_checks[i++] == 0x112233); // a1
			verify(m_checks[i++] == 0x000011); // a2
			verify(m_checks[i++] == 0x8899aa); // b0
			verify(m_checks[i++] == 0x556677); // b1
			verify(m_checks[i++] == 0xffffff); // b2
		});

		runTest([&]()
		{
			m_checks.fill(0);
			dsp.x0(0xaaabbb);
			dsp.x1(0xcccddd);
			dsp.y0(0xeeefff);
			dsp.y1(0x111222);

			int i=0;
			DspValue r(*block);

			emit("move x0,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));
			emit("move x1,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));
			emit("move y0,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i++], r32(r.get()));
			emit("move y1,x:$5");
			block->mem().readDspMemory(r, MemArea_X, 0x5);
			block->mem().mov(m_checks[i], r32(r.get()));
		}, [&]()
		{
			int i=0;
			verify(m_checks[i++] == 0xaaabbb);
			verify(m_checks[i++] == 0xcccddd);
			verify(m_checks[i++] == 0xeeefff);
			verify(m_checks[i++] == 0x111222);
		});
	}

	void JitUnittests::getSS_build()
	{
		const RegGP temp(*block);

		dsp.regs().sp.var = 0xf0;

		for(int i=0; i<dsp.regs().ss.size(); ++i)
		{			
			dsp.regs().ss[i].var = 0x111111111111 * i;

			block->regs().getSS(temp);
			ops->incSP();
			block->mem().mov(m_checks[i], temp);
		}
	}

	void JitUnittests::getSS_verify()
	{
		for(int i=0; i<dsp.regs().ss.size(); ++i)
			verify(dsp.regs().ss[i].var == 0x111111111111 * i);
	}

	void JitUnittests::getSetRegs_build()
	{
		dsp.regs().ep.var = 0x112233;
		dsp.regs().vba.var = 0x223344;
		dsp.regs().sc.var = 0x33;
		dsp.regs().sz.var = 0x556677;
		dsp.regs().omr.var = 0x667788;
		dsp.regs().sp.var = 0x778899;
		dsp.regs().la.var = 0x8899aa;
		dsp.regs().lc.var = 0x99aabb;

		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00ffaabbcc112233)));
		dsp.setALU(true , TReg56(static_cast<TReg56::MyType>(0x00ee112233445566)));

		dsp.regs().x.var = 0x0000aabbccddeeff;

		const RegGP temp(*block);
		const auto r = r32(temp.get());

		auto& regs = block->regs();

		auto modify = [&](const JitRegGP& t)
		{
			block->asm_().shl(r32(t), asmjit::Imm(4));

#ifdef HAVE_ARM64
			block->asm_().and_(r32(t), r32(t), asmjit::Imm(0xffffff));
#else
			block->asm_().and_(r32(t), asmjit::Imm(0xffffff));
#endif
		};

		auto modify64 = [&](const JitRegGP& t)
		{
			block->asm_().shl(r64(t), asmjit::Imm(4));
		};

		DspValue v32(*block, UsePooledTemp);

		regs.getEP(v32);				modify(v32.get());		regs.setEP(v32);
		regs.getVBA(v32);				modify(v32.get());		regs.setVBA(v32);
		regs.getSC(v32);				modify(v32.get());		regs.setSC(v32);
		regs.getSZ(v32);				modify(v32.get());		regs.setSZ(v32);
		regs.getOMR(v32);				modify(v32.get());		regs.setOMR(v32);
		regs.getSP(v32);				modify(v32.get());		regs.setSP(v32);
		regs.getLA(v32);				modify(v32.get());		regs.setLA(v32);
		regs.getLC(v32);				modify(v32.get());		regs.setLC(v32);

		ops->getALU0(v32, 0);			modify64(v32.get());		ops->setALU0(0,v32);
		ops->getALU1(v32, 0);			modify64(v32.get());		ops->setALU1(0,v32);
		ops->getALU2signed(v32, 0);	modify64(v32.get());		ops->setALU2(0,v32);

		DspValue v64(*block, UsePooledTemp);
		v64.temp(DspValue::Temp56);

		regs.getALU(v64.get(), 1);			modify64(v64.get());	regs.setALU(1, v64);

		ops->getXY0(v32, 0, false);	modify(v32.get());		ops->setXY0(0, v32);
		ops->getXY1(v32, 0, false);	modify(v32.get());		ops->setXY1(0, v32);
	}

	void JitUnittests::getSetRegs_verify()
	{
		auto& r = dsp.regs();

		verify(r.ep.var == 0x122330);
		verify(r.vba.var == 0x233440);
		verify(r.sc.var == 0x30);
		verify(r.sz.var == 0x566770);
		verify(r.omr.var == 0x677880);
		verify(r.sp.var == 0x788990);
		verify(r.la.var == 0x899aa0);
		verify(r.lc.var == 0x9aabb0);

		verify(dsp.aluA().var == 0x00f0abbcc0122330);
		verify(dsp.aluB().var == 0x00e1122334455660);

		verify(r.x.var == 0x0000abbcc0deeff0);
	}

	void JitUnittests::jitDiv()
	{
		static constexpr uint64_t expectedValues[24] =
		{
			0xffef590e000000,
			0xffef790e000000,
			0xffefb90e000000,
			0xfff0390e000000,
			0xfff1390e000000,
			0xfff3390e000000,
			0xfff7390e000000,
			0xffff390e000000,
			0x000f390e000000,
			0x000dab2a000001,
			0x000a8f62000003,
			0x000457d2000007,
			0xfff7e8b200000f,
			0x0000985600001e,
			0xfff069ba00003d,
			0xfff19a6600007a,
			0xfff3fbbe0000f4,
			0xfff8be6e0001e8,
			0x000243ce0003d0,
			0xfff3c0aa0007a1,
			0xfff84846000f42,
			0x0001577e001e84,
			0xfff1e80a003d09,
			0xfff49706007a12
		};

		dsp.setSR(dsp.getSR().var & 0xfe);
		dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00001000000000)));
		dsp.regs().y.var =   0x04444410c6f2;

		runTest([&]()
		{
			for(size_t i=0; i<24; ++i)
			{
				emit("div y0,a");
				RegGP r(*block);
				block->regs().getALU(r, 0);
				block->mem().mov(m_checks[i], r.get());
			}
		},
		[&]()
		{
			for(size_t i=0; i<24; ++i)
			{
				// reads the raw accumulator, so the expected value follows the representation
				verify(m_checks[i] == aluTestValue(expectedValues[i]));
			}
		});
	}

	void JitUnittests::rep_div()
	{
		{
			// regular mode for comparison

			dsp.y0(0x218dec);
			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00008000000000)));
			dsp.setSR(0x0800d4);

			static constexpr uint64_t expectedValues[24] =
			{
				0xffdf7214000000,
				0xffe07214000000,
				0xffe27214000000,
				0xffe67214000000,
				0xffee7214000000,
				0xfffe7214000000,
				0x001e7214000000,
				0x001b563c000001,
				0x00151e8c000003,
				0x0008af2c000007,
				0xffefd06c00000f,
				0x00012ec400001e,
				0xffe0cf9c00003d,
				0xffe32d2400007a,
				0xffe7e8340000f4,
				0xfff15e540001e8,
				0x00044a940003d0,
				0xffe7073c0007a1,
				0xffef9c64000f42,
				0x0000c6b4001e84,
				0xffdfff7c003d09,
				0xffe18ce4007a12,
				0xffe4a7b400f424,
				0xffeadd5401e848
			};

			for (size_t i = 0; i < 24; ++i)
			{
				runTest([&]()
				{

					emit("div y0,a");
				},
					[&]()
				{
					verify(dsp.aluA().var == static_cast<int64_t>(expectedValues[i]));
				});
			}
		}

		runTest([&]()
		{
			dsp.y0(0x218dec);
			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00008000000000)));
			dsp.setSR(0x0800d4);

			dsp.memory().set(MemArea_P, 0, 0x0618a0);	// rep #<18
			dsp.memory().set(MemArea_P, 1, 0x018050);	// div y0,a

			ops->emit(0);
		},
		[&]()
		{
			verify(dsp.aluA().var == 0xffeadd5401e848);
			verify(dsp.getSR().var == 0x0800d4);
		});
		{
			// regular mode for comparison

			dsp.y0(0xde7214);
			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00008000000000)));
			dsp.setSR(0x0800d4);

			static constexpr uint64_t expectedValues[24] =
			{
				0xffdf7214000000,
				0xffe07214000000,
				0xffe27214000000,
				0xffe67214000000,
				0xffee7214000000,
				0xfffe7214000000,
				0x001e7214000000,
				0x001b563c000001,
				0x00151e8c000003,
				0x0008af2c000007,
				0xffefd06c00000f,
				0x00012ec400001e,
				0xffe0cf9c00003d,
				0xffe32d2400007a,
				0xffe7e8340000f4,
				0xfff15e540001e8,
				0x00044a940003d0,
				0xffe7073c0007a1,
				0xffef9c64000f42,
				0x0000c6b4001e84,
				0xffdfff7c003d09,
				0xffe18ce4007a12,
				0xffe4a7b400f424,
				0xffeadd5401e848,
			};

			for (size_t i = 0; i < 24; ++i)
			{
				runTest([&]()
				{

					emit("div y0,a");
				},
					[&]()
				{
					verify(dsp.aluA().var == static_cast<int64_t>(expectedValues[i]));
				});
			}
		}

		runTest([&]()
		{
			dsp.y0(0xde7214);
			dsp.setALU(false, TReg56(static_cast<TReg56::MyType>(0x00008000000000)));
			dsp.setSR(0x0800d4);

			dsp.memory().set(MemArea_P, 0, 0x0618a0);	// rep #<18
			dsp.memory().set(MemArea_P, 1, 0x018050);	// div y0,a

			ops->emit(0);
		},
		[&]()
		{
			verify(dsp.aluA().var == 0xffeadd5401e848);
			verify(dsp.getSR().var == 0x0800d4);
		});
	}

	void JitUnittests::parallelMoveXY()
	{
		runTest([&]()
		{
			dsp.set_m(0, 0xff);
			dsp.set_m(4, 0xff);

			emit(0xf09818);	// add a,b x:(r0)+,x0 y:(r4)+,y0
			emit(0x950818);	// add a,b x1,x:(r0)+n0 y1,y:(r4)+n4
			emit(0xbb9818);	// add a,b x:(r0)+,a b,y:(r4)+
		}, [&]()
		{
			dsp.set_m(0, 0xffffff);
			dsp.set_m(4, 0xffffff);
		});
	}

	void JitUnittests::emit(const TWord _opA, TWord _opB, TWord _pc)
	{
		JitDspMode mode;

		if constexpr(g_useDspMode)
		{
			mode.initialize(dsp);
			block->setMode(&mode);
		}

		block->asm_().nop();
		ops->emit(_pc, _opA, _opB);
		block->asm_().nop();

		if constexpr(g_useDspMode)
			block->setMode(nullptr);
	}
}
