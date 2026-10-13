#!/bin/sh
# Writes third_party/<library>/rcsdk_<library>_names.h and .asm: every global
# symbol the vendored library defines, renamed with the prefix rcsdk_, so
# that a host linking its own copy of the library never clashes with the one
# inside the SDK, static builds included (hidden visibility only applies to
# shared ones). The .h is included first by every C source of the library
# and by the SDK code that calls it; the .asm, rewritten where the library
# has one, by its NASM sources (where the library's own symbol prefix is
# rcsdk_'s too). Run it after updating the library, from the repository
# root, with build trees of every architecture the SDK ships (their objects
# together hold each SIMD variant):
#   third_party/rename-symbols.sh libvpx build/release build/windows-x64-release
# NM may name an nm that reads all of their object formats (llvm-nm). C++
# symbols are known by their outermost name (a namespace, class or function:
# the macro renames all it holds); CXXFILT names the demangler (c++filt).
set -eu
library=$1
shift
nm=${NM:-nm}
out=third_party/$library/rcsdk_${library}_names
# Mach-O prefixes every C symbol with an underscore; a tree where every
# symbol has one is Mach-O, and loses it.
symbols() {
    find "$1/third_party/$library/CMakeFiles" \( -name '*.o' -o -name '*.obj' \) \
        -exec "$nm" -g --defined-only {} + 2>/dev/null |
        awk 'NF == 3 { print $3 }' | grep -v -e '^\.' -e ':' -e '^\$' | sort -u
}
outermost() {
    "${CXXFILT:-c++filt}" -n | sed -E \
        -e 's/^(vtable|typeinfo name|typeinfo|VTT|guard variable|construction vtable|(non-)?virtual thunk) (for|to) //' \
        -e 's/(::|\(|<| ).*//'
}
names=$(for tree in "$@"; do
    list=$(symbols "$tree")
    if echo "$list" | grep -qv '^_'; then echo "$list"; else echo "$list" | sed -e 's/^_//'; fi
done | outermost | sed -e 's/^rcsdk_//' | grep -v -e '^__' -e '^std$' | grep '^[A-Za-z_][A-Za-z0-9_]*$' | sort -u)
# (Trees built with the header already have the prefix; the standard
# library's own are not renamed; assembly labels such as "f.main" are no C
# names, and follow their function's.)
{
    echo "/* Every global symbol of the vendored $library gets the prefix rcsdk_ (see"
    echo ' * third_party/rename-symbols.sh, which wrote this file). */'
    guard=RCSDK_$(echo "$library" | tr 'a-z' 'A-Z')_NAMES_H
    echo "#ifndef $guard"
    echo "#define $guard"
    for name in $names; do
        echo "#define $name rcsdk_$name"
    done
    echo '#endif'
} > "$out.h"
[ -f "$out.asm" ] && {
    echo "; Every global symbol of the vendored $library gets the prefix rcsdk_ (see"
    echo '; third_party/rename-symbols.sh, which wrote this file).'
    for name in $names; do
        echo "%define $name rcsdk_$name"
    done
} > "$out.asm"
echo "$out.h: $(echo "$names" | wc -l | tr -d ' ') symbols"
