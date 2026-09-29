# SplendidDarwin – AirPlay Audio Receiver for Android 2.3

## Предварительные требования

| Инструмент | Версия |
|---|---|
| Android NDK | **r14b** (последняя с поддержкой API 10 + GCC) |
| Android SDK Build-tools | 28.0.3 |
| Gradle | 6.1.1 (через wrapper) |
| AGP | 3.6.4 |
| Java | 8 |

> **NDK r14b** скачать: https://github.com/android/ndk/wiki/Unsupported-Downloads

## Быстрый старт

```bash
export ANDROID_NDK=/path/to/android-ndk-r14b
export ANDROID_SDK=/path/to/android-sdk
chmod +x build.sh
./build.sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.splendid.airplay/.ui.MainActivity
```

## Структура проекта

```
splendid-darwin/
├── build.gradle             ← root (AGP 3.6.4)
├── settings.gradle
├── build.sh                 ← ndk-build + gradle wrapper
└── app/
    ├── build.gradle         ← minSdk=10, armeabi-v7a, ndk-build
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/com/splendid/airplay/
        │   ├── jni/
        │   │   └── AirPlayNative.java     ← JNI wrapper + callbacks
        │   ├── service/
        │   │   ├── AirPlayService.java    ← Foreground Service
        │   │   └── BootReceiver.java
        │   └── ui/
        │       └── MainActivity.java
        ├── jni/
        │   ├── Android.mk / Application.mk
        │   ├── airplay_jni.c              ← JNI bridge
        │   ├── mdns/
        │   │   ├── mdns.c/h               ← mDNS socket + loop
        │   │   └── service_announce.c/h   ← DNS-SD packet builder
        │   ├── rtsp/
        │   │   ├── rtsp_server.c/h        ← TCP accept loop
        │   │   └── rtsp_session.c/h       ← RTSP parser + dispatcher
        │   ├── alac/
        │   │   └── alac_decoder.c/h       ← ALAC stub (Phase 1)
        │   └── audio/
        │       └── opensl_output.c/h      ← OpenSL ES double-buffer
        └── res/
            ├── layout/activity_main.xml
            └── values/strings.xml
```

## Фазы разработки

| Фаза | Что делается |
|---|---|
| **1 (текущая)** | mDNS анонс, скелет RTSP, заглушка ALAC, OpenSL ES каркас — устройство видно в списке AirPlay |
| **2** | AES-CBC расшифровка потока (mbedTLS), реальный ALAC-декодер, RTP-приёмник |
| **3** | Криптографический handshake AirPlay 2 (MFiSAP/FairPlay), метаданные прогресса |
| **4** | Оптимизация latency, NEON-ускоренный ALAC, буферизация jitter |

## Ключевые технические заметки

- **MulticastLock** обязателен на Android ≥2.3 для получения multicast UDP.
- **OpenSL ES** доступен с API 9 — используем `SLAndroidSimpleBufferQueue` (не `SLBufferQueue`).  
- **NDK r14b** — последняя версия с `gnustl` и GCC 4.9; для API 10 нужен именно он.
- **AGP 3.6.4 + Gradle 6.1.1** — последняя пара, поддерживающая `minSdkVersion 10` без предупреждений.
- ALAC Phase-1 выводит тишину — это нормально; цель фазы — увидеть устройство в AirPlay-списке.
