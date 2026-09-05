# Compound condition-code truth tables

## Independent requirements

[DSP56300 Family Manual, Rev. 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf),
tables 12-17 and 12-18 (printed pages 12-23/24), specifies both the condition
equations and their instruction encodings. Its `+` denotes logical OR, not
integer addition or parity:

- NR: `Z || (!U && !E)`; NN is its complement. Zero is normalized.
- LE: `Z || (N != V)`; GT is its complement.
- GE/LT depend only on equality/inequality of N and V. Other conditions test
  the named C/E/Z/L/N bit, with their corresponding complements.

Software can write CCR combinations that are not a typical arithmetic result.
In particular, Z and `N != V` may both be true. The decoder must handle those
states; arithmetic-result examples alone are insufficient.

## Corrections and staged reproduction

All three backends incorrectly implemented NR as `(Z | U | E) == 0`.
Interpreter and ARM64 LE used integer addition followed by equality to one,
rejecting the case where both terms are true. x86-64 GT/LE used parity of
Z/N/V, which incorrectly cancels two true terms. Interpreter/ARM64 GT's old
sum-equals-zero expression happened to give the right answer; it is now
written consistently as the complement of LE.

The interpreter uses explicit boolean equations. Each JIT shares one
computation between NR/NN and another between GT/LE, selecting complementary
native conditions. Deferred flags are resolved before reserving condition
temporaries, retaining the prior ARM64 register-pressure correction.

An initial, uncommitted x86-64 implementation combined entire temporaries even
though `ccr_getBitValue` only initializes their low byte for these flags. The
existing arithmetic `testCCCC` regression failed before reaching the new
truth-table test. Inspecting its synthetic generated code identified stale
upper bits; the final implementation combines only the low bytes. This was
a defect in the proposed change, not evidence of another inherited firmware
bug. It was not committed or pushed in its failing state.

## Regression coverage

`UnitTests::conditionCodes` now exercises all 256 CCR patterns against all
16 condition encodings through `Tcc R0,R1`: 4,096 cases per backend. Expected
results come from an independent boolean table. It checks taken/untaken
transfer behavior, both accumulators, source R0, and the entire unchanged SR.
Two old arithmetic tests that expected zero to be unnormalized are corrected.

Before the changes, all backends fail CCR=4/NN: the transfer occurs when it
must not. After correcting only normalization, x86-64 fails CCR=6/GT, while
ARM64/interpreter fail CCR=6/LE. These are separate staged reproductions.

The actual-dispatch `conditionalTransferWithDeferredFlags` test grows from
24 to 36 cases, adding zero to the existing nonzero normalized/unnormalized
inputs. It covers three scaling modes, both NR/NN and cap-1/cap-32 dispatch,
including a held accumulator-transfer operand while flags are deferred.

## Visible history

The incorrect JIT normalization simplification appears in `9cc57d072`
(April 2023). Interpreter `564b2ad28` (March 2026) explicitly changed its
equation to match that JIT behavior. Its predecessor also used integer `+`,
so blindly reverting it would not satisfy the entire truth table.
Interpreter LE's integer sum is present at `76115b260` (April 2021);
x86-64 parity appears in the `402a280ca` import (December 2022); ARM64's
sum/equality version is present at `979e2b582` (February 2023). These are
local historical boundaries, not claims about today's external upstream.

## Validation

The final full core runners pass ARM64 JIT, x86-64 JIT and forced ARM64
interpreter. The 1,225-pair and original 361-pair diagnostics and all 51
arithmetic/repeat cases pass both architectures. Normal MM six-track sine and
parameter sweeps pass both JITs, and MD UW/RAM passes both (ROM peak 0.276559,
RAM correlation 0.990227–0.991797).

The stricter unresolved symptoms are unchanged: interpreter MM idle RMS
2.51706e-6 fails its gate; post-setup cap-1 playback fails both JITs at
first-note RMS 0.112626 and roughness 4.9428e-5. These corrections did not
resolve either symptom or the MM transport-loss/panel requirements.

No firmware hook, audio threshold, transport behavior, emulated instruction
timing or block cap was changed. Confidence is high in the tested decoder
equations, not every conditional instruction's timing, complete processor
semantics, performance equivalence, physical-hardware equivalence or
clean-room provenance. The synthetic tests need no firmware image.
