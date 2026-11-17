#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "Run this script on macOS with Xcode Command Line Tools and CMake 3.22 or newer."
  exit 1
fi
for sculpt_tool in cmake python3 xcode-select lipo codesign ditto; do
  if ! command -v "$sculpt_tool" >/dev/null 2>&1; then
    echo "Missing build tool: $sculpt_tool"
    exit 1
  fi
done
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Install the Xcode Command Line Tools with: xcode-select --install"
  exit 1
fi

sculpt_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
sculpt_build="${SCULPT_LEAD_BUILD_DIR:-${sculpt_root}/build-macos}"
sculpt_dist="${SCULPT_LEAD_DIST_DIR:-${sculpt_root}/dist}"
sculpt_jobs="${SCULPT_LEAD_BUILD_JOBS:-3}"

python3 "${sculpt_root}/tools/verify_factory.py"
sculpt_args=(
  -S "$sculpt_root"
  -B "$sculpt_build"
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
  -DSCULPT_LEAD_BUILD_QA=ON
  -DBUILD_TESTING=ON
)
if [[ -n "${SCULPT_JUCE_SOURCE_DIR:-}" ]]; then
  sculpt_args+=("-DSCULPT_JUCE_SOURCE_DIR=${SCULPT_JUCE_SOURCE_DIR}")
fi

cmake "${sculpt_args[@]}"
cmake --build "$sculpt_build" --config Release \
  --target SculptLead_VST3 SculptLeadCoreTests SculptLeadStateTests \
  --parallel "$sculpt_jobs"
ctest --test-dir "$sculpt_build" -C Release --output-on-failure

sculpt_bundle="${sculpt_build}/SculptLead_artefacts/Release/VST3/Sculpt Lead.vst3"
sculpt_binary="${sculpt_bundle}/Contents/MacOS/Sculpt Lead"
if [[ ! -d "$sculpt_bundle" || ! -f "$sculpt_binary" ]]; then
  echo "Build did not produce the expected VST3: $sculpt_bundle"
  exit 1
fi

lipo "$sculpt_binary" -verify_arch arm64 x86_64
plutil -lint "${sculpt_bundle}/Contents/Info.plist"
codesign --force --deep --sign - "$sculpt_bundle"
codesign --verify --deep --strict --verbose=2 "$sculpt_bundle"

mkdir -p "$sculpt_dist"
sculpt_archive="${sculpt_dist}/Sculpt-Lead-macOS-VST3.zip"
ditto -c -k --sequesterRsrc --keepParent "$sculpt_bundle" "$sculpt_archive"
echo "Built and tested universal VST3: $sculpt_archive"
echo "Apple Silicon + Intel; macOS 11 or newer. Ad-hoc signed, not notarised."
