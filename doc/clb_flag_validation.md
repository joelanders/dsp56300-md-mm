# CLB result and deferred-flag validation

## Public requirement

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
CLB (printed page 13-42), defines a signed leading-bit count in the destination
MSP, zero LSP and sign-extended EXP. N reflects the count's sign, Z reflects a
zero count, V is cleared, and S/L/E/U/C are preserved. The all-zero source has
a special zero result. This correction uses that specification and synthetic
programs, not firmware disassembly, private addresses or internal algorithms.

## Defects and correction

Both JIT implementations previously computed N from the destination before
installing the count, and from the wrong destination bit. For a separate
destination the host register need not even have been loaded. ARM64 also
derived Z from native flags that did not describe the count: its LSL does not
set NZCV. Such dependencies can vary with the preceding instructions and
register allocation. The correction installs the result first, reads its bit
47 for N, and explicitly tests the result for Z after other flag-writing code.

The interpreter had the same pending-arithmetic-flag lifetime problem found
for CLR: CLB's directly written N could later be overwritten by the preceding
arithmetic result's N. It now retires that pending result before applying CLB,
which preserves the preceding E/U while replacing N/Z/V.
The count is positioned with an unsigned shift to avoid undefined C++ behavior
when the count is negative.

Local history contains the JIT read-before-write pattern in `b49c5f6c1`
(April 2024); later accumulator-alignment changes adjusted offsets but did not
remove that dependency. This is historical provenance within the available
repository, not an audit of current external upstream state.

## Independent regression

`UnitTests::clb` now checks every possible leading-bit count using positive
and complemented operands, the all-zero special case, all ones, and same versus
separate destination accumulators. Initial flags deliberately contain N/Z/V
and preserved S/L/E/U/C. A separate ASR/CLB pair checks that CLB replaces the
deferred N while retaining E/U. There are 225 new cases, in addition to the
existing four value-only cases. No status read is inserted between ASR and CLB.
The new flag test fails both JIT suites before the correction.

Final core suites pass ARM64 JIT (2.65 s), x86-64 JIT (4.47 s), and forced
ARM64 interpreter (3.51 s), including after the unsigned count-placement
cleanup. The main 361-pair arithmetic diagnostic still
passes both architectures. Focused `nop; clb a,b` and `clb a,b; move a,x0`
comparisons each exercise 1024 deterministic inputs and now agree across
interpreter, single-instruction JIT and grouped JIT.

Normal MM strict sine tests pass both JIT architectures after this correction.
The ARM64 post-setup cap-1 diagnostic still fails first-note quality with RMS
0.112626 and roughness 4.9428e-5, unchanged from before the correction.
The forced-interpreter idle gate also still fails at RMS 2.51706e-6. This does
not establish a fix for MM audio, complete deferred-flag handling or hardware
equivalence. The final ARM64 MD UW/RAM regression passes (ROM peak 0.276559,
RAM correlations 0.990227–0.991797). Sixteen-bit arithmetic mode is not covered
by this regression or newly implemented here.
