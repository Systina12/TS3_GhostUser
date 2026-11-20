#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 ]]; then
  printf 'Usage: bash %s <Windows DLL>\n' "$0" >&2
  exit 2
fi

table=$(LC_ALL=C objdump -p "$1")
if [[ $table != *'file format pei-x86-64'* ]] ||
   ! awk '$1 == "DLL" && NF == 1 { found = 1 } END { exit !found }' <<< "$table"; then
  printf 'Expected a Windows x64 DLL: %s\n' "$1" >&2
  exit 1
fi
exports=$(awk '
  /^\[Ordinal\/Name Pointer\] Table/ { names = 1; next }
  names && /^[[:blank:]]*\[/ { print $NF }
  names && /^$/ { exit }
' <<< "$table")

for callback in name version apiVersion author description setFunctionPointers init shutdown \
    registerPluginID initMenus freeMemory onMenuItemEvent currentServerConnectionChanged; do
  symbol="ts3plugin_$callback"
  if ! awk -v symbol="$symbol" '$0 == symbol { found = 1 } END { exit !found }' <<< "$exports"; then
    printf 'Missing DLL export: %s\n' "$symbol" >&2
    exit 1
  fi
done
printf 'PASS Windows x64 DLL and required TeamSpeak menu exports\n'
