#!/bin/bash

# Script to build and test Mako in Ubuntu 24.04 container

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Docker image and container names
IMAGE_NAME="mako-build:ubuntu24"
CONTAINER_NAME="mako-dev"

# Environment variables/options for Docker runs
DOCKER_ENV_OPTS=(-e CARGO_TARGET_DIR=/workspace/target-docker -e BUILD_DIR=build_docker)
DOCKER_SECURITY_OPTS=()
if docker info --format '{{json .SecurityOptions}}' 2>/dev/null | grep -q "name=apparmor"; then
    # Rust tooling (cargo/rustc) can fail under restrictive AppArmor profiles.
    DOCKER_SECURITY_OPTS=(--security-opt apparmor=unconfined)
fi

ensure_image() {
    if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
        echo -e "${YELLOW}Image '${IMAGE_NAME}' not found locally; building it first...${NC}"
        docker build -f Dockerfile.ubuntu24 -t ${IMAGE_NAME} .
    fi
}

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
        docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" ${IMAGE_NAME} \
            bash -c "cd /workspace && \
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
        docker run --rm -it "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" -w /workspace ${IMAGE_NAME} \
            bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
        ;;

    test)
        echo -e "${YELLOW}Running Docker smoke test (build + dbtest runtime)...${NC}"
        ensure_image
        docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" -w /workspace ${IMAGE_NAME} \
            bash -c "NEED_BUILD=1; \
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
                docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "rm -rf build_docker && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
                ;;
            cleanup)
                # cleanup should not force an expensive build first.
                docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh cleanup"
                ;;
            *)
                docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" -w /workspace ${IMAGE_NAME} \
                    bash -c "rm -rf build_docker && CI_MAKE_JOBS=${CI_JOBS} make BUILD_DIR=build_docker -j${CI_JOBS} && CI_MAKE_JOBS=${CI_JOBS} BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
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

        # Check if binary exists and has Docker build path in RUNPATH.
        if [ -f "build_docker/dbtest" ]; then
            RUNPATH=$(readelf -d build_docker/dbtest 2>/dev/null | grep RUNPATH | grep -o '\[.*\]' | tr -d '[]')
            if [[ ":$RUNPATH:" != *":/workspace/build_docker:"* ]]; then
                echo -e "${RED}Error: build_docker/dbtest is not Docker-compatible (RUNPATH: $RUNPATH)${NC}"
                echo -e "${YELLOW}Cannot run locally-built binary in Docker due to library path mismatch.${NC}"
                echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' to rebuild and test in Docker.${NC}"
                exit 1
            fi
            if [ "$RUNPATH" != "/workspace/build_docker" ]; then
                echo -e "${YELLOW}Warning: RUNPATH has extra entries ($RUNPATH); forcing Docker library path.${NC}"
            fi
        else
            echo -e "${RED}Error: build_docker/dbtest not found. Run './docker_build.sh ci' first.${NC}"
            exit 1
        fi
        missing_bins=()
        for required_bin in "${REQUIRED_BINS[@]}"; do
            if [ ! -f "${required_bin}" ]; then
                missing_bins+=("${required_bin}")
            fi
        done
        if [ "${#missing_bins[@]}" -gt 0 ]; then
            echo -e "${RED}Error: Required binaries are missing for CI test '${CI_TEST}':${NC}"
            for bin in "${missing_bins[@]}"; do
                echo -e "${RED}  - ${bin}${NC}"
            done
            echo -e "${YELLOW}'build' compiles core runtime binaries (dbtest, simpleTransaction, simplePaxos, simpleTransactionRep).${NC}"
            echo -e "${YELLOW}It also compiles RocksDB test binaries for ci-quick rocksdbTests.${NC}"
            echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' to build test-specific binaries and run this suite.${NC}"
            exit 1
        fi

        echo -e "${YELLOW}Running CI test '${CI_TEST}' (no rebuild)...${NC}"
        ensure_image
        docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -e LD_LIBRARY_PATH=/workspace/build_docker -v "$(pwd):/workspace" -w /workspace ${IMAGE_NAME} \
            bash -c "BUILD_DIR=build_docker ./ci/ci.sh ${CI_TEST}"
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;

    clean)
        echo -e "${YELLOW}Cleaning build artifacts...${NC}"
        if docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
            docker run --rm "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" ${IMAGE_NAME} \
                bash -c "cd /workspace && rm -rf build_docker target-docker"
        else
            echo -e "${YELLOW}Image '${IMAGE_NAME}' not found; cleaning workspace directly.${NC}"
            rm -rf build_docker target-docker 2>/dev/null || true
            if [ -d build_docker ] || [ -d target-docker ]; then
                echo -e "${YELLOW}Host cleanup lacked permissions; using temporary ubuntu helper container.${NC}"
                docker run --rm -v "$(pwd):/workspace" ubuntu:24.04 \
                    bash -c "cd /workspace && rm -rf build_docker target-docker"
            fi
        fi
        echo -e "${GREEN}Clean completed!${NC}"
        ;;

    compose-up)
        echo -e "${YELLOW}Starting services with docker-compose...${NC}"
        ensure_image
        docker compose up -d dev
        echo -e "${GREEN}Container started. Connect with: docker compose exec dev /bin/bash${NC}"
        ;;

    compose-down)
        echo -e "${YELLOW}Stopping services...${NC}"
        docker compose down
        echo -e "${GREEN}Services stopped!${NC}"
        ;;

    create)
        echo -e "${YELLOW}Creating persistent dev container...${NC}"
        ensure_image
        if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            echo -e "${YELLOW}Container '${CONTAINER_NAME}' already exists; reusing it.${NC}"
            if ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
                echo -e "${YELLOW}Starting stopped container...${NC}"
                docker start ${CONTAINER_NAME}
            fi
            docker exec -it -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash
        else
            docker run -it "${DOCKER_SECURITY_OPTS[@]}" "${DOCKER_ENV_OPTS[@]}" -v "$(pwd):/workspace" -w /workspace --name ${CONTAINER_NAME} ${IMAGE_NAME} \
                bash -lc "echo 'Tip: BUILD_DIR is set to build_docker for CI/scripts.'; exec /bin/bash"
            echo -e "${GREEN}Container session ended. Use '$0 enter' to reconnect.${NC}"
        fi
        ;;

    enter)
        echo -e "${YELLOW}Entering persistent dev container...${NC}"
        if ! docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            echo -e "${YELLOW}Standalone container '${CONTAINER_NAME}' not found; using docker compose service 'dev'.${NC}"
            "$0" compose-up
            docker compose exec dev /bin/bash
            exit $?
        fi
        if ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
            echo -e "${YELLOW}Starting stopped container...${NC}"
            docker start ${CONTAINER_NAME}
        fi
        docker exec -it -e BUILD_DIR=build_docker ${CONTAINER_NAME} /bin/bash
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
        echo "  $0 ci shardNoReplication # Build and run shardNoReplication test"
        echo "  $0 ci shardNoReplication 8 # Use 8 build jobs for CI flow"
        echo "  $0 ci-quick shard2Replication # Run shard2Replication without rebuild"
        exit 1
        ;;
esac
