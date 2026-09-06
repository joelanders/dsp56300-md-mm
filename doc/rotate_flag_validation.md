# 24-bit rotate operands and condition flags

## Public requirements

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
ROL/ROR (printed pages 13-165/166), specifies rotation through C of accumulator
bits 47–24 only. EXP/LSP and E/U are preserved. N follows result bit 47,
Z tests the 24-bit result, and V is cleared. S/L follow their standard rules;
these no-parallel-move cases must not clear an already latched L.

The ROR page is internally inconsistent: its description and diagram send
old bit 24 to C, while the C-condition footnote says bit 47. We follow the
operation description and diagram, corroborated by the worked ROR example
in the public [DSP56000/DSP56001 manual, A-196](https://www.nxp.com/docs/en/user-guide/DSP56001UMA2.pdf).
The existing bit-24 carry-out behavior is retained, not changed by this fix.
This interpretation is documented rather than treating the contradictory
footnote as unambiguous hardware evidence.

## Defects and corrections

1. Both JITs' `ccr_n_update_by23` added the accumulator storage offset to a
   right-aligned 24-bit temporary returned by `getALU1`. Its four callers are
   the two rotate instructions on both architectures. Read temporary bit 23,
   without an accumulator offset.
2. Both JIT ROL implementations tested a wider intermediate for Z before
   discarding the shifted-out bit. MSP=0x800000 with C=0 produces MSP=0 and
   C=1; the temporary's bit 24 must not make Z false. Mask to 24 bits before
   the zero test.
3. x86-64 ROR computed Z after updating N, which clobbers native flags. Test
   the rotate result explicitly before recording Z. With MSP=0, C=0 and
   initial N/Z/V set, the pre-fix implementation produced CCR=0 instead of 4.
4. Interpreter ROR shifted EXP along with the MSP, then masked too late.
   EXP bit 0 could become result bit 47 even when incoming carry was clear.
   Isolate the 24-bit MSP first, rotate it, and replace only that field.

The synthetic `nop; ror a` diagnostic reproduces the interpreter value error:
input A=0x015a7b3f37c905 and SR=0xbc gave interpreter
A=0x01ad3d9f37c905 versus JIT A=0x012d3d9f37c905. Both JITs also had the
separate N defect. Backend agreement alone is not the oracle for either fix.

## Regression and history

`UnitTests::rotateFlags` adds 864 specification-derived cases: 768 individual
rotates across both directions/destinations, four EXP values, six MSP
boundaries, both carry values and four preserved-flag patterns; another
96 ASR/rotate sequences check same/separate accumulators and all three valid
scaling modes without an intervening status read. Both accumulators and
CCR bits 0–6 are checked. Disabled standard S computation and 16-bit arithmetic
mode remain outside this coverage. Failed flag cases log their exact inputs.

The pre-fix interpreter suite fails the accumulator assertion; both JIT
suites fail the CCR assertion. Correcting N alone leaves JIT failures. After
also correcting ROL masking and interpreter ROR, x86-64 still fails the
zero-MSP ROR case described above. ARM64 then exposes a separate runtime U
scaling failure, documented in `runtime_unnormalized_flag_validation.md`.
The stronger regression is retained; that independent failure is not hidden
by changing its expectations or inserting a status read between instructions.

Local history associates the incorrect temporary-bit offset with `a60612a43`
(x86-64, August 19, 2026) and `981495216` (ARM64, August 20). Interpreter
ROR's late masking and x86-64 ROR's flag ordering appear in `e4879ba66`
(March 2026). ROL's wider zero test predates these changes. These observations
describe the inherited local history, not current external upstream state.

## Validation

With the independent runtime-U correction included, the full core suites
pass ARM64 JIT (3.72 s), x86-64 JIT (6.35 s), and forced ARM64 interpreter
(5.20 s). The original 361-pair diagnostic and 51 arithmetic/repeat cases
pass both architectures. The larger 1225-pair diagnostic drops from 107 to
zero failing pairs on ARM64 and from 179 to 72 on x86-64; all remaining
x86-64 first witnesses involve LSL/LSR. Some still differ between single-
instruction and grouped JIT execution. Counts describe first witnesses,
not independent bugs or exhaustive ISA correctness. Shared condition
equations still need an independent truth-table audit.

Normal MM strict sine tests pass both JIT architectures, including all six
tracks and parameter sweeps. ARM64 MD UW/RAM passes (ROM peak 0.276559,
RAM correlation 0.990227–0.991797). Forced-interpreter MM still fails its
unchanged idle gate at RMS 2.51706e-6. ARM64 post-setup cap-1 playback still
fails first-note quality with RMS 0.112626 and roughness 4.9428e-5. Neither
firmware-level symptom was resolved by this increment.

Confidence is high in the isolated rotate corrections, backed by explicit
public-ISA boundaries, pre-fix failures and both JIT/interpreter regressions.
Instruction timing, block caps, transport behavior, firmware hooks and audio
thresholds are unchanged. This is not proof of complete ISA semantics or
hardware equivalence; other products using the shared core were not tested.
