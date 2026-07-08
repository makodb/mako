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
DOCKER_ENV_OPTS=(-e CARGO_TARGET_DIR=/workspace/target-docker -e CARGO_HOME=/workspace/.cargo-docker -e BUILD_DIR=build_docker)
# Forward MAKO_ASAN so CMakeLists.txt builds with -fsanitize=address + libc
# malloc, and the dbtest runtime sees a sane ASAN_OPTIONS for the test run.
if [ -n "${MAKO_ASAN:-}" ]; then
    DOCKER_ENV_OPTS+=(-e "MAKO_ASAN=${MAKO_ASAN}")
    : "${ASAN_OPTIONS:=abort_on_error=0:halt_on_error=0:detect_leaks=0:symbolize=1:print_stacktrace=1:strict_string_checks=1:strict_init_order=1}"
    DOCKER_ENV_OPTS+=(-e "ASAN_OPTIONS=${ASAN_OPTIONS}")
fi
# Forward MAKO_CLUSTER_CONFIG so a multi-shard CI run can exercise the
# cluster-config bootstrap (shard-0 config service + per-node watcher).
# Off by default; the dbtest children inherit it from the outer run.
if [ -n "${MAKO_CLUSTER_CONFIG:-}" ]; then
    DOCKER_ENV_OPTS+=(-e "MAKO_CLUSTER_CONFIG=${MAKO_CLUSTER_CONFIG}")
fi
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
DOCKER_DEV_USER="${MAKO_DOCKER_DEV_USER:-$(id -u):$(id -g)}"
DOCKER_DEV_USER_OPTS=()
DOCKER_DEV_USER_CMD_PREFIX=""
COMPOSE_EXEC_USER_OPTS=()
COMPOSE_EXEC_USER_CMD_PREFIX=""
if [ "${DOCKER_DEV_USER}" != "root" ]; then
    DOCKER_DEV_USER_OPTS=(--user "${DOCKER_DEV_USER}")
    DOCKER_DEV_USER_CMD_PREFIX="--user ${DOCKER_DEV_USER} "
    COMPOSE_EXEC_USER_OPTS=(--user "${DOCKER_DEV_USER}")
    COMPOSE_EXEC_USER_CMD_PREFIX="--user ${DOCKER_DEV_USER} "
fi
DOCKER_SCRIPT_USER="${MAKO_DOCKER_SCRIPT_USER:-$(id -u):$(id -g)}"
DOCKER_SCRIPT_USER_OPTS=()
if [ "${DOCKER_SCRIPT_USER}" != "root" ]; then
    DOCKER_SCRIPT_USER_OPTS=(--user "${DOCKER_SCRIPT_USER}")
fi

# Docker CI/build surfaces should prioritize reproducible runtime binaries.
# Borrow checking is configurable, but defaults OFF to avoid unrelated static
# analysis failures blocking Docker test matrix execution.
DOCKER_ENABLE_BORROW_CHECKING="${MAKO_DOCKER_ENABLE_BORROW_CHECKING:-OFF}"
if [ "${DOCKER_ENABLE_BORROW_CHECKING}" != "ON" ] && [ "${DOCKER_ENABLE_BORROW_CHECKING}" != "OFF" ]; then
    echo -e "${RED}Error: MAKO_DOCKER_ENABLE_BORROW_CHECKING must be ON or OFF (got '${DOCKER_ENABLE_BORROW_CHECKING}').${NC}"
    exit 1
fi
DOCKER_CMAKE_BORROW_ARG="-DENABLE_BORROW_CHECKING=${DOCKER_ENABLE_BORROW_CHECKING}"

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

normalize_script_build_ownership() {
    local needs_fix=0
    local target_path
    local script_uid=""
    local script_gid=""

    if [ "${DOCKER_SCRIPT_USER}" = "root" ]; then
        return 0
    fi

    for target_path in build_docker target-docker .cargo-docker; do
        if [ -e "${target_path}" ] && [ ! -w "${target_path}" ]; then
            needs_fix=1
            break
        fi
    done

    if [ "${needs_fix}" -eq 0 ]; then
        return 0
    fi

    if [[ "${DOCKER_SCRIPT_USER}" =~ ^([0-9]+):([0-9]+)$ ]]; then
        script_uid="${BASH_REMATCH[1]}"
        script_gid="${BASH_REMATCH[2]}"
    else
        echo -e "${YELLOW}Warning: MAKO_DOCKER_SCRIPT_USER='${DOCKER_SCRIPT_USER}' is not numeric uid:gid; skipping auto-fix for stale root-owned build artifacts.${NC}"
        echo -e "${YELLOW}If build/test/ci fail with permission errors, run './docker_build.sh clean' or set MAKO_DOCKER_SCRIPT_USER=root.${NC}"
        return 0
    fi

    echo -e "${YELLOW}Detected stale non-writable Docker build/cache artifacts; normalizing ownership to ${script_uid}:${script_gid}.${NC}"
    docker run --rm "${DOCKER_SECURITY_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" ${IMAGE_NAME} \
        bash -lc "for d in /workspace/build_docker /workspace/target-docker /workspace/.cargo-docker; do if [ -e \"\$d\" ]; then chown -R ${script_uid}:${script_gid} \"\$d\"; fi; done"
}

is_positive_integer() {
    local value="$1"
    [[ "${value}" =~ ^[0-9]+$ ]] && [ "${value}" -gt 0 ]
}

warn_incomplete_build_docker() {
    local required_bins=(
        "build_docker/dbtest"
        "build_docker/simpleTransaction"
        "build_docker/simplePaxos"
        "build_docker/simpleTransactionRep"
        "build_docker/continuousTransactions"
    )
    local missing_bins=()
    local incompatible_bins=()
    local use_docker_readelf=0
    local required_bin

    if ! readelf --version >/dev/null 2>&1; then
        use_docker_readelf=1
    fi

    for required_bin in "${required_bins[@]}"; do
        if [ ! -x "${required_bin}" ]; then
            missing_bins+=("${required_bin}")
            continue
        fi

        local runpath
        if [ "${use_docker_readelf}" -eq 1 ]; then
            runpath=$(docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                bash -lc "readelf -d '${required_bin}' 2>/dev/null | awk '/RUNPATH/ {print \$5}' | tr -d '[]'")
        else
            runpath=$(readelf -d "${required_bin}" 2>/dev/null | awk '/RUNPATH/ {print $5}' | tr -d '[]')
        fi

        if [[ ":${runpath}:" != *":/workspace/build_docker:"* ]]; then
            incompatible_bins+=("${required_bin} (RUNPATH: ${runpath:-<none>})")
        fi
    done
    if [ "${#missing_bins[@]}" -gt 0 ] || [ "${#incompatible_bins[@]}" -gt 0 ]; then
        echo -e "${YELLOW}Warning: Docker build artifacts are missing or incompatible in build_docker.${NC}"
        for required_bin in "${missing_bins[@]}"; do
            echo -e "${YELLOW}  - ${required_bin}${NC}"
        done
        for required_bin in "${incompatible_bins[@]}"; do
            echo -e "${YELLOW}  - ${required_bin}${NC}"
        done
        if [ "${use_docker_readelf}" -eq 1 ]; then
            echo -e "${YELLOW}Host 'readelf' is unavailable or not working; compatibility was checked with Docker tools.${NC}"
        fi
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

has_expected_workspace_source() {
    local container_name="$1"
    local mount_source
    local mount_type

    mount_source=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace"}}{{.Source}}{{end}}{{end}}' "${container_name}" 2>/dev/null || echo "")
    mount_type=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace"}}{{.Type}}{{end}}{{end}}' "${container_name}" 2>/dev/null || echo "")

    [ "${mount_type}" = "bind" ] && [ "${mount_source}" = "${WORKSPACE_ROOT}" ]
}

list_stale_compose_dev_containers() {
    local active_compose_container="${COMPOSE_PROJECT_NAME}-dev-1"
    local candidate

    while IFS= read -r candidate; do
        case "${candidate}" in
            mako-*-dev-1)
                if [ "${candidate}" = "${active_compose_container}" ]; then
                    continue
                fi
                if has_expected_workspace_source "${candidate}"; then
                    echo "${candidate}"
                fi
                ;;
        esac
    done < <(docker ps --format '{{.Names}}')
}

list_stale_compose_projects() {
    local stale_compose_container

    while IFS= read -r stale_compose_container; do
        [ -n "${stale_compose_container}" ] || continue
        echo "${stale_compose_container%-dev-1}"
    done < <(list_stale_compose_dev_containers)
}

count_stale_compose_projects() {
    local stale_compose_count=0
    local stale_compose_project

    while IFS= read -r stale_compose_project; do
        [ -n "${stale_compose_project}" ] || continue
        stale_compose_count=$((stale_compose_count + 1))
    done < <(list_stale_compose_projects)

    echo "${stale_compose_count}"
}

select_single_stale_compose_project() {
    local stale_compose_projects=()
    local stale_compose_project

    while IFS= read -r stale_compose_project; do
        [ -n "${stale_compose_project}" ] || continue
        stale_compose_projects+=("${stale_compose_project}")
    done < <(list_stale_compose_projects)

    if [ "${#stale_compose_projects[@]}" -eq 1 ]; then
        echo "${stale_compose_projects[0]}"
        return 0
    fi

    return 1
}

warn_stale_compose_dev_containers() {
    local force_warn_single_stale="${1:-0}"
    local stale_compose_containers=()
    local stale_compose_projects=()
    local container_name
    local project_name
    local current_compose_running=0

    while IFS= read -r container_name; do
        [ -n "${container_name}" ] || continue
        stale_compose_containers+=("${container_name}")
    done < <(list_stale_compose_dev_containers)

    while IFS= read -r project_name; do
        [ -n "${project_name}" ] || continue
        stale_compose_projects+=("${project_name}")
    done < <(list_stale_compose_projects)

    if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
        current_compose_running=1
    fi

    # A single same-checkout stale compose service with no current-project compose
    # service is usually a recoverable state that enter/compose-up can reuse cleanly.
    # Skip warning noise in that case and keep warnings for ambiguous/conflicting states.
    if [ "${force_warn_single_stale}" != "1" ] && [ "${#stale_compose_containers[@]}" -eq 1 ] && [ "${current_compose_running}" -eq 0 ]; then
        return 0
    fi

    if [ "${#stale_compose_containers[@]}" -gt 0 ]; then
        echo -e "${YELLOW}Warning: found running compose dev container(s) for this checkout under non-current project ID(s): ${stale_compose_containers[*]}.${NC}"
        echo -e "${YELLOW}These containers use different compose project IDs from '${COMPOSE_PROJECT_NAME}' and may cause confusing behavior.${NC}"
        if [ "${#stale_compose_projects[@]}" -eq 1 ]; then
            echo -e "${YELLOW}Stop stale compose project with: MAKO_COMPOSE_PROJECT=${stale_compose_projects[0]} docker compose down${NC}"
        elif [ "${#stale_compose_projects[@]}" -gt 1 ]; then
            echo -e "${YELLOW}Stop stale compose projects with:${NC}"
            for project_name in "${stale_compose_projects[@]}"; do
                echo -e "${YELLOW}  MAKO_COMPOSE_PROJECT=${project_name} docker compose down${NC}"
            done
        fi
        echo -e "${YELLOW}If needed, stop/remove individual containers with: docker stop <container> (or docker rm -f <container>).${NC}"
    fi
}

print_stale_compose_selection_commands() {
    local color="$1"
    local stale_project_name
    local printed_any=0

    while IFS= read -r stale_project_name; do
        [ -n "${stale_project_name}" ] || continue
        printed_any=1
        echo -e "${color}  MAKO_COMPOSE_PROJECT=${stale_project_name} docker compose exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
        echo -e "${color}  MAKO_COMPOSE_PROJECT=${stale_project_name} docker compose exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
    done < <(list_stale_compose_projects)

    [ "${printed_any}" -eq 1 ]
}

print_compose_project_enumeration_fallback() {
    local color="$1"
    local action_hint="${2:-${ACTION}}"

    echo -e "${color}Could not enumerate compose project IDs automatically.${NC}"
    echo -e "${color}Inspect running dev containers with: docker ps --format '{{.Names}}' | grep -- '-dev-1'${NC}"
    if [ -n "${action_hint}" ]; then
        echo -e "${color}Then rerun '$0 ${action_hint}' for project-scoped compose commands.${NC}"
    fi
}

print_compose_access_guidance() {
    local stale_compose_project=""
    local stale_compose_count
    local stale_compose_projects
    local stale_compose_cmd_prefix

    if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
        echo -e "${GREEN}Compose service 'dev' is also running.${NC}"
        echo -e "${GREEN}Compose access: ${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
        echo -e "${GREEN}Compose non-interactive: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
        echo -e "${GREEN}Compose teardown: ${COMPOSE_CMD_PREFIX} down${NC}"
        return 0
    fi

    stale_compose_count=$(count_stale_compose_projects)
    if stale_compose_project=$(select_single_stale_compose_project); then
        stale_compose_cmd_prefix="MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose"
        echo -e "${GREEN}Found running compose service 'dev' for this checkout under project '${stale_compose_project}'.${NC}"
        echo -e "${GREEN}Compose access: ${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
        echo -e "${GREEN}Compose non-interactive: ${stale_compose_cmd_prefix} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
        echo -e "${GREEN}Compose teardown: ${stale_compose_cmd_prefix} down${NC}"
        return 0
    fi

    if [ "${stale_compose_count}" -gt 1 ]; then
        stale_compose_projects=$(list_stale_compose_projects | paste -sd' ' -)
        echo -e "${GREEN}Found multiple running compose services for this checkout: ${stale_compose_projects}.${NC}"
        echo -e "${GREEN}Select one of the running compose projects:${NC}"
        if ! print_stale_compose_selection_commands "${GREEN}"; then
            print_compose_project_enumeration_fallback "${GREEN}" "shell"
        fi
        echo -e "${GREEN}Stop stale compose projects with:${NC}"
        while IFS= read -r stale_project_name; do
            [ -n "${stale_project_name}" ] || continue
            echo -e "${GREEN}  MAKO_COMPOSE_PROJECT=${stale_project_name} docker compose down${NC}"
        done < <(list_stale_compose_projects)
        return 0
    fi

    return 1
}

print_legacy_container_scope_note() {
    local legacy_workspace_source

    # Only relevant when automatic scoping is active and legacy default exists.
    if [ -n "${MAKO_DEV_CONTAINER_NAME:-}" ] || [ "${CONTAINER_NAME}" = "${CONTAINER_NAME_DEFAULT}" ]; then
        return 1
    fi
    if ! docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME_DEFAULT}$"; then
        return 1
    fi
    if has_expected_workspace_source "${CONTAINER_NAME_DEFAULT}"; then
        return 1
    fi

    legacy_workspace_source=$(docker inspect -f '{{range .Mounts}}{{if eq .Destination "/workspace"}}{{.Source}}{{end}}{{end}}' "${CONTAINER_NAME_DEFAULT}" 2>/dev/null || echo "")
    echo -e "${GREEN}Detected legacy standalone container '${CONTAINER_NAME_DEFAULT}' bound to a different workspace.${NC}"
    if [ -n "${legacy_workspace_source}" ]; then
        echo -e "${GREEN}Legacy '${CONTAINER_NAME_DEFAULT}' workspace: ${legacy_workspace_source}${NC}"
    fi
    echo -e "${GREEN}This checkout uses scoped standalone name '${CONTAINER_NAME}'.${NC}"
    return 0
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
        if ! has_expected_workspace_source "${CONTAINER_NAME_DEFAULT}"; then
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
EXTRA_ARG=${2:-}

ensure_no_extra_args() {
    local action_name="$1"
    if [ -n "${EXTRA_ARG}" ]; then
        echo -e "${RED}Error: '${action_name}' does not accept extra arguments (got '${EXTRA_ARG}').${NC}"
        echo -e "${YELLOW}Run '$0' without args to see supported usage.${NC}"
        exit 1
    fi
}

case "$ACTION" in
    build-image)
        ensure_no_extra_args "build-image"
        echo -e "${YELLOW}Building Docker image...${NC}"
        docker build -f Dockerfile.ubuntu24 -t ${IMAGE_NAME} .
        echo -e "${GREEN}Docker image built successfully!${NC}"
        ;;

    build)
        if [ -n "${3:-}" ]; then
            echo -e "${RED}Error: Too many arguments for build.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh build' or './docker_build.sh build <jobs>'.${NC}"
            exit 1
        fi
        if ! [[ "${JOBS}" =~ ^[0-9]+$ ]] || [ "${JOBS}" -lt 1 ]; then
            echo -e "${RED}Error: Build jobs must be a positive integer (got '${JOBS}').${NC}"
            exit 1
        fi
        echo -e "${YELLOW}Building Mako in container...${NC}"
        echo -e "${YELLOW}Docker CMake arg: ${DOCKER_CMAKE_BORROW_ARG}${NC}"
        ensure_image
        normalize_script_build_ownership
        docker run --rm "${DOCKER_SCRIPT_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" ${IMAGE_NAME} \
            bash -c "${DOCKER_CORE_ULIMIT_CMD}; cd /workspace && \
                     if [ -f build_docker/CMakeCache.txt ]; then \
                         CACHE_INCOMPATIBLE=0; \
                         CACHE_REASON=''; \
                         if ! grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=/workspace$' build_docker/CMakeCache.txt; then \
                             CACHE_INCOMPATIBLE=1; \
                             CACHE_REASON='CMAKE_HOME_DIRECTORY mismatch'; \
                         else \
                             C_COMPILER=\$(grep -E '^CMAKE_C_COMPILER:(FILEPATH|STRING)=' build_docker/CMakeCache.txt | head -n1 | cut -d= -f2-); \
                             CXX_COMPILER=\$(grep -E '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=' build_docker/CMakeCache.txt | head -n1 | cut -d= -f2-); \
                             if [ -n \"\$C_COMPILER\" ]; then \
                                 if [[ \"\$C_COMPILER\" == /* ]]; then \
                                     if [ ! -x \"\$C_COMPILER\" ]; then \
                                         CACHE_INCOMPATIBLE=1; \
                                         CACHE_REASON=\"missing cached C compiler '\$C_COMPILER'\"; \
                                     fi; \
                                 elif ! command -v \"\$C_COMPILER\" >/dev/null 2>&1; then \
                                     CACHE_INCOMPATIBLE=1; \
                                     CACHE_REASON=\"missing cached C compiler '\$C_COMPILER'\"; \
                                 fi; \
                             fi; \
                             if [ \"\$CACHE_INCOMPATIBLE\" -eq 0 ] && [ -n \"\$CXX_COMPILER\" ]; then \
                                 if [[ \"\$CXX_COMPILER\" == /* ]]; then \
                                     if [ ! -x \"\$CXX_COMPILER\" ]; then \
                                         CACHE_INCOMPATIBLE=1; \
                                         CACHE_REASON=\"missing cached CXX compiler '\$CXX_COMPILER'\"; \
                                     fi; \
                                 elif ! command -v \"\$CXX_COMPILER\" >/dev/null 2>&1; then \
                                     CACHE_INCOMPATIBLE=1; \
                                     CACHE_REASON=\"missing cached CXX compiler '\$CXX_COMPILER'\"; \
                                 fi; \
                             fi; \
                             if [ \"\$CACHE_INCOMPATIBLE\" -eq 0 ]; then \
                                 CACHE_BORROW_CHECKING=\$(grep -E '^ENABLE_BORROW_CHECKING:BOOL=' build_docker/CMakeCache.txt | head -n1 | cut -d= -f2-); \
                                 if [ -n \"\$CACHE_BORROW_CHECKING\" ] && [ \"\$CACHE_BORROW_CHECKING\" != \"${DOCKER_ENABLE_BORROW_CHECKING}\" ]; then \
                                     CACHE_INCOMPATIBLE=1; \
                                     CACHE_REASON=\"ENABLE_BORROW_CHECKING mismatch (cache=\$CACHE_BORROW_CHECKING expected=${DOCKER_ENABLE_BORROW_CHECKING})\"; \
                                 fi; \
                             fi; \
                         fi; \
                         if [ \"\$CACHE_INCOMPATIBLE\" -eq 1 ]; then \
                             echo \"Cleaning incompatible build_docker cache (\${CACHE_REASON})\"; \
                             rm -rf build_docker; \
                         fi; \
                     fi && \
                     if [ ! -f build_docker/CMakeCache.txt ]; then \
                         echo 'Configuring build_docker'; \
                         cmake -S . -B build_docker -DCMAKE_BUILD_TYPE=Release ${DOCKER_CMAKE_BORROW_ARG}; \
                     else \
                         echo 'Reusing existing build_docker CMake cache'; \
                     fi && \
                     cmake --build build_docker --parallel ${JOBS} --target \
                         dbtest simpleTransaction simplePaxos simpleTransactionRep continuousTransactions \
                         test_rocksdb_persistence test_callback_demo test_ordered_callbacks \
                         test_partitioned_queues test_stress_partitioned_queues"
        echo -e "${GREEN}Build completed successfully!${NC}"
        ;;

    shell)
        ensure_no_extra_args "shell"
        echo -e "${YELLOW}Starting interactive shell in container...${NC}"
        ensure_image
        warn_incomplete_build_docker
        if [ "${HAS_TTY}" -eq 1 ]; then
            docker run --rm "${DOCKER_INTERACTIVE_OPTS[@]}" "${DOCKER_DEV_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
        else
            echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
            warn_stale_compose_dev_containers
            if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                if is_container_running "${CONTAINER_NAME}"; then
                    # Only advertise direct docker exec when standalone stays up
                    # for a short window; legacy containers can flap briefly.
                    if is_container_stably_running "${CONTAINER_NAME}" 3 1; then
                        if ! has_expected_image "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but uses an unexpected image.${NC}"
                            echo -e "${GREEN}Use '$0 create' or '$0 enter' to recreate it with '${IMAGE_NAME}'.${NC}"
                            print_compose_access_guidance || true
                        elif ! has_expected_workspace_mount "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but points to a different /workspace mount or working directory.${NC}"
                            echo -e "${GREEN}Use '$0 create' or '$0 enter' to recreate/normalize it for this checkout.${NC}"
                            print_compose_access_guidance || true
                        elif ! has_init_enabled "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but was created without Docker init support.${NC}"
                            echo -e "${GREEN}Use '$0 create' or '$0 enter' to recreate/normalize it with '--init'.${NC}"
                            print_compose_access_guidance || true
                        elif ! has_keepalive_command "${CONTAINER_NAME}"; then
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is running but uses a non-keepalive command.${NC}"
                            echo -e "${GREEN}It may exit unexpectedly; use '$0 create' or '$0 enter' to normalize it for persistent dev usage.${NC}"
                            print_compose_access_guidance || true
                        else
                            echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' is already running.${NC}"
                            echo -e "${GREEN}Use '$0 enter' from a TTY, or run: docker exec -it ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                            echo -e "${GREEN}For non-interactive usage, run: docker exec ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
                            print_compose_access_guidance || true
                        fi
                    else
                        echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' was running briefly but exited.${NC}"
                        if print_compose_access_guidance; then
                            echo -e "${GREEN}If you need standalone '${CONTAINER_NAME}', use '$0 create' to recover it explicitly.${NC}"
                            echo -e "${GREEN}Or stop compose services first, then run '$0 enter' to target standalone.${NC}"
                        else
                            echo -e "${GREEN}Use '$0 enter' to start/recover standalone '${CONTAINER_NAME}' and attach when possible.${NC}"
                            echo -e "${GREEN}Or run '$0 create' to explicitly recreate/refresh standalone before entering it.${NC}"
                        fi
                    fi
                else
                    echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' exists but is stopped.${NC}"
                    if print_compose_access_guidance; then
                        echo -e "${GREEN}If you need standalone '${CONTAINER_NAME}', use '$0 create' to explicitly recreate/refresh it.${NC}"
                        echo -e "${GREEN}Or stop compose services first, then run '$0 enter' to target standalone.${NC}"
                    else
                        echo -e "${GREEN}Use '$0 enter' to start/recover standalone '${CONTAINER_NAME}' and attach when possible.${NC}"
                        echo -e "${GREEN}Or run '$0 create' to explicitly recreate/refresh standalone before entering it.${NC}"
                        echo -e "${GREEN}If you use raw Docker commands, verify '${CONTAINER_NAME}' stays running before docker exec.${NC}"
                    fi
                fi
            else
                echo -e "${GREEN}Standalone container '${CONTAINER_NAME}' was not found.${NC}"
                print_legacy_container_scope_note || true
                if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                    echo -e "${GREEN}Compose service 'dev' is already running.${NC}"
                    echo -e "${GREEN}From a TTY, run: ${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
                    echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                    echo -e "${GREEN}Compose teardown: ${COMPOSE_CMD_PREFIX} down${NC}"
                else
                    stale_compose_project=""
                    stale_compose_count=$(count_stale_compose_projects)
                    if stale_compose_project=$(select_single_stale_compose_project); then
                        stale_compose_cmd_prefix="MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose"
                        echo -e "${GREEN}Found running compose service 'dev' for this checkout under project '${stale_compose_project}'.${NC}"
                        echo -e "${GREEN}From a TTY, run: ${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
                        echo -e "${GREEN}For non-interactive usage, run: ${stale_compose_cmd_prefix} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                        echo -e "${GREEN}Compose teardown: ${stale_compose_cmd_prefix} down${NC}"
                    elif [ "${stale_compose_count}" -gt 1 ]; then
                        stale_compose_projects=$(list_stale_compose_projects | paste -sd' ' -)
                        echo -e "${GREEN}Found multiple running compose services for this checkout: ${stale_compose_projects}.${NC}"
                        echo -e "${GREEN}Select one of the running compose projects:${NC}"
                        if ! print_stale_compose_selection_commands "${GREEN}"; then
                            print_compose_project_enumeration_fallback "${GREEN}" "shell"
                        fi
                        echo -e "${GREEN}Stop stale compose projects with:${NC}"
                        while IFS= read -r stale_project_name; do
                            [ -n "${stale_project_name}" ] || continue
                            echo -e "${GREEN}  MAKO_COMPOSE_PROJECT=${stale_project_name} docker compose down${NC}"
                        done < <(list_stale_compose_projects)
                        echo -e "${GREEN}Or stop stale compose containers, then run '$0 enter'.${NC}"
                    else
                        echo -e "${GREEN}Use '$0 compose-up' to start compose service 'dev'.${NC}"
                        echo -e "${GREEN}From a TTY, run: ${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
                        echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                    fi
                fi
            fi
        fi
        ;;

    test)
        ensure_no_extra_args "test"
        echo -e "${YELLOW}Running Docker smoke test (build + dbtest runtime)...${NC}"
        ensure_image
        DOCKER_TEST_MAX_WAIT_SECONDS=180
        if [ -n "${MAKO_DOCKER_TEST_MAX_WAIT_SECONDS:-}" ]; then
            if is_positive_integer "${MAKO_DOCKER_TEST_MAX_WAIT_SECONDS}"; then
                DOCKER_TEST_MAX_WAIT_SECONDS="${MAKO_DOCKER_TEST_MAX_WAIT_SECONDS}"
            else
                echo -e "${YELLOW}Warning: MAKO_DOCKER_TEST_MAX_WAIT_SECONDS='${MAKO_DOCKER_TEST_MAX_WAIT_SECONDS}' is invalid.${NC}"
                if [ -n "${MAKO_MAX_WAIT_SECONDS:-}" ]; then
                    if is_positive_integer "${MAKO_MAX_WAIT_SECONDS}"; then
                        DOCKER_TEST_MAX_WAIT_SECONDS="${MAKO_MAX_WAIT_SECONDS}"
                        echo -e "${YELLOW}Using fallback MAKO_MAX_WAIT_SECONDS='${MAKO_MAX_WAIT_SECONDS}'.${NC}"
                    else
                        echo -e "${YELLOW}Warning: MAKO_MAX_WAIT_SECONDS='${MAKO_MAX_WAIT_SECONDS}' is invalid; using default 180.${NC}"
                    fi
                else
                    echo -e "${YELLOW}Using default 180.${NC}"
                fi
            fi
        elif [ -n "${MAKO_MAX_WAIT_SECONDS:-}" ]; then
            if is_positive_integer "${MAKO_MAX_WAIT_SECONDS}"; then
                DOCKER_TEST_MAX_WAIT_SECONDS="${MAKO_MAX_WAIT_SECONDS}"
            else
                echo -e "${YELLOW}Warning: MAKO_MAX_WAIT_SECONDS='${MAKO_MAX_WAIT_SECONDS}' is invalid; using default 180.${NC}"
            fi
        fi
        echo -e "${YELLOW}Using shardNoReplication wait timeout: ${DOCKER_TEST_MAX_WAIT_SECONDS}s (override with MAKO_DOCKER_TEST_MAX_WAIT_SECONDS or MAKO_MAX_WAIT_SECONDS).${NC}"
        normalize_script_build_ownership
        docker run --rm "${DOCKER_SCRIPT_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -e "MAKO_MAX_WAIT_SECONDS=${DOCKER_TEST_MAX_WAIT_SECONDS}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
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
                         if [ -f build_docker/CMakeCache.txt ]; then \
                             CACHE_INCOMPATIBLE=0; \
                             CACHE_REASON=''; \
                             if ! grep -q '^CMAKE_HOME_DIRECTORY:INTERNAL=/workspace$' build_docker/CMakeCache.txt; then \
                                 CACHE_INCOMPATIBLE=1; \
                                 CACHE_REASON='CMAKE_HOME_DIRECTORY mismatch'; \
                             else \
                                 C_COMPILER=\$(grep -E '^CMAKE_C_COMPILER:(FILEPATH|STRING)=' build_docker/CMakeCache.txt | head -n1 | cut -d= -f2-); \
                                 CXX_COMPILER=\$(grep -E '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=' build_docker/CMakeCache.txt | head -n1 | cut -d= -f2-); \
                                 if [ -n \"\$C_COMPILER\" ]; then \
                                     if [[ \"\$C_COMPILER\" == /* ]]; then \
                                         if [ ! -x \"\$C_COMPILER\" ]; then \
                                             CACHE_INCOMPATIBLE=1; \
                                             CACHE_REASON=\"missing cached C compiler '\$C_COMPILER'\"; \
                                         fi; \
                                     elif ! command -v \"\$C_COMPILER\" >/dev/null 2>&1; then \
                                         CACHE_INCOMPATIBLE=1; \
                                         CACHE_REASON=\"missing cached C compiler '\$C_COMPILER'\"; \
                                     fi; \
                                 fi; \
                                 if [ \"\$CACHE_INCOMPATIBLE\" -eq 0 ] && [ -n \"\$CXX_COMPILER\" ]; then \
                                     if [[ \"\$CXX_COMPILER\" == /* ]]; then \
                                         if [ ! -x \"\$CXX_COMPILER\" ]; then \
                                             CACHE_INCOMPATIBLE=1; \
                                             CACHE_REASON=\"missing cached CXX compiler '\$CXX_COMPILER'\"; \
                                         fi; \
                                     elif ! command -v \"\$CXX_COMPILER\" >/dev/null 2>&1; then \
                                         CACHE_INCOMPATIBLE=1; \
                                         CACHE_REASON=\"missing cached CXX compiler '\$CXX_COMPILER'\"; \
                                     fi; \
                                 fi; \
                             fi; \
                             if [ \"\$CACHE_INCOMPATIBLE\" -eq 1 ]; then \
                                 echo \"Cleaning incompatible build_docker cache (\${CACHE_REASON})\"; \
                                 rm -rf build_docker; \
                             fi; \
                         fi && \
                         if [ ! -f build_docker/CMakeCache.txt ]; then \
                             echo 'Configuring build_docker'; \
                             cmake -S . -B build_docker -DCMAKE_BUILD_TYPE=Release; \
                         else \
                             echo 'Reusing existing build_docker CMake cache'; \
                         fi && \
                         cmake --build build_docker --parallel ${JOBS} --target dbtest && \
                         echo 'SUCCESS: dbtest build completed'; \
                     fi && \
                     BUILD_DIR=build_docker ./ci/ci.sh shardNoReplication && \
                     ls -la build_docker/dbtest"
        echo -e "${GREEN}Test completed successfully!${NC}"
        ;;

    ci)
        # Run a specific CI test or all tests
        if [ -n "${4:-}" ]; then
            echo -e "${RED}Error: Too many arguments for ci.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh ci', './docker_build.sh ci <jobs>', or './docker_build.sh ci <test> <jobs>'.${NC}"
            exit 1
        fi
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
            compile|cleanup|simpleTransaction|simplePaxos|shardNoReplication|shard1Replication|shard2Replication|shard1ReplicationSimple|shard2ReplicationSimple|shard1ReplicationRaft|shard2ReplicationRaft|shard1ReplicationSimpleRaft|shard2ReplicationSimpleRaft|rocksdbTests|multiShardSingleProcess|shard2SingleProcess|shard2SingleProcessReplication|rrrTests|cpuThrottlingScaling|clientServer|all)
                ;;
            *)
                echo -e "${RED}Error: Unknown CI test '${CI_TEST}'.${NC}"
                echo -e "${YELLOW}Run '$0' without args to see supported CI tests.${NC}"
                exit 1
                ;;
        esac
        echo -e "${YELLOW}Running CI test '${CI_TEST}' in container with ${CI_JOBS} build jobs...${NC}"
        ensure_image
        normalize_script_build_ownership
        case "${CI_TEST}" in
            compile|all)
                # ci.sh compile/all already performs compilation; avoid redundant outer build.
                docker run --rm "${DOCKER_SCRIPT_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "${DOCKER_CORE_ULIMIT_CMD}; rm -rf build_docker && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
                ;;
            cleanup)
                # cleanup should not force an expensive build first.
                docker run --rm "${DOCKER_SCRIPT_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "${DOCKER_CORE_ULIMIT_CMD}; CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh cleanup"
                ;;
            *)
                # Build via ci.sh's compile phase (cmake+ninja). The top-level
                # Makefile was removed by commit 0cf5b9724's CMake migration;
                # invoking `make` directly no longer works.
                docker run --rm "${DOCKER_SCRIPT_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "${DOCKER_CORE_ULIMIT_CMD}; rm -rf build_docker && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh compile && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
                ;;
        esac
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;

    ci-quick)
        # Run CI tests without rebuild (assumes build exists and was built in Docker)
        if [ -n "${3:-}" ]; then
            echo -e "${RED}Error: Too many arguments for ci-quick.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh ci-quick' or './docker_build.sh ci-quick <test>'.${NC}"
            exit 1
        fi
        if [[ "${2:-}" =~ ^[0-9]+$ ]]; then
            echo -e "${RED}Error: ci-quick does not accept a jobs-only argument ('${2}').${NC}"
            echo -e "${YELLOW}Use './docker_build.sh ci ${2}' to set build jobs, or './docker_build.sh ci-quick <test>' without jobs.${NC}"
            exit 1
        fi
        CI_TEST=${2:-shardNoReplication}
        case "${CI_TEST}" in
            compile|cleanup|all|rrrTests)
                echo -e "${RED}Error: ci-quick does not support '${CI_TEST}'.${NC}"
                echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' instead.${NC}"
                exit 1
                ;;
            simpleTransaction|simplePaxos|shardNoReplication|shard1Replication|shard2Replication|shard1ReplicationSimple|shard2ReplicationSimple|shard1ReplicationRaft|shard2ReplicationRaft|shard1ReplicationSimpleRaft|shard2ReplicationSimpleRaft|rocksdbTests|multiShardSingleProcess|shard2SingleProcess|shard2SingleProcessReplication|cpuThrottlingScaling|clientServer)
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
        USE_DOCKER_READELF=0
        if ! readelf --version >/dev/null 2>&1; then
            USE_DOCKER_READELF=1
            echo -e "${YELLOW}Host 'readelf' is unavailable or not working; using Docker tools for RUNPATH checks.${NC}"
            ensure_image
        fi
        for required_bin in "${REQUIRED_BINS[@]}"; do
            if [ ! -f "${required_bin}" ]; then
                missing_bins+=("${required_bin}")
                continue
            fi
            if [ ! -x "${required_bin}" ]; then
                non_executable_bins+=("${required_bin}")
                continue
            fi
            if [ "${USE_DOCKER_READELF}" -eq 1 ]; then
                RUNPATH=$(docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -lc "readelf -d '${required_bin}' 2>/dev/null | awk '/RUNPATH/ {print \$5}' | tr -d '[]'")
            else
                RUNPATH=$(readelf -d "${required_bin}" 2>/dev/null | awk '/RUNPATH/ {print $5}' | tr -d '[]')
            fi
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
            echo -e "${YELLOW}'build' compiles core runtime binaries (dbtest, simpleTransaction, simplePaxos, simpleTransactionRep, continuousTransactions)${NC}"
            echo -e "${YELLOW}and RocksDB test binaries for ci-quick rocksdbTests.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh build' to rebuild Docker binaries, then rerun './docker_build.sh ci-quick ${CI_TEST}'.${NC}"
            echo -e "${YELLOW}Or run './docker_build.sh ci ${CI_TEST}' to build and run this suite in one command.${NC}"
            exit 1
        fi
        if [ "${#non_executable_bins[@]}" -gt 0 ]; then
            echo -e "${RED}Error: Required binaries are not executable for CI test '${CI_TEST}':${NC}"
            for bin in "${non_executable_bins[@]}"; do
                echo -e "${RED}  - ${bin}${NC}"
            done
            echo -e "${YELLOW}Use './docker_build.sh build' to refresh binaries, then rerun './docker_build.sh ci-quick ${CI_TEST}'.${NC}"
            echo -e "${YELLOW}Or run './docker_build.sh ci ${CI_TEST}' to rebuild and run this suite in one command.${NC}"
            exit 1
        fi
        if [ "${#incompatible_bins[@]}" -gt 0 ]; then
            echo -e "${RED}Error: Required binaries are not Docker-compatible for CI test '${CI_TEST}':${NC}"
            for bin in "${incompatible_bins[@]}"; do
                echo -e "${RED}  - ${bin}${NC}"
            done
            echo -e "${YELLOW}Cannot run locally-built binaries in Docker due to library path mismatch.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh build' to regenerate Docker-compatible binaries, then rerun './docker_build.sh ci-quick ${CI_TEST}'.${NC}"
            echo -e "${YELLOW}Or run './docker_build.sh ci ${CI_TEST}' to rebuild and run this suite in one command.${NC}"
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
        normalize_script_build_ownership
        docker run --rm "${DOCKER_SCRIPT_USER_OPTS[@]}" "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -e LD_LIBRARY_PATH=/workspace/build_docker -v "${WORKSPACE_ROOT}:/workspace" -w /workspace ${IMAGE_NAME} \
            bash -c "${DOCKER_CORE_ULIMIT_CMD}; BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;

    clean)
        ensure_no_extra_args "clean"
        echo -e "${YELLOW}Cleaning build artifacts...${NC}"
        if docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
            docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" ${IMAGE_NAME} \
                bash -c "cd /workspace && rm -rf build_docker target-docker .cargo-docker && find . -maxdepth 1 -type f -name 'core.*' -delete"
        else
            echo -e "${YELLOW}Image '${IMAGE_NAME}' not found; cleaning workspace directly.${NC}"
            rm -rf build_docker target-docker .cargo-docker 2>/dev/null || true
            find . -maxdepth 1 -type f -name 'core.*' -delete 2>/dev/null || true
            HAS_LEFTOVERS=0
            if [ -d build_docker ] || [ -d target-docker ] || [ -d .cargo-docker ]; then
                HAS_LEFTOVERS=1
            fi
            if find . -maxdepth 1 -type f -name 'core.*' -print -quit | grep -q .; then
                HAS_LEFTOVERS=1
            fi
            if [ "${HAS_LEFTOVERS}" -eq 1 ]; then
                echo -e "${YELLOW}Host cleanup lacked permissions; using temporary ubuntu helper container.${NC}"
                docker run --rm -v "${WORKSPACE_ROOT}:/workspace" ubuntu:24.04 \
                    bash -c "cd /workspace && rm -rf build_docker target-docker .cargo-docker && find . -maxdepth 1 -type f -name 'core.*' -delete"
            fi
        fi
        echo -e "${GREEN}Clean completed!${NC}"
        ;;

    compose-up)
        ensure_no_extra_args "compose-up"
        echo -e "${YELLOW}Starting services with docker-compose...${NC}"
        ensure_image
        warn_incomplete_build_docker
        compose_was_running=0
        if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
            compose_was_running=1
        fi
        stale_compose_project=""
        stale_compose_count=0
        compose_already_running_for_checkout=${compose_was_running}
        if [ "${compose_was_running}" -eq 0 ]; then
            stale_compose_count=$(count_stale_compose_projects)
            if stale_compose_project=$(select_single_stale_compose_project); then
                compose_already_running_for_checkout=1
            elif [ "${stale_compose_count}" -gt 0 ]; then
                compose_already_running_for_checkout=1
            fi
        fi
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                if [ "${compose_already_running_for_checkout}" -eq 1 ]; then
                    if [ "${compose_was_running}" -eq 1 ]; then
                        echo -e "${YELLOW}Warning: compose service 'dev' is already running for the current project alongside standalone container '${CONTAINER_NAME}'.${NC}"
                    elif [ "${stale_compose_count}" -gt 1 ]; then
                        echo -e "${YELLOW}Warning: multiple compose services are already running for this checkout under different project IDs, alongside standalone container '${CONTAINER_NAME}'.${NC}"
                    else
                        echo -e "${YELLOW}Warning: compose service 'dev' is already running for this checkout alongside standalone container '${CONTAINER_NAME}'.${NC}"
                    fi
                    echo -e "${YELLOW}'$0 enter' will keep using standalone '${CONTAINER_NAME}' while it is running.${NC}"
                else
                    echo -e "${YELLOW}Warning: starting compose service 'dev' will run alongside standalone container '${CONTAINER_NAME}'.${NC}"
                    echo -e "${YELLOW}'$0 enter' will keep using standalone '${CONTAINER_NAME}' while it is running.${NC}"
                fi
            else
                if [ "${compose_was_running}" -eq 1 ]; then
                    echo -e "${YELLOW}Warning: standalone container '${CONTAINER_NAME}' exists but is stopped while compose service 'dev' is running for the current project.${NC}"
                    echo -e "${YELLOW}'$0 enter' will reuse compose service 'dev' while standalone remains stopped.${NC}"
                elif [ "${stale_compose_count}" -gt 1 ]; then
                    echo -e "${YELLOW}Warning: standalone container '${CONTAINER_NAME}' exists but is stopped, and multiple stale compose services are already running for this checkout under different project IDs.${NC}"
                    echo -e "${YELLOW}'$0 enter' will refuse to start standalone '${CONTAINER_NAME}' to avoid duplicate sessions; select one of the running compose projects shown below.${NC}"
                elif [ "${compose_already_running_for_checkout}" -eq 1 ]; then
                    echo -e "${YELLOW}Warning: standalone container '${CONTAINER_NAME}' exists but is stopped, and stale compose service(s) are already running for this checkout under different project IDs.${NC}"
                    if [ -n "${stale_compose_project}" ]; then
                        echo -e "${YELLOW}'$0 enter' will reuse the running compose service for this checkout; use MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash to target it.${NC}"
                    else
                        echo -e "${YELLOW}'$0 enter' will reuse the running compose service for this checkout; use the project-scoped compose exec commands shown below.${NC}"
                    fi
                else
                    echo -e "${YELLOW}Warning: standalone container '${CONTAINER_NAME}' exists but is stopped.${NC}"
                    echo -e "${YELLOW}If compose service 'dev' starts, '$0 enter' will reuse it while standalone remains stopped.${NC}"
                fi
            fi
            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                echo -e "${YELLOW}Use '${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' to enter compose service 'dev'.${NC}"
            fi
        fi
        if [ "${compose_was_running}" -eq 0 ]; then
            if [ -n "${stale_compose_project}" ]; then
                stale_compose_cmd_prefix="MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose"
                echo -e "${YELLOW}Found running compose service 'dev' for this checkout under project '${stale_compose_project}'.${NC}"
                echo -e "${YELLOW}Reusing it to avoid starting a duplicate compose container.${NC}"
                echo -e "${YELLOW}To stop this reused compose project later, run: ${stale_compose_cmd_prefix} down${NC}"
                if [ "${HAS_TTY}" -eq 1 ]; then
                    echo -e "${GREEN}Container already running. Connect with: ${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
                else
                    echo -e "${GREEN}Container already running.${NC}"
                    echo -e "${GREEN}Non-interactive session detected; run commands with: ${stale_compose_cmd_prefix} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                fi
                exit 0
            elif [ "${stale_compose_count}" -gt 1 ]; then
                stale_compose_projects=$(list_stale_compose_projects | paste -sd' ' -)
                echo -e "${YELLOW}Found multiple running compose services for this checkout: ${stale_compose_projects}.${NC}"
                echo -e "${YELLOW}Refusing to start another compose container to avoid duplicates.${NC}"
                echo -e "${YELLOW}Select one of the running compose projects:${NC}"
                if ! print_stale_compose_selection_commands "${YELLOW}"; then
                    print_compose_project_enumeration_fallback "${YELLOW}" "compose-up"
                fi
                echo -e "${YELLOW}Stop stale compose projects with:${NC}"
                while IFS= read -r stale_compose_project; do
                    [ -n "${stale_compose_project}" ] || continue
                    echo -e "${YELLOW}  MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose down${NC}"
                done < <(list_stale_compose_projects)
                echo -e "${YELLOW}Or stop stale compose containers, then run '$0 compose-up'.${NC}"
                exit 1
            fi
        fi
        compose_cmd up -d dev
        warn_stale_compose_dev_containers
        if [ "${HAS_TTY}" -eq 1 ]; then
            if [ "${compose_was_running}" -eq 1 ]; then
                echo -e "${GREEN}Container already running. Connect with: ${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
            else
                echo -e "${GREEN}Container started. Connect with: ${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
            fi
            echo -e "${GREEN}Stop compose service 'dev' with: ${COMPOSE_CMD_PREFIX} down${NC}"
        else
            if [ "${compose_was_running}" -eq 1 ]; then
                echo -e "${GREEN}Container already running.${NC}"
            else
                echo -e "${GREEN}Container started.${NC}"
            fi
            echo -e "${GREEN}Non-interactive session detected; run commands with: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
            echo -e "${GREEN}To stop compose service 'dev', run: ${COMPOSE_CMD_PREFIX} down${NC}"
        fi
        ;;

    compose-down)
        ensure_no_extra_args "compose-down"
        echo -e "${YELLOW}Stopping services...${NC}"
        compose_was_running=0
        if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
            compose_was_running=1
        fi
        if [ "${compose_was_running}" -eq 0 ]; then
            single_stale_compose_project=""
            if single_stale_compose_project=$(select_single_stale_compose_project); then
                echo -e "${YELLOW}No running services found for current compose project '${COMPOSE_PROJECT_NAME}'.${NC}"
                echo -e "${YELLOW}Found running compose service 'dev' for this checkout under project '${single_stale_compose_project}'.${NC}"
                echo -e "${YELLOW}Reusing it for compose-down to avoid leaving stale dev sessions running.${NC}"
                MAKO_COMPOSE_PROJECT="${single_stale_compose_project}" docker compose down
                stale_compose_count=$(count_stale_compose_projects)
                if [ "${stale_compose_count}" -gt 0 ]; then
                    warn_stale_compose_dev_containers 1
                    echo -e "${YELLOW}Stopped compose project '${single_stale_compose_project}', but stale compose services are still running for this checkout.${NC}"
                    exit 1
                fi
                echo -e "${GREEN}Stopped compose service 'dev' for project '${single_stale_compose_project}'.${NC}"
                exit 0
            fi
        fi
        compose_cmd down
        stale_compose_count=$(count_stale_compose_projects)
        if [ "${stale_compose_count}" -gt 0 ]; then
            warn_stale_compose_dev_containers 1
            if [ "${compose_was_running}" -eq 1 ]; then
                echo -e "${YELLOW}Current compose project services were stopped, but stale compose services are still running for this checkout.${NC}"
            else
                echo -e "${YELLOW}No running services were found for the current compose project, and stale compose services are still running for this checkout.${NC}"
            fi
            exit 1
        else
            if [ "${compose_was_running}" -eq 1 ]; then
                echo -e "${GREEN}Services stopped!${NC}"
            else
                echo -e "${GREEN}No running services were found for the current compose project.${NC}"
            fi
        fi
        ;;

    create)
        ensure_no_extra_args "create"
        echo -e "${YELLOW}Creating persistent dev container...${NC}"
        ensure_image
        warn_incomplete_build_docker
        create_compose_was_running=0
        create_stale_compose_project=""
        create_stale_compose_count=0
        if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
            create_compose_was_running=1
        else
            create_stale_compose_count=$(count_stale_compose_projects)
            create_stale_compose_project=$(select_single_stale_compose_project || true)
        fi
        if [ "${create_compose_was_running}" -eq 1 ]; then
            echo -e "${YELLOW}Warning: compose service 'dev' is already running for this checkout.${NC}"
            echo -e "${YELLOW}'$0 create' will start/recover standalone '${CONTAINER_NAME}' in parallel; while standalone is running, '$0 enter' will prefer standalone.${NC}"
            echo -e "${YELLOW}To stop compose service 'dev' later, run: ${COMPOSE_CMD_PREFIX} down${NC}"
        elif [ -n "${create_stale_compose_project}" ]; then
            echo -e "${YELLOW}Warning: found running compose service 'dev' for this checkout under project '${create_stale_compose_project}'.${NC}"
            echo -e "${YELLOW}'$0 create' will start/recover standalone '${CONTAINER_NAME}' in parallel; while standalone is running, '$0 enter' will prefer standalone.${NC}"
            echo -e "${YELLOW}Use 'MAKO_COMPOSE_PROJECT=${create_stale_compose_project} docker compose exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' if you want that compose container.${NC}"
            echo -e "${YELLOW}Non-interactive: MAKO_COMPOSE_PROJECT=${create_stale_compose_project} docker compose exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
            echo -e "${YELLOW}To stop this compose project later, run: MAKO_COMPOSE_PROJECT=${create_stale_compose_project} docker compose down${NC}"
        elif [ "${create_stale_compose_count}" -gt 1 ]; then
            create_stale_compose_projects=$(list_stale_compose_projects | paste -sd' ' -)
            echo -e "${YELLOW}Warning: multiple compose services are running for this checkout: ${create_stale_compose_projects}.${NC}"
            echo -e "${YELLOW}'$0 create' will start/recover standalone '${CONTAINER_NAME}' in parallel; while standalone is running, '$0 enter' will prefer standalone.${NC}"
            echo -e "${YELLOW}Select one of the running compose projects:${NC}"
            if ! print_stale_compose_selection_commands "${YELLOW}"; then
                print_compose_project_enumeration_fallback "${YELLOW}" "create"
            fi
            echo -e "${YELLOW}Stop stale compose projects with:${NC}"
            while IFS= read -r create_stale_project; do
                [ -n "${create_stale_project}" ] || continue
                echo -e "${YELLOW}  MAKO_COMPOSE_PROJECT=${create_stale_project} docker compose down${NC}"
            done < <(list_stale_compose_projects)
        fi
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
                docker start ${CONTAINER_NAME} >/dev/null
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
                docker exec "${DOCKER_INTERACTIVE_OPTS[@]}" "${DOCKER_DEV_USER_OPTS[@]}" -e BUILD_DIR=build_docker ${CONTAINER_NAME} \
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
                echo -e "${GREEN}Use '$0 enter' from a TTY or run: docker exec -it ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                echo -e "${GREEN}For non-interactive usage, run: docker exec ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
            fi
        else
            if [ "${HAS_TTY}" -eq 1 ]; then
                docker run "${DOCKER_INIT_OPTS[@]}" -d "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "${WORKSPACE_ROOT}:/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                    bash -lc "exec tail -f /dev/null" >/dev/null
                INTERACTIVE_EXIT_CODE=0
                set +e
                docker exec "${DOCKER_INTERACTIVE_OPTS[@]}" "${DOCKER_DEV_USER_OPTS[@]}" -e BUILD_DIR=build_docker ${CONTAINER_NAME} \
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
                echo -e "${GREEN}Use '$0 enter' from a TTY or run: docker exec -it ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                echo -e "${GREEN}For non-interactive usage, run: docker exec ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
            fi
        fi
        ;;

    enter)
        ensure_no_extra_args "enter"
        echo -e "${YELLOW}Entering persistent dev container...${NC}"
        ensure_image
        warn_incomplete_build_docker
        warn_stale_compose_dev_containers
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$" && \
           compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
            if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                echo -e "${YELLOW}Compose service 'dev' is also running; '$0 enter' will use standalone '${CONTAINER_NAME}'.${NC}"
                echo -e "${YELLOW}Use '${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' if you want the compose container.${NC}"
                echo -e "${YELLOW}To stop compose service 'dev', run: ${COMPOSE_CMD_PREFIX} down${NC}"
            else
                echo -e "${YELLOW}Standalone '${CONTAINER_NAME}' exists but is stopped while compose service 'dev' is running.${NC}"
                echo -e "${YELLOW}Reusing running compose service 'dev' instead of starting standalone.${NC}"
                echo -e "${YELLOW}To stop this compose service later, run: ${COMPOSE_CMD_PREFIX} down${NC}"
                if [ "${HAS_TTY}" -eq 1 ]; then
                    COMPOSE_INTERACTIVE_EXIT_CODE=0
                    set +e
                    compose_cmd exec "${COMPOSE_EXEC_OPTS[@]}" "${COMPOSE_EXEC_USER_OPTS[@]}" dev /bin/bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                    COMPOSE_INTERACTIVE_EXIT_CODE=$?
                    set -e
                    echo -e "${GREEN}Compose service 'dev' remains running. Use '$0 enter' or '${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' to reconnect.${NC}"
                    exit "${COMPOSE_INTERACTIVE_EXIT_CODE}"
                fi
                echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
                echo -e "${GREEN}Use '${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' from a TTY to enter compose service 'dev'.${NC}"
                echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                exit 0
            fi
        fi
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$" && \
           ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            stale_compose_project=""
            stale_compose_count=$(count_stale_compose_projects)
            if stale_compose_project=$(select_single_stale_compose_project); then
                stale_compose_cmd_prefix="MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose"
                echo -e "${YELLOW}Standalone '${CONTAINER_NAME}' is stopped while compose service 'dev' is running under project '${stale_compose_project}'.${NC}"
                echo -e "${YELLOW}Reusing compose service 'dev' instead of starting standalone to avoid duplicate sessions.${NC}"
                echo -e "${YELLOW}To stop this reused compose project later, run: ${stale_compose_cmd_prefix} down${NC}"
                if [ "${HAS_TTY}" -eq 1 ]; then
                    COMPOSE_INTERACTIVE_EXIT_CODE=0
                    set +e
                    MAKO_COMPOSE_PROJECT="${stale_compose_project}" docker compose exec "${COMPOSE_EXEC_OPTS[@]}" "${COMPOSE_EXEC_USER_OPTS[@]}" dev /bin/bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                    COMPOSE_INTERACTIVE_EXIT_CODE=$?
                    set -e
                    echo -e "${GREEN}Compose service 'dev' (project '${stale_compose_project}') remains running. Reconnect with '${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash'.${NC}"
                    exit "${COMPOSE_INTERACTIVE_EXIT_CODE}"
                fi
                echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
                echo -e "${GREEN}Use '${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' from a TTY to enter compose service 'dev'.${NC}"
                echo -e "${GREEN}For non-interactive usage, run: ${stale_compose_cmd_prefix} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                exit 0
            elif [ "${stale_compose_count}" -gt 1 ]; then
                stale_compose_projects=$(list_stale_compose_projects | paste -sd' ' -)
                echo -e "${YELLOW}Standalone '${CONTAINER_NAME}' is stopped while multiple compose services are running for this checkout: ${stale_compose_projects}.${NC}"
                echo -e "${YELLOW}Refusing to start standalone '${CONTAINER_NAME}' to avoid duplicate dev sessions.${NC}"
                echo -e "${YELLOW}Select one of the running compose projects:${NC}"
                if ! print_stale_compose_selection_commands "${YELLOW}"; then
                    print_compose_project_enumeration_fallback "${YELLOW}" "enter"
                fi
                echo -e "${YELLOW}Or stop stale compose containers, then run '$0 enter' again.${NC}"
                exit 1
            fi
        fi
        if ! docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            echo -e "${YELLOW}Standalone container '${CONTAINER_NAME}' not found; using docker compose service 'dev'.${NC}"
            if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                echo -e "${GREEN}Compose service 'dev' is already running; skipping compose-up.${NC}"
                echo -e "${GREEN}To stop compose service 'dev', run: ${COMPOSE_CMD_PREFIX} down${NC}"
            else
                stale_compose_project=""
                stale_compose_count=$(count_stale_compose_projects)
                if stale_compose_project=$(select_single_stale_compose_project); then
                    stale_compose_cmd_prefix="MAKO_COMPOSE_PROJECT=${stale_compose_project} docker compose"
                    echo -e "${YELLOW}Found running compose service 'dev' for this checkout under project '${stale_compose_project}'.${NC}"
                    echo -e "${YELLOW}Reusing it to avoid starting a duplicate compose container.${NC}"
                    echo -e "${YELLOW}To stop this reused compose project later, run: ${stale_compose_cmd_prefix} down${NC}"
                    if [ "${HAS_TTY}" -eq 1 ]; then
                        COMPOSE_INTERACTIVE_EXIT_CODE=0
                        set +e
                        MAKO_COMPOSE_PROJECT="${stale_compose_project}" docker compose exec "${COMPOSE_EXEC_OPTS[@]}" "${COMPOSE_EXEC_USER_OPTS[@]}" dev /bin/bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                        COMPOSE_INTERACTIVE_EXIT_CODE=$?
                        set -e
                        echo -e "${GREEN}Compose service 'dev' (project '${stale_compose_project}') remains running. Reconnect with '${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash'.${NC}"
                        exit "${COMPOSE_INTERACTIVE_EXIT_CODE}"
                    fi
                    echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
                    echo -e "${GREEN}Use '${stale_compose_cmd_prefix} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' from a TTY to enter compose service 'dev'.${NC}"
                    echo -e "${GREEN}For non-interactive usage, run: ${stale_compose_cmd_prefix} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
                    exit 0
                elif [ "${stale_compose_count}" -gt 1 ]; then
                    stale_compose_projects=$(list_stale_compose_projects | paste -sd' ' -)
                    echo -e "${YELLOW}Found multiple running compose services for this checkout: ${stale_compose_projects}.${NC}"
                    echo -e "${YELLOW}Refusing to start another compose container to avoid duplicates.${NC}"
                    echo -e "${YELLOW}Select one of the running compose projects:${NC}"
                    if ! print_stale_compose_selection_commands "${YELLOW}"; then
                        print_compose_project_enumeration_fallback "${YELLOW}" "enter"
                    fi
                    echo -e "${YELLOW}Or stop stale compose containers, then run '$0 enter'.${NC}"
                    exit 1
                fi
                echo -e "${YELLOW}Starting services with docker-compose...${NC}"
                ensure_image
                compose_cmd up -d dev
                echo -e "${GREEN}Compose service 'dev' started.${NC}"
                echo -e "${GREEN}To stop compose service 'dev', run: ${COMPOSE_CMD_PREFIX} down${NC}"
            fi
            if [ "${HAS_TTY}" -eq 1 ]; then
                COMPOSE_INTERACTIVE_EXIT_CODE=0
                set +e
                compose_cmd exec "${COMPOSE_EXEC_OPTS[@]}" "${COMPOSE_EXEC_USER_OPTS[@]}" dev /bin/bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
                COMPOSE_INTERACTIVE_EXIT_CODE=$?
                set -e
                echo -e "${GREEN}Compose service 'dev' remains running. Use '$0 enter' or '${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' to reconnect.${NC}"
                echo -e "${GREEN}Stop compose service 'dev' with: ${COMPOSE_CMD_PREFIX} down${NC}"
                exit "${COMPOSE_INTERACTIVE_EXIT_CODE}"
            fi
            echo -e "${YELLOW}Non-interactive session detected; not opening an interactive shell.${NC}"
            echo -e "${GREEN}Use '${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash' from a TTY to enter compose service 'dev'.${NC}"
            echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
            echo -e "${GREEN}To stop compose service 'dev', run: ${COMPOSE_CMD_PREFIX} down${NC}"
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
            docker start ${CONTAINER_NAME} >/dev/null
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
            docker exec "${DOCKER_INTERACTIVE_OPTS[@]}" "${DOCKER_DEV_USER_OPTS[@]}" -e BUILD_DIR=build_docker ${CONTAINER_NAME} \
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
                echo -e "${GREEN}Use '$0 enter' from a TTY or run: docker exec -it ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash${NC}"
                echo -e "${GREEN}For non-interactive usage, run: docker exec ${DOCKER_DEV_USER_CMD_PREFIX}-e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash -lc '<command>'${NC}"
            else
                echo -e "${GREEN}Container '${CONTAINER_NAME}' exited after startup checks.${NC}"
                if compose_cmd ps --services --status running 2>/dev/null | grep -qx "dev"; then
                    echo -e "${GREEN}Compose service 'dev' is running; use: ${COMPOSE_CMD_PREFIX} exec ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash${NC}"
                    echo -e "${GREEN}For non-interactive usage, run: ${COMPOSE_CMD_PREFIX} exec -T ${COMPOSE_EXEC_USER_CMD_PREFIX}dev /bin/bash -lc '<command>'${NC}"
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
        echo "  build [jobs] - Build core runtime + RocksDB quick-test binaries (default jobs: 32)"
        echo "  shell        - Start temporary interactive shell (auto-removed on exit)"
        echo "  create       - Create persistent dev container named '${CONTAINER_NAME}'"
        echo "  enter        - Enter existing '${CONTAINER_NAME}' container (auto-starts if stopped unless compose duplicate-avoidance applies)"
        echo "  test         - Build dbtest and run shardNoReplication smoke test"
        echo "  ci [test] [jobs] - Build and run CI test (default: all, jobs: 32)"
        echo "  ci-quick [test] - Run CI test without rebuild (default: shardNoReplication)"
        echo "  clean        - Clean build artifacts"
        echo "  compose-up   - Start persistent dev container via docker-compose"
        echo "  compose-down - Stop persistent dev container"
        echo ""
        echo "CI Test Names:"
        echo "  all, compile, cleanup, simpleTransaction, simplePaxos,"
        echo "  shardNoReplication,"
        echo "  shard1Replication, shard2Replication,"
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
