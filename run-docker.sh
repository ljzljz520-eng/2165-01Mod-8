#!/usr/bin/env bash
set -euo pipefail

docker compose build
docker compose run --rm -it -e TD_MODE=play game
