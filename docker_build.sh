#!/bin/bash

# Script to build and test Mako in Ubuntu 22.04 container

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== Mako Ubuntu 22.04 Docker Build Script ===${NC}"
echo

# Parse command line arguments
ACTION=${1:-build}
JOBS=${2:-32}

case "$ACTION" in
    build-image)
        echo -e "${YELLOW}Building Docker image...${NC}"
        docker build -f Dockerfile.ubuntu22 -t mako-build:ubuntu22 .
        echo -e "${GREEN}Docker image built successfully!${NC}"
        ;;
        
    build)
        echo -e "${YELLOW}Building Mako in container...${NC}"
        docker run --rm -v "$(pwd):/workspace" mako-build:ubuntu22 \
            bash -c "cd /workspace && \
                     rm -rf build && \
                     mkdir -p build && \
                     cd build && \
                     cmake .. && \
                     make -j${JOBS} dbtest"
        echo -e "${GREEN}Build completed successfully!${NC}"
        ;;
        
    shell)
        echo -e "${YELLOW}Starting interactive shell in container...${NC}"
        docker run --rm -it -v "$(pwd):/workspace" janus-build:ubuntu22 /bin/bash
        ;;
        
    test)
        echo -e "${YELLOW}Running build test in container...${NC}"
        docker run --rm -v "$(pwd):/workspace" -w /workspace janus-ci:ubuntu22 \
            bash -c "rm -rf build && make -j${JOBS} && \
                     echo 'SUCCESS: build completed' && \
                     ls -la build/dbtest"
        echo -e "${GREEN}Test completed successfully!${NC}"
        ;;

    ci)
        # Run a specific CI test or all tests
        CI_TEST=${2:-all}
        echo -e "${YELLOW}Running CI test '${CI_TEST}' in container...${NC}"
        docker run --rm -v "$(pwd):/workspace" -w /workspace janus-ci:ubuntu22 \
            bash -c "rm -rf build && make -j32 && ./ci/ci.sh ${CI_TEST}"
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;

    ci-quick)
        # Run CI tests without rebuild (assumes build exists and was built in Docker)
        CI_TEST=${2:-shardNoReplication}

        # Check if binary exists and was built for Docker (RUNPATH should be /workspace/build)
        if [ -f "build/dbtest" ]; then
            RUNPATH=$(readelf -d build/dbtest 2>/dev/null | grep RUNPATH | grep -o '\[.*\]' | tr -d '[]')
            if [ "$RUNPATH" != "/workspace/build" ]; then
                echo -e "${RED}Error: build/dbtest was built locally (RUNPATH: $RUNPATH)${NC}"
                echo -e "${YELLOW}Cannot run locally-built binary in Docker due to library path mismatch.${NC}"
                echo -e "${YELLOW}Use './docker_build.sh ci ${CI_TEST}' to rebuild and test in Docker.${NC}"
                exit 1
            fi
        else
            echo -e "${RED}Error: build/dbtest not found. Run './docker_build.sh ci' first.${NC}"
            exit 1
        fi

        echo -e "${YELLOW}Running CI test '${CI_TEST}' (no rebuild)...${NC}"
        docker run --rm -v "$(pwd):/workspace" -w /workspace janus-ci:ubuntu22 \
            bash -c "./ci/ci.sh ${CI_TEST}"
        echo -e "${GREEN}CI test '${CI_TEST}' completed!${NC}"
        ;;
        
    clean)
        echo -e "${YELLOW}Cleaning build artifacts...${NC}"
        docker run --rm -v "$(pwd):/workspace" mako-build:ubuntu22 \
            bash -c "cd /workspace && rm -rf build"
        echo -e "${GREEN}Clean completed!${NC}"
        ;;
        
    compose-up)
        echo -e "${YELLOW}Starting services with docker-compose...${NC}"
        docker-compose up -d ubuntu22-dev
        echo -e "${GREEN}Container started. Connect with: docker exec -it mako-ubuntu22-dev /bin/bash${NC}"
        ;;
        
    compose-down)
        echo -e "${YELLOW}Stopping services...${NC}"
        docker-compose down
        echo -e "${GREEN}Services stopped!${NC}"
        ;;

    create)
        echo -e "${YELLOW}Creating persistent dev container...${NC}"
        docker run -it -v "$(pwd):/workspace" --name mako-dev mako-build:ubuntu22 /bin/bash
        echo -e "${GREEN}Container session ended. Use '$0 enter' to reconnect.${NC}"
        ;;

    enter)
        echo -e "${YELLOW}Entering persistent dev container...${NC}"
        if ! docker ps -a --format '{{.Names}}' | grep -q '^mako-dev$'; then
            echo -e "${RED}Error: Container 'mako-dev' does not exist.${NC}"
            echo -e "${YELLOW}Create it first with: $0 create${NC}"
            exit 1
        fi
        if ! docker ps --format '{{.Names}}' | grep -q '^mako-dev$'; then
            echo -e "${YELLOW}Starting stopped container...${NC}"
            docker start mako-dev
        fi
        docker exec -it mako-dev /bin/bash
        ;;

    *)
        echo "Usage: $0 {build-image|build|shell|create|enter|test|ci|ci-quick|clean|compose-up|compose-down} [arg]"
        echo ""
        echo "Commands:"
        echo "  build-image  - Build the Docker image"
        echo "  build        - Build dbtest in container (default)"
        echo "  shell        - Start temporary interactive shell (auto-removed on exit)"
        echo "  create       - Create persistent dev container named 'mako-dev'"
        echo "  enter        - Enter existing 'mako-dev' container (auto-starts if stopped)"
        echo "  test         - Run quick build test"
        echo "  ci [test]    - Build and run CI test (default: all)"
        echo "  ci-quick [test] - Run CI test without rebuild (default: shardNoReplication)"
        echo "  clean        - Clean build artifacts"
        echo "  compose-up   - Start persistent dev container via docker-compose"
        echo "  compose-down - Stop persistent dev container"
        echo ""
        echo "CI Test Names:"
        echo "  all, simpleTransaction, simplePaxos, shardNoReplication,"
        echo "  shard1Replication, shard2Replication, shard1ReplicationSimple,"
        echo "  shard2ReplicationSimple, rocksdbTests, shardFaultTolerance,"
        echo "  multiShardSingleProcess, shard2SingleProcess, shard2SingleProcessReplication"
        echo ""
        echo "Examples:"
        echo "  $0 ci                    # Build and run all CI tests"
        echo "  $0 ci shardNoReplication # Build and run shardNoReplication test"
        echo "  $0 ci-quick shard2Replication # Run shard2Replication without rebuild"
        exit 1
        ;;
esac