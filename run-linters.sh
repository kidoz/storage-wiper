#!/bin/bash
# run-linters.sh - Convenience script for running static analysis tools
# Usage: ./run-linters.sh [clang-tidy|cppcheck|all]

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_ROOT"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

run_clang_tidy() {
    echo -e "${YELLOW}=== Running clang-tidy ===${NC}"

    if ! command -v clang-tidy &> /dev/null; then
        echo -e "${RED}Error: clang-tidy not found. Install with: sudo pacman -S clang-tools-extra${NC}"
        return 1
    fi

    # Generate compile_commands.json if it doesn't exist
    if [ ! -f "compile_commands.json" ]; then
        echo "Generating compile_commands.json..."
        cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
    fi

    # Run clang-tidy on all source files
    find src -name "*.cpp" | while read -r file; do
        echo "Checking $file..."
        clang-tidy "$file" -p . -- -std=c++20 || true
    done

    echo -e "${GREEN}clang-tidy analysis complete!${NC}"
}

run_cppcheck() {
    echo -e "${YELLOW}=== Running cppcheck ===${NC}"

    if ! command -v cppcheck &> /dev/null; then
        echo -e "${RED}Error: cppcheck not found. Install with: sudo pacman -S cppcheck${NC}"
        return 1
    fi

    cppcheck \
        --enable=warning,style,performance,portability \
        --std=c++20 \
        --inline-suppr \
        --suppressions-list=.cppcheck-suppressions \
        --suppress=missingIncludeSystem \
        --quiet \
        -I include/ \
        src/

    echo -e "${GREEN}cppcheck analysis complete!${NC}"
}

run_all() {
    run_clang_tidy
    echo ""
    run_cppcheck
}

# Main script logic
case "${1:-all}" in
    clang-tidy)
        run_clang_tidy
        ;;
    cppcheck)
        run_cppcheck
        ;;
    all)
        run_all
        ;;
    *)
        echo "Usage: $0 [clang-tidy|cppcheck|all]"
        echo ""
        echo "Options:"
        echo "  clang-tidy  - Run only clang-tidy analysis"
        echo "  cppcheck    - Run only cppcheck analysis"
        echo "  all         - Run both tools (default)"
        exit 1
        ;;
esac

echo -e "${GREEN}Done!${NC}"
