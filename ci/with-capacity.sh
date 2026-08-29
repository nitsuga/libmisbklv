#!/usr/bin/env bash
set -euo pipefail

if (($# < 2)); then
  echo "usage: $0 <slots: 1|2> <command> [args...]" >&2
  exit 2
fi

slots=$1
shift
# ponytail: fixed two-slot gate; revisit only if host CPU allocation changes.
capacity_dir=${CI_CAPACITY_DIR:-/ci-capacity}
mkdir -p "$capacity_dir"

fds=()
release() {
  for fd in "${fds[@]}"; do
    flock -u "$fd"
    eval "exec ${fd}>&-"
  done
}
trap release EXIT

if [[ "$slots" == 2 ]]; then
  for slot in 0 1; do
    exec {fd}>"$capacity_dir/slot-$slot.lock"
    flock "$fd"
    fds+=("$fd")
  done
elif [[ "$slots" == 1 ]]; then
  while :; do
    for slot in 0 1; do
      exec {fd}>"$capacity_dir/slot-$slot.lock"
      if flock -n "$fd"; then
        fds=("$fd")
        break 2
      fi
      eval "exec ${fd}>&-"
    done
    sleep 1
  done
else
  echo "slots must be 1 or 2" >&2
  exit 2
fi

"$@"
