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
        docker run --rm -v "$(pwd):/workspace" mako-build:ubuntu22 \
            bash -c "cd /workspace && \
                     rm -rf build && \
                     mkdir -p build && \
                     cd build && \
                     cmake .. && \
                     make -j${JOBS} mako && \
                     echo 'SUCCESS: libmako.a built' && \
                     ls -la libmako.a"
        echo -e "${GREEN}Test completed successfully!${NC}"
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
        echo "Usage: $0 {build-image|build|shell|create|enter|remove|test|clean|compose-up|compose-down} [num_jobs]"
        echo ""
        echo "Commands:"
        echo "  build-image  - Build the Docker image"
        echo "  build       - Build dbtest in container (default)"
        echo "  shell       - Start temporary interactive shell (auto-removed on exit)"
        echo "  create      - Create persistent dev container named 'mako-dev'"
        echo "  enter       - Enter existing 'mako-dev' container (auto-starts if stopped)"
        echo "  test        - Run quick build test (libmako.a only)"
        echo "  clean       - Clean build artifacts"
        echo "  compose-up  - Start persistent dev container via docker-compose"
        echo "  compose-down - Stop persistent dev container"
        echo ""
        echo "Options:"
        echo "  num_jobs    - Number of parallel jobs for make"
        echo "              Default: Auto-detected (1 on ARM Mac, 32 on x86_64)"
        echo ""
        echo "Platform Notes:"
        echo "  - Docker image uses native architecture by default"
        echo "  - Mac M4: Builds native ARM64 (fast, -j1 default due to resources)"
        echo "  - Windows/Linux x86_64: Builds native AMD64 (fast, -j32 default)"
        exit 1
        ;;
esac