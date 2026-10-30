#!/bin/sh
# Build UniversalFunctionBlocksModule for this computer (macOS or Linux) into
# dist/<platform>-<machine>/, for openDAQ Studio on the desktop.
#
# A module must be built against the SDK release that loads it, so the SDK's
# core is built first, at SDK_REF: the release of the openDAQ SDK that openDAQ
# Studio ships. The SDK is cloned into build/openDAQ from OPENDAQ_SOURCE, the
# openDAQ repository on GitHub unless set (a local clone is faster). The
# first run takes a few minutes; later runs only rebuild what changed.
#
#   tool/build_desktop.sh
#
# The SDK's install in build/sdk-<platform>-<machine>-install is also what
# openDAQ Studio's engine compiles against. CMAKE and NINJA name the tools
# when they are not on the PATH.

set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SDK_SOURCE="${OPENDAQ_SOURCE:-https://github.com/openDAQ/openDAQ.git}"
SDK_REF="${SDK_REF:-v3.40.3}"
BUILD="$HERE/build"
PLATFORM="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
DIST="$HERE/dist/$PLATFORM"
CMAKE="${CMAKE:-cmake}"
NINJA="${NINJA:-ninja}"
for tool in git "$CMAKE" "$NINJA"; do
  command -v "$tool" > /dev/null 2>&1 || { echo "$tool is missing; see Requirements in README.md" >&2; exit 1; }
done

mkdir -p "$BUILD"
if [ ! -d "$BUILD/openDAQ/.git" ]; then
  git clone -q "$SDK_SOURCE" "$BUILD/openDAQ"
fi
if [ "$(git -C "$BUILD/openDAQ" describe --tags --exact-match 2> /dev/null)" != "$SDK_REF" ]; then
  git -C "$BUILD/openDAQ" fetch -q --tags origin
  git -C "$BUILD/openDAQ" checkout -q --force "$SDK_REF"
fi

COMMON="-G Ninja -DCMAKE_MAKE_PROGRAM=$NINJA -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
if [ "$(uname -s)" = Darwin ]; then
  # The deployment target of the opendaq wheel's libraries.
  COMMON="$COMMON -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"
fi
# A compiler cache: a file compiled before, in any build folder, with the
# same flags, comes from the cache instead of the compiler.
if command -v ccache > /dev/null 2>&1; then
  COMMON="$COMMON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
  export CCACHE_BASEDIR="$HERE"
fi

# Configures a build folder only when it has none yet or its options changed.
# Ninja configures again by itself when a CMakeLists.txt changes.
configure() {
  source="$1"
  folder="$2"
  log="$3"
  shift 3
  options="$source $*"
  if [ -f "$folder/build.ninja" ] && [ "$(cat "$folder/.configure-options" 2> /dev/null)" = "$options" ]; then
    return
  fi
  "$CMAKE" -S "$source" -B "$folder" "$@" > "$log"
  printf '%s' "$options" > "$folder/.configure-options"
}

echo "== openDAQ SDK core, $SDK_REF"
# shellcheck disable=SC2086
configure "$BUILD/openDAQ" "$BUILD/sdk-$PLATFORM" "$BUILD/sdk-configure.log" $COMMON \
  -DOPENDAQ_ENABLE_TESTS=OFF -DOPENDAQ_ENABLE_TEST_UTILS=OFF
"$CMAKE" --build "$BUILD/sdk-$PLATFORM" > "$BUILD/sdk-build.log"
"$CMAKE" --install "$BUILD/sdk-$PLATFORM" --prefix "$BUILD/sdk-$PLATFORM-install" > "$BUILD/sdk-install.log"

echo "== function blocks module"
# shellcheck disable=SC2086
configure "$HERE" "$BUILD/$PLATFORM" "$BUILD/configure.log" $COMMON -DCMAKE_PREFIX_PATH="$BUILD/sdk-$PLATFORM-install"
"$CMAKE" --build "$BUILD/$PLATFORM"

mkdir -p "$DIST"
# A new file, not one written over the old: macOS keeps a library's code
# signature per file, and kills a process that loads a changed one in place.
for library in "$BUILD/$PLATFORM"/libuniversal_function_blocks_module.module.*; do
  rm -f "$DIST/$(basename "$library")"
  cp "$library" "$DIST/"
done
ls -l "$DIST"
