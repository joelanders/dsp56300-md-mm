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
