#!/bin/sh
# Runs an Android test program on the device adb reaches (an emulator or a
# connected phone; ANDROID_SERIAL picks one of several), as ctest's
# CMAKE_CROSSCOMPILING_EMULATOR (set by cmake/toolchains/android.cmake):
# the program is pushed stripped, once per build of it (by its checksum), runs
# in /data/local/tmp/rcsdk with its arguments, and its exit status is this
# script's. With --abi, a device that cannot run that ABI fails the test
# with the reason.
#   android_run.sh [--abi <abi>] [--strip <llvm-strip>] <program> [arguments...]
set -e
abi=""
if [ "$1" = "--abi" ]; then
    abi="$2"
    shift 2
fi
strip=""
if [ "$1" = "--strip" ]; then
    strip="$2"
    shift 2
fi
program="$1"
shift
adb="${ADB:-adb}"
if ! command -v "$adb" >/dev/null 2>&1; then
    for sdk in "$ANDROID_HOME" "$ANDROID_SDK_ROOT" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
        if [ -n "$sdk" ] && [ -x "$sdk/platform-tools/adb" ]; then
            adb="$sdk/platform-tools/adb"
            break
        fi
    done
fi
if [ -n "$abi" ]; then
    abis=$("$adb" shell getprop ro.product.cpu.abilist | tr -d '\r')
    case ",$abis," in
    *",$abi,"*) ;;
    *)
        echo "android_run.sh: the device runs ${abis:-no ABI adb could read}, not $abi: start an emulator or connect a device of $abi (ANDROID_SERIAL picks one)" >&2
        exit 1
        ;;
    esac
fi
if command -v md5sum >/dev/null 2>&1; then
    sum=$(md5sum "$program" | cut -c1-32)
else
    sum=$(md5 -q "$program")
fi
root=/data/local/tmp/rcsdk
name=$(basename "$program")
remote="$root/$sum/$name"
if ! "$adb" shell "test -x $remote" >/dev/null 2>&1; then
    # Several tests may push the same program at once: each under a name of its own, then renamed.
    pushed="$program"
    if [ -n "$strip" ]; then
        pushed=$(mktemp)
        "$strip" --strip-all -o "$pushed" "$program"
    fi
    "$adb" shell "rm -f $root/*/$name; mkdir -p $root/$sum $root/tmp" >/dev/null
    "$adb" push "$pushed" "$remote.$$" >/dev/null
    if [ "$pushed" != "$program" ]; then
        rm -f "$pushed"
    fi
    "$adb" shell "chmod 755 $remote.$$ && mv -f $remote.$$ $remote" >/dev/null
fi
quoted=""
for argument in "$@"; do
    quoted="$quoted '$(printf '%s' "$argument" | sed "s/'/'\\\\''/g")'"
done
exec "$adb" shell "cd $root && TMPDIR=$root/tmp $remote$quoted"
