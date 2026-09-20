#!/usr/bin/env bash
#
# 对 App/*.c 做编译检查 —— 不需要硬件，也不需要打开 Keil。
#
# 原理：调用 Keil 自带的 armcc（ARMCC V5），只编译不链接（-c），
# 头文件路径直接指向工程里的真 HAL 源码和本机 Cube 包里的 FreeRTOS，
# 所以它能查出**真实的**语法与语义错误，不是玩具检查。
#
#   能查出：未声明的变量/函数、函数签名不匹配、类型错误、少分号、
#           结构体成员名写错、CMSIS-RTOS2 API 用错……
#   查不出：链接错误（未定义的外部符号）、任何运行时行为
#
# 用法：
#   bash Tools/check_app.sh              # 检查全部 App/*.c
#   bash Tools/check_app.sh app_i2c.c    # 只检查某一个
#
# 退出码：0 = 全部通过；1 = 有文件没通过

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# Keil 装在哪块盘、哪个目录因人而异（本机是 D:/keil，但另一台可能是
# C:/Keil_v5），写死一个路径的话换台机器脚本就废了。
# 所以：先用 ARMCC 环境变量，没设就在常见位置里找一遍。
ARMCC="${ARMCC:-}"
if [ -z "$ARMCC" ]; then
  for cand in D:/keil E:/keil C:/keil G:/keil \
              C:/Keil_v5 D:/Keil_v5 E:/Keil_v5 \
              "C:/Program Files/Keil_v5" "C:/Program Files (x86)/Keil_v5"; do
    if [ -x "$cand/ARM/ARMCC/Bin/armcc.exe" ]; then
      ARMCC="$cand/ARM/ARMCC/Bin/armcc.exe"
      break
    fi
  done
fi

FW="${CUBE_FW:-C:/Users/Administrator/STM32Cube/Repository/STM32Cube_FW_F1_V1.8.4}"

if [ -z "$ARMCC" ] || [ ! -x "$ARMCC" ]; then
  echo "找不到 armcc（找过 D:/keil、C:/Keil_v5 等常见位置）。" >&2
  echo "如果你的 Keil 装在别处，用环境变量指定：ARMCC=/你的路径/armcc.exe bash Tools/check_app.sh" >&2
  exit 1
fi

if [ ! -d "$FW/Middlewares/Third_Party/FreeRTOS/Source" ]; then
  echo "找不到 CubeF1 固件包：$FW" >&2
  echo "可用环境变量 CUBE_FW 指定实际路径。" >&2
  exit 1
fi

INCLUDES=(
  "$SCRIPT_DIR/syntax_stub"                                   # 桩 main.h（覆盖真 main.h）
  "$ROOT/App"
  "$ROOT/Inc"                                                 # hal_conf.h 在这里
  "$ROOT/Drivers/STM32F1xx_HAL_Driver/Inc"
  "$ROOT/Drivers/CMSIS/Device/ST/STM32F1xx/Include"
  "$ROOT/Drivers/CMSIS/Include"
  # ★ 兜底路径，必须放在工程自己的 Drivers 之后。
  #
  # CubeMX 勾了 "Copy only necessary library files"，所以工程里**只有当前
  # 用到**的 HAL 模块源码。App/ 引用了 CubeMX 还没配的外设时（比如现在的
  # UART / ADC / IWDG），那些头文件在工程里根本不存在。
  # 这里回落到本机 Cube 包的完整 HAL 目录，缺什么补什么。
  # 工程自己的头文件排在前面，所以不会覆盖已复制的版本。
  "$FW/Drivers/STM32F1xx_HAL_Driver/Inc"
  "$FW/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2"
  "$FW/Middlewares/Third_Party/FreeRTOS/Source/include"
  "$FW/Middlewares/Third_Party/FreeRTOS/Source/portable/RVDS/ARM_CM3"
)

INC_ARGS=()
for d in "${INCLUDES[@]}"; do
  INC_ARGS+=("-I" "$d")
done

# ★ 预先打开这次要用到的 HAL 模块。
#
# CubeMX 生成的 Inc/stm32f1xx_hal_conf.h 会把**没用到**的模块写成
# 注释形式（`/*#define HAL_UART_MODULE_ENABLED   */`），而后面用
# `#ifdef` 包住对应头文件的 include。所以命令行 -D 能顶替它生效。
#
# 为什么要这么做：App/ 的代码往往写在 CubeMX 配置完成之前。此时
# HAL_UART_MODULE_ENABLED 还没打开，UART_HandleTypeDef 就是未定义类型，
# 检查会报一堆假错误，把真问题淹掉。
MODULE_DEFS=(
  -DHAL_UART_MODULE_ENABLED
  -DHAL_ADC_MODULE_ENABLED
  -DHAL_IWDG_MODULE_ENABLED
)

OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

if [ "$#" -gt 0 ]; then
  FILES=("$@")
else
  FILES=()
  for f in "$ROOT"/App/*.c; do
    [ -e "$f" ] && FILES+=("$(basename "$f")")
  done
fi

if [ "${#FILES[@]}" -eq 0 ]; then
  echo "App/ 下没有 .c 文件"
  exit 0
fi

pass=0
fail=0

for name in "${FILES[@]}"; do
  src="$ROOT/App/$name"
  if [ ! -f "$src" ]; then
    echo "[跳过] $name —— 文件不存在"
    continue
  fi

  out="$("$ARMCC" --c99 -c --cpu Cortex-M3 \
          -DUSE_HAL_DRIVER -DSTM32F103xB "${MODULE_DEFS[@]}" \
          "${INC_ARGS[@]}" \
          -o "$OUT_DIR/${name%.c}.o" "$src" 2>&1)"
  rc=$?

  if [ $rc -eq 0 ]; then
    echo "[通过] $name"
    pass=$((pass + 1))
  else
    echo "[失败] $name"
    # 只打印错误行和紧随其后的源码行，别把整个日志倒出来
    echo "$out" | grep -E "Error:|Warning:|error:|warning:" | head -20
    fail=$((fail + 1))
  fi
done

echo "----------------------------------------"
echo "通过 $pass / 共 $((pass + fail))"

[ "$fail" -eq 0 ]
