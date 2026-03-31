#!/usr/bin/env bash
set -euo pipefail

# Automated capture test — launches a command, records video frames with Metal HUD,
# collects logs, and optionally captures a .gputrace for Xcode debugging.

OUTPUT_DIR="./test_output"
DURATION=3
CAPTURE=0
EXE_NAME=""
FRAME_NUM=100
CHILD_PID=""

usage() {
  cat <<'EOF'
Usage: capture_test.sh [OPTIONS] -- <command to launch>

Options:
  -o DIR       Output directory (default: ./test_output)
  -t SECS      Recording duration after window appears (default: 3)
  -e NAME      Executable name for DXMT_CAPTURE_EXECUTABLE (without .exe)
  -f NUM       Frame to capture (default: 100, only with --capture)
  --capture    Enable .gputrace capture; waits for trace file instead of timeout

Examples:
  capture_test.sh -t 5 -o ./caps -- /Applications/turtle/run2.sh
  capture_test.sh --capture -e WoW_tweaked -f 300 -- /Applications/turtle/run2.sh
EOF
  exit 1
}

cleanup() {
  if [[ -n "$CHILD_PID" ]]; then
    # Kill the entire process group (child + all descendants)
    echo "Stopping process group $CHILD_PID..."
    kill -TERM -"$CHILD_PID" 2>/dev/null || true
    for i in 1 2 3; do
      kill -0 "$CHILD_PID" 2>/dev/null || break
      sleep 1
    done
    kill -KILL -"$CHILD_PID" 2>/dev/null || true
  fi
  # Clean up Wine
  wineserver -k 2>/dev/null || true
}

trap cleanup EXIT INT TERM

# --- Argument parsing ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o) OUTPUT_DIR="$2"; shift 2 ;;
    -t) DURATION="$2"; shift 2 ;;
    -e) EXE_NAME="$2"; shift 2 ;;
    -f) FRAME_NUM="$2"; shift 2 ;;
    --capture) CAPTURE=1; shift ;;
    --) shift; break ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1"; usage ;;
  esac
done

if [[ $# -eq 0 ]]; then
  echo "Error: no launch command specified after --"
  usage
fi

if [[ $CAPTURE -eq 1 && -z "$EXE_NAME" ]]; then
  echo "Error: --capture requires -e NAME"
  exit 1
fi

mkdir -p "$OUTPUT_DIR"

# --- Environment setup ---
export MTL_HUD_ENABLED=1

find_wine_window() {
  swift -e '
import CoreGraphics
let windows = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as! [[String: Any]]
for w in windows {
    let owner = w["kCGWindowOwnerName"] as? String ?? ""
    if owner.lowercased().contains("wine") {
        let layer = w["kCGWindowLayer"] as? Int ?? -1
        if layer == 0 {
            print(w["kCGWindowNumber"] as? Int ?? 0)
            break
        }
    }
}
' 2>/dev/null
}

capture_frames() {
  local window_id="$1"
  local output_dir="$2"
  local duration="$3"
  local frame=0
  local end_time=$(( $(date +%s) + duration ))

  while [[ $(date +%s) -lt $end_time ]]; do
    local name
    name=$(printf 'frame_%04d.png' "$frame")
    if screencapture -l "$window_id" "$output_dir/$name" 2>/dev/null; then
      frame=$((frame + 1))
    else
      break
    fi
  done

  echo "$frame frames in ${duration}s"
}

if [[ $CAPTURE -eq 1 ]]; then
  export MTL_CAPTURE_ENABLED=1
  export DXMT_CAPTURE_EXECUTABLE="$EXE_NAME"
  export DXMT_CAPTURE_FRAME="$FRAME_NUM"
  echo "Capture mode: waiting for ${EXE_NAME}_F.${FRAME_NUM}_*.gputrace"
else
  echo "Recording mode: ${DURATION}s after window appears"
fi

# --- Launch in its own process group ---
echo "Launching: $*"
set -m  # job control: background jobs get their own process group
"$@" 2>"$OUTPUT_DIR/stderr.log" &
CHILD_PID=$!
set +m
echo "PID: $CHILD_PID"

# --- Wait for Wine window to appear ---
echo "Waiting for Wine window..."
WINDOW_ID=""
while kill -0 "$CHILD_PID" 2>/dev/null; do
  WINDOW_ID=$(find_wine_window)
  if [[ -n "$WINDOW_ID" && "$WINDOW_ID" != "0" ]]; then
    echo "Window found (ID $WINDOW_ID)"
    break
  fi
  sleep 1
done

# --- Record frames ---
FRAME_COUNT_MSG=""
if [[ -n "$WINDOW_ID" && "$WINDOW_ID" != "0" ]]; then
  echo "Recording frames..."
  FRAME_COUNT_MSG=$(capture_frames "$WINDOW_ID" "$OUTPUT_DIR" "$DURATION") || true
  if [[ -n "$FRAME_COUNT_MSG" ]]; then
    echo "Recorded $FRAME_COUNT_MSG"
  else
    echo "Warning: frame capture failed (grant Screen Recording permission in System Settings)"
  fi
else
  echo "Warning: no Wine window found, skipping frame capture"
fi

# --- Wait for gputrace if in capture mode ---
if [[ $CAPTURE -eq 1 ]]; then
  echo "Waiting for .gputrace file..."
  while true; do
    TRACE_FILE=$(find . -maxdepth 1 -name "${EXE_NAME}_F.${FRAME_NUM}_*.gputrace" -print -quit 2>/dev/null || true)
    if [[ -n "$TRACE_FILE" ]]; then
      echo "Found: $TRACE_FILE"
      break
    fi
    if ! kill -0 "$CHILD_PID" 2>/dev/null; then
      echo "Process exited before trace was captured"
      break
    fi
    sleep 2
  done
fi

# --- Collect artifacts ---
if [[ $CAPTURE -eq 1 && -n "${TRACE_FILE:-}" ]]; then
  mv "$TRACE_FILE" "$OUTPUT_DIR/"
  TRACE_BASENAME=$(basename "$TRACE_FILE")
fi

echo ""
echo "=== Test Results ==="
FRAME_FILES=$(find "$OUTPUT_DIR" -name 'frame_*.png' 2>/dev/null | wc -l | tr -d ' ')
echo "Frames:      ${FRAME_FILES} in $OUTPUT_DIR/"
echo "Stderr Log:  $OUTPUT_DIR/stderr.log"
if [[ $CAPTURE -eq 1 && -n "${TRACE_BASENAME:-}" ]]; then
  echo "GPU Trace:   $OUTPUT_DIR/$TRACE_BASENAME"
fi
