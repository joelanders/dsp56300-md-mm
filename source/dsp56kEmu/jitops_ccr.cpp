#include "jitblock.h"
#include "jitops.h"
#include "jitregtypes.h"

#ifdef HAVE_ARM64
#include "asmjit/arm/a64utils.h"
#endif

namespace dsp56k
{
	constexpr bool g_useSRCache = true;

	void JitOps::ccr_set(CCRMask _mask)
	{
		ccr_clearDirty(_mask);
		m_asm.or_(r32(m_dspRegs.getSR(JitDspRegs::ReadWrite)), asmjit::Imm(_mask));
	}

	void JitOps::ccr_dirty(TWord _aluIndex, const JitReg64& _alu, const CCRMask _dirtyBits)
	{
		m_ccrWritten |= _dirtyBits;

		if constexpr(g_useSRCache)
		{
			// if the last dirty call marked bits as dirty that are no longer to be dirtied now, we need to update them
			const auto lastDirty = m_ccrDirty & ~_dirtyBits;
			updateDirtyCCR(static_cast<CCRMask>(lastDirty));

			if(!m_disableCCRUpdates)
			{
				m_block.stack().setUsed(regLastModAlu);
				m_asm.movq(regLastModAlu, _alu);
			}

			m_ccrDirty = static_cast<CCRMask>(m_ccrDirty | _dirtyBits);
		}
		else
		{
			updateDirtyCCR(_alu, _dirtyBits);
		}
	}

	void JitOps::ccr_clearDirty(const CCRMask _mask)
	{
		m_ccrWritten |= _mask;
		m_ccrDirty = static_cast<CCRMask>(m_ccrDirty & ~_mask);
	}

	void JitOps::updateDirtyCCR()
	{
		if(!m_ccrDirty)
			return;

		updateDirtyCCR(m_ccrDirty);
	}

	void JitOps::updateDirtyCCR(const CCRMask _whatToUpdate)
	{
		const auto dirty = m_ccrDirty & _whatToUpdate;
		if(!dirty)
			return;

		const RegGP r(m_block);
		updateDirtyCCRWithTemp(r, static_cast<CCRMask>(dirty));
	}

	void JitOps::updateDirtyCCRWithTemp(const JitRegGP& _temp, const CCRMask _whatToUpdate)
	{
		const auto dirty = m_ccrDirty & _whatToUpdate;
		if(!dirty)
			return;

		m_asm.movq(r64(_temp), regLastModAlu);
		updateDirtyCCR(r64(_temp), static_cast<CCRMask>(dirty));
	}

	void JitOps::updateDirtyCCR(const JitReg64& _alu, CCRMask _dirtyBits)
	{
		// Every recognized lazy flag below is assigned, not ORed, on ARM64.
		CcrBatchUpdate u(*this, _dirtyBits, true);

		if(_dirtyBits & CCR_V)
		{
			ccr_v_update(_alu);
			m_dspRegs.mask56(_alu);
		}
		if(_dirtyBits & CCR_Z)
		{
			m_asm.test_(_alu);
			ccr_update_ifZero(CCRB_Z);
		}
		if(_dirtyBits & CCR_N)
			ccr_n_update_by55(_alu);
		if(_dirtyBits & CCR_E)
			ccr_e_update(_alu);
		if(_dirtyBits & CCR_U)
			ccr_u_update(_alu);
	}

	void JitOps::ccr_v_update(const JitReg64& _nonMaskedResult)
	{
		{
			const RegScratch signextended(m_block);
			aluSignextendTo64(signextended, _nonMaskedResult);	// accumulator, not a raw 56-bit value
			m_asm.cmp(signextended, _nonMaskedResult);
		}

		ccr_vl_update_ifNotZero();
	}


	void JitOps::checkCondition(const TWord _cc, const std::function<void()>& _true, const std::function<void()>& _false, bool _hasFalseFunc, bool _updateDirtyCCR, bool _releaseRegPool)
	{
		DspValue sr(m_block, PoolReg::DspSR, true, false);

		If(m_block, m_blockRuntimeData, [&](const asmjit::Label& _toFalse)
		{
#ifdef HAVE_ARM64
			const auto cc = decode_cccc(_cc);
			m_block.dspRegPool().releaseNonLocked();
			m_asm.b(reverseCC(cc), _toFalse);
#else
			const auto cc = decode_cccc(_cc);
			m_block.dspRegPool().releaseNonLocked();
			m_asm.j(reverseCC(cc), _toFalse);
#endif
		}, _true, _false, _hasFalseFunc, _updateDirtyCCR, _releaseRegPool);
	}

	JitOps::CcrBatchUpdate::CcrBatchUpdate(JitOps& _ops, const CCRMask _mask) : CcrBatchUpdate(_ops, _mask, false) {}

	JitOps::CcrBatchUpdate::CcrBatchUpdate(JitOps& _ops, const CCRMask _mask, const bool _allAssigned) : m_ops(_ops)
	{
		initialize(_mask, _allAssigned);
	}

	JitOps::CcrBatchUpdate::CcrBatchUpdate(JitOps& _ops, CCRMask _maskA, CCRMask _maskB) : CcrBatchUpdate(_ops,  static_cast<CCRMask>(_maskA | _maskB)) {}
	JitOps::CcrBatchUpdate::CcrBatchUpdate(JitOps& _ops, CCRMask _maskA, CCRMask _maskB, CCRMask _maskC) : CcrBatchUpdate(_ops,  static_cast<CCRMask>(_maskA | _maskB | _maskC)) {}

	JitOps::CcrBatchUpdate::~CcrBatchUpdate()
	{
		m_ops.m_ccr_update_clear = true;
	}

	void JitOps::CcrBatchUpdate::initialize(CCRMask _mask, const bool _allAssigned) const
	{
#ifdef HAVE_ARM64
		const auto keep = static_cast<uint32_t>(~_mask);
		constexpr auto assignedFlags = CCR_N | CCR_V | CCR_Z | CCR_E | CCR_U;
		if(m_ops.getBlock().getConfig().optimizeCcrSequences && _allAssigned &&
			(_mask & ~assignedFlags) == 0)
		{
			// Each bit is replaced by BFI below. None of these updates observes
			// another bit in this mask before its assignment; sticky L is updated
			// from the newly assigned V. Keep all compile-time bookkeeping below.
		}
		else if(m_ops.getBlock().getConfig().optimizeCcrSequences &&
			asmjit::a64::Utils::isLogicalImm(keep, 32))
		{
			// AND (not ANDS) must leave the host condition flags intact.
			m_ops.m_asm.and_(r32(m_ops.m_dspRegs.getSR(JitDspRegs::ReadWrite)), asmjit::Imm(keep));
		}
		else
		{
			// A discontiguous CCR mask is not necessarily an ARM logical immediate.
			const RegScratch scratch(m_ops.getBlock());
			m_ops.m_asm.mov(r32(scratch), asmjit::Imm(keep));
			m_ops.m_asm.and_(r32(m_ops.m_dspRegs.getSR(JitDspRegs::ReadWrite)), r32(scratch));
		}
#else
		m_ops.m_asm.and_(r32(m_ops.m_dspRegs.getSR(JitDspRegs::ReadWrite)), asmjit::Imm(~_mask));
#endif

		m_ops.m_ccrDirty = static_cast<CCRMask>(m_ops.m_ccrDirty & ~_mask);
		m_ops.m_ccr_update_clear = false;
	}
}
