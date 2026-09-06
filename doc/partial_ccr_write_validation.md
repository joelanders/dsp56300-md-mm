# Partial writes to deferred interpreter flags

## Public requirement and scope

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
AND (printed page 13-11), EOR (13-68), NOT (13-149) and OR (13-150),
defines 24-bit logical operations on accumulator bits 47–24. N follows result
bit 47, Z tests that 24-bit result, V is cleared, and E/U/C are preserved.
EXP and LSP remain unchanged. These requirements come from the public ISA,
not firmware disassembly, private addresses or a product-specific algorithm.

## Reproducer and correction

An arithmetic instruction can defer its E/U/N computation until a status
read. A subsequent logical instruction writes N directly, but the old cache
did not retire that pending N. `updateDirtyCCR()` also recomputed all three
flags whenever any pending flag existed. The later read therefore overwrote
the logical result's N with the arithmetic result's N.

The application's synthetic `neg a; and x0,a` diagnostic reproduces this
with input A=0x015a7b3f37c905, B=0xd55ad0723547d7, X=0xa4cb864a3898,
Y=0x801dd1098ae9, SR=0xbc. All backends produce the same accumulators, but
interpreter SR=0xa8 differs from single-instruction and grouped JIT SR=0xa0.
Parity is a diagnostic lead, not an independent specification oracle.

The correction makes the existing dirty mask authoritative:

- Explicit CCR mask-set, mask-clear and bit-value writes retire only the
  pending bits they overwrite.
- Resolution computes only the still-pending E/U/N, preserving explicit
  writes while retaining flags that belong to the saved arithmetic result.
- Before saving a new result, resolve pending flags outside the new mask.
  A single saved result cannot represent preserved flags from two operations.

The full-SR setter already discards its pending cache. The separate mode-bit
helpers, disabled S computation, JIT generators, instruction timing and
earlier CLR/CLB flushes are unchanged. This is not a complete scaling-mode
or status-register semantics audit.

## Regression coverage and provenance

`UnitTests::partialFlagWrites` adds 96 public-ISA sequence cases: ASR followed
by AND/OR/EOR/NOT, four signed/extended arithmetic inputs, three logical
operands, and same versus separate destination accumulators. Both
accumulators, X0 and CCR bits 0–6 are checked without an intervening SR read.
S's standard scaling behavior is deliberately outside this regression;
L starts clear and must remain clear. The forced-interpreter suite fails
the new CCR assertion before the production correction.

`InterpreterUnitTests::testDeferredCCR` adds 168 cache-contract cases:
128 pairs of old/new E/U/N subsets with complementary source results, plus
40 explicit mask/bit write cases. It checks preserved flags, dirty-mask
retirement and repeated-read stability. This directly tests partial-mask
source replacement even though current arithmetic callers normally defer
all E/U/N together.

Local history dates the shared-result/unconditional-resolution design to
`c4408247f` (May 2021); the direct flag helpers predate or follow that design
without corresponding pending-bit retirement. This describes the available
inherited history, not the current external upstream tree.

## Validation

After the correction, the full `dsp56300_unitTests` suites pass on ARM64 JIT
(3.41 s), x86-64 JIT (4.22 s), and forced ARM64 interpreter (2.97 s), including
the new shared sequence and cache-contract regressions. The focused
`neg a; and x0,a` diagnostic passes all 1024 inputs on ARM64. The original
361-pair diagnostic and 51 arithmetic/repeat cases pass both architectures.

The larger 1225-pair logical diagnostic drops from 208 to 107 failing pairs
on ARM64 and from 247 to 179 on x86-64. Every remaining ARM64 first witness
involves ROL or ROR; x86-64 also has LSL/LSR failures, including cases where
single-instruction JIT agrees with the interpreter but grouped JIT does not.
These totals are first-witness counts, not independent bug counts; newly
correct interpreter flags can also expose previously hidden JIT discrepancies.
Condition equations shared by all backends still need a separate public-ISA
truth-table audit. Neither agreement nor this narrowed failure set proves
complete ISA correctness.

Normal MM strict sine tests pass both JIT architectures after the correction,
including all six tracks and parameter sweeps. ARM64 MD UW/RAM also passes
(ROM peak 0.276559, RAM correlation 0.990227–0.991797). Forced-interpreter MM
still fails its unchanged strict idle gate: RMS 2.51706e-6 versus 1e-7.
No firmware hook, audio threshold or instruction-block cap was changed; this is not a demonstrated
fix for the firmware-level idle-noise or block-size sensitivity symptoms.

Confidence is high in the isolated deferred E/U/N lifetime correction, based
on the specification-derived failing regression and cross-platform suites.
This does not validate disabled S handling, every scaling-mode transition,
every partial status-register path, or physical-device equivalence. Other
products using this shared core have not received product-level validation.
