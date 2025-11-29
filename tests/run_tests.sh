#!/bin/bash

# Mako Silo/STO Test Suite Runner
# Usage: ./run_tests.sh [all]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_BUILD_DIR="$BUILD_DIR/tests"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Run unit tests
run_unit_tests() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}Running Unit Tests${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    cd "$TEST_BUILD_DIR"
    
    echo -e "${YELLOW}→ Running Silo Varint Tests...${NC}"
    if ./test_silo_varint; then
        echo -e "${GREEN}✓ Silo Varint Tests PASSED${NC}"
    else
        echo -e "${RED}✗ Silo Varint Tests FAILED${NC}"
        return 1
    fi
    
    echo ""
    
    echo -e "${YELLOW}→ Running Silo RCU+Thread Tests...${NC}"
    if ./test_silo_rcu_thread; then
        echo -e "${GREEN}✓ Silo RCU+Thread Tests PASSED${NC}"
    else
        echo -e "${RED}✗ Silo RCU+Thread Tests FAILED${NC}"
        return 1
    fi
    
    echo ""
    
    echo -e "${YELLOW}→ Running STO Transaction Tests...${NC}"
    if ./test_sto_trcu; then
        echo -e "${GREEN}✓ STO Transaction Tests PASSED${NC}"
    else
        echo -e "${RED}✗ STO Transaction Tests FAILED${NC}"
        return 1
    fi
    
    echo ""
    
    echo -e "${YELLOW}→ Running Silo Allocator+Tuple Tests...${NC}"
    if ./test_silo_allocator_tuple; then
        echo -e "${GREEN}✓ Silo Allocator+Tuple Tests PASSED${NC}"
    else
        echo -e "${RED}✗ Silo Allocator+Tuple Tests FAILED${NC}"
        return 1
    fi
    
    echo ""
    echo -e "${GREEN}✓ All unit tests PASSED${NC}"
}

# Main script logic
main() {
    local mode="${1:-all}"
    
    case "$mode" in
        all)
            run_unit_tests
            ;;
        *)
            echo "Usage: $0 [all]"
            echo ""
            echo "  all - Run all tests (default)"
            exit 1
            ;;
    esac
}

# Run main
main "$@"
