# Android AirPlay Receiver (Native C / NDK / JNI)

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Android_2.3%2B_(API_10)-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Android 2.3+" />
  <img src="https://img.shields.io/badge/Language-C99%20%2F%20Java-00599C?style=flat-square&logo=c&logoColor=white" alt="C99 / Java" />
  <img src="https://img.shields.io/badge/NDK-r14b_(GCC_4.9)-blue?style=flat-square" alt="NDK r14b" />
  <img src="https://img.shields.io/badge/Audio-OpenSL_ES-orange?style=flat-square" alt="OpenSL ES" />
  <img src="https://img.shields.io/badge/License-MIT-success?style=flat-square" alt="License" />
</p>

A high-performance, low-level native AirPlay audio receiver engineered specifically for legacy ARMv7 Android hardware (targeting Android 2.3.3+ / API 10). Built with a native C core, custom RFC 6762 mDNS / DNS-SD announcer, RTSP session engine, and OpenSL ES audio playback pipeline.

---

## Architecture Overview

```
Apple Device (iOS / macOS)
        │
        ├── [UDP 5353]  mDNS Discovery (RAOP / AirTunes service broadcast)
        ├── [TCP 5000]  RTSP Control (ANNOUNCE, SETUP, RECORD, PAUSE, TEARDOWN)
        └── [UDP RTP]   Encrypted ALAC Audio Stream
                │
                ▼
Android Device (API 10+ / ARMv7)
 ┌──────────────────────────────────────────────────────────────┐
 │ Native C Core (libairplay_core.so)                           │
 │                                                              │
 │  ┌─────────────────┐   ┌─────────────────┐   ┌────────────┐  │
 │  │  mDNS Responder │   │   RTSP Server   │   │ AES Crypto │  │
 │  │ (Unicast/Multi) │   │ (Keepalive/DACP)│   │  & Decrypt │  │
 │  └─────────────────┘   └─────────────────┘   └────────────┘  │
 │           │                     │                   │        │
 │           ▼                     ▼                   ▼        │
 │  ┌────────────────────────────────────────────────────────┐  │
 │  │            OpenSL ES Double-Buffered Output            │  │
 │  └────────────────────────────────────────────────────────┘  │
 └──────────────────────────────┬───────────────────────────────┘
                                │ JNI Callbacks
                                ▼
 ┌──────────────────────────────────────────────────────────────┐
 │ Java Android UI & Foreground Service (Retro iPod/iOS UI)     │
 └──────────────────────────────────────────────────────────────┘
```

---

## Key Technical Achievements

* **Targeting Legacy Hardware:** Successfully runs on Android 2.3.3 (API 10) hardware with strict memory and CPU constraints.
* **Low-Level Native C Implementation:** Core network services (mDNS, RTSP, cryptography, audio decompression) run purely in native C via JNI for minimal overhead.
* **Custom mDNS / DNS-SD Engine:** Hand-crafted binary packet generator broadcasting `_raop._tcp` and `_airplay._tcp` records. Includes direct unicast reply support to prevent packet loss on strict Wi-Fi APs.
* **Low-Latency Audio Queue:** OpenSL ES native audio pipeline using double-buffering with `SLAndroidSimpleBufferQueue`.
* **State & Keepalive Management:** Robust RTSP connection handling supporting PAUSE/RESUME without tearing down state, plus reverse DACP remote control resolution.

---

## Project Structure

```
.
├── app/
│   ├── build.gradle              # minSdk=10, armeabi-v7a target
│   └── src/main/
│       ├── AndroidManifest.xml   # Foreground service, MulticastLock & wake locks
│       ├── java/                 # Android UI & Service layer
│       │   └── com/splendid/airplay/
│       │       ├── jni/AirPlayNative.java    # JNI Bridge
│       │       ├── service/AirPlayService.java # Background Service
│       │       └── ui/MainActivity.java      # Player UI (Retro iPod aesthetic)
│       └── jni/                  # Native C engine
│           ├── mdns/             # Custom mDNS / DNS-SD responder
│           ├── rtsp/             # RTSP session server & parser
│           ├── crypto/           # AES-CBC decryption & RSA handshake
│           ├── alac/             # ALAC decoder
│           └── audio/            # OpenSL ES buffer queue driver
├── build.sh                      # Unified build script (ndk-build + gradle wrapper)
└── README.md
```

---

## Prerequisites & Build Guide

### Required Toolchain

| Tool | Version | Notes |
| :--- | :--- | :--- |
| **Android NDK** | `r14b` | Required: Last version supporting GCC 4.9 and API 10 |
| **Android SDK Build-tools** | `28.0.3` | Installed via `sdkmanager` |
| **Gradle** | `6.1.1` | Run via `./gradlew` wrapper |
| **Android Gradle Plugin** | `3.6.4` | Last version supporting API 10 without warnings |
| **Java JDK** | `8` | For AGP 3.6.4 compatibility |

### Building from Source

```bash
# 1. Set environment variables
export ANDROID_NDK=/path/to/android-ndk-r14b
export ANDROID_SDK=/path/to/android-sdk

# 2. Build native C libraries and assemble APK
chmod +x build.sh
./build.sh

# 3. Install on connected device via ADB
adb install -r app/build/outputs/apk/debug/app-debug.apk

# 4. Start receiver service
adb shell am start -n com.splendid.airplay/.ui.MainActivity
```

---

## Technical Notes

* **Wi-Fi Multicast Lock:** Android devices filter multicast UDP packets by default to preserve battery. The application acquires an explicit `WifiManager.MulticastLock` during discovery.
* **Dual Unicast/Multicast Response:** Many modern consumer routers throttle multicast packets on 2.4 GHz. The native mDNS loop detects the source IP of queries and responds directly via unicast UDP alongside the standard `224.0.0.251:5353` multicast group.
* **OpenSL ES Buffer Queue:** Employs `SLAndroidSimpleBufferQueue` instead of standard Java `AudioTrack` to bypass Dalvik garbage collection pauses.

---

## License

This project is licensed under the [MIT License](LICENSE).
