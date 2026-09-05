# ARM64 conditional transfers with deferred flags

## Reproducer and cause

The ROM-free pair `abs a; tnr x0,b` can corrupt B in grouped ARM64 JIT
execution while single-instruction JIT and interpreter execution agree. The
main application's focused diagnostic reproduces this at trial 532 with
A=0xff55e39dba8f13, B=0x896b98d85a5ecd, X=0xed3b46c4f930,
Y=0x8a4a67d53cc2, SR=0x43b. Grouped JIT writes B=0 rather than transferring
X0, although its final SR and PC match the reference. Other arithmetic
predecessors show the same family of failures.

Tcc keeps its converted source operand in a temporary register. ARM64 NR/NN
decoding then reserves two condition temporaries before resolving deferred
E/U flags. U computation and its bit-copy helper need two additional
temporaries: five simultaneous requests from a four-register pool. In builds
without assertions, the old allocator could read/pop an empty vector, with
undefined behavior and incorrect generated code. The focused core regression
also reproduced a grouped dispatch reaching PC zero rather than its endpoint.
Some pre-fix manual launches exited with signal-derived status 132 and no
diagnostic output; those exits were not independently attributed to this
defect and are not counted as successful reproductions.

The correction resolves the required flags before reserving the condition's
two temporaries. The allocator also rejects exhausted strong and weak
acquisitions with `std::runtime_error` in release builds, retaining its existing
weak-register reclamation. This is a checked failure, not automatic spilling,
an interpreter fallback, or a guarantee of recovery after arbitrary JIT errors.

The lifetime pattern is visible in local ARM64 history at `979e2b582`
(February 2023). The unchecked vector access and weak-reclamation path are
present in the available `402a280ca` history (December 2022). These observations
do not identify current external upstream state or attribute original intent.

## Independent regression and specification boundary

`JitUnittests::conditionalTransferWithDeferredFlags` executes actual compiled
blocks at caps 1 and 32 for all three scaling modes, both NR and NN, and
nonzero normalized/unnormalized operands (24 cases). It checks dispatch count,
PC, preserved A/X0 and the transferred-or-preserved B. The expected nonzero
condition follows the public E/U definitions and table 12-17 in the
[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf).
No firmware disassembly, private addresses or inferred firmware algorithm is
used. This test fails before the decoding correction and passes afterward.

`temporaryRegisterExhaustion` fills the temporary pool, verifies that both
strong and weak excess requests fail without consuming another register, then
checks release/reuse and existing weak-register reclamation. Both are part of
the normal JIT suite. The forced-interpreter configuration builds the code but
does not execute the JIT-only suite.

This change deliberately does not assert that all condition equations are
correct. The current NR/NN treatment of zero and compound signed conditions
still need an independent truth-table audit; backend parity alone cannot prove
their correctness. The new regression uses nonzero inputs to isolate register
pressure from that separate issue.

## Validation and limits

Core suites pass ARM64 JIT (2.61 s), x86-64 JIT (4.09 s) and forced ARM64
interpreter (3.04 s). The focused ARM64 pair now passes all 1024 inputs.
The 1225-pair logical matrix drops from 223 to 208 failing pairs on ARM64;
x86-64 remains at 247. No ARM64 pair's first reported mismatch is now a
single-JIT-pass/grouped-JIT-fail case in that matrix, but each pair stops at its
first mismatch, so this is not an exhaustive equivalence claim.

The ARM64 normal MM sine test passed after the decoding correction; its
post-setup cap-1 diagnostic retained the same first-note failure (RMS 0.112626,
roughness 4.9428e-5). With the final allocator guard, normal MM sine passes on
both JIT architectures and ARM64 MD UW/RAM passes (ROM peak 0.276559, RAM
correlations 0.990227–0.991797), without pool-exhaustion errors. Interpreter MM
audio was not rerun in this JIT-only increment; its last strict idle failure
remains open. Transport loss, private panel startup dependencies and the wider
MD/MM remediation acceptance matrix also remain unresolved.
