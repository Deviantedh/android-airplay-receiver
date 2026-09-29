#!/usr/bin/env bash
# build.sh  –  Полная сборка и деплой AirPlay Receiver
#
# Использование:
#   ./build.sh           – собрать и установить на подключённый телефон
#   ./build.sh clean     – очистить артефакты сборки
#   ./build.sh log       – показать logcat с фильтром по нашим тегам

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── Пути к инструментам ────────────────────────────────────────────────────
NDK="${NDK:-/Users/devianted/android-ndk-r14b}"
GRADLE="${GRADLE:-/Users/devianted/gradle-6.1.1/bin/gradle}"
JAVA_HOME="${JAVA_HOME:-/Library/Java/JavaVirtualMachines/temurin-11.jdk/Contents/Home}"
SDK="/opt/homebrew/share/android-commandlinetools"
ADB="$SDK/platform-tools/adb"

export JAVA_HOME
export ANDROID_SDK_ROOT="$SDK"

JNI="$SCRIPT_DIR/app/src/main/jni"
APK="$SCRIPT_DIR/app/build/outputs/apk/debug/app-debug.apk"

# ── Команды ────────────────────────────────────────────────────────────────

if [ "${1:-}" = "clean" ]; then
  echo "→ Clean"
  rm -rf "$SCRIPT_DIR/app/build" "$SCRIPT_DIR/app/src/main/libs"
  exit 0
fi

if [ "${1:-}" = "log" ]; then
  "$ADB" logcat -s mDNS SA RTSP-SRV RTSP-SES ALAC OpenSL AirPlayJNI AirPlayService
  exit 0
fi

echo "════════════════════════════════════"
echo " SplendidDarwin AirPlay Receiver"
echo "════════════════════════════════════"

# ── Step 1: ndk-build (через Rosetta 2 на Apple Silicon) ──────────────────
echo ""
echo "▶ ndk-build (armeabi-v7a, API 10)..."
arch -x86_64 "$NDK/ndk-build" \
    NDK_PROJECT_PATH="$SCRIPT_DIR/app" \
    APP_BUILD_SCRIPT="$JNI/Android.mk" \
    NDK_APPLICATION_MK="$JNI/Application.mk" \
    NDK_OUT="$SCRIPT_DIR/app/build/ndk-obj" \
    NDK_LIBS_OUT="$SCRIPT_DIR/app/src/main/libs" \
    NDK_HOST_AWK=/opt/homebrew/bin/gawk \
    -j4

echo "✓ Native build OK"

# ── Step 2: Gradle assembleDebug ──────────────────────────────────────────
echo ""
echo "▶ gradle assembleDebug..."
"$GRADLE" -p "$SCRIPT_DIR" assembleDebug --no-daemon -q

echo "✓ APK: $APK"
ls -lh "$APK"

# ── Step 3: ADB install ───────────────────────────────────────────────────
echo ""
DEVICES=$("$ADB" devices | grep -v "List of" | grep "device$" | wc -l | tr -d ' ')
if [ "$DEVICES" -eq "0" ]; then
  echo "⚠  Телефон не найден (ADB). Подключите GT-S6500D и включите USB-отладку."
  echo "   APK находится по адресу: $APK"
else
  echo "▶ Устанавливаем APK..."
  "$ADB" install -r "$APK"
  echo ""
  echo "▶ Запускаем приложение..."
  "$ADB" shell am start -n com.splendid.airplay/.ui.MainActivity
  echo ""
  echo "✓ Готово! Откройте AirPlay на iPhone/Mac и выберите «SplendidDarwin»"
  echo ""
  echo "  Логи (Ctrl+C для выхода):"
  echo "  $ADB logcat -s AirPlayJNI AirPlayService mDNS SA RTSP-SRV RTSP-SES"
fi
