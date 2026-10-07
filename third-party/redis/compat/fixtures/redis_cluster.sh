#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="${REDIS_CLUSTER_DIR:-/tmp/redis-compat-cluster}"
HOST="${REDIS_CLUSTER_HOST:-127.0.0.1}"
PORTS="${REDIS_CLUSTER_PORTS:-7000 7001 7002 7003 7004 7005}"

require_redis_tools() {
    if ! command -v redis-server >/dev/null 2>&1 || ! command -v redis-cli >/dev/null 2>&1; then
        echo "redis-server and redis-cli are required"
        exit 78
    fi
}

start_cluster() {
    require_redis_tools
    mkdir -p "${BASE_DIR}"
    for port in ${PORTS}; do
        local node_dir="${BASE_DIR}/${port}"
        mkdir -p "${node_dir}"
        cat >"${node_dir}/redis.conf" <<EOF
port ${port}
bind ${HOST}
cluster-enabled yes
cluster-config-file nodes.conf
cluster-node-timeout 5000
appendonly no
save ""
dir ${node_dir}
protected-mode no
logfile ${node_dir}/redis.log
pidfile ${node_dir}/redis.pid
daemonize yes
EOF
        redis-server "${node_dir}/redis.conf"
    done

    local nodes=()
    for port in ${PORTS}; do
        nodes+=("${HOST}:${port}")
    done
    printf 'yes\n' | redis-cli --cluster create "${nodes[@]}" --cluster-replicas 1 >/dev/null
    echo "Redis Cluster started ports=${PORTS}"
}

stop_cluster() {
    require_redis_tools
    for port in ${PORTS}; do
        redis-cli -h "${HOST}" -p "${port}" SHUTDOWN NOSAVE >/dev/null 2>&1 || true
    done
    echo "Redis Cluster stopped"
}

status_cluster() {
    require_redis_tools
    for port in ${PORTS}; do
        if redis-cli -h "${HOST}" -p "${port}" PING >/dev/null 2>&1; then
            echo "${HOST}:${port} up"
        else
            echo "${HOST}:${port} down"
        fi
    done
}

case "${1:-}" in
    start)
        start_cluster
        ;;
    stop)
        stop_cluster
        ;;
    status)
        status_cluster
        ;;
    *)
        echo "usage: $0 start|stop|status"
        exit 2
        ;;
esac
