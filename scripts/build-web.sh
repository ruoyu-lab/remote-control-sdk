#!/bin/bash
# Builds the browser package (preset wasm) and packs it for npm:
# build/wasm/remote-control-sdk-<version>.tgz with rcsdk.mjs, rcsdk.d.ts,
# rcsdk_core.mjs/.wasm, rcsdk_audio.js, LICENSE and THIRD_PARTY_NOTICES.md.
# Publishing is the user's decision; this only makes the file.
# Needs the Emscripten SDK (EMSDK or ~/emsdk) and npm.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset wasm >/dev/null
cmake --build --preset wasm --target rcsdk_web
version=$(sed -nE 's/^#define RCSDK_VERSION_(MAJOR|MINOR|PATCH) ([0-9]+)u/\2/p' include/rcsdk/rcsdk.h | paste -sd. -)
label=$(sed -nE 's/^#define RCSDK_VERSION_LABEL "(.*)"/\1/p' include/rcsdk/rcsdk.h)
[ -n "$label" ] && version="$version-$label"
staging=build/wasm/package
rm -rf "$staging"
mkdir -p "$staging"
cp build/wasm/web/rcsdk.mjs build/wasm/web/rcsdk_core.mjs build/wasm/web/rcsdk_core.wasm build/wasm/web/rcsdk_audio.js \
   web/rcsdk.d.ts LICENSE THIRD_PARTY_NOTICES.md "$staging/"
cat > "$staging/package.json" <<JSON
{
  "name": "remote-control-sdk",
  "version": "$version",
  "description": "Remote Control SDK in the browser: a viewer (WebAssembly, WebCodecs).",
  "license": "MIT",
  "type": "module",
  "main": "rcsdk.mjs",
  "types": "rcsdk.d.ts",
  "exports": { ".": { "types": "./rcsdk.d.ts", "default": "./rcsdk.mjs" } },
  "files": ["rcsdk.mjs", "rcsdk.d.ts", "rcsdk_core.mjs", "rcsdk_core.wasm", "rcsdk_audio.js", "LICENSE",
            "THIRD_PARTY_NOTICES.md"],
  "sideEffects": false
}
JSON
(cd "$staging" && npm pack --pack-destination .. >/dev/null)
ls -l "build/wasm/remote-control-sdk-$version.tgz"
