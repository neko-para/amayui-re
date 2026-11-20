#!/bin/bash
# 每次跑前：重建本次私有 appdata（saves/ 只读，复制进去；⛔ 不就地写 saves/）
# 用法：clean-appdata.sh <appdata-dir>（任意 cwd 可跑；仓库根由 git 解析）
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
APP="${1:?usage: clean-appdata.sh <appdata-dir>}"
GAME="$APP/Local/Eushully/天結いキャッスルマイスター"
rm -rf "$APP"
mkdir -p "$GAME/SAVE"
cp "$ROOT"/saves/SAVE*.DAT "$ROOT"/saves/SAVE*.STH "$ROOT"/saves/SAVE.DAT "$ROOT"/saves/RT.DAT "$GAME/SAVE/"
rm -f "$GAME/\$\$AGE.TMP" "$GAME/SAVE/SAVE.BAK"
