#!/usr/bin/env bash
# Compile one C file for the Windows GameAssembly.dll (called by tools/build_win_ga.sh, which sets
# INCLUDE/LIB from vcvars and WIN_CC_INCLUDES).
# usage: win_cc.sh <file.c> <objdir>   |   win_cc.sh --skip-fresh '' <objdir> <file.c>  (xargs form)
set -euo pipefail
export MSYS_NO_PATHCONV=1  # keep clang-cl options like /O1 as they are
skip=0
if [ "$1" = "--skip-fresh" ]; then skip=1; obj_dir="$3"; src="$4"; else src="$1"; obj_dir="$2"; fi
obj="$obj_dir/$(basename "${src%.c}").obj"
if [ $skip = 1 ] && [ -e "$obj" ] && [ "$obj" -nt "$src" ]; then exit 0; fi
inc=()
IFS=';' read -ra dirs <<< "$WIN_CC_INCLUDES"
for d in "${dirs[@]}"; do inc+=("/I$d"); done
opt=/O1; [ -n "${WIN_CC_DEBUG:-}" ] && opt="/Od /Z7"
"/c/Program Files/LLVM/bin/clang-cl.exe" /nologo $opt /c -mcx16 "${inc[@]}" -Wno-unused-label -Wno-parentheses-equality \
  -Wno-unused-variable -Wno-unused-function "/Fo$(cygpath -w "$obj")" "$(cygpath -w "$src")"
