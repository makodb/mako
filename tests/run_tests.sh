#!/bin/bash

# Mako Silo/STO Test Suite Runner
# Usage: ./run_tests.sh [unit|benchmark|all]

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

# Functions
print_banner() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_info() {
    echo -e "${YELLOW}→ $1${NC}"
}

# Build tests
build_tests() {
    print_banner "Building Test Suite"
    
    cd "$PROJECT_ROOT"
    
    # Add tests subdirectory to CMakeLists if not already there
    if ! grep -q "add_subdirectory(tests)" "$PROJECT_ROOT/CMakeLists.txt"; then
        print_info "Adding tests subdirectory to main CMakeLists.txt..."
        echo "# Tests subdirectory" >> "$PROJECT_ROOT/CMakeLists.txt"
        echo "add_subdirectory(tests)" >> "$PROJECT_ROOT/CMakeLists.txt"
    fi
    
    # Configure and build
    cmake -S . -B build
    cmake --build build --target test_silo_varint test_sto_transaction bench_silo_varint bench_sto_transaction -j$(nproc)
    
    print_success "Test suite built successfully"
}

# Run unit tests
run_unit_tests() {
    print_banner "Running Unit Tests"
    
    cd "$TEST_BUILD_DIR"
    
    print_info "Running Silo Varint Tests..."
    if ./test_silo_varint; then
        print_success "Silo Varint Tests PASSED"
    else
        print_error "Silo Varint Tests FAILED"
        return 1
    fi
    
    echo ""
    
    print_info "Running STO Transaction Tests..."
    if ./test_sto_transaction; then
        print_success "STO Transaction Tests PASSED"
    else
        print_error "STO Transaction Tests FAILED"
        return 1
    fi
    
    echo ""
    print_success "All unit tests PASSED"
}

# Run benchmarks
run_benchmarks() {
    print_banner "Running Performance Benchmarks"
    
    cd "$TEST_BUILD_DIR"
    
    print_info "Running Silo Varint Benchmarks..."
    ./bench_silo_varint --benchmark_format=console
    
    echo ""
    
    print_info "Running STO Transaction Benchmarks..."
    ./bench_sto_transaction --benchmark_format=console
    
    echo ""
    print_success "All benchmarks completed"
}

# Run CTest
run_ctest() {
    print_banner "Running Tests via CTest"
    
    cd "$BUILD_DIR"
    
    ctest --output-on-failure --verbose
    
    print_success "CTest completed"
}

# Generate test report
generate_report() {
    print_banner "Generating Test Report"
    
    REPORT_FILE="$PROJECT_ROOT/test_report.txt"
    
    cd "$TEST_BUILD_DIR"
    
    {
        echo "Mako Silo/STO Test Report"
        echo "Generated: $(date)"
        echo "=========================================="
        echo ""
        echo "Unit Test Results:"
        echo "===================="
        ./test_silo_varint --gtest_output=xml:silo_varint_results.xml
        ./test_sto_transaction --gtest_output=xml:sto_transaction_results.xml
        echo ""
        echo "Benchmark Results:"
        echo "===================="
        ./bench_silo_varint --benchmark_format=json > silo_varint_bench.json
        ./bench_sto_transaction --benchmark_format=json > sto_transaction_bench.json
        echo "Benchmark results saved to JSON files"
    } | tee "$REPORT_FILE"
    
    print_success "Test report generated: $REPORT_FILE"
}

# Main script logic
main() {
    local mode="${1:-all}"
    
    # Check if build is needed
    if [ ! -d "$TEST_BUILD_DIR" ] || [ ! -f "$TEST_BUILD_DIR/test_silo_varint" ]; then
        print_info "Tests not built, building now..."
        build_tests
    fi
    
    case "$mode" in
        unit)
            run_unit_tests
            ;;
        benchmark|bench)
            run_benchmarks
            ;;
        ctest)
            run_ctest
            ;;
        report)
            generate_report
            ;;
        build)
            build_tests
            ;;
        all)
            run_unit_tests
            echo ""
            run_benchmarks
            ;;
        *)
            echo "Usage: $0 [unit|benchmark|ctest|report|build|all]"
            echo ""
            echo "  unit      - Run unit tests only"
            echo "  benchmark - Run performance benchmarks only"
            echo "  ctest     - Run tests via CTest"
            echo "  report    - Generate detailed test report"
            echo "  build     - Build tests without running"
            echo "  all       - Run all tests and benchmarks (default)"
            exit 1
            ;;
    esac
}

# Run main
main "$@"
