# Accumulator representation regressions

DSP accumulators occupy bits 63..8 of host storage. Raw 24-bit fields and 48-bit memory operands do not use that representation. Confusing the two caused x86 EFM-SD phase stores to lose eight bits and exposed additional condition-flag errors.

The fixes cover raw A10/B10 loads and stores, shift carry, rotation sign/zero flags, sticky overflow, signed boundary arithmetic, interpreter extension detection, and saturating long-memory transfers. Four focused fixes are reused from the firmware-hook work via commits that retain their original attribution; the broader firmware-hook changes are not included.

Instruction expectations follow the [DSP56300 Family Manual, revision 5](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf), particularly status definitions in section 5.4 and ABS, ASL, ASR, DEC, INC, long-memory moves, NEG, ROL and ROR in section 13. The interpreter is corrected where it disagreed with those definitions; it is not treated as an infallible oracle.

`dsp56300_accumulatorTests` adds 50 explicit expected-value cases and 7,680 deterministic interpreter/JIT comparisons. It exercises both accumulators, partial registers, raw and saturating transfers, zero/immediate/variable shifts, nonzero initial flags, three scaling modes, boundary values and seeded random values. It checks resulting registers, status and addressed memory. Failures include the instruction and initial state.

Run the complete existing suite and the new regression test with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel 3
ctest --test-dir build -C Release --output-on-failure
```

The existing CI matrix runs these tests on ARM64 macOS, x86-64 Linux and x86-64 Windows. No firmware, sound device, GUI or tester recordings are required. This is targeted coverage of the reproduced defects, not exhaustive verification of the instruction set or all instruction sequences.

## Follow-up review and sequence coverage

The September 6 follow-up checks the implementation against the manual independently of interpreter/JIT agreement:

| Correction | Architectural basis |
| --- | --- |
| Raw A10/B10 transfers | Sections 3.2.1 and 13, long-memory moves: concatenate A1:A0 or B1:B0; preserve the extension on raw loads. Host alignment is not part of the memory value. |
| Saturating A/B long stores | Section 3.1.6 and the long-memory instruction: scale, then limit to signed 48-bit range, without changing the source accumulator. Scaling in the right-aligned 56-bit domain prevents host overflow before limiting. |
| ASL/ASR carry | Instruction definitions specify the last discarded accumulator bit and C=0 for zero shifts. ASL overflow also sets sticky L. |
| ROL/ROR and logical shifts | These operate on bits 47..24; N comes from bit 47, Z from the 24-bit result, and other accumulator fields survive. |
| ABS/NEG/INC/DEC boundaries | Their full 56-bit results and standard V/L definitions require overflow at the signed minimum/maximum boundaries. ABS/NEG preserve C; INC/DEC update it for full-width carry/borrow. Unsigned host arithmetic defines wraparound. |
| Scale-up E | Section 5.4: the signed integer portion includes bit 55 in every supported scaling mode. |

The new 460 sequence cases comprise 76 explicit branch outcomes and 384 deterministic 24-instruction interpreter/JIT comparisons. They run with multi-instruction blocks, block linking, and the optimizer both enabled and disabled. Programs include parallel accumulator stores, partial loads, cross-accumulator shifts, sticky flags and branches. Three supported scaling modes and nonzero initial CCR values are included.

This exposed an additional interpreter defect: logical shifts wrote N while preceding arithmetic still had a deferred N update. Reading CCR later reinstated the stale arithmetic sign. Materializing preceding flags before LSL/LSR preserves E/U and allows the logical result to replace N. Two explicit arithmetic/shift/branch programs protect this correction.

As a negative control, the same expanded tests were built against alpha.10's DSP revision `8c919d2b`: 16 explicit cases fail on ARM and 28 on x86, including the x86 raw transfers. The differential sweep reports 1,565 ARM and 2,636 x86 mismatches; the sequence suite also rejects that baseline. With the corrections, all three groups pass locally on both architectures. These failures demonstrate detection of the original defects, not a count of independent bugs.

Coverage remains bounded: reserved scaling combinations, arithmetic saturation/16-bit modes, every opcode and arbitrary instruction sequences are not exhaustively verified.
