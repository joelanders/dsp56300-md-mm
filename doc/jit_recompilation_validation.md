# Recompiling an unchanged program

`Jit::recompileAllBlocks()` discards compiled code while retaining known DO/DOR
loop descriptions for unchanged program memory. Call it outside DSP execution,
for example after a JIT configuration change. It allocates; this is not a
real-time-safe operation or a synchronization primitive.

`destroyAllBlocks()` remains the separate invalidation operation for program
replacement. It now explicitly clears loop descriptions, including descriptions
retained by an earlier recompilation that have no compiled setup block owning
them. Writes to either word of a known DO/DOR setup also invalidate that
description, even if the setup has not been recompiled since cache rebuilding.
Existing program-write notification remains required; direct unnotified memory
replacement is not supported by the new operation.

The motivating synthetic reproducer pauses a five-iteration DO loop after its
first addition. Destructive cache clearing loses the remaining iterations and
loop teardown: A contains one increment, LC remains 5. Unchanged-program
recompilation instead finishes five increments and restores LC. This is about
emulator cache metadata, not product firmware internals.

`JitUnittests::recompileActiveLoops`, part of `dsp56300_unitTests`, checks ordinary
and nested loops, with/without recompilation, arithmetic results, restored LC/LF,
later modification of the outer DO endpoint, and full invalidation of retained
metadata. The main application's manual `--cache-loop` diagnostic mirrors the
loop controls. ARM64 and x86-64 JIT controls pass. A forced-interpreter build
still builds and passes its core suite but skips this JIT-only test.

Scope limits: this does not establish arbitrary program-replacement/state-restore
semantics, all self-modifying-code cases, interrupt-time recompilation, exact
timing equivalence across configurations, or thread safety. No production synth
caller was migrated. The MD/MM audio diagnostic uses the new API to distinguish
cache reconstruction from a post-setup block-size change; its strict audio
failure at cap 1 remains a separate investigation.
