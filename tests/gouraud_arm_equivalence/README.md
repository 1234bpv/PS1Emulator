# Gouraud ARM equivalence test

Independent check of `gpuLightingTXTGouraudARM` in the current candidate against
the pinned baseline commit `72877425b09481908a8c69b13afc6aa8fb44349f`.
The test compiles the actual header twice with GCCE 4.6.3,
links freestanding ARM binaries, and executes them in Unicorn's ARM1136 model.
It does not substitute a translated copy of the assembly. The actual `gcol_t`
union and its ARM `operator+=` are extracted from `gpu_unai.h` for the wrappers.

Run from the workspace root:

```powershell
python -m pip install --target tests/gouraud_arm_equivalence/artifacts/deps unicorn==2.1.4
python tests/gouraud_arm_equivalence/run.py
```

Each run reads the current working header and overwrites `artifacts/candidate.h`.
Snapshots in `artifacts/` are outputs, never implicit inputs. To use another
revision or a separately preserved candidate, pass explicit inputs:

```powershell
python tests/gouraud_arm_equivalence/run.py --baseline-ref 72877425b09481908a8c69b13afc6aa8fb44349f --candidate-header path/to/gpu_inner_light_arm.h
```

`--baseline-ref` defaults to the pinned commit above; `--candidate-header`
defaults to `pcsx_rearmed/plugins/gpu_unai/gpu_inner_light_arm.h` in this workspace.
An explicit relative header path is resolved against the caller's working directory.
Both inputs are captured once per run, and the resolved baseline revision, candidate
path and content hashes are recorded in `results.json`. Every subprocess, including
each compiler invocation, has a 120-second timeout.
All generated sources, binaries, disassembly, dependency files and results are
under ignored `artifacts/`; nothing is installed into the global Python environment.

## Verified result (2026-09-27)

All tests passed at `-O2`, `-O3`, and `-Os`, with `-march=armv6 -marm`
and `-fomit-frame-pointer`, using Sourcery CodeBench Lite 2012.03-42 GCC 4.6.3.
Baseline commit: `72877425b09481908a8c69b13afc6aa8fb44349f`.
Candidate SHA-256: `1ffa614ab848473687653e52da1fcdc51b9b1f2c8c90882aad95ef54faace1ab`.

Per optimization configuration:

- 458,752 cases: every 16-bit texel, including both bit-15 states, against each
  uniform light intensity 0, 1, 127, 128, 129, 254, 255.
- 35,152 cases: all 13 cubed combinations of RGB light boundary values
  0, 1, 7, 8, 63, 64, 127, 128, 129, 247, 248, 254, 255, against eight
  mixed/isolated texels and both bit-15 states; fractional and counter bits set.
- 49,152 cases: every 5-bit texel component times every 8-bit lighting component,
  each channel isolated, with bit 15 clear/set; covers all per-channel saturation
  thresholds and zero/max products.
- 100,000 deterministic random full-width source and packed-color words, seed
  `0x47555241`, exercising unused/fractional/counter bits as well.
- Four 65,536-pixel runs retaining and incrementing the same packed color between
  inline calls; ascending, descending, mixed signed increments and wraparound.
  Every output pixel and both preserved pre-increment color words are compared.

Total: 643,056 batch inputs plus 262,144 sequential pixels per configuration;
2,715,600 baseline/candidate output comparisons across all three configurations.
Every batch output additionally matches an independent scalar saturating formula.
Every invocation checks restored SP, preserved r4-r11, unchanged input memory,
and normal return before the instruction budget. Local `results.json` records
hashes and compiler; `*.dis` retains actual generated machine-code disassembly.

## Limits

This establishes equivalence for the tested standalone inline contexts and data,
not an exhaustive proof over all packed-color words or all register allocations.
The full renderer's register pressure, clipping/blending/dithering, texture
addressing, complete frame output, Symbian integration, hardware timing and
performance are outside this test. It does not establish a speedup or hardware
correctness of the dynarec. No phone, SDK, configurations or build libraries were
accessed or modified. `docs/architecture/data-spec.md` concerns dynarec memory and
contains no additional Gouraud-specific contract; those dynarec checks are not
claimed as covered here. Tester did not read programmer or reviewer notes.
