#!/usr/bin/env bash
# nvme-phase7.sh - Phase 7 acceptance & hardening runner for the AvoryOS NVMe
# driver.  This script owns the long suites and calls scripts/nvme-stress.sh
# (which builds the kernel/images and boots QEMU) once per configuration.
#
# Subcommands:
#   ahci    AHCI/ATA regression: one NVMe-vs-AHCI comparison boot plus N
#           host-verified pattern/SHA boots writing through the AHCI stack.
#   matrix  the 72-cell acceptance matrix:
#             LBA format     512e | 4Kn                          (2)
#             topology       single NS | multi NS | multi ctrl  (3)
#             firmware       baseline | constrained knobs       (2)
#             accelerator    KVM | TCG                          (2)
#             SMP            1 | 2 | 4                          (3)
#           -> 72 cells.  Each cell boots with NVME_SELFTEST=1 (3 disable/
#           enable cycles and 10000 Identify commands) and the NVME-AUDIT
#           marker required.
#   boots   N boot-loop configurations (default 50) drawn round-robin from a
#           rotating list: NVMe root, 4Kn/data selftest, polled IRQ, lost-IRQ
#           deep queue, constrained firmware.
#   tib     accumulate verified writes across boots until the target
#           (default 1 TiB).  The ledger is parsed from the guest's
#           NVME-VERIFIED total markers; state resumes across invocations.
#   chaos   N power-cut cycles (default 100) with host e2fsck after each cut;
#           mixed nvme-root/AHCI root by default.
#   soak    worst-case soak (default 4 h): 1 vCPU TCG, 4Kn+512e namespaces,
#           QD 64 random writes on the 4Kn namespace, lost-interrupt build.
#   all     ahci -> matrix -> boots -> tib -> chaos -> soak.
#
# Global flags:
#   --dry-run             print the stress commands without booting QEMU
#   --quick               reduced counts for a smoke run
#   --list                matrix: print the 72 cells and exit
#   --limit=N             matrix: run at most N selected cells
#   --only=SUBSTR         matrix: select cells whose "lba=... topo=... fw=...
#                         accel=... smp=..." label contains SUBSTR (e.g. 4kn,
#                         topo=multi-ns, accel=tcg, smp=2)
#   --no-resume           ignore (and reset) matrix/tib/chaos state
#   --hours=N             soak length in hours (decimal allowed, default 4)
#   --soak-seconds=N      soak length in seconds (overrides --hours)
#   --count=N             boots: boot count (50); ahci: regression boots (5)
#   --cuts=N              chaos: power-cut cycles (100)
#   --target-gib=N        tib: target size in GiB (1024 = 1 TiB)
#   --gib-per-boot=N      tib: verified GiB per boot (64)
#   --device=nvme|ahci    tib: ledger device (nvme = nvme0n1 scratch)
#   --chaos-config=X      chaos: nvme-root | ide-scratch | mixed (default)
#   --smp=N               soak SMP override (default 1)
#   --accel=kvm|tcg       soak accelerator override (default tcg)
#   --timeout=SECONDS     pass an explicit QEMU timeout to nvme-stress.sh
#
# Examples:
#   ./scripts/nvme-phase7.sh --list
#   ./scripts/nvme-phase7.sh --dry-run matrix --limit=4
#   ./scripts/nvme-phase7.sh matrix --only=accel=tcg
#   ./scripts/nvme-phase7.sh matrix --only=lba=4kn --limit=8
#   ./scripts/nvme-phase7.sh --quick all
#   ./scripts/nvme-phase7.sh soak --hours=4
#   ./scripts/nvme-phase7.sh tib --target-gib=1 --gib-per-boot=1
#
# State (resumable):
#   build/nvme/phase7/state/matrix.done   completed matrix cell keys
#   build/nvme/phase7/state/tib-bytes     verified-byte ledger
#   build/nvme/phase7/state/chaos.done    last completed power-cut index
#   build/nvme/phase7/state/last-log      log of the most recent boot
# Logs: build/nvme/phase7/<run-id>-<suite>.log

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

STRESS="$ROOT_DIR/scripts/nvme-stress.sh"
LOG_DIR="build/nvme/phase7"
STATE_DIR="$LOG_DIR/state"
RUN_ID="$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOG_DIR" "$STATE_DIR"

DRY=0
QUICK=0
LIST=0
RESUME=1
LIMIT=""
ONLY=""
HOURS="4"
SOAK_SECONDS=""
BOOTS_COUNT="50"
AHCI_COUNT="5"
CUTS="100"
TARGET_GIB="1024"
GIB_PER_BOOT="64"
DEVICE="nvme"
CHAOS_CONFIG="mixed"
SOAK_SMP="1"
SOAK_ACCEL="tcg"
TIMEOUT=""
AHCI_BENCH_SECONDS="10"

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

SUITE=""

for arg in "$@"; do
    case "$arg" in
    --dry-run) DRY=1 ;;
    --quick) QUICK=1 ;;
    --list) LIST=1 ;;
    --no-resume) RESUME=0 ;;
    --limit=*) LIMIT="${arg#--limit=}" ;;
    --only=*) ONLY="${arg#--only=}" ;;
    --hours=*) HOURS="${arg#--hours=}" ;;
    --soak-seconds=*) SOAK_SECONDS="${arg#--soak-seconds=}" ;;
    --count=*) BOOTS_COUNT="${arg#--count=}" ; AHCI_COUNT="${arg#--count=}" ;;
    --cuts=*) CUTS="${arg#--cuts=}" ;;
    --target-gib=*) TARGET_GIB="${arg#--target-gib=}" ;;
    --gib-per-boot=*) GIB_PER_BOOT="${arg#--gib-per-boot=}" ;;
    --device=*) DEVICE="${arg#--device=}" ;;
    --chaos-config=*) CHAOS_CONFIG="${arg#--chaos-config=}" ;;
    --smp=*) SOAK_SMP="${arg#--smp=}" ;;
    --accel=*) SOAK_ACCEL="${arg#--accel=}" ;;
    --timeout=*) TIMEOUT="${arg#--timeout=}" ;;
    -h | --help) usage ;;
    -*) 
        echo "nvme-phase7.sh: unknown argument '$arg'" >&2
        exit 2
        ;;
    *)
        if [ -n "$SUITE" ]; then
            echo "nvme-phase7.sh: unexpected extra argument '$arg'" >&2
            exit 2
        fi
        SUITE="$arg"
        ;;
    esac
done
SUITE="${SUITE:-all}"

case "$SUITE" in
ahci | matrix | boots | tib | chaos | soak | all) ;;
*)
    echo "nvme-phase7.sh: unknown suite '$SUITE'" >&2
    usage
    ;;
esac

case "$CHAOS_CONFIG" in
nvme-root | ide-scratch | mixed) ;;
*)
    echo "nvme-phase7.sh: unknown --chaos-config '$CHAOS_CONFIG'" >&2
    exit 2
    ;;
esac

case "$DEVICE" in
nvme | ahci) ;;
*)
    echo "nvme-phase7.sh: unknown --device '$DEVICE' (nvme|ahci)" >&2
    exit 2
    ;;
esac

for numeric in "$LIMIT" "$BOOTS_COUNT" "$AHCI_COUNT" "$CUTS" "$TARGET_GIB" \
    "$GIB_PER_BOOT" "$SOAK_SECONDS" "$TIMEOUT"; do
    if [ -n "$numeric" ] && ! [[ "$numeric" =~ ^[0-9]+$ ]]; then
        echo "nvme-phase7.sh: numeric option got '$numeric'" >&2
        exit 2
    fi
done

# Reduced counts for smoke runs, but only where the user kept the defaults.
if [ "$QUICK" = 1 ]; then
    [ "$BOOTS_COUNT" = 50 ] && BOOTS_COUNT=6
    [ "$AHCI_COUNT" = 5 ] && AHCI_COUNT=2
    [ "$CUTS" = 100 ] && CUTS=6
    [ "$TARGET_GIB" = 1024 ] && TARGET_GIB=2
    [ "$GIB_PER_BOOT" = 64 ] && GIB_PER_BOOT=1
    [ -z "$LIMIT" ] && LIMIT=9
    [ "$HOURS" = 4 ] && [ -z "$SOAK_SECONDS" ] && SOAK_SECONDS=120
fi

say() { printf '[*] %s\n' "$*"; }

# Run one nvme-stress.sh boot, tee the serial log and return its status.
run_stress() {
    local tag="$1"
    shift
    local log="$LOG_DIR/$RUN_ID-$tag.log"
    say "$tag: $STRESS $*"
    say "log: $log"
    if [ "$DRY" = 1 ]; then
        return 0
    fi
    local rc=0
    "$STRESS" "$@" 2>&1 | tee "$log" || rc=$?
    printf '%s\n' "$log" >"$STATE_DIR/last-log"
    return "$rc"
}

# --- AHCI/ATA regression ---------------------------------------------------

ahci_suite() {
    say "AHCI/ATA regression: comparison boot + $AHCI_COUNT host-verified boots"
    run_stress ahci-bench --config=ide-scratch --selftest --cycles=1 \
        --bench --bench-compare --bench-qd=4 \
        --bench-seconds="$AHCI_BENCH_SECONDS" || return 1
    local i
    for ((i = 1; i <= AHCI_COUNT; i++)); do
        run_stress "ahci-$i" --config=ide-scratch --selftest --cycles=3 ||
            return 1
    done
    if [ "$DRY" = 1 ]; then
        say "RESULT: DRY-RUN (AHCI regression)"
    else
        say "RESULT: PASS (AHCI regression x$AHCI_COUNT)"
    fi
}

# --- 72-cell acceptance matrix ---------------------------------------------

matrix_cells() {
    local lba topo fw accel smp
    for lba in 512 4kn; do
        for topo in single multi-ns multi-ctrl; do
            for fw in baseline constrained; do
                for accel in kvm tcg; do
                    for smp in 1 2 4; do
                        printf '%s %s %s %s %s\n' "$lba" "$topo" "$fw" \
                            "$accel" "$smp"
                    done
                done
            done
        done
    done
}

matrix_args() {
    local lba="$1" topo="$2" fw="$3" accel="$4" smp="$5"
    local -a args
    if [ "$lba" = 4kn ]; then
        # Keep the root on AHCI so the destructive 4Kn/multi-NS checks only
        # touch the scratch namespaces; --pair adds the 4Kn/512e twin.
        args=(--config=ide-scratch --selftest --cycles=3 --identify-ops=10000
            --4kn --pair --data-selftest --allow-data-selftest
            --accel="$accel" --smp="$smp")
    else
        args=(--config=nvme-root --selftest --cycles=3 --identify-ops=10000
            --accel="$accel" --smp="$smp")
    fi
    case "$topo" in
    multi-ns) args+=(--multi-ns) ;;
    multi-ctrl) args+=(--controllers=2 --no-namespace) ;;
    esac
    case "$fw" in
    constrained) args+=(--mdts=3 --max-ioqpairs=2 --msix-qsize=1) ;;
    esac
    [ -n "$TIMEOUT" ] && args+=(--timeout="$TIMEOUT")
    printf '%s\n' "${args[@]}"
}

matrix_selected() {
    [ -z "$ONLY" ] && return 0
    case "$1" in
    *"$ONLY"*) return 0 ;;
    esac
    return 1
}

matrix_suite() {
    local done_file="$STATE_DIR/matrix.done"
    if [ "$RESUME" = 0 ]; then
        rm -f "$done_file"
    fi
    touch "$done_file"

    local total=0 selected=0 skipped=0 attempted=0 failed=0
    local lba topo fw accel smp key label
    local -a args
    total=$(matrix_cells | wc -l)
    while read -r lba topo fw accel smp; do
        key="$lba/$topo/$fw/$accel/$smp"
        label="lba=$lba topo=$topo fw=$fw accel=$accel smp=$smp"
        if ! matrix_selected "$label"; then
            continue
        fi
        if [ "$LIST" = 1 ]; then
            selected=$((selected + 1))
            printf '%-4s topo=%-10s fw=%-11s accel=%-3s smp=%s\n' \
                "$lba" "$topo" "$fw" "$accel" "$smp"
            continue
        fi
        if [ -n "$LIMIT" ] && [ "$attempted" -ge "$LIMIT" ]; then
            break
        fi
        selected=$((selected + 1))
        if [ "$RESUME" = 1 ] && grep -qxF "$key" "$done_file"; then
            skipped=$((skipped + 1))
            continue
        fi
        if [ "$accel" = kvm ] && { [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; }; then
            say "matrix $key: SKIP (no /dev/kvm)"
            attempted=$((attempted + 1))
            continue
        fi

        mapfile -t args < <(matrix_args "$lba" "$topo" "$fw" "$accel" "$smp")
        if ! run_stress "matrix-$lba-$topo-$fw-$accel-smp$smp" "${args[@]}"; then
            failed=$((failed + 1))
            say "matrix $key: FAIL (stopping for inspection; rerun resumes)"
            break
        fi
        if [ "$DRY" = 0 ]; then
            printf '%s\n' "$key" >>"$done_file"
        fi
        attempted=$((attempted + 1))
    done < <(matrix_cells)

    if [ "$LIST" = 1 ]; then
        say "matrix: $total cells total, $selected selected"
        return 0
    fi
    say "matrix: attempted=$attempted skipped=$skipped failed=$failed " \
        "(of $selected selected, $total total)"
    [ "$failed" = 0 ] || return 1
    if [ "$DRY" = 1 ]; then
        say "RESULT: DRY-RUN (matrix)"
    else
        say "RESULT: PASS (matrix)"
    fi
}

# --- 50 boot-loop configurations -------------------------------------------

boots_suite() {
    local done_file="$STATE_DIR/boots.done"
    local start=1
    if [ "$RESUME" = 1 ] && [ -s "$done_file" ]; then
        start=$(( $(cat "$done_file") + 1 ))
    else
        rm -f "$done_file"
    fi

    local -a configs=(
        "--config=nvme-root --selftest --cycles=3"
        "--config=ide-scratch --selftest --cycles=3 --data-selftest --4kn --pair"
        "--config=nvme-root --selftest --cycles=1 --irq-mode=poll"
        "--config=ide-scratch --selftest --cycles=1 --drop-irq --deep"
        "--config=nvme-root --selftest --cycles=1 --mdts=3 --max-ioqpairs=2 --msix-qsize=1"
    )
    say "Boot loop: $BOOTS_COUNT configurations (starting at $start)"
    local i idx
    local -a args
    for ((i = start; i <= BOOTS_COUNT; i++)); do
        idx=$(((i - 1) % ${#configs[@]}))
        args=()
        read -r -a args <<<"${configs[$idx]}"
        if ! run_stress "boots-$i" "${args[@]}"; then
            say "boots $i: FAIL (rerun resumes at $i)"
            return 1
        fi
        if [ "$DRY" = 0 ]; then
            printf '%s\n' "$i" >"$done_file"
        fi
    done
    if [ "$DRY" = 1 ]; then
        say "RESULT: DRY-RUN (boot loop)"
    else
        say "RESULT: PASS (boot loop x$BOOTS_COUNT)"
    fi
}

# --- 1 TiB cumulative verified writes --------------------------------------

tib_suite() {
    local ledger="$STATE_DIR/tib-bytes"
    local done_bytes=0
    if [ "$RESUME" = 1 ] && [ -f "$ledger" ]; then
        done_bytes=$(cat "$ledger")
    else
        printf '0\n' >"$ledger"
    fi
    local target_bytes
    target_bytes=$(awk -v g="$TARGET_GIB" 'BEGIN { printf "%.0f", g * 1073741824 }')
    say "Verified-write ledger: $(awk -v b="$done_bytes" \
        'BEGIN { printf "%.2f", b / 1073741824 }') GiB of $TARGET_GIB GiB"

    while awk -v d="$done_bytes" -v t="$target_bytes" \
        'BEGIN { exit !(d < t) }'; do
        local -a args=(--config=ide-scratch)
        case "$DEVICE" in
        nvme) args+=(--devices=/dev/nvme0n1 --selftest --cycles=1) ;;
        ahci) args+=(--devices=/dev/sata02 --selftest --cycles=1) ;;
        esac
        args+=(--verified-gib="$GIB_PER_BOOT")
        [ -n "$TIMEOUT" ] && args+=(--timeout="$TIMEOUT")

        if ! run_stress "tib-$((done_bytes / 1073741824))gib" "${args[@]}"; then
            say "tib: boot failed; ledger kept at $done_bytes bytes"
            return 1
        fi
        if [ "$DRY" = 1 ]; then
            say "RESULT: DRY-RUN (tib)"
            return 0
        fi

        local log got
        log=$(cat "$STATE_DIR/last-log")
        got=$(sed -n 's/.*NVME-VERIFIED: PASS .* total=\([0-9]*\).*/\1/p' \
            "$log" | tail -1)
        if [ -z "$got" ]; then
            say "tib: no NVME-VERIFIED total marker in $log"
            return 1
        fi
        done_bytes=$((done_bytes + got))
        printf '%s\n' "$done_bytes" >"$ledger"
        say "verified ledger: $(awk -v b="$done_bytes" \
            'BEGIN { printf "%.2f", b / 1073741824 }') GiB"
    done
    say "RESULT: PASS (verified writes, $TARGET_GIB GiB)"
}

# --- 100 power-cut/fsck chaos cycles ---------------------------------------

chaos_suite() {
    local done_file="$STATE_DIR/chaos.done"
    local start=1
    if [ "$RESUME" = 1 ] && [ -s "$done_file" ]; then
        start=$(( $(cat "$done_file") + 1 ))
    else
        rm -f "$done_file"
    fi
    say "Chaos: $CUTS power cuts (config=$CHAOS_CONFIG, starting at $start)"
    local b cfg
    for ((b = start; b <= CUTS; b++)); do
        cfg="nvme-root"
        case "$CHAOS_CONFIG" in
        ide-scratch) cfg="ide-scratch" ;;
        mixed) [ $((b % 2)) -eq 0 ] && cfg="ide-scratch" ;;
        esac
        if ! run_stress "chaos-$b" --config="$cfg" --crash-run \
            --crash-boot="$b" --timeout="${TIMEOUT:-300}"; then
            say "chaos $b: FAIL (rerun resumes at $b)"
            return 1
        fi
        if [ "$DRY" = 0 ]; then
            printf '%s\n' "$b" >"$done_file"
        fi
    done
    if [ "$DRY" = 1 ]; then
        say "RESULT: DRY-RUN (chaos)"
    else
        say "RESULT: PASS (chaos x$CUTS, host e2fsck after each cut)"
    fi
}

# --- worst-case soak --------------------------------------------------------

soak_suite() {
    local seconds="$SOAK_SECONDS"
    if [ -z "$seconds" ]; then
        seconds=$(awk -v h="$HOURS" 'BEGIN { printf "%d", h * 3600 }')
    fi
    if [ "$seconds" -le 0 ]; then
        say "soak: duration is zero, nothing to do"
        return 0
    fi
    # Worst case from the Phase 7 checklist: 1 vCPU TCG, a 4Kn namespace and
    # its 512e twin, QD 64 random writes on the 4Kn device, lost-interrupt
    # build (NVMe_FAULT_DROP_IRQ).  With --4kn the first added namespace is
    # /dev/nvme0n2 (ns4k) and /dev/nvme0n3 is the 512e pair.
    local -a args=(--config=ide-scratch --selftest --cycles=1
        --4kn --pair --drop-irq
        --accel="$SOAK_ACCEL" --smp="$SOAK_SMP"
        --bench --bench-mode=randwrite --bench-block=4096 --bench-qd=64
        --bench-seconds="$seconds" --devices=/dev/nvme0n2 --no-fsck)
    if [ -n "$TIMEOUT" ]; then
        args+=(--timeout="$TIMEOUT")
    else
        args+=(--timeout=$((seconds + 1800)))
    fi
    say "Worst-case soak: ${seconds}s ($(awk -v s="$seconds" \
        'BEGIN { printf "%.2f", s / 3600 }') h), accel=$SOAK_ACCEL smp=$SOAK_SMP"
    run_stress soak "${args[@]}" || return 1
    if [ "$DRY" = 1 ]; then
        say "RESULT: DRY-RUN (soak)"
    else
        say "RESULT: PASS (soak ${seconds}s)"
    fi
}

# --- dispatch ---------------------------------------------------------------

# --list is a matrix inspection mode: print the cells and exit.
if [ "$LIST" = 1 ]; then
    matrix_suite
    exit 0
fi

status=0
case "$SUITE" in
ahci) ahci_suite || status=1 ;;
matrix) matrix_suite || status=1 ;;
boots) boots_suite || status=1 ;;
tib) tib_suite || status=1 ;;
chaos) chaos_suite || status=1 ;;
soak) soak_suite || status=1 ;;
all)
    ahci_suite || status=1
    if [ "$status" = 0 ]; then matrix_suite || status=1; fi
    if [ "$status" = 0 ]; then boots_suite || status=1; fi
    if [ "$status" = 0 ]; then tib_suite || status=1; fi
    if [ "$status" = 0 ]; then chaos_suite || status=1; fi
    if [ "$status" = 0 ]; then soak_suite || status=1; fi
    ;;
esac

if [ "$status" != 0 ]; then
    say "RESULT: FAIL (suite $SUITE)"
    exit 1
fi
say "RESULT: PASS (suite $SUITE)"
