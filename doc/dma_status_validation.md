# DMA transfer-done status

The previous implementation initialized all six DSTR.DTD bits to one but never
updated them on enable or completion. A synthetic DSP56303 request channel
remained marked done after sixteen NOPs with DE set and no request supplied.
All six channels reproduced this on ARM64 JIT, x86-64 JIT and interpreter.

The basis is [DSP56300FM Rev. 5, table 10-10 and section 10.6](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf):
DTD clears after the enable pipeline delay, and reflects block completion or
software disable. A captured request must not be discarded on disable. This
work uses synthetic registers/data and the public manual, not product firmware.

The implementation schedules a per-channel delayed clear in the existing
instruction-domain peripheral dispatcher. Completion supersedes that clear;
disable/re-enable replaces the old deadline. Requests following a completed
continuous-mode block make the channel busy again. A disabled request channel
has no outstanding work after its synchronous request callback returns;
an already accepted delayed block still completes before reporting done.

`dspDmaStatusTest` / CTest `dsp56300_dmaStatus` checks all six channels:

- reset, enabled waiting state, two/three NOP enable-delay boundaries;
- normal dispatcher wake without DACT or explicit DMA dispatch;
- request-by-request status, completion, DE clearing and transferred data;
- disable/re-enable before the previous deadline;
- completion before the deferred clear;
- continuous blocks with DE retained and new requests;
- disable with an accepted delayed block still pending.

In the original cleanup integration, the test and full core suite passed on
ARM64 JIT, x86-64 JIT and a forced ARM64 interpreter build. MD UW/RAM on ARM64
and MM sine on both JIT architectures also passed there. Those historical
integration results are not substituted for testing the independent extraction
below. This correction was not established as the MM interpreter idle-noise
root cause.

Limits: this does not implement a cycle-accurate DMA bus arbiter, deferred
request capture, reset of the entire DMA controller, or hardware validation.
The pipeline boundary test uses NOPs and one-instruction blocks; effects of
long instructions/REP and larger dispatcher batches need separate timing
coverage. Existing delayed-block scheduling and DACT arbitration are unchanged.
The same omission is present in this checkout's 2022 commit `402a280ca`; this
is not a claim about the current state of any external upstream branch.

## Independent extraction, 2026-09-06

Extracted original commit `608a6542` onto release base `da3aaf31` as
`46d84dec`, preserving its attribution. This branch contains only the DMA
status correction, its standalone regression, CTest registration and this note.
It does not depend on the unfinished DSP #6/main #43/MCU #3 cleanup stack or
the separate boot-reply removal. No product submodule pointer is changed here.

Fresh Release builds pass the focused DMA test and the complete available
core runner on native ARM64, x86-64/Rosetta, and forced ARM64 interpreter.
Restoring only `dma.cpp/.h` to the release base makes the new ARM64 regression
fail on all six channels: DTD remains set after enable, with done-mask 63
instead of the expected per-channel cleared bit. The corrected files were
restored afterward. The pre-fix comparison used the same test, not a different
oracle or weakened assertion.

Reproduction from this branch (choose `arm64` or `x86_64`; add
`-DDSP56K_FORCE_INTERPRETER=ON` for the interpreter build):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON
cmake --build build --target dsp56kTestRunner dspDmaStatusTest --parallel 2
ctest --test-dir build -R '^dsp56300_(unitTests|dmaStatus)$' --output-on-failure
```

The existing CI workflow builds all targets and runs CTest on macOS, Linux and
Windows, so this new target participates without a separate workflow change.
CI success and fresh product-integration results must be checked separately;
the local core results above do not claim either.

Fresh ARM64 product integration also passes with only this DMA implementation
temporarily applied to main release `7cd7afa0` (plus the new external MM boot
regression). Its pinned DSP `8c919d2b` has the same tree as `da3aaf31`; no other
cleanup changes or boot-hook removal were enabled for this comparison. MM
boot/kit/panel, MD UW/RAM and the MD/MM randomized scheduler/resampler audio
soak all returned zero. MD ROM peak stayed `0.276559`; RAM correlations remained
above 0.990. Some RAM measurements changed slightly, so this is a passing
waveform-fidelity regression, not bit-identical output. The temporary product
edits were removed afterward. This extraction has not repeated the full MM sine
or x86-64 product-integration matrix.
