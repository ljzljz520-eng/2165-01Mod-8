#!/usr/bin/env bash
set -euo pipefail

WORKDIR="$(cd "$(dirname "$0")" && pwd)"
cd "$WORKDIR"

make >/dev/null

output_file="$(mktemp)"
trap 'rm -f "$output_file"' EXIT

TD_MAX_WAVES=10 TD_TICK_MS=0 TD_WAVE_CLEAR_MS=0 ./arknights_td >"$output_file" <<'EOF'
place 3 2
place 5 4
start
upgrade 3 2
place 7 4
start
upgrade 5 4
place 8 4
start
upgrade 7 4
place 9 4
start
upgrade 8 4
place 10 4
start
upgrade 9 4
place 2 2
start
upgrade 10 4
place 6 4
start
upgrade 2 2
place 5 3
start
upgrade 6 4
upgrade 5 3
start
upgrade 3 2
upgrade 5 4
start
EOF

if grep -q "基地失守，游戏失败" "$output_file"; then
    echo "SMOKE TEST FAILED: game ended in defeat"
    tail -n 60 "$output_file"
    exit 1
fi

if ! grep -q "恭喜通关！你成功守住了所有波次。" "$output_file"; then
    echo "SMOKE TEST FAILED: victory message not found"
    tail -n 60 "$output_file"
    exit 1
fi

if ! grep -q "波次: 10/10" "$output_file"; then
    echo "SMOKE TEST FAILED: final wave summary does not show 10/10"
    tail -n 60 "$output_file"
    exit 1
fi

completed_waves="$(grep -E -c "波次 [0-9]+ 完成！额外获得 5 费用。" "$output_file")"
if [[ "$completed_waves" -ne 10 ]]; then
    echo "SMOKE TEST FAILED: expected 10 cleared waves, got $completed_waves"
    tail -n 60 "$output_file"
    exit 1
fi

echo "SMOKE TEST PASSED: cleared 10/10 waves with no failure."
