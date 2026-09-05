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

The test and full core suite pass on ARM64 JIT, x86-64 JIT and a forced ARM64
interpreter build. Integration checks pass for MD UW/RAM on ARM64 and MM sine
on both JIT architectures. The existing MM interpreter idle-noise failure is
unchanged; this correction is not its established root cause.

Limits: this does not implement a cycle-accurate DMA bus arbiter, deferred
request capture, reset of the entire DMA controller, or hardware validation.
The pipeline boundary test uses NOPs and one-instruction blocks; effects of
long instructions/REP and larger dispatcher batches need separate timing
coverage. Existing delayed-block scheduling and DACT arbitration are unchanged.
The same omission is present in this checkout's 2022 commit `402a280ca`; this
is not a claim about the current state of any external upstream branch.
