# Performance regression tests

Configure with `-DENABLE_PERFORMANCE_TESTS=ON`, build, then run:

```powershell
ctest --test-dir build_x64 -C RelWithDebInfo --output-on-failure
```

The frame-policy test needs no GPU. It checks the real limiter against matching 15/24/30/60 FPS clocks with jitter, source-rate downsampling, rate changes, pauses, and processing-size/aspect-ratio rules.

## Manual GPU smoke test

Also configure with `-DENABLE_GPU_SMOKE_TEST=ON`. The harness uses an animated synthetic 4K source, not the user's camera or saved scene collection. It requires OBS installed at `C:\Program Files\obs-studio` and a user-supplied NR runtime in the normal OBS plugin config directory.

```powershell
$nrPluginRoot = $PWD.Path
$env:PATH = 'C:\Program Files\obs-studio\bin\64bit;' + $env:PATH
Push-Location 'C:\Program Files\obs-studio\bin\64bit'
& "$nrPluginRoot/build_x64/tests/RelWithDebInfo/obs-performance-smoke.exe" `
  "$nrPluginRoot/build_x64/RelWithDebInfo/obs-dlss5-nr.dll" `
  "$nrPluginRoot/data" `
  "$env:APPDATA/obs-studio/plugin_config" 1
Pop-Location
```

Scenarios: `0` full-resolution Low latency + Temporal (baseline), `1` 1080p Smooth, `2` 1080p Smooth + Temporal, `3` 1080p Low latency, `4` all dropdown resolutions and mode transitions, `5` 720p Smooth + Temporal.

The normal runs warm up for five seconds and measure ten seconds. Scenario 4 also switches through all five options for six seconds each and verifies the status dimensions and unchanged output dimensions. The harness checks that output is nonblack and changing, that the five-choice dropdown exists, and (except the legacy baseline) that the plugin reports enhanced output rather than passing raw frames through. Scenarios 1 and 5 additionally require at least 27 changing frames/sec; this performance threshold targets the development RTX 5090 and is not a minimum GPU compatibility requirement.

## NGX diagnostics and failure cases (1.3.0)

`ngx-diagnostics-test` runs in CTest without a GPU. It checks NGX error decoding, Windows x64 ABI offsets/sizes, the full five-second cooldown, and immediate bypass for changed creation settings, manual retry, and session reset.

With `ENABLE_GPU_SMOKE_TEST=ON`, the build also creates **fake-nr-runtime.dll** and **fake-ngx-core.dll**, test fixtures that initializes, invokes the NGX logging callback, and always returns FeatureNotSupported at feature creation. Never install or release these fixtures. The fake core isolates the injected failure from NVIDIA core session state. Copy them as `nvngx_dlssnr.dll` and `_nvngx.dll` under an **isolated** `<test-config>/obs-dlss5-nr/runtime/` directory and pass that test-config directory to smoke scenarios `6` (Smooth) and `7` (Low latency). Both cases require diagnostic logging, live changing/nonblack pass-through, a decoded unsupported status, a working Retry Feature button, and a style-change retry. Check the log for a bounded number of actual creation calls (roughly three in the first fifteen seconds, then manual/style retries), throttled processing warnings, and the callback ABI validation message. This deliberately exercises the unsupported-feature branch on a supported development GPU; it does not reproduce an RTX 4080 Laptop hardware environment.

Scenario `8` runs normal 1080p Smooth processing with diagnostics enabled to check the real NVIDIA runtime with the corrected logging ABI. This requires the user's actual runtime, not the fixture.

The callback lifetime test (also in CTest) calls the retained shim trampoline concurrently while detaching and reattaching its consumer. Late logging must safely stop after detach.
