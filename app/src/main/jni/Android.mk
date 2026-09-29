# ─────────────────────────────────────────────────────────────────────────────
# Android.mk  –  top-level module descriptor for ndk-build
#
# NDK r14b is the last NDK that fully supports API level 10 / armeabi-v7a.
# Build with:
#   ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=app/src/main/jni/Android.mk \
#             APP_PLATFORM=android-10 APP_ABI=armeabi-v7a APP_STL=c++_static
# ─────────────────────────────────────────────────────────────────────────────

LOCAL_PATH := $(call my-dir)

# ── 1. libairplay_core  (mDNS + RTSP + ALAC + OpenSL output + JNI bridge) ────
include $(CLEAR_VARS)

LOCAL_MODULE    := airplay_core
LOCAL_MODULE_TAGS := optional

# All C/C++ translation units
LOCAL_SRC_FILES := \
    mdns/mdns.c          \
    mdns/service_announce.c \
    rtsp/rtsp_server.c   \
    rtsp/rtsp_session.c  \
    alac/alac_decoder.c  \
    alac/alac.c          \
    crypto/bigint.c      \
    crypto/aes.c         \
    crypto/sha1.c        \
    crypto/base64.c      \
    crypto/rsakey.c      \
    crypto/rsapem.c      \
    audio/opensl_output.c \
    airplay_jni.c


LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/mdns   \
    $(LOCAL_PATH)/rtsp   \
    $(LOCAL_PATH)/alac   \
    $(LOCAL_PATH)/audio  \
    $(LOCAL_PATH)/crypto

# System libs available on API 10
LOCAL_LDLIBS := \
    -llog         \
    -lOpenSLES    \
    -lz

# Tune for Cortex-A5 (same ISA as Cortex-A7, NEON available)
LOCAL_ARM_NEON   := true
LOCAL_CFLAGS     := -O2 -ffast-math -DANDROID -DLOG_TAG=\"AirPlay\" \
                    -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=softfp \
                    -ffunction-sections -fdata-sections -fvisibility=hidden
LOCAL_CPPFLAGS   := $(LOCAL_CFLAGS) -std=c++11 -fno-exceptions -fno-rtti
LOCAL_LDFLAGS    := -Wl,--gc-sections

include $(BUILD_SHARED_LIBRARY)
