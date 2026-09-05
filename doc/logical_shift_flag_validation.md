# x86-64 logical-shift flags and carry boundary

## Public requirements and limits

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
LSL/LSR (printed pages 13-93 through 13-96), describes a 24-bit operation on
accumulator bits 47–24. EXP/LSP and E/U are preserved; N/Z describe the MSP,
V clears, and C receives the last shifted-out bit. A zero count clears C.

The narrative limits the count to 24, but the immediate-field tables list
inconsistent narrower ranges (LSL 0–16, LSR 0–23); LSR's multi-bit paragraph
also names a bit position inconsistent with the 24-bit diagram. Tests through
24 follow the narrative and operation width, not a claim that these editorial
contradictions have been resolved by physical-device measurements. Counts
above 24, upper control-register bits, 16-bit arithmetic mode and standard
S computation are not validated here. Existing count-28 value tests remain
compatibility tests, not independent hardware evidence.

## Defects and correction

Both x86-64 helpers used `CcrBatchUpdate` without Z in its mask, despite
updating Z inside that batch. This disables per-flag clearing, leaving a set
old Z in SR and potentially retaining a deferred arithmetic Z. Include every
replaced flag (N/Z/C/V); E/U remain preserved.

LSL also added eight to the count to place the carry at the native 32-bit
boundary. An immediate count of 24 therefore became a native count of 32,
which is masked to zero. The register-count path instead used a 64-bit shift
with the same eight-bit offset, reading carry from the wrong boundary.
Shift a zero-extended MSP directly in a 64-bit temporary, copy temporary
bit 24 to C, then mask to 24 bits before testing Z. For count zero, bit 24 is
already zero. N still reads result bit 23. LSR's data/carry path is unchanged.

No firmware addresses, code inspection, thresholds, production block caps,
instruction timing or transport changes are involved. ARM64 and interpreter
production paths are unchanged.

## Independent regression and staged evidence

`UnitTests::logicalShiftFlags` adds 9,168 cases with a repeated-one-bit oracle,
not the implementation's native shift/carry algorithm:

- 8,448 individual cases: both directions/destinations, counts 0–24,
  immediate and all six register sources, implicit single-bit forms, six MSP
  boundaries and clear/set initial CCR. Accumulator-source aliases are read
  before the destination is written. Nonselected X/Y controls use distinct
  sentinels. Both complete accumulators, X/Y and CCR bits 0–6 are checked.
- 720 ASR/shift sequences: both arithmetic destinations, same/separate shift
  destinations, five inputs including zero, three scaling modes, counts
  0/1/24 and immediate/register forms. No SR read separates instructions.
  These check replaced N/Z/C/V and preserved pending arithmetic E/U.

Before either correction, x86-64 fails `lsl #0,a` with nonzero MSP=1 and
initial CCR=0xff: SR=0xf4 retains Z, while expected CCR bits 0–6 are 0x70.
With only the two batch-mask corrections, the 1,225-pair diagnostic already
passes, but the independent regression still fails `lsl x0,a`, count=1,
MSP=0x800000: CCR=4 instead of 5 (missing C). The wider tests therefore
prevent treating that diagnostic's zero mismatches as complete shift evidence.

The helpers, including the missing Z mask and mixed-width carry shortcut,
are present unchanged in local history at `402a280ca` (December 2022,
“update OSS version”). This is the visible import boundary, not attribution
of original authorship or verification of today's external upstream branch.

## Validation

The final full core runners pass ARM64 JIT, x86-64 JIT and forced ARM64
interpreter, including the new cases. The original 361-pair and 51
arithmetic/repeat diagnostics pass both architectures. The expanded
1,225-pair diagnostic has zero mismatches on both (previously 72 x86-64).
Core runners were invoked directly with streamed, filtered output after
CTest could not create its log on the full filesystem; all runner exit codes
were independently captured as zero. CTest's infrastructure errors are not
counted as test passes.

Normal MM strict sine tests pass both JITs, including all six tracks and
parameter sweeps. MD UW/RAM passes both, with ROM peak 0.276559 and RAM
correlations 0.990227–0.991797. The x86-64 post-setup cap-1 diagnostic still
fails first-note quality (RMS 0.112626, roughness 4.9428e-5). Interpreter MM
was not rerun for this x86-64-only production change; its preceding idle
failure remains unresolved, as does the preceding ARM64 cap-1 failure.

Confidence is high for these isolated corrections, not complete ISA semantics
or physical-hardware equivalence. The sequence tests rely on the previously
corrected partial-CCR cache and runtime-U scaling behavior; an upstream
backport needs to account for those test dependencies. The shared
compound-condition truth-table audit, interpreter shift-language/count safety,
and broader MD/MM remediation checklist remain open. Other synth products
using this shared core were not tested.
