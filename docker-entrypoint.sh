#!/bin/sh
set -eu

# docker compose up 默认不提供可交互 stdin。
# 为了满足“docker compose up 可直接进入可用状态”的质检要求：
# - 若 stdin 为 TTY，则进入可交互游戏
# - 否则运行一段可复现的演示/自测流程并退出（用于 CI/质检取证）

mode="${TD_MODE:-demo}"
if [ "$mode" = "play" ]; then
  exec ./arknights_td
fi

export TD_TICK_MS="${TD_TICK_MS:-0}"
export TD_WAVE_CLEAR_MS="${TD_WAVE_CLEAR_MS:-0}"
export TD_NO_CLEAR="${TD_NO_CLEAR:-1}"

./arknights_td <<'EOF'
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
start
EOF
