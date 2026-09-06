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
		case CCCC_NotNormalized:							// NN			Not normalized
			{
				// Tcc may already hold a transfer operand. Resolve the flags
				// before reserving both condition temporaries: deferred U/E
				// computation otherwise exceeds the four-register temp pool.
				updateDirtyCCR(static_cast<CCRMask>(CCR_U | CCR_E | CCR_Z));
				// NR = Z || !(U || E); NN is its complement.
				const RegGP dst(m_block);
				const RegGP r(m_block);
				ccr_getBitValue(dst, CCRB_U);
				ccr_getBitValue(r, CCRB_E);
				m_asm.orr(dst, dst, r);
				m_asm.eor(dst, dst, asmjit::Imm(1));
				ccr_getBitValue(r, CCRB_Z);
				m_asm.orr(dst, dst, r);
				m_asm.tst(dst, dst);
				return cccc == CCCC_Normalized ? asmjit::arm::CondCode::kNotZero : asmjit::arm::CondCode::kZero;
			}
		case CCCC_GreaterThan:								// GT			Greater than
		case CCCC_LessEqual:								// LE			Less than or equal
			{
				// LE = Z || (N != V); GT is its complement.
				updateDirtyCCR(static_cast<CCRMask>(CCR_N | CCR_V | CCR_Z));
				const RegGP r(m_block);
				const RegGP dst(m_block);

				ccr_getBitValue(dst, CCRB_N);
				ccr_getBitValue(r, CCRB_V);

				m_asm.eor(dst, dst, r.get());
				ccr_getBitValue(r, CCRB_Z);
				m_asm.orr(dst, dst, r);
				m_asm.tst(dst, dst);
				return cccc == CCCC_GreaterThan ? asmjit::arm::CondCode::kZero : asmjit::arm::CondCode::kNotZero;
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
