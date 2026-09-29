# panDXVK

panDXVK is a specialized DXVK translation layer built exclusively for the panVK (Panfrost / ARM Mali Vulkan) driver.

Based on [DXVK v1.10.3](https://github.com/doitsujin/dxvk/tree/v1.10.3), forked from [pythonlover02/dxvk-sarek](https://github.com/pythonlover02/dxvk-sarek). Adds a BC texture → ASTC 4x4 transcode pipeline so that PC games using BC1–BC7 compressed textures can render correctly on Mali GPUs. PanVK does not report `textureCompressionBC`; where a wrapper advertises it back instead, the transcode is activated with `PANDXVK_FORCE_TRANSCODE=1`.

## Key Features
- **BC→ASTC Transcode**: Software decode of BC1–BC7 to RGBA8, then encode to ASTC 4x4. Mali hardware decodes ASTC natively.
- **Automatic Detection**: Transcode activates when `DxvkAdapter::isPanVk()` (ARM Mali, vendor ID `0x13B5`) AND `textureCompressionASTC_LDR` is available AND (`PANDXVK_FORCE_TRANSCODE=1` OR `textureCompressionBC = false`).
- **Format Remap**: VkImage, SRV, RTV and creation-time initializer uploads are remapped from BC to ASTC transparently. No game-side changes required.
- **Spec-Conformant Encoder**: ASTC 4x4 block mode `0x241`, field layout assembled per `GL_KHR_texture_compression_astc_ldr` C.2.6 / Table C.2.7. Verified **6/6** against basis-universal.
- **ASTC Spec Gate in CI**: `tests/host/rmchk.cpp` decodes block modes from the spec, sharing no code with the encoder, and fails the build if the shipped mode or the `R` field reading drifts. Both cross-build jobs depend on it.
- **Heap-Allocated Buffers**: RGBA8 intermediate and ASTC output are heap-allocated to avoid stack overflow on DXVK's small thread stacks.
- **Transcode Timing**: Per-subresource transcode time for profiling, compiled out of release builds (`#ifndef NDEBUG`). Enable with `DXVK_LOG_LEVEL=debug`.
- **x64 + x32**: Both 64-bit and 32-bit DLLs built and verified.

## Installation (Winlator / Bannerlator)

**Option A — WCP (recommended):**
1. Download `wcp.zip` from the [Releases](https://github.com/isygold/panDXVK/releases) page.
2. In Winlator/Bannerlator, go to **Container Settings → Content**.
3. Tap **Install** and select the downloaded `wcp.zip`.
4. Launch your game. panDXVK detects Mali and remaps BC textures to ASTC. If your setup reports `textureCompressionBC = 1` (wrapper), set `PANDXVK_FORCE_TRANSCODE=1` to activate the transcode.

**Option B — Manual DLL replacement:**
1. Download `merged.zip` (x64 + x32 DLLs) from the [Releases](https://github.com/isygold/panDXVK/releases) page.
2. Extract the archive.
3. Inside Winlator/Bannerlator, go to **Container Settings → Advanced → DXVK/Sarek**.
4. Replace the existing `d3d11.dll`, `dxgi.dll`, `d3d10.dll`, `d3d10_1.dll`, `d3d9.dll` with the ones from the extracted folder matching your container's architecture:
   - 64-bit containers → `x64/` folder
   - 32-bit containers → `x32/` folder
5. Alternatively, copy the DLLs into the Wine prefix's `system32` (64-bit) or `syswow64` (32-bit) directory.
6. Launch your game.

## Configuration
panDXVK uses the same configuration mechanism as upstream DXVK. Set `DXVK_CONFIG_FILE` to point to a `dxvk.conf` file, or use environment variables:

| Variable | Values | Description |
|----------|--------|-------------|
| `DXVK_HUD` | `devinfo`, `fps`, `frametimes`, `full`, etc. | HUD overlay. See upstream docs. |
| `DXVK_LOG_LEVEL` | `none`, `error`, `warn`, `info`, `debug` | Logging verbosity. Transcode markers (`remap VkImage`, `initializer upload`, `SRV`/`RTV BC→ASTC`) need `debug` — they are runtime-gated, so no special build is required. |
| `DXVK_LOG_PATH` | path | Directory for log files. |
| `DXVK_FRAME_RATE` | `0` (uncap), or FPS limit | Frame rate cap. |
| `PANDXVK_FORCE_TRANSCODE` | `0`, `1` | Test knob: force BC→ASTC on Mali even when the driver claims BC (wrapper setups). Explicit CPU overhead. Needs game restart. |

## Notes
- **I need your logs.** If you hit a crash, rendering glitch, or anything weird, grab the log file from your Wine prefix's drive_c (usually `wine_debug.log` or `d3d11.log` in the app directory) and paste it to [panDXVK Logs](https://github.com/isygold/panDXVK-logs/issues). For BC→ASTC transcode diagnostics, set `DXVK_LOG_LEVEL=debug` — the markers are runtime-gated, so no special build is needed. Without logs, I cannot help you.
- **ASTC 4x4 is lossy.** BC1–BC7 textures are decoded to RGBA8 and re-encoded to ASTC 4x4. This introduces compression artifacts not present in the original. For most games the visual difference is minimal, but texture-heavy UIs or screenshots may show subtle banding.
- **BC6H maps to ASTC 4x4 LDR.** BC6H (HDR float RGB) is clamped (negatives/NaN to 0, highlights saturate) and approximated as ASTC 4x4 UNORM. Full HDR fidelity is not preserved.
- **panDXVK detects Mali, not a particular driver.** `isPanVk()` tests the ARM vendor ID `0x13B5`, so it returns true for the blob driver and for PanVK alike — it cannot tell the two apart. If the device reports `textureCompressionBC = true` (typical with a wrapper's BCN layer) and force is off, the transcode is skipped and BC textures are uploaded as-is. Set `PANDXVK_FORCE_TRANSCODE=1` to force it (test mode, expect CPU overhead). **In wrapper setups the automatic path stays closed, so force is what actually activates the transcode.**
- **Transcode happens at CPU time.** Each `UpdateTexture` call triggers a full BC decode + ASTC encode on the CPU. Large textures (4K+) may take 10–30ms per subresource on mobile CPUs. This is a one-time cost per texture load, not per frame.
- **TBDR architecture.** Mali is a tile-based deferred renderer. ASTC textures are natively supported by the tile buffer. No special TBDR handling is needed for the transcode path — the ASTC data is uploaded via standard `vkCmdCopyBufferToImage` and decoded by the texture unit before fragment processing.
- **AppendSlice path not yet patched.** The `AppendSlice` D3D11 path may also encounter BC textures. This is a known gap. If you see BC format errors in `AppendSlice`, file an issue.
- **CI builds are automated.** The workflow runs the `ASTC spec gate` (`tests/host/rmchk`) first, then builds both x64 and x32 on Fedora 44 with MinGW-w64 — a gate failure blocks both builds. Release builds set `-Db_ndebug=true`. If the Fedora mirror is temporarily unreachable, the workflow retries automatically.

## Why Some Users Need the BC Wrapper and Others Don't

The wrapper does this:
- It's a Vulkan layer that tells the GPU: "I support BC textures"
- panDXVK sees this and says: "OK, I'll skip the transcode and upload BC textures directly"

The problem:
- On some devices, the blob driver actually handles BC textures fine → game works
- On other devices, the driver genuinely can't handle BC → game breaks

So:
- Users where it works = their device's blob driver secretly supports BC even though PanVK doesn't
- Users where it doesn't work = their device truly can't handle BC textures

The fix:
The transcode gate is kept intentionally — modern wrappers carry the BCN layer, so the pipeline stays dormant behind `!textureCompressionBC` until a wrapper-free device appears. **In practice that means the automatic arm does not open on a wrapped device, and `PANDXVK_FORCE_TRANSCODE=1` is what actually activates BC→ASTC.** Release builds now set `-Db_ndebug=true`, so per-block timing, statistics and the encoder's runtime `assert()`s are compiled out. The four transcode markers (`remap VkImage`, `initializer upload`, `SRV`/`RTV BC→ASTC`) are deliberately *not* `NDEBUG`-gated — they are functional evidence, not diagnostics — and are gated at runtime on `DXVK_LOG_LEVEL=debug`, so they cost nothing by default but remain available on demand.

## Validation Status

**v1.10.3-panVK.8 — BC→ASTC proven end to end on device:**

| Device / driver | Title (build) | `remap VkImage` | errors / backtraces |
|---|---|---|---|
| Mali-G52 MC2 / 49.1.0 | Nine Sols (v8) | **715** | 0 / 0 |
| Mali-G52 MC2 / 49.1.0 | Hollow Knight: Silksong (v6) | 227 | 0 / 0 |
| Apex(Mali-G615 MC6) / 44.1.0 | GTA V (v8) | **295** | 0 / 0 |

- Testers (Gustave and others) confirm textures render cleanly and substantially better than prior releases.
- Independent spec decoder **6/6** vs basis-universal (pre-fix layouts **2/6**); `rtr2/format_gate` **100/100**; `qdiag` **39.92 dB** on `crop_clean`, 0/2820 failures; CI `ASTC spec gate` green.
- All of these ran with the wrapper active (`textureCompressionBC = 1`), so the automatic arm was closed and the runs used `PANDXVK_FORCE_TRANSCODE=1`.

Earlier / non-BC results:
- AIO Graphics Test on Mali-G615 (PanVK 26.2.99 and blob 44.1.0) and Mali-G720 — init parity only.
- 70 FPS panDXVK vs 51 FPS stock DXVK on AIO spin-cube (non-BC test, Us5rman).
- Mali-G99 MC3: 700+ FPS AIO with P11, 900+ FPS with P9 (Proton 9 ARM64EC).

Current state:
- Real Mali hardware has no BC support (gpuinfo + leegao unsupported-device list) — the `= 1` in tester logs comes from the wrapper layer.
- Because the wrapper reports BC, the automatic `!textureCompressionBC` arm never opens in-container; force is what activates the transcode.
- Transcode hot spots optimized: word-level bit extract/insert, verified bit-identical over 12,298 checks.

Still missing:
- A wrapper-free run confirming `textureCompressionBC = 0` in `d3d11.log`.

> Reading a `d3d11.log`: `panDXVK: BC→ASTC gate: BC=… ASTC_LDR=… force=… -> transcode ENABLED/disabled` is printed once at device creation and states the gate's inputs directly — read it instead of inferring them. The `PANDXVK_FORCE_TRANSCODE` one-time notice only fires on the `UpdateSubresource` path, so titles that upload through the initializer show **zero** of them even with force on. The authoritative per-texture markers are the four `BC→ASTC` lines above.

## Upstream DXVK Reference
- Upstream DXVK: [doitsujin/dxvk](https://github.com/doitsujin/dxvk)
- DXVK Sarek: [pythonlover02/dxvk-sarek](https://github.com/pythonlover02/dxvk-sarek)
- Forked from: [pythonlover02/dxvk-sarek@v1.10.3](https://github.com/pythonlover02/dxvk-sarek/tree/v1.10.3)
- Upstream latest: [v3.1](https://github.com/doitsujin/dxvk/releases/tag/v3.1)
