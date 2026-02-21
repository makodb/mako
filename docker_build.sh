#!/bin/bash

# Script to build and test Mako in Ubuntu 24.04 container

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
cd "$SCRIPT_DIR"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Docker image and container names
IMAGE_NAME="mako-build:ubuntu24"
CONTAINER_NAME_DEFAULT="mako-dev"
CONTAINER_NAME="${MAKO_DEV_CONTAINER_NAME:-${CONTAINER_NAME_DEFAULT}}"
WORKSPACE_ROOT="$(pwd -P)"
WORKSPACE_HASH="$(printf '%s' "${WORKSPACE_ROOT}" | sha256sum | cut -c1-10)"
COMPOSE_PROJECT_NAME="mako-${WORKSPACE_HASH}"
COMPOSE_CMD_PREFIX="MAKO_COMPOSE_PROJECT=${COMPOSE_PROJECT_NAME} docker compose"

# Environment variables/options for Docker runs
DOCKER_ENV_OPTS=(-e CARGO_TARGET_DIR=/workspace/target-docker -e BUILD_DIR=build_docker)
DOCKER_INIT_OPTS=(--init)
DOCKER_SECURITY_OPTS=()
if docker info --format '{{json .SecurityOptions}}' 2>/dev/null | grep -q "name=apparmor"; then
    # Rust tooling (cargo/rustc) can fail under restrictive AppArmor profiles.
    DOCKER_SECURITY_OPTS=(--security-opt apparmor=unconfined)
fi
HAS_TTY=1
DOCKER_INTERACTIVE_OPTS=(-it)
COMPOSE_EXEC_OPTS=()
if [ ! -t 0 ] || [ ! -t 1 ]; then
    # Avoid hard failures like "the input device is not a TTY" in non-interactive environments.
    HAS_TTY=0
    DOCKER_INTERACTIVE_OPTS=(-i)
    COMPOSE_EXEC_OPTS=(-T)
fi

# Disable core dumps in script-driven Docker runs by default to avoid polluting
# the workspace with large core.* artifacts after transient test crashes.
DOCKER_CORE_ULIMIT_CMD="ulimit -c 0"
if [ "${MAKO_DOCKER_ENABLE_COREDUMP:-0}" = "1" ]; then
    DOCKER_CORE_ULIMIT_CMD="ulimit -c unlimited"
fi

compose_cmd() {
    MAKO_COMPOSE_PROJECT="${COMPOSE_PROJECT_NAME}" docker compose "$@"
}

ensure_image() {
    if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
        echo -e "${YELLOW}Image '${IMAGE_NAME}' not found locally; building it first...${NC}"
        docker build -f Dockerfile.ubuntu24 -t ${IMAGE_NAME} .
    fi
}

warn_incomplete_build_docker() {
    local required_bins=(
        "build_docker/dbtest"
        "build_docker/simpleTransaction"
        "build_docker/simplePaxos"
        "build_docker/simpleTransactionRep"
    )
    local missing_bins=()
    local required_bin
    for required_bin in "${required_bins[@]}"; do
        if [ ! -x "${required_bin}" ]; then
            missing_bins+=("${required_bin}")
        fi
    done
    if [ "${#missing_bins[@]}" -gt 0 ]; then
        echo -e "${YELLOW}Warning: Docker build artifacts are missing or incomplete in build_docker.${NC}"
        for required_bin in "${missing_bins[@]}"; do
            echo -e "${YELLOW}  - ${required_bin}${NC}"
        done
        echo -e "${YELLOW}Run '$0 build' before running './ci/ci.sh ...' inside dev containers.${NC}"
        echo -e "${YELLOW}Or run '$0 ci <test>' to build and execute a CI suite in one command.${NC}"
    fi
}

is_container_running() {
    local container_name="$1"
    docker ps --format '{{.Names}}' | grep -q "^${container_name}$"
}

is_container_stably_running() {
    local container_name="$1"
    local checks="${2:-3}"
    local interval_seconds="${3:-1}"
    local i=1

    while [ "${i}" -le "${checks}" ]; do
        if ! is_container_running "${container_name}"; then
            return 1
        fi
        if [ "${i}" -lt "${checks}" ]; then
            sleep "${interval_seconds}"
        fi
        i=$((i + 1))
    done
    return 0
}

has_keepalive_command() {
    local container_name="$1"
    local container_cmd

    container_cmd=$(docker inspect -f '{{join .Config.Cmd " "}}' "${container_name}" 2>/dev/null || echo "")
    [ "${container_cmd}" = "bash -lc exec tail -f /dev/null" ] || \
        [ "${container_cmd}" = "/bin/bash -lc exec tail -f /dev/null" ]
}

has_init_enabled() {
    local container_name="$1"
    local existing_init

    existing_init=$(docker inspect -f '{{if .HostConfig.Init}}true{{else}}false{{end}}' "${container_name}" 2>/dev/null || echo false)
    [ "${existing_init}" = "true" ]
}

has_expected_image() {
    local container_name="$1"
    local expected_image_id
    local container_image_id

    expected_image_id=$(docker image inspect -f '{{.Id}}' "${IMAGE_NAME}" 2>/dev/null || echo "")
    container_image_id=$(docker inspect -f '{{.Image}}' "${container_name}" 2>/dev/null || echo "")

    [ -n "${expected_image_id}" ] && [ "${container_image_id}" = "${expected_image_id}" ]
}

has_expected_workspace_mount() {
    local container_name="$1"
    local mount_source
    local mount_type
    local working_dir

    mount_source=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace"}}{{.Source}}{{end}}{{end}}' "${container_name}" 2>/dev/null || echo "")
    mount_type=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace"}}{{.Type}}{{end}}{{end}}' "${container_name}" 2>/dev/null || echo "")
    working_dir=$(docker inspect -f '{{.Config.WorkingDir}}' "${container_name}" 2>/dev/null || echo "")

    [ "${mount_type}" = "bind" ] && [ "${mount_source}" = "${WORKSPACE_ROOT}" ] && [ "${working_dir}" = "/workspace" ]
}

resolve_container_name_for_workspace() {
    local scoped_container_name

    # Respect explicit caller override.
    if [ -n "${MAKO_DEV_CONTAINER_NAME:-}" ]; then
        return
    fi

    # Only auto-scope legacy default name.
    if [ "${CONTAINER_NAME}" != "${CONTAINER_NAME_DEFAULT}" ]; then
        return
    fi

    scoped_container_name="${CONTAINER_NAME_DEFAULT}-${WORKSPACE_HASH}"

    # If legacy name is already tied to a different checkout, avoid clobbering it.
    if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME_DEFAULT}$"; then
        if ! has_expected_workspace_mount "${CONTAINER_NAME_DEFAULT}"; then
            CONTAINER_NAME="${scoped_container_name}"
        fi
    fi
}

resolve_container_name_for_workspace

echo -e "${GREEN}=== Mako Ubuntu 24.04 Docker Build Script ===${NC}"
echo

# Parse command line arguments
ACTION=${1:-build}
JOBS=${2:-32}

case "$ACTION" in
    build-image)
        echo -e "${YELLOW}Building Docker image...${NC}"
        docker build -f Dockerfile.ubuntu24 -t ${IMAGE_NAME} .
        echo -e "${GREEN}Docker image built successfully!${NC}"
        ;;

    build)
        echo -e "${YELLOW}Building Mako in container...${NC}"
        ensure_image
        docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" ${IMAGE_NAME} \
            bash -c "${DOCKER_CORE_ULIMIT_CMD}; cd /workspace && \
                     if [ -f build_docker/CMakeCache.txt ] && \
                        ! grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=/workspace$' build_docker/CMakeCache.txt; then \
                         echo 'Cleaning incompatible build_docker cache'; \
                         rm -rf build_docker; \
                     fi && \
                     if [ ! -f build_docker/CMakeCache.txt ]; then \
                         echo 'Configuring build_docker'; \
                         cmake -S . -B build_docker; \
                     else \
                         echo 'Reusing existing build_docker CMake cache'; \
                     fi && \
                     cmake --build build_docker --parallel ${JOBS} --target \
                         dbtest simpleTransaction simplePaxos simpleTransactionRep \
                         test_rocksdb_persistence test_callback_demo test_ordered_callbacks \
                         test_partitioned_queues test_stress_partitioned_queues"
        echo -e "${GREEN}Build completed successfully!${NC}"
        ;;

    shell)
        echo -e "${YELLOW}Starting interactive shell in container...${NC}"
        ensure_image
        warn_incomplete_build_docker
        if [ "${HAS_TTY}" -eq 1 ]; then
            docker run --rm "${DOCKER_INTERACTIVE_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
        else
            echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
            if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                if is_container_running "${CONTAINER_NAME}"; then
                    # Only advertise direct docker exec when standalone stays up
                    # for a short window; legacy containers can flap briefly.
                    if is_container_stably_running "${CONTAINER_NAME}" 3 1; then
                        if ! has_expected_image "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but uses an unexpected image.${NC}"
                            echo -e "${GREEN}Use '$0 create' or '$0 enter' to recreate it with '${IMAGE_NAME}'.${NC}"
                            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                                echo -e "${GREEN}Compose service 'dev' is also running.${NC}"
                                echo -e "${GREEN}Compose access: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                                echo -e "${GREEN}Compose non-interactive: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                            fi
                        elif ! has_expected_workspace_mount "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but points to a different /workspace mount or working directory.${NC}"
                            echo -e "${GREEN}Use '$0 create' or '$0 enter' to recreate/normalize it for this checkout.${NC}"
                            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                                echo -e "${GREEN}Compose service 'dev' is also running.${NC}"
                                echo -e "${GREEN}Compose access: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                                echo -e "${GREEN}Compose non-interactive: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                            fi
                        elif ! has_init_enabled "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but was created without Docker init support.${NC}"
                            echo -e "${GREEN}Use '$0 create' or '$0 enter' to recreate/normalize it with '--init'.${NC}"
                            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                                echo -e "${GREEN}Compose service 'dev' is also running.${NC}"
                                echo -e "${GREEN}Compose access: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                                echo -e "${GREEN}Compose non-interactive: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                            fi
                        elif ! has_keepalive_command "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but uses a non-keepalive command.${NC}"
                            echo -e "${GREEN}It may exit unexpectedly; use '$0 create' or '$0 enter' to normalize it for persistent dev usage.${NC}"
                            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                                echo -e "${GREEN}Compose service 'dev' is also running.${NC}"
                                echo -e "${GREEN}Compose access: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                                echo -e "${GREEN}Compose non-interactive: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                            fi
                        else
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is already running.${NC}"
                            echo -e "${GREEN}Use '$0 enter' from a TTY, or run: docker exec -it -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                            echo -e "${GREEN}For non-interactive usage, run: docker exec -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
                            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                                echo -e "${GREEN}Compose service 'dev' is also running.${NC}"
                                echo -e "${GREEN}Compose access: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                                echo -e "${GREEN}Compose non-interactive: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                            fi
                        fi
                    else
                        echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' was running briefly but exited.${NC}"
                        if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                            echo -e "${GREEN}Compose service 'dev' is running; use: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                            echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                            echo -e "${GREEN}If you need standalone '${CONTAINER_NAME}', use '$0 enter' or '$0 create' for recovery-safe setup.${NC}"
                        else
                            echo -e "${GREEN}Use '$0 enter' to start/recover standalone '${CONTAINER_NAME}' and attach when possible.${NC}"
                            echo -e "${GREEN}Or run '$0 create' to explicitly recreate/refresh standalone before entering it.${NC}"
                        fi
                    fi
                else
                    echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' exists but is stopped.${NC}"
                    if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                        echo -e "${GREEN}Compose service 'dev' is already running.${NC}"
                        echo -e "${GREEN}From a TTY, run: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                        echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                        echo -e "${GREEN}If you need standalone '${CONTAINER_NAME}', use '$0 enter' (auto-recovers legacy containers that exit immediately after start).${NC}"
                        echo -e "${GREEN}Or run '$0 create' to explicitly recreate/refresh standalone before entering it.${NC}"
                    else
                        echo -e "${GREEN}Use '$0 enter' to start/recover standalone '${CONTAINER_NAME}' and attach when possible.${NC}"
                        echo -e "${GREEN}Or run '$0 create' to explicitly recreate/refresh standalone before entering it.${NC}"
                        echo -e "${GREEN}If you use raw Docker commands, verify '${CONTAINER_NAME}' stays running before docker exec.${NC}"
                    fi
                fi
            else
                echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' was not found.${NC}"
                if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                    echo -e "${GREEN}Compose service 'dev' is already running.${NC}"
                    echo -e "${GREEN}From a TTY, run: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                    echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                else
                    echo -e "${GREEN}Use '$0 compose-up' to start compose service 'dev'.${NC}"
                    echo -e "${GREEN}From a TTY, run: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                    echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                fi
            fi
        fi
        ;;

    test)
        echo -e "${YELLOW}Running Docker smoke test (build + dbtest runtime)...${NC}"
        ensure_image
        docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
            bash -c "${DOCKER_CORE_ULIMIT_CMD}; NEED_BUILD=1; \
                     if [ -x build_docker/dbtest ]; then \
                         RUNPATH=\$(readelf -d build_docker/dbtest 2>/dev/null | awk '/RUNPATH/ {print \$5}' | tr -d '[]'); \
                         if [[ \":\$RUNPATH:\" == *\":/workspace/build_docker:\"* ]]; then \
                             NEED_BUILD=0; \
                             echo 'Reusing existing Docker-compatible build_docker/dbtest'; \
                         else \
                             echo \"Rebuilding dbtest (incompatible RUNPATH: \${RUNPATH:-<none>})\"; \
                         fi; \
                     fi; \
                     if [ \"\$NEED_BUILD\" -eq 1 ]; then \
                         rm -rf build_docker && \
                         cmake -S . -B build_docker && \
                         cmake --build build_docker --parallel ${JOBS} --target dbtest && \
                         echo 'SUCCESS: dbtest build completed'; \
                     fi && \
                     BUILD_DIR=build_docker ./ci/ci.sh shardNoReplication && \
                     ls -la build_docker/dbtest"
        echo -e "${GREEN}Test completed successfully!${NC}"
        ;;

    ci)
        # Run a specific CI test or all tests
        CI_TEST=${2:-all}
        CI_JOBS=${3:-32}
        if [[ "${2:-}" =~ ^[0-9]+$ ]]; then
            CI_TEST=all
            CI_JOBS=${2}
            if [ -n "${3:-}" ]; then
                echo -e "${RED}Error: Too many arguments for jobs-only CI shorthand.${NC}"
                echo -e "${YELLOW}Use './docker_build.sh ci ${2}' or './docker_build.sh ci <test> <jobs>'.${NC}"
                exit 1
            fi
        fi
        if ! [[ "${CI_JOBS}" =~ ^[0-9]+$ ]] || [ "${CI_JOBS}" -lt 1 ]; then
            echo -e "${RED}Error: CI jobs must be a positive integer (got '${CI_JOBS}').${NC}"
            exit 1
        fi
        case "${CI_TEST}" in
            compile|cleanup|simpleTransaction|simplePaxos|shardNoReplication|shardNoReplicationErpc|shard1Replication|shard2Replication|shard2ReplicationErpc|shard1ReplicationSimple|shard2ReplicationSimple|shard1ReplicationRaft|shard2ReplicationRaft|shard1ReplicationSimpleRaft|shard2ReplicationSimpleRaft|rocksdbTests|multiShardSingleProcess|shard2SingleProcess|shard2SingleProcessReplication|rrrTests|cpuThrottlingScaling|clientServer|all)
                ;;
            *)
                echo -e "${RED}Error: Unknown CI test '${CI_TEST}'.${NC}"
                echo -e "${YELLOW}Run '$0' without args to see supported CI tests.${NC}"
                exit 1
                ;;
        esac
        echo -e "${YELLOW}Running CI test '${CI_TEST}' in container with ${CI_JOBS} build jobs...${NC}"
        ensure_image
        case "${CI_TEST}" in
            compile|all)
                # ci.sh compile/all already performs compilation; avoid redundant outer build.
                docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "${DOCKER_CORE_ULIMIT_CMD}; rm -rf build_docker && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
                ;;
            cleanup)
                # cleanup should not force an expensive build first.
                docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "${DOCKER_CORE_ULIMIT_CMD}; CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh cleanup"
                ;;
            *)
                docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "${DOCKER_CORE_ULIMIT_CMD}; rm -rf build_docker && CI_MAKE_JOBS=${CI_JOBS} make BUILD_DIR=build_docker -j${CI_JOBS} && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
                ;;
        esac
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;

    ci-quick)
        # Run CI tests without rebuild (assumes build exists and was built in Docker)
        CI_TEST=${2:-shardNoReplication}
        case "${CI_TEST}" in
            compile|cleanup|all|rrrTests)
                echo -e "${RED}Error: ci-quick does not support '${CI_TEST}'.${NC}"
                echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' instead.${NC}"
                exit 1
                ;;
            simpleTransaction|simplePaxos|shardNoReplication|shardNoReplicationErpc|shard1Replication|shard2Replication|shard2ReplicationErpc|shard1ReplicationSimple|shard2ReplicationSimple|shard1ReplicationRaft|shard2ReplicationRaft|shard1ReplicationSimpleRaft|shard2ReplicationSimpleRaft|rocksdbTests|multiShardSingleProcess|shard2SingleProcess|shard2SingleProcessReplication|cpuThrottlingScaling|clientServer)
                ;;
            *)
                echo -e "${RED}Error: Unknown CI test '${CI_TEST}'.${NC}"
                echo -e "${YELLOW}Run '$0' without args to see supported CI tests.${NC}"
                exit 1
                ;;
        esac
        REQUIRED_BINS=("build_docker/dbtest")
        case "${CI_TEST}" in
            simpleTransaction)
                REQUIRED_BINS=("build_docker/simpleTransaction")
                ;;
            simplePaxos)
                REQUIRED_BINS=("build_docker/simplePaxos")
                ;;
            rocksdbTests)
                REQUIRED_BINS=(
                    "build_docker/test_rocksdb_persistence"
                    "build_docker/test_callback_demo"
                    "build_docker/test_ordered_callbacks"
                    "build_docker/test_partitioned_queues"
                    "build_docker/test_stress_partitioned_queues"
                )
                ;;
            shard1ReplicationSimple|shard2ReplicationSimple|shard1ReplicationSimpleRaft|shard2ReplicationSimpleRaft|clientServer)
                REQUIRED_BINS=("build_docker/simpleTransactionRep")
                ;;
        esac

        missing_bins=()
        non_executable_bins=()
        incompatible_bins=()
        warning_runpaths=()
        for required_bin in "${REQUIRED_BINS[@]}"; do
            if [ ! -f "${required_bin}" ]; then
                missing_bins+=("${required_bin}")
                continue
            fi
            if [ ! -x "${required_bin}" ]; then
                non_executable_bins+=("${required_bin}")
                continue
            fi
            RUNPATH=$(readelf -d "${required_bin}" 2>/dev/null | grep RUNPATH | grep -o '\[.*\]' | tr -d '[]')
            if [[ ":$RUNPATH:" != *":/workspace/build_docker:"* ]]; then
                incompatible_bins+=("${required_bin} (RUNPATH: ${RUNPATH:-<none>})")
                continue
            fi
            if [ "$RUNPATH" != "/workspace/build_docker" ]; then
                warning_runpaths+=("${required_bin}: ${RUNPATH}")
            fi
        done
        if [ "${#missing_bins[@]}" -gt 0 ]; then
            echo -e "${RED}Error: Required binaries are missing for CI test '${CI_TEST}':${NC}"
            for bin in "${missing_bins[@]}"; do
                echo -e "${RED}  - ${bin}${NC}"
            done
            echo -e "${YELLOW}'build' compiles core runtime binaries (dbtest, simpleTransaction, simplePaxos, simpleTransactionRep)${NC}"
            echo -e "${YELLOW}and RocksDB test binaries for ci-quick rocksdbTests.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' to build missing binaries and run this suite.${NC}"
            exit 1
        fi
        if [ "${#non_executable_bins[@]}" -gt 0 ]; then
            echo -e "${RED}Error: Required binaries are not executable for CI test '${CI_TEST}':${NC}"
            for bin in "${non_executable_bins[@]}"; do
                echo -e "${RED}  - ${bin}${NC}"
            done
            echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' to rebuild executable binaries in Docker.${NC}"
            exit 1
        fi
        if [ "${#incompatible_bins[@]}" -gt 0 ]; then
            echo -e "${RED}Error: Required binaries are not Docker-compatible for CI test '${CI_TEST}':${NC}"
            for bin in "${incompatible_bins[@]}"; do
                echo -e "${RED}  - ${bin}${NC}"
            done
            echo -e "${YELLOW}Cannot run locally-built binaries in Docker due to library path mismatch.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' to rebuild and test in Docker.${NC}"
            exit 1
        fi
        if [ "${#warning_runpaths[@]}" -gt 0 ]; then
            echo -e "${YELLOW}Warning: Some RUNPATH entries include extra paths; forcing Docker library path.${NC}"
            for entry in "${warning_runpaths[@]}"; do
                echo -e "${YELLOW}  - ${entry}${NC}"
            done
        fi

        echo -e "${YELLOW}Running CI test '${CI_TEST}' (no rebuild)...${NC}"
        ensure_image
        docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -e LD_LIBRARY_PATH=/workspace/build_docker -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
            bash -c "${DOCKER_CORE_ULIMIT_CMD}; BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;

    clean)
        echo -e "${YELLOW}Cleaning build artifacts...${NC}"
        if docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
            docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" ${IMAGE_NAME} \
                bash -c "cd /workspace && rm -rf build_docker target-docker && find . -maxdepth 1 -type f -name 'core.*' -delete"
        else
            echo -e "${YELLOW}Image '${IMAGE_NAME}' not found; cleaning workspace directly.${NC}"
            rm -rf build_docker target-docker 2>/dev/null || true
            find . -maxdepth 1 -type f -name 'core.*' -delete 2>/dev/null || true
            HAS_LEFTOVERS=0
            if [ -d build_docker ] || [ -d target-docker ]; then
                HAS_LEFTOVERS=1
            fi
            if find . -maxdepth 1 -type f -name 'core.*' -print -quit | grep -q .; then
                HAS_LEFTOVERS=1
            fi
            if [ "${HAS_LEFTOVERS}" -eq 1 ]; then
                echo -e "${YELLOW}Host cleanup lacked permissions; using temporary ubuntu helper container.${NC}"
                docker run --rm -v "${WORKSPACE_ROOT}:/workspace" ubuntu:24.04 \
                    bash -c "cd /workspace && rm -rf build_docker target-docker && find . -maxdepth 1 -type f -name 'core.*' -delete"
            fi
        fi
        echo -e "${GREEN}Clean completed!${NC}"
        ;;

    compose-up)
        echo -e "${YELLOW}Starting services with docker-compose...${NC}"
        ensure_image
        warn_incomplete_build_docker
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                echo -e "${YELLOW}Warning: standalone container '${CONTAINER_NAME}' is running alongside compose service 'dev'.${NC}"
                echo -e "${YELLOW}'$0 enter' will use standalone '${CONTAINER_NAME}' while it is running.${NC}"
            else
                echo -e "${YELLOW}Warning: standalone container '${CONTAINER_NAME}' exists alongside compose service 'dev'.${NC}"
                echo -e "${YELLOW}'$0 enter' will reuse compose service 'dev' while standalone '${CONTAINER_NAME}' remains stopped.${NC}"
            fi
            echo -e "${YELLOW}Use '${COMPOSE_CMD_PREFIX} exec dev /bin/bash' to enter compose service 'dev'.${NC}"
        fi
        compose_cmd up -d dev
        if [ "${HAS_TTY}" -eq 1 ]; then
            echo -e "${GREEN}Container started. Connect with: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
        else
            echo -e "${GREEN}Container started.${NC}"
            echo -e "${GREEN}Non-interactive session detected; run commands with: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
        fi
        ;;

    compose-down)
        echo -e "${YELLOW}Stopping services...${NC}"
        compose_cmd down
        echo -e "${GREEN}Services stopped!${NC}"
        ;;

    create)
        echo -e "${YELLOW}Creating persistent dev container...${NC}"
        ensure_image
        warn_incomplete_build_docker
        CREATE_RECREATED=0
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            EXISTING_INIT=$(docker inspect -f '{{if .HostConfig.Init}}true{{else}}false{{end}}' ${CONTAINER_NAME} 2>/dev/null || echo false)
            if [ "${EXISTING_INIT}" != "true" ]; then
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' was created without Docker init support; recreating it to enable child-process reaping.${NC}"
                docker rm -f ${CONTAINER_NAME} >/dev/null
                CREATE_RECREATED=1
            fi
        fi
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            if ! has_expected_image "${CONTAINER_NAME}"; then
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' uses image '$(docker inspect -f '{{.Config.Image}}' ${CONTAINER_NAME} 2>/dev/null || echo unknown)'; recreating it with '${IMAGE_NAME}'.${NC}"
                docker rm -f ${CONTAINER_NAME} >/dev/null
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                CREATE_RECREATED=1
            fi
            if ! has_keepalive_command "${CONTAINER_NAME}"; then
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' uses a non-keepalive command; recreating it for persistent dev usage.${NC}"
                docker rm -f ${CONTAINER_NAME} >/dev/null
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                CREATE_RECREATED=1
            fi
            if ! has_expected_workspace_mount "${CONTAINER_NAME}"; then
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' is bound to a different workspace or working directory; recreating it for this checkout.${NC}"
                docker rm -f ${CONTAINER_NAME} >/dev/null
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                CREATE_RECREATED=1
            fi
            if ! is_container_running "${CONTAINER_NAME}"; then
                echo -e "${YELLOW}Starting stopped container...${NC}"
                docker start ${CONTAINER_NAME}
            fi
            if ! is_container_stably_running "${CONTAINER_NAME}" 4 1; then
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' is transient or exited after start checks.${NC}"
                echo -e "${YELLOW}Recreating '${CONTAINER_NAME}' with a persistent keepalive command for reliable re-entry.${NC}"
                docker rm -f ${CONTAINER_NAME} >/dev/null 2>&1 || true
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                CREATE_RECREATED=1
            fi
            if [ "${CREATE_RECREATED}" -eq 1 ]; then
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' was recreated for persistent dev usage.${NC}"
            else
                echo -e "${YELLOW}Container '${CONTAINER_NAME}' already exists; reusing it.${NC}"
            fi
            if [ "${HAS_TTY}" -eq 1 ]; then
                INTERACTIVE_EXIT_CODE=0
                set +e
                docker exec "${DOCKER_INTERACTIVE_OPTS[@]}" -e BUILD_DIR=build_docker ${CONTAINER_NAME} \
                    bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                INTERACTIVE_EXIT_CODE=$?
                set -e
                if [ "${CREATE_RECREATED}" -eq 1 ]; then
                    echo -e "${GREEN}Container '${CONTAINER_NAME}' was recreated and remains running. Use '$0 enter' to reconnect.${NC}"
                else
                    echo -e "${GREEN}Container '${CONTAINER_NAME}' remains running. Use '$0 enter' to reconnect.${NC}"
                fi
                if [ "${INTERACTIVE_EXIT_CODE}" -ne 0 ]; then
                    exit "${INTERACTIVE_EXIT_CODE}"
                fi
            else
                echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
                echo -e "${GREEN}Container '${CONTAINER_NAME}' is running.${NC}"
                echo -e "${GREEN}Use '$0 enter' from a TTY or run: docker exec -it -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                echo -e "${GREEN}For non-interactive usage, run: docker exec -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
            fi
        else
            if [ "${HAS_TTY}" -eq 1 ]; then
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                INTERACTIVE_EXIT_CODE=0
                set +e
                docker exec "${DOCKER_INTERACTIVE_OPTS[@]}" -e BUILD_DIR=build_docker ${CONTAINER_NAME} \
                    bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                INTERACTIVE_EXIT_CODE=$?
                set -e
                if [ "${CREATE_RECREATED}" -eq 1 ]; then
                    echo -e "${GREEN}Container '${CONTAINER_NAME}' was recreated and remains running. Use '$0 enter' to reconnect.${NC}"
                else
                    echo -e "${GREEN}Container '${CONTAINER_NAME}' remains running. Use '$0 enter' to reconnect.${NC}"
                fi
                if [ "${INTERACTIVE_EXIT_CODE}" -ne 0 ]; then
                    exit "${INTERACTIVE_EXIT_CODE}"
                fi
            else
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                if [ "${CREATE_RECREATED}" -eq 1 ]; then
                    echo -e "${GREEN}Container '${CONTAINER_NAME}' was recreated and started in background (non-interactive mode).${NC}"
                else
                    echo -e "${GREEN}Container '${CONTAINER_NAME}' created and started in background (non-interactive mode).${NC}"
                fi
                echo -e "${GREEN}Use '$0 enter' from a TTY or run: docker exec -it -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                echo -e "${GREEN}For non-interactive usage, run: docker exec -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
            fi
        fi
        ;;

    enter)
        echo -e "${YELLOW}Entering persistent dev container...${NC}"
        warn_incomplete_build_docker
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$" && \
           compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
            if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                echo -e "${YELLOW}Compose service 'dev' is also running; '$0 enter' will use standalone '${CONTAINER_NAME}'.${NC}"
                echo -e "${YELLOW}Use '${COMPOSE_CMD_PREFIX} exec dev /bin/bash' if you want the compose container.${NC}"
            else
                echo -e "${YELLOW}Standalone '${CONTAINER_NAME}' exists but is stopped while compose service 'dev' is running.${NC}"
                echo -e "${YELLOW}Reusing running compose service 'dev' instead of starting standalone.${NC}"
                if [ "${HAS_TTY}" -eq 1 ]; then
                    COMPOSE_INTERACTIVE_EXIT_CODE=0
                    set +e
                    compose_cmd exec "${COMPOSE_EXEC_OPTS[@]}" dev /bin/bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                    COMPOSE_INTERACTIVE_EXIT_CODE=$?
                    set -e
                    echo -e "${GREEN}Compose service 'dev' remains running. Use '$0 enter' or '${COMPOSE_CMD_PREFIX} exec dev /bin/bash' to reconnect.${NC}"
                    exit "${COMPOSE_INTERACTIVE_EXIT_CODE}"
                fi
                echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
                echo -e "${GREEN}Use '${COMPOSE_CMD_PREFIX} exec dev /bin/bash' from a TTY to enter compose service 'dev'.${NC}"
                echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                exit 0
            fi
        fi
        if ! docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            echo -e "${YELLOW}Standalone container '${CONTAINER_NAME}' not found; using docker compose service 'dev'.${NC}"
            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                echo -e "${GREEN}Compose service 'dev' is already running; skipping compose-up.${NC}"
            else
                echo -e "${YELLOW}Starting services with docker-compose...${NC}"
                ensure_image
                compose_cmd up -d dev
                echo -e "${GREEN}Compose service 'dev' started.${NC}"
            fi
            if [ "${HAS_TTY}" -eq 1 ]; then
                COMPOSE_INTERACTIVE_EXIT_CODE=0
                set +e
                compose_cmd exec "${COMPOSE_EXEC_OPTS[@]}" dev /bin/bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                COMPOSE_INTERACTIVE_EXIT_CODE=$?
                set -e
                echo -e "${GREEN}Compose service 'dev' remains running. Use '$0 enter' or '${COMPOSE_CMD_PREFIX} exec dev /bin/bash' to reconnect.${NC}"
                exit "${COMPOSE_INTERACTIVE_EXIT_CODE}"
            fi
            echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
            echo -e "${GREEN}Use '${COMPOSE_CMD_PREFIX} exec dev /bin/bash' from a TTY to enter compose service 'dev'.${NC}"
            echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
            exit 0
        fi
        EXISTING_INIT=$(docker inspect -f '{{if .HostConfig.Init}}true{{else}}false{{end}}' ${CONTAINER_NAME} 2>/dev/null || echo false)
        if [ "${EXISTING_INIT}" != "true" ]; then
            echo -e "${YELLOW}Container '${CONTAINER_NAME}' was created without Docker init support; recreating it to enable child-process reaping.${NC}"
            docker rm -f ${CONTAINER_NAME} >/dev/null
            ensure_image
            docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                bash -lc "exec tail -f /dev/null" >/dev/null
        fi
        if ! has_expected_image "${CONTAINER_NAME}"; then
            echo -e "${YELLOW}Container '${CONTAINER_NAME}' uses image '$(docker inspect -f '{{.Config.Image}}' ${CONTAINER_NAME} 2>/dev/null || echo unknown)'; recreating it with '${IMAGE_NAME}'.${NC}"
            docker rm -f ${CONTAINER_NAME} >/dev/null
            ensure_image
            docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                bash -lc "exec tail -f /dev/null" >/dev/null
        fi
        if ! has_keepalive_command "${CONTAINER_NAME}"; then
            echo -e "${YELLOW}Container '${CONTAINER_NAME}' uses a non-keepalive command; recreating it for persistent dev usage.${NC}"
            docker rm -f ${CONTAINER_NAME} >/dev/null
            ensure_image
            docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                bash -lc "exec tail -f /dev/null" >/dev/null
        fi
        if ! has_expected_workspace_mount "${CONTAINER_NAME}"; then
            echo -e "${YELLOW}Container '${CONTAINER_NAME}' is bound to a different workspace or working directory; recreating it for this checkout.${NC}"
            docker rm -f ${CONTAINER_NAME} >/dev/null
            ensure_image
            docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                bash -lc "exec tail -f /dev/null" >/dev/null
        fi
        if ! is_container_running "${CONTAINER_NAME}"; then
            echo -e "${YELLOW}Starting stopped container...${NC}"
            docker start ${CONTAINER_NAME}
        fi
        if ! is_container_stably_running "${CONTAINER_NAME}" 4 1; then
            echo -e "${YELLOW}Container '${CONTAINER_NAME}' is transient or exited after start checks.${NC}"
            echo -e "${YELLOW}Recreating '${CONTAINER_NAME}' with a persistent keepalive command for reliable re-entry.${NC}"
            ensure_image
            docker rm -f ${CONTAINER_NAME} >/dev/null 2>&1 || true
            docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                bash -lc "exec tail -f /dev/null" >/dev/null
        fi
        if [ "${HAS_TTY}" -eq 1 ]; then
            INTERACTIVE_EXIT_CODE=0
            set +e
            docker exec "${DOCKER_INTERACTIVE_OPTS[@]}" -e BUILD_DIR=build_docker ${CONTAINER_NAME} \
                bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
            INTERACTIVE_EXIT_CODE=$?
            set -e
            echo -e "${GREEN}Container '${CONTAINER_NAME}' remains running. Use '$0 enter' to reconnect.${NC}"
            if [ "${INTERACTIVE_EXIT_CODE}" -ne 0 ]; then
                exit "${INTERACTIVE_EXIT_CODE}"
            fi
        else
            echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
            if is_container_stably_running "${CONTAINER_NAME}" 2 1; then
                echo -e "${GREEN}Container '${CONTAINER_NAME}' is running.${NC}"
                echo -e "${GREEN}Use '$0 enter' from a TTY or run: docker exec -it -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                echo -e "${GREEN}For non-interactive usage, run: docker exec -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
            else
                echo -e "${GREEN}Container '${CONTAINER_NAME}' exited after startup checks.${NC}"
                if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                    echo -e "${GREEN}Compose service 'dev' is running; use: ${COMPOSE_CMD_PREFIX} exec dev /bin/bash${NC}"
                    echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T dev /bin/bash -lc '<command>'${NC}"
                    echo -e "${GREEN}If you need standalone '${CONTAINER_NAME}', run '$0 create' to refresh it first.${NC}"
                else
                    echo -e "${GREEN}Use '$0 create' to recreate/refresh standalone '${CONTAINER_NAME}'.${NC}"
                    echo -e "${GREEN}Then use '$0 enter' again to access it.${NC}"
                fi
            fi
        fi
        ;;

    *)
        echo "Usage: $0 {build-image|build|shell|create|enter|test|ci|ci-quick|clean|compose-up|compose-down} [arg]"
        echo ""
        echo "Commands:"
        echo "  build-image  - Build the Docker image"
        echo "  build        - Build core runtime + RocksDB quick-test binaries (default)"
        echo "  shell        - Start temporary interactive shell (auto-removed on exit)"
        echo "  create       - Create persistent dev container named '${CONTAINER_NAME}'"
        echo "  enter        - Enter existing '${CONTAINER_NAME}' container (auto-starts if stopped)"
        echo "  test         - Build dbtest and run shardNoReplication smoke test"
        echo "  ci [test] [jobs] - Build and run CI test (default: all, jobs: 32)"
        echo "  ci-quick [test] - Run CI test without rebuild (default: shardNoReplication)"
        echo "  clean        - Clean build artifacts"
        echo "  compose-up   - Start persistent dev container via docker-compose"
        echo "  compose-down - Stop persistent dev container"
        echo ""
        echo "CI Test Names:"
        echo "  all, compile, cleanup, simpleTransaction, simplePaxos,"
        echo "  shardNoReplication, shardNoReplicationErpc,"
        echo "  shard1Replication, shard2Replication, shard2ReplicationErpc,"
        echo "  shard1ReplicationSimple, shard2ReplicationSimple,"
        echo "  shard1ReplicationRaft, shard2ReplicationRaft,"
        echo "  shard1ReplicationSimpleRaft, shard2ReplicationSimpleRaft,"
        echo "  rocksdbTests, multiShardSingleProcess,"
        echo "  shard2SingleProcess, shard2SingleProcessReplication,"
        echo "  rrrTests, cpuThrottlingScaling, clientServer"
        echo ""
        echo "Examples:"
        echo "  $0 ci                    # Build and run all CI tests"
        echo "  $0 ci 8                  # Build and run all CI tests with 8 build jobs"
        echo "  $0 ci shardNoReplication # Build and run shardNoReplication test"
        echo "  $0 ci shardNoReplication 8 # Use 8 build jobs for CI flow"
        echo "  $0 ci-quick shard2Replication # Run shard2Replication without rebuild"
        exit 1
        ;;
esac
