# Tests and acceptance boundaries

## Firmware-free public checks

Run `sh tests/run.sh` on the prepared Linux host. This export passed:

| Check | Result | Scope |
| --- | --- | --- |
| Python unit tests | 11 passed | RAW input validation, CFA channel selection, levels, little-endian storage and padding |
| C renderer contracts | 270 passed | Synthetic path formats, fills, YUV storage, chroma, transparent source-over, strokes, caps, joins and bounds |
| C ISP contracts | 398 passed | Synthetic black/white RAW, padding, invalid dimensions, cache invalidation and recomputation |
| ASan / UBSan | No diagnostics in the above C runs | Host pixel modules in those cases |
| Cold QEMU 10.2.4 build | Passed locally | Integration, compiler and linkage |
| Firmware-free QMP smoke | Passed locally | Display machine realization, status query and clean quit |
| Public launcher with local BYO firmware | 21 checks passed | Synthetic scene, RAW/ISP/LCD, pause/resume and three MENU roundtrips in one session |

The copied native icon fixtures and private RAW/output oracle were removed.
These synthetic checks do not establish pixel equivalence for every camera icon
or Nikon ISP behavior. ASan/UBSan here do not instrument the entire QEMU guest.

Public CI checks the reviewed inventory and reachable history, runs these
contracts, builds QEMU, confirms machine registration and generates synthetic
CMOS files. It uses no Nikon firmware and uploads no runtime artifacts. A green
CI result therefore does not certify original-camera boot or shooting UI.

## Local firmware-dependent verification

After starting a local `--photo-liveview` session with your own compatible MAIN:

```sh
python3 tools/verify_ui.py --url http://127.0.0.1:8787 --menu
```

Use a new `--output DIRECTORY` for a repeated run; earlier evidence is preserved.
Output defaults to `.local/ui-verification/`. The verifier checks input geometry,
actual RAW transfers, ISP completion, native processed-buffer scanout, scene
detail, red no-card pixels, pause/resume stability and continued processing.
It sends three native MENU roundtrips, checks scene-region recovery and confirms
the same session remains running. These screenshots and raw states stay local.

The scene comparison excludes parts of the UI that can change. A scene match
does not prove all overlays or arbitrary menu paths. Numeric ISO/shutter and
outlined text/icons need visual inspection of the actual guest screenshot;
the host does not synthesize them. In the local export verification, the final
native screenshot showed `1/30`, `ISO AUTO 3200`, black text/icon outlines and
the red no-card indicator over the generated color bars/ramp. The screenshot
and firmware-dependent result remain local and are not shipped. This document
does not pretend that visual inspection runs in public CI.

## What to record

Record source/build and firmware fingerprints, assistance profile, input layout,
initial/final session identity, actual transfer/processing/scanout observations,
menu cycles, failure/unknown-access information and untested behavior. Include
the specific negative or invalid cases relevant to a device change.

Do not equate accepted key actions with successful shooting. No physical CMOS
protocol, AF/PDAF, MVD, recording, hardware add-on loading or flash acceptance
is certified by this test. The board has no complete VMState snapshot support.

## Optional shoulder replay

See [shoulder-display.md](shoulder-display.md) for the firmware-free observer,
protocol and service tests, and the opt-in private native-function reference
checks. Public CI needs no companion firmware or Unicorn. Neither test scope
certifies full MCU RTOS execution or camera hardware equivalence.

## Native Windows host checks

The native Windows port was exercised in an independent modified lab with
MSYS2 UCRT64 and QEMU 10.2.4. The extracted contribution keeps the original
stock-firmware input guard. Run the Python contracts from PowerShell:

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s tests -p 'test_*.py' -v
```

From UCRT64, the synthetic C contracts can be checked without sanitizers:

```sh
mkdir -p .build/tests
cc -std=c11 -O1 -g -Itests/include tests/test_gpu.c $(pkg-config --cflags --libs glib-2.0) -lm -o .build/tests/gpu.exe
cc -std=c11 -O1 -g tests/test_isp.c $(pkg-config --cflags --libs glib-2.0) -lm -o .build/tests/isp.exe
.build/tests/gpu.exe
.build/tests/isp.exe
```

The 11 Python tests, 270 renderer contracts, 398 ISP contracts and firmware-free
QMP realization were rerun using this branch's host tools. The smoke used the
existing lab-built QEMU executable; a new QEMU build from this extracted branch
was not performed locally. The smoke helper uses
localhost TCP QMP and requires replies to capability negotiation and the paused
status query. After sending quit, it requires a zero process exit within ten
seconds rather than a QMP acknowledgement; QEMU may terminate before delivering
that reply. These Windows runs did not use ASan/UBSan; retain the Linux
sanitizer workflow above as a separate validation scope. Earlier assisted UI
results belong to the independent lab and were not rerun on this branch.

The follow-up quit regression adds 12 synthetic TCP/process cases, bringing the
Python suite to 23 tests. They cover acknowledged quit, EOF/reset after quit,
missing/reset/error capability and status replies, an unexpected running status,
a nonzero process exit and a quit timeout. Earlier replies remain mandatory, and
a missing quit acknowledgement cannot hide a crash or a hung process.

## PR #5 review regression checks (2026-10-30)

After integrating main's native Windows support, the Linux ARM64 container run
passed 37 public Python tests, with the optional private integration test skipped.
The 12 synthetic TCP QMP/process regressions are included in that count.
`sh tests/run.sh` also passed 270 GPU checks, 398 ISP checks and the passive
shoulder observer contracts with ASan/UBSan and no diagnostics.

The CRLF regression now checks both completed records, in order, using distinct
payloads. A separate test completes only the missing LF and expects exactly one
snapshot. Both cases check repeated polls and rejection counts; malformed hex,
extra CRs, trailing characters and invalid frame fields remain rejected.
The production parser and original ex1610 function harness are unchanged.

With locally supplied firmware, all 38 Python tests passed. Seven historical
payload cases (four distinct payloads) matched the existing reference harness's
1536-byte framebuffers byte for byte, including repeated updates. These private
inputs and outputs are excluded from the public tree. Native Windows execution
was not available for this review; its regression run remains to be performed.

A fresh build from the verified QEMU 10.2.4 archive passed, followed by real
localhost TCP QMP realization/status/quit checks with shoulder observation both
disabled and enabled. The main menu also booted using a Python installation
without Unicorn, both without the companion option and with valid companion
firmware supplied; in the latter case only the optional shoulder service
reported the missing dependency.
