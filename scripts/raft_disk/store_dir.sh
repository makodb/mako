# Run directories for Raft disk-mode stores (docs/verus/disk-persistence-plan.md
# §1, "Run directories"). Source it, then:
#
#   raft_store_make_run <label>    sweep, make and lock a fresh run directory,
#                                  export MAKO_RAFT_DATA_DIR to it
#   raft_store_cleanup             delete this launcher's run directory
#   raft_store_sweep               delete run directories no live process holds
#
# Stores live on the local disk, /var/tmp/raft-wal-$USER (MAKO_RAFT_DATA_ROOT
# moves it), never on tmpfs (RAM the tests need) or the NFS home; the server
# refuses a non-local filesystem itself. Each run directory holds RUN.lock,
# flock()ed on an inherited descriptor by the launcher and the servers it
# starts. The kernel drops the lock when the last of them dies, so a run whose
# launcher was SIGKILLed or OOM-killed is deleted by the next sweep instead of
# leaking. Only directories named run-* are swept.

raft_store_root() {
    echo "${MAKO_RAFT_DATA_ROOT:-/var/tmp/raft-wal-${USER:-$(id -un)}}"
}

# Deletes every run-* directory whose RUN.lock nobody holds. A directory with
# no RUN.lock yet is skipped for its first minute (a launcher between mktemp
# and flock).
raft_store_sweep() {
    local root d lock
    root=$(raft_store_root)
    [ -d "$root" ] || return 0
    for d in "$root"/run-*; do
        [ -d "$d" ] || continue
        lock="$d/RUN.lock"
        if [ -e "$lock" ]; then
            if flock -n "$lock" true 2>/dev/null; then
                rm -rf -- "$d" && echo "raft store sweep: deleted $d" >&2
            fi
        elif [ -n "$(find "$d" -maxdepth 0 -mmin +1 2>/dev/null)" ]; then
            rm -rf -- "$d" && echo "raft store sweep: deleted $d (never locked)" >&2
        fi
    done
}

# raft_store_make_run <label> [min_free_gb]
raft_store_make_run() {
    local label=${1:-run} min_gb=${2:-8} root avail
    root=$(raft_store_root)
    mkdir -p "$root" || return 1
    raft_store_sweep
    avail=$(df --output=avail -B1G "$root" | tail -1 | tr -d ' ')
    if [ "${avail:-0}" -lt "$min_gb" ]; then
        echo "raft store: only ${avail} GB free under $root (need ${min_gb})" >&2
        return 1
    fi
    RAFT_RUN_DIR=$(mktemp -d "$root/run-$label.XXXXXX") || return 1
    exec {RAFT_RUN_LOCK_FD}>"$RAFT_RUN_DIR/RUN.lock"
    if ! flock -n "$RAFT_RUN_LOCK_FD"; then
        echo "raft store: cannot lock $RAFT_RUN_DIR/RUN.lock" >&2
        return 1
    fi
    export RAFT_RUN_DIR RAFT_RUN_LOCK_FD
    export MAKO_RAFT_DATA_DIR="$RAFT_RUN_DIR"
}

raft_store_cleanup() {
    if [ -n "${RAFT_RUN_LOCK_FD:-}" ]; then
        exec {RAFT_RUN_LOCK_FD}>&-
        unset RAFT_RUN_LOCK_FD
    fi
    if [ -n "${RAFT_RUN_DIR:-}" ] && [ -d "$RAFT_RUN_DIR" ]; then
        rm -rf -- "$RAFT_RUN_DIR"
    fi
    unset RAFT_RUN_DIR
}
