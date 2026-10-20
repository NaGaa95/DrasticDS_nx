# Native Deko3D display backend

Select **Graphics → Renderer → Deko3D (native)** before starting a game.
The stored renderer key is `Wrapper/Renderer=deko3d`; global and per-game
profiles use the same host. Both Deko3D and Vulkan support LSFG. The Vulkan
low-latency option does not configure Deko3D.

## What is native

This backend operates at DraStic's completed-framebuffer boundary. The ARM64
core in r2.6.0.4a build 109 rasterizes the DS screens in software, including
3D. Unlike Cemu's Latte renderer and NetherSX2's GS interface, its Android
display interface does not submit guest GPU draws. Replacing that display
interface cannot move DS rasterization to the GPU.

The native path is:

```
DraStic software rasterizer → completed core bank → Maxwell copy engine
    → native post-FX passes → layout / stylus / overlay → Horizon swapchain
```

It never calls the core's Android `renderFrame` export or translates GLES or
Vulkan calls. The imported 3 MiB allocation is the original producer storage.
There is no per-frame CPU screen conversion, intermediate CPU capture buffer,
or CPU staging memcpy. Native RGB565 and RGBA8 textures preserve the core's
byte layout. Menu overlays have their own generation-tracked BGRA upload.

The swapchain, image storage, frame arenas, descriptor sets and built-in shader
code are allocated at initialization. Two fenced command/data slots and two
swapchain images are reused by default (three with low latency disabled).
Both screen resolutions and formats are
preallocated, so resolution changes do not allocate or stall the queue. Built-in
intermediate filter passes are cached until their screen changes; custom passes
run with updated time uniforms. Loading a different custom shader and
suspend/shutdown can drain the queue; steady presentation does not call
`dkQueueWaitIdle`.

## Verified core ABI

Offsets below are relative to the loaded ARM64 module, not file offsets.
The loader validates the complete bank-flip and buffer-address routines plus
the allocator and reader instructions before modifying anything. Other core
versions are rejected for Deko3D.

| Address | Observed operation |
| --- | --- |
| `0x19030` | JNI `renderFrame`; forwards texture IDs and swap flag to `0x1ceac` |
| `0x1cb14` | Locks the screen mutex, complements/masks the write-bank index, signals the screen condition, unlocks |
| `0x1cb5c` | Marks a screen dirty and signals under the same mutex |
| `0x1cb9c` | Pre-flip dirty notification; replaced with NOP so display wakes on publication |
| `0x1cbc0` | Sets output color depth and GLES format/type |
| `0x1cbec` | Initializes display state, allocates/clears `0x300000` bytes and initializes mutex/condition |
| `0x1cc30` | `mov w1,#16` alignment argument; changed to `mov w1,#4096` |
| `0x1cc48` | Calls `posix_memalign` with size `0x300000`; core ownership/free remain intact |
| `0x1cd18` | Returns `bank[writer] + (screen & 1) * 0xc0000` to the producer |
| `0x1cde4` | Updates per-screen high-resolution state and dimensions before rendering |
| `0x1ce40` | Waits on the screen condition |
| `0x1ceac` | Reads bank `(~writer)&1`, uploads two screens with GLES, clears dirty flags |
| `0x3d2a8` / `0x3d2b8` | Producer marks completed screens dirty |
| `0x3d2bc` | Producer flips the completed bank |

Display state starts at `0x03f2d1f8`:

| State offset | Meaning |
| --- | --- |
| `0x000`, `0x008` | Bank pointers, separated by `0x180000` bytes |
| `0x950` | Original allocation pointer |
| `0x958` | Current write bank, 0 or 1 |
| `0x95c` | Output color depth, 16 or 32 bits |
| `0x960`, `0x964` | GLES element type and format used by the original reader |
| `0x968`, `0x96c` | Independent high-resolution flags for the two screens |
| `0x978`, `0x97c` | Screen widths |
| `0x980`, `0x984` | Screen heights |
| `0x988` | Dirty-screen bits |
| `0x98c` | Bionic mutex storage (40 bytes) |
| `0x9b4` | Bionic condition storage (48 bytes) |

Each bank reserves `0xc0000` bytes per screen. Active pixels are packed at
256×192 or 512×384, independently for each screen, in RGB565 or RGBA8. GLES
RGB565 has red in bits 11–15; deko's matching packed format is
`DkImageFormat_BGR565_Unorm`. RGBA bytes use `RGBA8_Unorm`. Only core source
views force alpha to one: intermediate alpha must survive FXAA/SMAA passes.

## Producer and GPU ownership

The replacement bank-flip function preserves the original mutex, index and
condition operations. It also publishes a snapshot of the completed bank's
format and both resolutions. This is necessary because the producer updates
resolution fields outside the mutex while preparing its next frame. Native
consumers use the completed-bank snapshot, rather than mutable producer state.

Dirty bits are published at the flip and accumulated until uploaded. Consuming
an old frame cannot clear an unpublished producer change. The two verified
mark-dirty callers both proceed to the flip; their premature condition signal
is suppressed. The flip signals the same retained frame condition after the
completed bank is published. Explicit wake/stop notifications remain intact.
This avoids redundant old-frame presentation between a dirty mark and its flip
and removes up to two condition signals per produced frame.

The consumer locks the mutex, obtains the completed bank, flushes only the
active CPU pixel ranges and records native buffer-to-image DMA. A full barrier
orders previous shader reads before texture overwrites and another makes DMA
writes visible to sampling. A bank fence is signaled immediately after the
upload, before post-processing. The command list is submitted and flushed
before the mutex is unlocked and that fence is published as a bank lease.

Before reusing a bank, the producer waits for its lease, if one exists. This
protects asynchronous GPU reads even during fast-forward, when software can
produce more than two frames before display finishes. The producer waits for
the copy fence, not all later filter/presentation work. Command/data arenas and
descriptors separately wait for their frame fence before reuse. Copy-fence
failure stops the process instead of allowing the producer to overwrite a bank
still owned by the GPU.

Shutdown drains the GPU, clears leases under the core mutex and destroys the
imported mapping **before** `JNI_OnUnload` frees the allocation. The core keeps
ownership throughout. Suspend drains submissions and resume invalidates the
overlay cache.

## Shader and coordinate conventions

`tools/build_deko3d.py` converts the existing DFX uniform/varying ABI to GLSL
450 and invokes `uam` directly. It generates eleven original DFX programs plus
the overlay/solid-color presenter. Built-in and custom display shaders use
precompiled Maxwell code. No Vulkan device or SPIR-V interpreter is involved.

The built-in Scanline filter uses the shared presenter's filter math instead
of the legacy DFX program that produced corrupted colors on hardware. The
native shader has a single UV varying and explicit level-zero sampling for
the one-level source texture. Target-size axes follow screen rotation.

Custom `.dfx.nxdk/` packs contain `passN.vert.dksh`, `passN.frag.dksh` and a
`pack.info` completion marker. The runtime verifies size, stage, DKSH sections,
program bounds and scratch requirements before passing control data to deko.
Multi-pass targets, all sixteen named samplers, raw texture formats, independent
min/mag filtering, scaled targets and time uniforms are supported. Custom image
storage has a 256 MiB budget; manifest/pack validation errors retain the previous
working shader. Unrecoverable GPU errors use the host's process-exit path.
All 56 bundled shaders ship with OpenGL sources, Vulkan packs and native packs.

The backend uses deko's UpperLeft framebuffer origin, with clip-space +Y up.
Vertices therefore use `y = 1 - 2*pixel_y/height`, unlike the positive-height
Vulkan viewport. Rotation uses the existing shared display-to-source UV
transform; quarter-turns also exchange the target-size uniform axes. Overlays
use the same rotation, including portrait menu canvases. The shared layout
rectangles cover vertical, horizontal, hybrid, single-screen and custom layouts.

The reference designs were inspected in `Cemu-nx/src/Cafe/HW/Latte/Renderer/Deko3D`
and `NetherSX2_nx/source/deko3d`. Relevant deko behavior is defined by its
[image transfer implementation](https://github.com/devkitPro/deko3d/blob/master/source/dk_image.cpp),
[format traits](https://github.com/devkitPro/deko3d/blob/master/source/maxwell/format_traits.inc)
and [viewport transform](https://github.com/devkitPro/deko3d/blob/master/source/maxwell/gpu_3d_base.cpp).

## Presentation latency

**Graphics → Low latency** (`Wrapper/LowLatency=true`, the default)
uses two swapchain images, following the double-buffer option in
[sa2-nx v1.0.3.1](https://github.com/ChanseyIsTheBest/sa2-nx/blob/v1.0.3.1/docs/deko3d-renderer-guide.md#8-step-6-pacing-and-input-latency).
This removes a place for another display frame to queue. Image acquisition
precedes the native core lock/upload, so it selects the newest completed bank
after any scanout wait. VSYNC remains enabled. The three-image mode remains
available for filters that miss refresh deadlines with two images.

Native LSFG also supports two images: its real frame is retained in the LSFG
history pool, and generated/original outputs are acquired and presented
sequentially. There is no dependency on a third swapchain image. Two fenced
command/data slots still protect both submissions. The separate Vulkan LSFG
backend's image-count requirements do not govern this native path.

SA2's other latency changes rely on its game-thread input hook and frame
limiter, plus batches of hundreds of guest GPU draws. Here the core rasterizes
the DS screens independently, and the dedicated HID thread already updates
native core input every millisecond even during graphics waits. Adding a
display-clock delay to that producer or splitting these few display draws
would require different evidence; neither is introduced by this change.
No input-to-photon improvement is claimed without hardware measurement.

## Native LSFG

Enable **Frame Generation → LSFG 2x (Deko3D / Vulkan)** before launching.
Place your `Lossless.dll` at `/switch/drastic/lsfg/Lossless.dll`. Startup extracts
the shader resources, translates them through SPIRV-Cross, and compiles native
Maxwell compute programs through UAM. These tools are pinned to the revisions
used by the Cemu/NetherSX2 native backend. No proprietary shader binaries or
DLL are bundled. The verified cache at `/switch/drastic/cache/lsfg-deko3d/`
is keyed by the DLL contents, graph options, compiler revisions and cache ABI.
Cache corruption triggers recompilation at startup.

The game scene is rendered directly into the current history image, avoiding
an extra capture copy. The common LSFG graph schedules the flow and generation
passes, binds native sampled/storage descriptors and inserts image hazards.
After warmup, the renderer copies the generated frame to a swapchain image,
draws the HUD/stylus and presents it. Only then does it acquire the next image,
copy the preserved original and draw the same UI. Separate fenced command/data
slots preserve both submissions, and the producer's DMA lease is released after
submitting the first list. VI never has two acquired images at once.

Compute shaders, uniforms, descriptors and images are allocated before play;
there are no per-frame GPU allocations, compilation or queue-idle calls. The
image pool has a 192 MiB limit, including history and generated images, and
shader scratch is checked against the queue's 64 KiB-per-warp capacity. With
LSFG disabled at launch there is no compute session or history-render pass.
Menus, suspend, filter/resolution/layout/rotation changes and toggles reset
history warmup. Performance and Quarter/Half flow settings use the launcher
preferences. Generation runs at physical output resolution for every layout.

Two FIFO presents per source frame cap source presentation at 30 Hz on a 60 Hz
display; emulation continues independently and may publish newer frames before
the next display iteration. This is fixed 2x generation, without an automatic
high-frame-rate bypass. LSFG adds GPU work and presentation latency; Switch
performance and image quality must be measured on the device.

Building the native compiler requires Git, CMake/Ninja, Bison, Flex and Python
with Mako. `source/deko_compiler` downloads pinned UAM and SPIRV-Cross sources
and builds isolated compiler symbols to avoid conflicts with Mesa. The library
under `third_party/lsfg-deko3d` was adapted from the local NetherSX2 checkout;
its source/license notices are preserved.

## Validation and remaining hardware work

`tools/check_deko3d.py` runs the original core allocator and display functions
under Unicorn, intercepting only imported libc/pthread/GLES entry points. It
checks all 32 combinations of color depth, independent screen resolutions,
write-bank index and swap flag. It also executes the actual native bridge as
AArch64 code through 4096 flips, checking bank leases, metadata races, dirty
retention, bad profiles, invalid state and detach. The same AArch64 DKSH loader
accepts compiled shaders and rejects corrupted, truncated and wrong-stage input.

Example (the proprietary core is not included in the test sources):

```sh
python3 tools/check_deko3d.py --core /path/to/libdrastic_arm64.so \
  --compiler "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" \
  --libnx "$DEVKITPRO/libnx" --shaders /path/to/generated/shaders
```

The Switch host and packaged NRO build with the installed devkitA64/libnx/deko3d
toolchain, and all bundled native shader stages compile with `uam`. These checks
do not execute Maxwell shaders or Horizon presentation. Switch testing is still
required for image correctness, fast-forward, resolution/filter/layout changes,
suspend/resume and exit. No frame-rate or latency improvement is claimed until
measured on hardware. DS GPU command translation would require replacing the
core's software rasterizer.

`tools/tests/deko_lsfg_validation.cpp` compiles all four Performance/flow modes
from a user-provided DLL, validates compute metadata, round-trips and damages
the shader cache, and checks every graph binding at 720p and 1080p. With the
libnx headers supplied, `deko_lsfg_recording.cpp` also executes the production
integration against a recording deko API, checking warmup/reset, history
ownership, shader/image bounds, descriptors and GPU block lifetime. This does
not execute Maxwell shader instructions or Horizon presentation.

For a POSIX host with a C++20 compiler:

```sh
cmake -S source/deko_compiler -B /tmp/drastic-lsfg-test -G Ninja \
  -DDEKO3D_SHADER_TESTS=ON -DDEKO3D_TEST_LIBNX_INCLUDE="$DEVKITPRO/libnx/include" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/drastic-lsfg-test --parallel
/tmp/drastic-lsfg-test/deko_lsfg_validation /path/to/Lossless.dll /tmp/drastic-lsfg-cache
/tmp/drastic-lsfg-test/deko_present_validation
```

The presentation recorder runs the actual renderer initialization, presentation
and teardown against a mock deko API. It checks both queue depths, LSFG on/off,
720p/1080p output, four rotations, cached menu frames and suspend/resume. It
verifies acquisition before source capture, one acquired VI image, generated
then original ordering, UI after interpolation, fenced slot reuse and no
presentation-time GPU allocations. Synthetic graphics shaders and a minimal
LSFG double isolate these API/lifetime checks from the real compiler tests.
