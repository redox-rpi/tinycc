#!/bin/bash
# Weekly csmith run (WBS 2.15 point 4, tinycc#3): builds vc4-tcc from the
# newest $CSMITH_BRANCH in its own clone, runs csmith-diff.sh and appends
# the JSON line to tcc.jsonl in redox-rpi/pi-tests. Started by
# csmith-weekly.timer; mismatches are kept for reduction by hand
# (csmith-reduce.sh), they do not fail the run.
# SPDX-License-Identifier: LGPL-2.1-or-later
#
#   CSMITH_ROOT=dir      the clone this script builds in (default: its own)
#   CSMITH_BRANCH=name   branch to test (default main)
#   CSMITH_WORK=dir      logs and kept programs, per week (default
#                        ~/rbc-build/tcc-csmith-work)
#   CSMITH_COUNT=n       programs (default 1000)
#   VC4_PREFIX           toolchain prefix
#   VC4EMUL              resim's vc4emul (default: built in $CSMITH_WORK/resim
#                        at RESIM_REV of tests/vc4/ci/pins.sh, as in CI;
#                        an older resim gives false mismatches, e.g. W48 with
#                        upstream e9f04a3 and vc4-tcc's fused compare and branch)
#   PI_TESTS_CLONE=dir   pi-tests clone (default ~/pi-tests-tcc; cloned if
#                        missing)
#   TOKEN_FILE=file      GitHub token for pi-tests (default
#                        ~/.config/redox-rpi/github-token); without it the
#                        line stays in $CSMITH_WORK/csmith.jsonl only
#   SEED, JOBS, TIMEOUT  passed to csmith-diff.sh; the seed defaults to the
#                        ISO week (e.g. 202646000), so a week's run can be
#                        repeated
set -eu
root=${CSMITH_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
branch=${CSMITH_BRANCH:-main}
week=$(date -u +%G-W%V)
work=${CSMITH_WORK:-$HOME/rbc-build/tcc-csmith-work}
count=${CSMITH_COUNT:-1000}
prefix=${VC4_PREFIX:-$HOME/rbc-build/toolchains/prefix/bin/vc4-elf-}
emul=${VC4EMUL:-}
tests=${PI_TESTS_CLONE:-$HOME/pi-tests-tcc}
token=${TOKEN_FILE:-$HOME/.config/redox-rpi/github-token}
log() { echo "$(date -u +%H:%M:%S) $*"; }

# The token never appears on a command line, in a URL or in .git/config.
git_auth() {
	git -c credential.helper= \
		-c "credential.helper=!f() { echo username=x-access-token; printf 'password='; tr -d '[:space:]' < '$token'; echo; }; f" "$@"
}

log "csmith weekly $week, $branch, $count programs"
git -C "$root" fetch -q origin
git -C "$root" checkout -q --detach "origin/$branch"
[ -f "$root/config.mak" ] || (cd "$root" && ./configure --config-bcheck=no --config-backtrace=no > /dev/null)
make -C "$root" -s vc4-tcc > /dev/null
log "vc4-tcc $(git -C "$root" rev-parse --short=8 HEAD)"

# The simulator CI uses (pins.sh), built once per pin in the work dir.
if [ -z "$emul" ]; then
	. "$root/tests/vc4/ci/pins.sh"
	r=$work/resim
	[ -d "$r/.git" ] || git clone -q "https://github.com/$RESIM_REPO" "$r"
	if [ "$(git -C "$r" rev-parse HEAD)" != "$RESIM_REV" ] || [ ! -x "$r/build/vc4emul/vc4emul" ]; then
		git -C "$r" fetch -q origin
		git -C "$r" checkout -q --detach "$RESIM_REV"
		cmake -S "$r" -B "$r/build" > /dev/null
		make -C "$r/build" -s -j"$(nproc)" vc4emul > /dev/null
	fi
	emul=$r/build/vc4emul/vc4emul
	log "resim $(git -C "$r" rev-parse --short=8 HEAD)"
fi

mkdir -p "$work/$week"
cd "$work/$week"
SEED=${SEED:-$(date -u +%G%V)000} KEEP=keep TIMEOUT=${TIMEOUT:-30} \
	OUT="$work/csmith.jsonl" nice -n 10 \
	"$root/tests/vc4/csmith-diff.sh" "$prefix" "$emul" "$count" > run.log 2>&1 || true
line=$(tail -1 run.log)
case $line in '{"kind":"csmith"'*) ;; *) log "no record line, see $PWD/run.log"; exit 1;; esac
log "$line"

[ -r "$token" ] || { log "no token file, line not sent to pi-tests"; exit 0; }
[ -d "$tests/.git" ] || git_auth clone -q https://github.com/redox-rpi/pi-tests "$tests"
for try in 1 2 3; do
	git_auth -C "$tests" fetch -q origin
	git -C "$tests" checkout -q --detach origin/main
	echo "$line" >> "$tests/tcc.jsonl"
	git -C "$tests" add tcc.jsonl
	git -C "$tests" -c user.name="redox-rpi csmith" -c user.email=csmith@homenodellm \
		commit -q -m "tcc.jsonl: csmith $week"
	git_auth -C "$tests" push -q origin HEAD:main && { log "sent to pi-tests tcc.jsonl"; exit 0; }
	log "push failed (try $try)"; sleep 30
done
exit 1
