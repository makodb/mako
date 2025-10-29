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
        
    *)
        echo "Usage: $0 {build-image|build|shell|test|clean|compose-up|compose-down} [num_jobs]"
        echo ""
        echo "Commands:"
        echo "  build-image  - Build the Docker image"
        echo "  build       - Build dbtest in container (default)"
        echo "  shell       - Start interactive shell in container"
        echo "  test        - Run quick build test (libmako.a only)"
        echo "  clean       - Clean build artifacts"
        echo "  compose-up  - Start persistent dev container"
        echo "  compose-down - Stop persistent dev container"
        echo ""
        echo "Options:"
        echo "  num_jobs    - Number of parallel jobs for make (default: 32)"
        exit 1
        ;;
esac