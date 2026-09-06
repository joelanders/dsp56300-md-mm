# ARM64 runtime scaling and the unnormalized flag

## Public requirement and defect

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
printed page 5-15, defines U by equality of two adjacent result bits:
47/46 without scaling, 48/47 when scaling down, and 46/45 when scaling up.
This is independent of the compound NR/NN condition equations, which still
need their own audit.

ARM64's runtime-mode path in `ccr_u_update` first shifted the accumulator by
46 plus its storage offset, then shifted again by S0-S1. In scale-up mode
that second count is -1; a native variable right shift does not shift the
operand back left. The first shift has already discarded bit 45. This path
could report U=1 for a normalized result. The compile-time-mode path computes
one complete shift and does not have this defect.

The correction computes `46 + storageOffset + S0 - S1` first and performs
one right shift. The count is nonnegative for every valid scaling mode. It
does not change constant-mode generation, the x86-64 implementation, flag
caching policy, status-register layout or instruction timing.

## Reproduction and provenance

The ASR/ROL flag-preservation regression exposed U=1 instead of U=0 in
scale-up mode when the test runner resolved deferred flags without a
compile-time mode. Its input A=0x00800000000001 becomes 0x00400000000000
after ASR; bits 46 and 45 differ. Observed final SR=0x838 versus expected
0x828. This was not another rotate-operand defect.

`JitUnittests::runtimeUnnormalizedFlag` independently tests TST A with every
pattern of bits 48–45 in all three valid scaling modes: 48 cases. It verifies
that the final deferred-flag resolution has no compile-time mode, initializes
U opposite its expected value, and checks U, the accumulator and scaling bits.
Before the correction, it fails at scaling=2, bits=1: U=1 versus expected 0.
No ROM, private address, firmware trace or product-specific behavior is used.

Local history contains the two-stage shift in `f987c9e68` (August 2021).
Later refactoring and the August 2026 accumulator-alignment port retained it.
This is provenance within the available repository, not a current external
upstream audit.

## Validation and limits

The 48-case runtime-U regression passes both JIT architectures after the
correction. At the final working-tree state, together with the separately
documented rotate fixes, the full core suites pass ARM64 JIT (3.72 s),
x86-64 JIT (6.35 s), and forced ARM64 interpreter (5.20 s). The latter does
not execute this JIT-only helper regression.

Normal MM strict sine tests pass both JIT architectures, including all six
tracks and parameter sweeps; ARM64 MD UW/RAM also passes. Forced-interpreter
MM still fails at idle RMS 2.51706e-6, and ARM64 post-setup cap-1 playback
still fails the first-note quality check (RMS 0.112626, roughness 4.9428e-5).
These are combined branch-state regression results, not standalone
product-level validation of this one helper in isolation.

The application's ARM64 instruction-pair matrix already passes with the
rotate correction alone, so that matrix is not proof of runtime-mode U
correctness. Confidence is high in this isolated shift-count correction;
no claim is made that this path causes the MM audio symptoms, that every
dynamic-mode path is correct, or that tests establish hardware equivalence.
