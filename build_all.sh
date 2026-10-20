#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITA64=$DEVKITPRO/devkitA64
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || {
  echo "JOBS must be a positive integer." >&2
  exit 1
}

APP="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$APP")"
APK_DIR=${DRASTIC_APK_DIR:-"$ROOT/com.dsemu.drastic_r2.6.0.4a-109_minAPI14(arm64-v8a)(nodpi)_drasticds.com"}
MESA_SDK=${MESA_SDK_DIR:-"$ROOT/mesa-switch-unified-sdk"}
# Accept either the installed Switch prefix itself or the root produced by
# extracting mesa-*-switch-unified-horizon-sdk.zip.
if [[ -d "$MESA_SDK/opt/devkitpro/portlibs/switch" ]]; then
  MESA_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch"
fi
BUILD_CACHE=${DRASTIC_BUILD_CACHE_DIR:-"$APP/.drasticds-nx-cache"}
CORE="$APK_DIR/lib/arm64-v8a/libdrastic_arm64.so"
ASSETS="$APK_DIR/assets"
VULKAN_HEADERS="$MESA_SDK/include/vulkan"
DFX_SOURCE="$ASSETS/shaders"
BUNDLED_SHADER_SOURCE="$APP/third_party/drastic-ds-shaders"
BUNDLED_CHEAT_SOURCE=${DRASTIC_USRCHEAT_DATABASE:-"$APP/third_party/nds-i-cheat-databases/usrcheat.dat"}
BUNDLED_CHEAT_LICENSE="$APP/third_party/nds-i-cheat-databases"

required=(
  "$CORE"
  "$ASSETS/game_database.xml"
  "$BUNDLED_CHEAT_SOURCE"
  "$BUNDLED_CHEAT_LICENSE/NOTICE.md"
  "$BUNDLED_CHEAT_LICENSE/COPYING"
  "$DFX_SOURCE/None.dfx"
  "$DFX_SOURCE/Linear.dfx"
  "$DFX_SOURCE/Quilez.dfx"
  "$DFX_SOURCE/Scanline.dfx"
  "$DFX_SOURCE/Scale2X.dfx"
  "$DFX_SOURCE/HQ2X.dfx"
  "$DFX_SOURCE/FXAA.dfx"
  "$DFX_SOURCE/FXAA HQ.dfx"
  "$DFX_SOURCE/SMAA.dfx"
  "$DFX_SOURCE/linear.dsd"
  "$DFX_SOURCE/quilez.dsd"
  "$DFX_SOURCE/scanline.dsd"
  "$DFX_SOURCE/scale2x.dsd"
  "$DFX_SOURCE/hq2x.dsd"
  "$DFX_SOURCE/fxaa.dsd"
  "$DFX_SOURCE/fxaa/fxaa_luma.dsd"
  "$DFX_SOURCE/fxaa/fxaa.dsd"
  "$DFX_SOURCE/smaa/smaa_edge.dsd"
  "$DFX_SOURCE/smaa/smaa_weight.dsd"
  "$DFX_SOURCE/smaa/smaa_blend.dsd"
  "$DFX_SOURCE/smaa/SMAA.hlsl"
  "$DFX_SOURCE/smaa/AreaTexRGB.raw"
  "$DFX_SOURCE/smaa/SearchTexRGB.raw"
  "$BUNDLED_SHADER_SOURCE/README.md"
  "$BUNDLED_SHADER_SOURCE/NOTICE.md"
  "$BUNDLED_SHADER_SOURCE/COPYING"
  "$VULKAN_HEADERS/vk_layer.h"
  "$VULKAN_HEADERS/vk_platform.h"
  "$VULKAN_HEADERS/vulkan.h"
  "$VULKAN_HEADERS/vulkan_core.h"
  "$VULKAN_HEADERS/vulkan_vi.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codecs_common.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_av1std.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_av1std_decode.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_av1std_encode.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_h264std.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_h264std_decode.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_h264std_encode.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_h265std.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_h265std_decode.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_h265std_encode.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_vp9std.h"
  "$MESA_SDK/include/vk_video/vulkan_video_codec_vp9std_decode.h"
  "$MESA_SDK/lib/libEGL.a"
  "$MESA_SDK/lib/libGLESv2.a"
  "$MESA_SDK/lib/libglapi.a"
  "$MESA_SDK/lib/libvulkan.a"
  "$MESA_SDK/lib/libmesa_util.a"
  "$MESA_SDK/lib/libmesa_util_c11.a"
  "$MESA_SDK/lib/libmesa_util_simd.a"
  "$MESA_SDK/lib/libblake3.a"
  "$MESA_SDK/lib/libxmlconfig.a"
  "$MESA_SDK/share/drirc.d/00-zink-defaults.conf"
)
for file in "${required[@]}"; do
  [[ -f "$file" ]] || {
    echo "Missing build input: $file" >&2
    exit 1
  }
done
for command_name in cmake ninja make find wc; do
  command -v "$command_name" >/dev/null || {
    echo "$command_name is required." >&2
    exit 1
  }
done
BUNDLED_SHADER_COUNT="$(find "$BUNDLED_SHADER_SOURCE" -type f -iname '*.dfx' | wc -l)"
if (( BUNDLED_SHADER_COUNT < 1 )); then
  echo "No bundled custom shader manifests were found in $BUNDLED_SHADER_SOURCE" >&2
  exit 1
fi
PYTHON3=${PYTHON3:-$(command -v python3 || true)}
[[ -n "$PYTHON3" && -x "$PYTHON3" ]] || {
  echo "python3 is required to generate the Drastic filter programs." >&2
  exit 1
}
echo "==== DraStic launcher localization gate ===="
"$PYTHON3" "$APP/tools/check_launcher_localization.py"
GLSLANG=${GLSLANG_VALIDATOR:-$(command -v glslangValidator || true)}
if [[ -z "$GLSLANG" && -x /ucrt64/bin/glslangValidator.exe ]]; then
  GLSLANG=/ucrt64/bin/glslangValidator.exe
fi
[[ -n "$GLSLANG" && -x "$GLSLANG" ]] || {
  echo "glslangValidator is required to compile the Vulkan filters." >&2
  exit 1
}
UAM=${UAM:-$(command -v uam || true)}
[[ -n "$UAM" && -x "$UAM" ]] || {
  echo "uam (devkitPro tools) is required to compile the native Deko3D shaders." >&2
  exit 1
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/drasticds-nx.XXXXXX")"
cleanup() {
  rm -rf "$WORK"
}
trap cleanup EXIT

ROMFS_STAGE="$WORK/romfs"
DFX_STAGE="$WORK/dfx"
mkdir -p "$ROMFS_STAGE" "$DFX_STAGE"
cp -f "$APP/launcher/romfs/logo.png" "$ROMFS_STAGE/logo.png"
cp -f "$APP/launcher/icon.jpg" "$ROMFS_STAGE/forwarder-icon.jpg"
mkdir -p "$ROMFS_STAGE/icons"
cp -f "$APP/launcher/romfs/icons/"* "$ROMFS_STAGE/icons/"

echo "==== clean previous outputs ===="
make -C "$APP" clean >/dev/null
make -C "$APP/launcher" clean >/dev/null
make -C "$APP/launcher/fwd" clean >/dev/null

echo "==== Drastic Android post-FX programs ===="
"$PYTHON3" "$APP/tools/build_dfx.py" \
  --source "$DFX_SOURCE" --output "$DFX_STAGE" --glslang "$GLSLANG"
"$PYTHON3" "$APP/tools/build_deko3d.py" \
  --source "$DFX_SOURCE" --output "$DFX_STAGE" --uam "$UAM"
dfx_args=(DFX_GENERATED="$DFX_STAGE")

DEPS_BUILD="$BUILD_CACHE/storage"
if [[ -f "$DEPS_BUILD/CMakeCache.txt" ]]; then
  if ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$DEPS_BUILD/CMakeCache.txt" ||
     ! grep -Fqx "CMAKE_HOME_DIRECTORY:INTERNAL=$APP/launcher/dependencies" \
       "$DEPS_BUILD/CMakeCache.txt"; then
    cmake -E rm -rf "$DEPS_BUILD"
  fi
fi

echo "==== launcher storage dependencies ===="
deps_args=(
  -S "$APP/launcher/dependencies"
  -B "$DEPS_BUILD"
  -G Ninja
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
)
if [[ -n "${LIBSMB2_SOURCE:-}" ]]; then
  deps_args+=( -DFETCHCONTENT_SOURCE_DIR_LIBSMB2="$LIBSMB2_SOURCE" )
else
  deps_args+=( -DFETCHCONTENT_SOURCE_DIR_LIBSMB2= )
fi
if [[ -n "${LIBUSBHSFS_SOURCE:-}" ]]; then
  deps_args+=( -DFETCHCONTENT_SOURCE_DIR_LIBUSBHSFS="$LIBUSBHSFS_SOURCE" )
else
  deps_args+=( -DFETCHCONTENT_SOURCE_DIR_LIBUSBHSFS= )
fi
cmake "${deps_args[@]}"
cmake --build "$DEPS_BUILD" --parallel "$JOBS"

LIBSMB2_INCLUDE_DIR="${LIBSMB2_SOURCE:-$DEPS_BUILD/_deps/libsmb2-src}/include"
LIBUSBHSFS_INCLUDE_DIR="${LIBUSBHSFS_SOURCE:-$DEPS_BUILD/_deps/libusbhsfs-src}/include"
[[ -f "$LIBSMB2_INCLUDE_DIR/smb2/libsmb2.h" ]] || {
  echo "Missing libsmb2 headers: $LIBSMB2_INCLUDE_DIR" >&2
  exit 1
}
[[ -f "$LIBUSBHSFS_INCLUDE_DIR/usbhsfs.h" ]] || {
  echo "Missing libusbhsfs headers: $LIBUSBHSFS_INCLUDE_DIR" >&2
  exit 1
}
storage_include_args=(
  STORAGE_BUILD="$DEPS_BUILD"
  LIBSMB2_INCLUDE="$LIBSMB2_INCLUDE_DIR"
  LIBUSBHSFS_INCLUDE="$LIBUSBHSFS_INCLUDE_DIR"
)

DEKO_DEPS="$BUILD_CACHE/deko-deps"
echo "==== native Deko3D LSFG dependencies ===="
cmake -S "$APP/source/deko_compiler" -B "$DEKO_DEPS" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$DEKO_DEPS" --parallel "$JOBS"

echo "==== unified DraStic host: Deko3D + Vulkan + NVC0 + Zink ===="
make -C "$APP" -j"$JOBS" "${storage_include_args[@]}" \
  "${dfx_args[@]}" MESA_SDK="$MESA_SDK" DEKO_DEPS="$DEKO_DEPS"

echo "==== assemble temporary ROMFS ===="
mkdir -p "$ROMFS_STAGE/cores" "$ROMFS_STAGE/emu" "$ROMFS_STAGE/res"
cp -f "$CORE" "$ROMFS_STAGE/cores/libdrastic_arm64.so"
cp -f "$APP/DrasticDS_nx.nro" "$ROMFS_STAGE/emu/DrasticDS_nx.nro"
make -C "$APP" clean >/dev/null
"$PYTHON3" "$APP/tools/patch_game_database.py" \
  --input "$ASSETS/game_database.xml" \
  --output "$ROMFS_STAGE/res/game_database.xml"
cp -f "$BUNDLED_CHEAT_SOURCE" "$ROMFS_STAGE/res/usrcheat.dat"
mkdir -p "$ROMFS_STAGE/licenses/nds-i-cheat-databases"
cp -f "$BUNDLED_CHEAT_LICENSE/NOTICE.md" \
  "$ROMFS_STAGE/licenses/nds-i-cheat-databases/NOTICE.md"
cp -f "$BUNDLED_CHEAT_LICENSE/COPYING" \
  "$ROMFS_STAGE/licenses/nds-i-cheat-databases/COPYING"

echo "==== bundled custom shaders: $BUNDLED_SHADER_COUNT (OpenGL + Vulkan + Deko3D) ===="
mkdir -p "$ROMFS_STAGE/shaders"
cp -a "$BUNDLED_SHADER_SOURCE/." "$ROMFS_STAGE/shaders/"
"$PYTHON3" "$APP/tools/compile_custom_shader.py" \
  "$ROMFS_STAGE/shaders" --glslang "$GLSLANG" --uam "$UAM"

echo "==== forwarder stub ===="
make -C "$APP/launcher/fwd" clean OUT="$ROMFS_STAGE/fwd" >/dev/null
make -C "$APP/launcher/fwd" -j"$JOBS" OUT="$ROMFS_STAGE/fwd"

echo "==== SDL2 launcher ===="
make -C "$APP/launcher" -j"$JOBS" "${storage_include_args[@]}" \
  ROMFS_DIR="$ROMFS_STAGE"
mv -f "$APP/launcher/DrasticDS.nro" "$WORK/DrasticDS.nro"
make -C "$APP/launcher" clean >/dev/null
make -C "$APP/launcher/fwd" clean OUT="$ROMFS_STAGE/fwd" >/dev/null
mv -f "$WORK/DrasticDS.nro" "$APP/DrasticDS.nro"

trap - EXIT
rm -rf "$WORK"

echo
echo "Done. Copy this one file to the SD card:"
ls -la "$APP/DrasticDS.nro"
echo
echo "The launcher creates sdmc:/switch/drastic/, extracts one unified host on demand,"
echo "and selects Deko3D, Vulkan, native OpenGL or Zink at runtime."
