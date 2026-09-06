# CLR after a deferred arithmetic flag update

## Public specification and scope

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
CLR (printed page 13-44), specifies a full accumulator clear, fixed E/N/V=0
and U/Z=1, unchanged C, and the standard S/L behavior. No firmware image,
disassembly, private program address or product-specific algorithm is needed
to establish these requirements.

## Reproducer and cause

An arithmetic instruction followed immediately by CLR can leave the interpreter
with stale E/U/N. `setCCRDirty` saves an arithmetic result for later flag
evaluation. `alu_clr` used to write fixed flags without retiring that saved
result. A subsequent `getSR()`/condition evaluation runs `updateDirtyCCR()` and
replaces CLR's flags with E/U/N computed from the preceding arithmetic result.
The accumulator itself is correctly zeroed.

The main application's opt-in `mdDspArithmeticParityTest --sequences` found 15
of 361 instruction pairs with this discrepancy on both ARM64 and x86-64.
All 15 ended in CLR A. Both single-instruction and grouped JIT execution agreed
with each other; the interpreter differed. This is not evidence of a JIT
block-size error or proof that this defect causes the MM audio failure.

`UnitTests::clr` now has a specification-derived ASR/CLR sequence regression:
A and B destinations, positive extended and negative operands, both outgoing
carry values, and initially clear/set sticky S/L. No status observation occurs
between ASR and CLR. The initial even-operand version fails the interpreter
suite before the correction. Reading SR between instructions would hide the
defect, so isolated single-instruction tests were insufficient.

The correction retires pending flags before CLR installs its result and fixed
flags. It does not change the general cache representation, timing accounting,
JIT code generation, parallel-move order or firmware-specific behavior.

## Historical boundary

Local history places deferred E/U/N evaluation in `c4408247f` (May 2021).
CLR's direct flag writes are present in `c70d3743c` (June 2021), with no cache
retirement in the version inherited by this branch. These are local historical
observations, not a fresh assertion about the current external upstream tree.

## Validation

After the correction, `dsp56300_unitTests` passes in the ARM64 JIT (2.54 s),
x86-64 JIT (3.00 s), and forced ARM64 interpreter (2.42 s) builds. The full
16-case ASR/CLR matrix includes both carry values. The main 361-pair diagnostic
now reports zero failures on both JIT architectures; the existing 51
instruction/repeat comparisons also pass on both.

Normal MM strict sine tests pass on both JIT architectures, including all six
tracks and parameter sweeps. The ARM64 MD UW/RAM regression passes (ROM peak
0.276559, RAM correlation 0.990227–0.991797). The forced-interpreter MM sine
test still fails at the unchanged strict idle gate: RMS 2.51706e-6 versus
threshold 1e-7. This correction has not resolved that firmware-level symptom.
Tests used the existing validated firmware inputs; no payload is included here.

Confidence is high in this isolated flag-lifetime correction. The instruction
matrix is not exhaustive, does not prove all deferred-flag consumers correct,
and is not an independent hardware oracle. Shared-core tests do not establish
the behavior of every synth using the core.
