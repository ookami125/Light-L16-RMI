#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: ./build_client.sh [clean] [build] [run]

Commands can be combined in any order. If no command is provided, build is assumed.
EOF
}

do_clean=false
do_build=false
do_run=false

if [ "$#" -eq 0 ]; then
  do_build=true
else
  for arg in "$@"; do
    case "$arg" in
      clean)
        do_clean=true
        ;;
      build)
        do_build=true
        ;;
      run)
        do_run=true
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        echo "Unknown command: $arg"
        usage
        exit 1
        ;;
    esac
  done
fi

if $do_clean; then
  rm -rf client/build
fi

if $do_build; then
  cmake client -B client/build
  cmake --build client/build -j
fi

if $do_run; then
  if [ ! -x client/build/rmi_client ]; then
    cmake client -B client/build
    cmake --build client/build -j
  fi
  ./client/build/rmi_client
fi
