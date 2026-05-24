#!/bin/bash
# Test script for MystralNative examples
# Runs each example in headless mode and captures a screenshot
# Usage: ./scripts/test-examples.sh
# After running, check the screenshots in /tmp/mystral-test-*

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
MYSTRAL="$ROOT_DIR/build/mystral"
OUTPUT_DIR="/tmp"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== MystralNative Example Tests ==="
echo "Output directory: $OUTPUT_DIR"
echo ""

# Check if mystral binary exists
if [ ! -f "$MYSTRAL" ]; then
    echo -e "${RED}Error: mystral binary not found at $MYSTRAL${NC}"
    echo "Run 'cmake --build build' first"
    exit 1
fi

# Array to track results
declare -a PASSED=()
declare -a FAILED=()

# Function to run a test
run_test() {
    local name="$1"
    local script="$2"
    local frames="${3:-60}"
    local output="$OUTPUT_DIR/mystral-test-$name.png"

    echo -n "Testing $name... "

    if [ ! -f "$ROOT_DIR/$script" ]; then
        echo -e "${RED}SKIP (file not found)${NC}"
        return
    fi

    # Run the example
    if timeout 30 "$MYSTRAL" run "$ROOT_DIR/$script" --headless --screenshot "$output" --frames "$frames" > /tmp/mystral-test-$name.log 2>&1; then
        if [ -f "$output" ]; then
            # Check file size (should be > 1KB for a valid PNG)
            size=$(stat -f%z "$output" 2>/dev/null || stat --printf="%s" "$output" 2>/dev/null || echo "0")
            if [ "$size" -gt 1000 ]; then
                echo -e "${GREEN}PASS${NC} ($output)"
                PASSED+=("$name")
            else
                echo -e "${RED}FAIL (screenshot too small: ${size} bytes)${NC}"
                FAILED+=("$name")
            fi
        else
            echo -e "${RED}FAIL (no screenshot created)${NC}"
            FAILED+=("$name")
        fi
    else
        echo -e "${RED}FAIL (runtime error)${NC}"
        echo "  Log: /tmp/mystral-test-$name.log"
        FAILED+=("$name")
    fi
}

# Run tests
echo "--- Canvas 2D Tests ---"
run_test "impact" "examples/impact-test.js" 60
run_test "canvas2d" "examples/canvas2d-test.js" 30

echo ""
echo "--- PixiJS Tests ---"
run_test "pixi-test" "examples/pixi-test.js" 60
run_test "pixi-hello" "examples/pixi-hello.js" 60
run_test "pixi-alpha-test" "examples/pixi-alpha-test.js" 60

echo ""
echo "--- Three.js Tests ---"
run_test "threejs-cube" "examples/threejs-cube.js" 60
run_test "threejs-text" "examples/threejs-text.js" 60
run_test "threejs-docs-demo" "examples/threejs-docs-demo.js" 60

echo ""
echo "--- Mystral Engine Tests ---"
run_test "mystral-helmet" "examples/mystral-helmet.js" 120
run_test "sponza" "examples/sponza.js" 120

echo ""
echo "--- WebGPU Primitive Tests ---"
run_test "rotating-cube" "examples/rotating-cube.js" 60
run_test "simple-cube" "examples/simple-cube.js" 60

echo ""
echo "=== Summary ==="
echo -e "Passed: ${GREEN}${#PASSED[@]}${NC}"
echo -e "Failed: ${RED}${#FAILED[@]}${NC}"

if [ ${#FAILED[@]} -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED[@]}"; do
        echo "  - $test (log: /tmp/mystral-test-$test.log)"
    done
fi

echo ""
echo "=== Screenshot Paths ==="
echo "View these screenshots to verify rendering:"
for test in "${PASSED[@]}"; do
    echo "  $OUTPUT_DIR/mystral-test-$test.png"
done

# Exit with error if any tests failed
if [ ${#FAILED[@]} -gt 0 ]; then
    exit 1
fi
