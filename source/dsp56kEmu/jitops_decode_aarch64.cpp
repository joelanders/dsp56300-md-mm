#include "jittypes.h"

#ifdef HAVE_ARM64

#include "dsp56kBase/dspassert.h"

#include "jitops.h"
#include "asmjit/core/operand.h"

namespace dsp56k
{
	asmjit::arm::CondCode JitOps::reverseCC(asmjit::arm::CondCode _cc)
	{
		if (_cc == asmjit::arm::CondCode::kZero)		return asmjit::arm::CondCode::kNotZero;
		if (_cc == asmjit::arm::CondCode::kNotZero)		return asmjit::arm::CondCode::kZero;

		assert(false && "invalid CC");
		return _cc;
	}

	asmjit::arm::CondCode JitOps::decode_cccc(const TWord cccc)
	{
		auto ccrBitTest = [&](const CCRBit _bit)
		{
			const auto mask = static_cast<CCRMask>(1 << _bit);
			m_ccrRead |= mask;
			updateDirtyCCR(mask);
			m_asm.bitTest(r32(m_dspRegs.getSR(JitDspRegs::Read)), _bit);
		};

		if(m_block.getConfig().optimizeCcrSequences)
		{
			if(cccc == CCCC_GreaterEqual || cccc == CCCC_LessThan)
			{
				const RegGP r(m_block);
				m_ccrRead |= static_cast<CCRMask>(CCR_N | CCR_V);
				// Preserve the previous lazy-materialization order; only the final
				// extraction/comparison is folded into an XOR and single-bit test.
				updateDirtyCCRWithTemp(r, CCR_N);
				updateDirtyCCRWithTemp(r, CCR_V);
				const auto sr = r32(m_dspRegs.getSR(JitDspRegs::Read));
				static_assert(CCRB_N > CCRB_V);
				m_asm.eor(r32(r), sr, sr, asmjit::arm::lsl(CCRB_N - CCRB_V));
				m_asm.bitTest(r32(r), CCRB_N);
				return cccc == CCCC_GreaterEqual ? asmjit::arm::CondCode::kZero : asmjit::arm::CondCode::kNotZero;
			}
			if(cccc == CCCC_Normalized || cccc == CCCC_NotNormalized)
			{
				const RegGP r(m_block);
				constexpr auto mask = static_cast<CCRMask>(CCR_U | CCR_E | CCR_Z);
				m_ccrRead |= mask;
				updateDirtyCCRWithTemp(r, CCR_U);
				updateDirtyCCRWithTemp(r, CCR_E);
				updateDirtyCCRWithTemp(r, CCR_Z);
				// This mask is not an ARM logical immediate: keep it in a register.
				m_asm.mov(r32(r), asmjit::Imm(mask));
				m_asm.tst(r32(m_dspRegs.getSR(JitDspRegs::Read)), r32(r));
				return cccc == CCCC_Normalized ? asmjit::arm::CondCode::kZero : asmjit::arm::CondCode::kNotZero;
			}
		}

		switch (cccc)
		{
		case CCCC_CarrySet:									// CC(LO)		Carry Set	(lower)
			ccrBitTest(CCRB_C);
			return asmjit::arm::CondCode::kNotZero;
		case CCCC_CarryClear:								// CC(HS)		Carry Clear (higher or same)	
			ccrBitTest(CCRB_C);
			return asmjit::arm::CondCode::kZero;
		case CCCC_ExtensionSet:								// ES			Extension set	
			ccrBitTest(CCRB_E);
			return asmjit::arm::CondCode::kNotZero;
		case CCCC_ExtensionClear:							// EC			Extension clear	
			ccrBitTest(CCRB_E);
			return asmjit::arm::CondCode::kZero;
		case CCCC_Equal:									// EQ			Equal	
			ccrBitTest(CCRB_Z);
			return asmjit::arm::CondCode::kNotZero;
		case CCCC_NotEqual:									// NE			Not Equal
			ccrBitTest(CCRB_Z);
			return asmjit::arm::CondCode::kZero;
		case CCCC_LimitSet:									// LS			Limit set
			ccrBitTest(CCRB_L);
			return asmjit::arm::CondCode::kNotZero;
		case CCCC_LimitClear:								// LC			Limit clear
			ccrBitTest(CCRB_L);
			return asmjit::arm::CondCode::kZero;
		case CCCC_Minus:									// MI			Minus
			ccrBitTest(CCRB_N);
			return asmjit::arm::CondCode::kNotZero;
		case CCCC_Plus:										// PL			Plus
			ccrBitTest(CCRB_N);
			return asmjit::arm::CondCode::kZero;
		case CCCC_GreaterEqual:								// GE			Greater than or equal
			{
				// SRB_N == SRB_V
				const RegGP r(m_block);
				const RegGP dst(m_block);
				ccr_getBitValue(dst, CCRB_N);
				ccr_getBitValue(r, CCRB_V);
				m_asm.cmp(dst, r.get());
				return asmjit::arm::CondCode::kZero;
			}
		case CCCC_LessThan:									// LT			Less than
			{
				// SRB_N != SRB_V
				const RegGP r(m_block);
				const RegGP dst(m_block);
				ccr_getBitValue(dst, CCRB_N);
				ccr_getBitValue(r, CCRB_V);
				m_asm.cmp(dst, r);
				return asmjit::arm::CondCode::kNotZero;
			}
		case CCCC_Normalized:								// NR			Normalized
			{
				// (SRB_Z + ((!SRB_U) | (!SRB_E))) == 1
				const RegGP dst(m_block);
				const RegGP r(m_block);
				ccr_getBitValue(dst, CCRB_U);
				ccr_getBitValue(r, CCRB_E);
				m_asm.orr(dst, dst, r);
				ccr_getBitValue(r, CCRB_Z);
				m_asm.orr(dst, dst, r);
				m_asm.tst(dst, dst);
				return asmjit::arm::CondCode::kZero;
			}
		case CCCC_NotNormalized:							// NN			Not normalized
			{
				// (SRB_Z + ((!SRB_U) | !SRB_E)) == 0
				const RegGP dst(m_block);
				const RegGP r(m_block);
				ccr_getBitValue(dst, CCRB_U);
				ccr_getBitValue(r, CCRB_E);
				m_asm.orr(dst, dst, r);
				ccr_getBitValue(r, CCRB_Z);
				m_asm.orr(dst, dst, r);
				m_asm.tst(dst, dst);
				return asmjit::arm::CondCode::kNotZero;
			}
		case CCCC_GreaterThan:								// GT			Greater than
			{
				// (SRB_Z + (SRB_N != SRB_V)) == 0
				const RegGP r(m_block);
				const RegGP dst(m_block);

				ccr_getBitValue(dst, CCRB_N);
				ccr_getBitValue(r, CCRB_V);

				m_asm.eor(dst, dst, r.get());
				ccr_getBitValue(r, CCRB_Z);
				m_asm.adds(dst, dst, r.get());
				return asmjit::arm::CondCode::kZero;
			}
		case CCCC_LessEqual:								// LE			Less than or equal
			{
				// (SRB_Z + (SRB_N != SRB_V)) == 1
				const RegGP r(m_block);
				const RegGP dst(m_block);

				ccr_getBitValue(dst, CCRB_N);
				ccr_getBitValue(r, CCRB_V);

				m_asm.eor(dst, dst, r.get());
				ccr_getBitValue(r, CCRB_Z);
				m_asm.add(dst, dst, r.get());
				m_asm.cmp(dst, asmjit::Imm(1));
				return asmjit::arm::CondCode::kZero;
			}
		default:
			assert(0 && "invalid CCCC value");
			return asmjit::arm::CondCode::kMaxValue;
		}
	}

	void JitOps::decode_cccc(const JitRegGP& _dst, const TWord cccc)
	{
		const auto cc = decode_cccc(cccc);
		m_asm.cset(_dst, cc);
	}
}

#endif
