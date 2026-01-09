#!/bin/bash
#
# Ring Buffer Stress Test
#
# Tests rapid connect/disconnect cycles to find race conditions
# in the FrameBufferRing lifecycle management.
#
# Prerequisites:
# - Android device connected via ADB
# - Test app installed (usbCameraTest or similar)
# - USB camera connected to device
#
# Usage:
#   ./scripts/stress_test_ring_buffer.sh [cycles] [delay_min_ms] [delay_max_ms]
#
# Arguments:
#   cycles       - Number of connect/disconnect cycles (default: 50)
#   delay_min_ms - Minimum delay between operations (default: 100)
#   delay_max_ms - Maximum delay between operations (default: 2000)
#

set -e

# Configuration
CYCLES=${1:-50}
DELAY_MIN=${2:-100}
DELAY_MAX=${3:-2000}
PACKAGE="com.serenegiant.usbcameratest"
LOG_TAG="libUVCCamera"

echo "═══════════════════════════════════════════════════════════════════"
echo "Ring Buffer Stress Test"
echo "═══════════════════════════════════════════════════════════════════"
echo "Cycles:    $CYCLES"
echo "Delay:     ${DELAY_MIN}ms - ${DELAY_MAX}ms (randomized)"
echo "Package:   $PACKAGE"
echo "═══════════════════════════════════════════════════════════════════"

# Check ADB connection
if ! adb devices | grep -q "device$"; then
    echo "ERROR: No Android device connected"
    exit 1
fi

# Clear logcat
echo "[*] Clearing logcat..."
adb logcat -c

# Start logging in background
LOG_FILE="/tmp/ring_buffer_stress_$(date +%Y%m%d_%H%M%S).log"
echo "[*] Logging to: $LOG_FILE"
adb logcat -v threadtime "$LOG_TAG:V" "*:S" > "$LOG_FILE" 2>&1 &
LOGCAT_PID=$!
trap "kill $LOGCAT_PID 2>/dev/null" EXIT

# Generate random delay
random_delay() {
    local range=$((DELAY_MAX - DELAY_MIN))
    local delay=$((DELAY_MIN + RANDOM % range))
    echo "$delay"
}

# Run stress test
echo ""
echo "[*] Starting stress test..."
echo ""

CRASHES=0
TIMEOUTS=0
MAGIC_ERRORS=0

for i in $(seq 1 $CYCLES); do
    delay=$(random_delay)
    printf "[%3d/%3d] Cycle (delay=${delay}ms)... " "$i" "$CYCLES"

    # Simulate app lifecycle (start/stop camera)
    # This sends an intent to toggle the camera preview
    adb shell am broadcast -a com.serenegiant.action.TOGGLE_CAMERA \
        --es command "start" 2>/dev/null || true

    sleep "$(echo "scale=3; $delay/1000" | bc)"

    adb shell am broadcast -a com.serenegiant.action.TOGGLE_CAMERA \
        --es command "stop" 2>/dev/null || true

    # Check for crashes in logcat
    if grep -q "SIGSEGV\|SIGABRT\|SIGBUS" "$LOG_FILE" 2>/dev/null; then
        echo "CRASH DETECTED!"
        ((CRASHES++))
    elif grep -q "MAGIC_CORRUPT" "$LOG_FILE" 2>/dev/null; then
        echo "MAGIC ERROR!"
        ((MAGIC_ERRORS++))
    elif grep -q "TIMEOUT" "$LOG_FILE" 2>/dev/null; then
        echo "TIMEOUT WARNING"
        ((TIMEOUTS++))
    else
        echo "OK"
    fi

    # Small pause between cycles
    sleep 0.1
done

echo ""
echo "═══════════════════════════════════════════════════════════════════"
echo "Results"
echo "═══════════════════════════════════════════════════════════════════"
echo "Total cycles:    $CYCLES"
echo "Crashes:         $CRASHES"
echo "Magic errors:    $MAGIC_ERRORS"
echo "Drain timeouts:  $TIMEOUTS"
echo "Log file:        $LOG_FILE"
echo ""

# Analyze log for specific patterns
echo "═══════════════════════════════════════════════════════════════════"
echo "Log Analysis"
echo "═══════════════════════════════════════════════════════════════════"

echo -n "LIFECYCLE entries:     "
grep -c "LIFECYCLE:" "$LOG_FILE" 2>/dev/null || echo "0"

echo -n "Callback drains:       "
grep -c "Callback drain complete" "$LOG_FILE" 2>/dev/null || echo "0"

echo -n "Ring buffers deleted:  "
grep -c "FrameBufferRing deleted successfully" "$LOG_FILE" 2>/dev/null || echo "0"

echo -n "Re-entrancy guards:    "
grep -c "already in progress" "$LOG_FILE" 2>/dev/null || echo "0"

echo -n "Layout validations:    "
grep -c "All validations passed" "$LOG_FILE" 2>/dev/null || echo "0"

echo ""

# Final status
if [ $CRASHES -eq 0 ] && [ $MAGIC_ERRORS -eq 0 ]; then
    echo "STATUS: PASS"
    exit 0
else
    echo "STATUS: FAIL"
    echo ""
    echo "To investigate, check the log file:"
    echo "  less $LOG_FILE"
    echo "  grep -E 'SIGSEGV|SIGABRT|MAGIC_CORRUPT' $LOG_FILE"
    exit 1
fi
