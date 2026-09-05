#include "jittypes.h"

#ifdef HAVE_X86_64

#include "dsp56kBase/dspassert.h"

#include "jitops.h"
#include "asmjit/core/operand.h"

namespace dsp56k
{
	asmjit::x86::CondCode JitOps::reverseCC(const asmjit::x86::CondCode _cc)
	{
		return negateCond(_cc);
	}

	asmjit::x86::CondCode JitOps::decode_cccc(TWord cccc)
	{
		auto ccrMaskTest = [&](const CCRMask _mask)
		{
			m_ccrRead |= _mask;
			updateDirtyCCR(_mask);
			m_asm.test(m_dspRegs.getSR(JitDspRegs::Read).r32(), asmjit::Imm(_mask));
		};

		auto ccrBitTest = [&](const CCRBit _bit)
		{
			const auto mask = static_cast<CCRMask>(1 << _bit);
			m_ccrRead |= mask;
			updateDirtyCCR(mask);
			m_asm.bitTest(m_dspRegs.getSR(JitDspRegs::Read).r32(), _bit);
		};

		switch (cccc)
		{
		case CCCC_CarrySet:											// CC(LO)		Carry Set	(lower)
			ccrBitTest(CCRB_C);
			return asmjit::x86::CondCode::kNotZero;
		case CCCC_CarryClear:										// CC(HS)		Carry Clear (higher or same)	
			ccrBitTest(CCRB_C);
			return asmjit::x86::CondCode::kZero;
		case CCCC_ExtensionSet:										// ES			Extension set	
			ccrBitTest(CCRB_E);
			return asmjit::x86::CondCode::kNotZero;
		case CCCC_ExtensionClear:									// EC			Extension clear	
			ccrBitTest(CCRB_E);
			return asmjit::x86::CondCode::kZero;
		case CCCC_Equal:											// EQ			Equal	
			ccrBitTest(CCRB_Z);
			return asmjit::x86::CondCode::kNotZero;
		case CCCC_NotEqual:											// NE			Not Equal
			ccrBitTest(CCRB_Z);
			return asmjit::x86::CondCode::kZero;
		case CCCC_LimitSet:											// LS			Limit set
			ccrBitTest(CCRB_L);
			return asmjit::x86::CondCode::kNotZero;
		case CCCC_LimitClear:										// LC			Limit clear
			ccrBitTest(CCRB_L);
			return asmjit::x86::CondCode::kZero;
		case CCCC_Minus:											// MI			Minus
			ccrBitTest(CCRB_N);
			return asmjit::x86::CondCode::kNotZero;
		case CCCC_Plus:												// PL			Plus
			ccrBitTest(CCRB_N);
			return asmjit::x86::CondCode::kZero;
		case CCCC_GreaterEqual:										// GE			Greater than or equal
			// SRB_N == SRB_V
			ccrMaskTest(static_cast<CCRMask>(CCR_N | CCR_V));
			return asmjit::x86::CondCode::kP;
		case CCCC_LessThan:											// LT			Less than
			// SRB_N != SRB_V
			ccrMaskTest(static_cast<CCRMask>(CCR_N | CCR_V));
			return asmjit::x86::CondCode::kNP;
		case CCCC_Normalized:										// NR			Normalized
		case CCCC_NotNormalized:									// NN			Not normalized
			{
				// NR = Z || !(U || E); NN is its complement.
				updateDirtyCCR(static_cast<CCRMask>(CCR_U | CCR_E | CCR_Z));
				const RegGP dst(m_block);
				const RegGP r(m_block);
				// ccr_getBitValue materializes only the low byte on x86-64.
				ccr_getBitValue(dst, CCRB_U);
				ccr_getBitValue(r, CCRB_E);
				m_asm.or_(dst.get().r8(), r.get().r8());
				m_asm.xor_(dst.get().r8(), asmjit::Imm(1));
				ccr_getBitValue(r, CCRB_Z);
				m_asm.or_(dst.get().r8(), r.get().r8());
				return cccc == CCCC_Normalized ? asmjit::x86::CondCode::kNotZero : asmjit::x86::CondCode::kZero;
			}
		case CCCC_GreaterThan:										// GT			Greater than
		case CCCC_LessEqual:										// LE			Less than or equal
			{
				// LE = Z || (N != V); GT is its complement. Parity of Z/N/V
				// would incorrectly cancel Z when N != V is also true.
				updateDirtyCCR(static_cast<CCRMask>(CCR_N | CCR_V | CCR_Z));
				const RegGP dst(m_block);
				const RegGP r(m_block);
				// Ignore the unspecified upper bits of the flag temporaries.
				ccr_getBitValue(dst, CCRB_N);
				ccr_getBitValue(r, CCRB_V);
				m_asm.xor_(dst.get().r8(), r.get().r8());
				ccr_getBitValue(r, CCRB_Z);
				m_asm.or_(dst.get().r8(), r.get().r8());
				return cccc == CCCC_GreaterThan ? asmjit::x86::CondCode::kZero : asmjit::x86::CondCode::kNotZero;
			}
		default:
			assert(0 && "invalid CCCC value");
			return asmjit::x86::CondCode::kMaxValue;
		}
	}

	void JitOps::decode_cccc(const JitRegGP& _dst, const TWord cccc)
	{
		const auto cc = decode_cccc(cccc);
		m_asm.set(cc, _dst.r8());
	}
}

#endif
