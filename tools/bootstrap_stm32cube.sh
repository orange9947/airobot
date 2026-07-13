#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cube="$root/third_party/STM32CubeF1"

git -C "$root" submodule update --init --depth 1 third_party/STM32CubeF1
git -C "$cube" submodule update --init --depth 1 \
  Drivers/CMSIS/Device/ST/STM32F1xx \
  Drivers/STM32F1xx_HAL_Driver
