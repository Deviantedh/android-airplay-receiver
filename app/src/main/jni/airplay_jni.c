/*
 * airplay_jni.c  –  Полный JNI-мост между Java AirPlayService и нативным демоном
 *
 * Экспортируемые методы:
 *   nativeStart(ifaceAddr, hostname, hwAddr, rtspPort) → long handle
 *   nativeStop(long handle)
 *   nativeSendCommand(long handle, int cmd)
 *
 * Callbacks → Java:
 *   onConnected()
 *   onDisconnected()
 *   onTrackInfo(title, artist, album, durationSeconds)
 *   onProgress(elapsed, duration)
 *   onPlayState(isPlaying)
 *   onArtwork(byte[], mime)
 */

#include "mdns/mdns.h"
#include "rtsp/rtsp_server.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <android/log.h>

#define TAG "AirPlayJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define CMD_PLAY_PAUSE 1
#define CMD_PREV       2
#define CMD_NEXT       3

/* ── Daemon state ─────────────────────────────────────────────────────────── */

typedef struct {
    JavaVM           *jvm;
    jobject           java_obj;       /* global ref to AirPlayNative instance */
    mdns_ctx_t       *mdns;
    rtsp_server_t    *rtsp;
    pthread_t         mdns_thread;
} daemon_t;

/* ── Attach/detach helpers ─────────────────────────────────────────────────── */

static JNIEnv *jni_attach(daemon_t *d) {
    JNIEnv *env = NULL;
    if ((*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        JavaVMAttachArgs args = { JNI_VERSION_1_6, "AirPlayNative", NULL };
        (*d->jvm)->AttachCurrentThread(d->jvm, &env, &args);
    }
    return env;
}

/* ── Generic callback dispatcher ─────────────────────────────────────────── */

static void call_void(daemon_t *d, const char *method) {
    JNIEnv *env = jni_attach(d);
    jclass cls  = (*env)->GetObjectClass(env, d->java_obj);
    jmethodID m = (*env)->GetMethodID(env, cls, method, "()V");
    if (m) (*env)->CallVoidMethod(env, d->java_obj, m);
    (*env)->DeleteLocalRef(env, cls);
}

/* ── Callbacks from RTSP session ──────────────────────────────────────────── */

static void cb_on_connected(void *ud) {
    call_void((daemon_t *)ud, "onConnected");
}

static void cb_on_disconnected(void *ud) {
    call_void((daemon_t *)ud, "onDisconnected");
}

static void cb_on_track_info(const char *title, const char *artist,
                             const char *album, int duration_sec, void *ud) {
    daemon_t *d = (daemon_t *)ud;
    JNIEnv *env = jni_attach(d);
    jclass cls  = (*env)->GetObjectClass(env, d->java_obj);
    jmethodID m = (*env)->GetMethodID(env, cls, "onTrackInfo",
                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)V");
    if (m) {
        jstring jt = (*env)->NewStringUTF(env, title  ? title  : "");
        jstring ja = (*env)->NewStringUTF(env, artist ? artist : "");
        jstring jb = (*env)->NewStringUTF(env, album  ? album  : "");
        (*env)->CallVoidMethod(env, d->java_obj, m, jt, ja, jb, (jint)duration_sec);
        (*env)->DeleteLocalRef(env, jt);
        (*env)->DeleteLocalRef(env, ja);
        (*env)->DeleteLocalRef(env, jb);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static void cb_on_progress(int elapsed, int duration, void *ud) {
    daemon_t *d = (daemon_t *)ud;
    JNIEnv *env = jni_attach(d);
    jclass cls  = (*env)->GetObjectClass(env, d->java_obj);
    jmethodID m = (*env)->GetMethodID(env, cls, "onProgress", "(II)V");
    if (m) (*env)->CallVoidMethod(env, d->java_obj, m, (jint)elapsed, (jint)duration);
    (*env)->DeleteLocalRef(env, cls);
}

static void cb_on_play_state(int is_playing, void *ud) {
    daemon_t *d = (daemon_t *)ud;
    JNIEnv *env = jni_attach(d);
    jclass cls  = (*env)->GetObjectClass(env, d->java_obj);
    jmethodID m = (*env)->GetMethodID(env, cls, "onPlayState", "(Z)V");
    if (m) (*env)->CallVoidMethod(env, d->java_obj, m, (jboolean)(is_playing ? JNI_TRUE : JNI_FALSE));
    (*env)->DeleteLocalRef(env, cls);
}

static void cb_on_artwork(const uint8_t *data, int len,
                          const char *mime, void *ud) {
    daemon_t *d = (daemon_t *)ud;
    JNIEnv *env = jni_attach(d);
    jclass cls  = (*env)->GetObjectClass(env, d->java_obj);
    jmethodID m = (*env)->GetMethodID(env, cls, "onArtwork", "([BLjava/lang/String;)V");
    if (m) {
        jbyteArray jd = (*env)->NewByteArray(env, len);
        (*env)->SetByteArrayRegion(env, jd, 0, len, (const jbyte *)data);
        jstring jm = (*env)->NewStringUTF(env, mime ? mime : "image/jpeg");
        (*env)->CallVoidMethod(env, d->java_obj, m, jd, jm);
        (*env)->DeleteLocalRef(env, jd);
        (*env)->DeleteLocalRef(env, jm);
    }
    (*env)->DeleteLocalRef(env, cls);
}

static void cb_on_thread_exit(void *ud) {
    daemon_t *d = (daemon_t *)ud;
    if (d && d->jvm) {
        JNIEnv *env = NULL;
        if ((*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_EDETACHED) {
            LOGI("Detaching native thread from Dalvik VM");
            (*d->jvm)->DetachCurrentThread(d->jvm);
        }
    }
}

/* ── mDNS thread ─────────────────────────────────────────────────────────── */

static void *mdns_thread_fn(void *arg) {
    daemon_t *d = (daemon_t *)arg;
    mdns_loop(d->mdns);
    return NULL;
}

/* ── JNI exports ─────────────────────────────────────────────────────────── */

JNIEXPORT jlong JNICALL
Java_com_splendid_airplay_jni_AirPlayNative_nativeStart(
        JNIEnv *env, jobject thiz,
        jstring jiface, jstring jhost, jbyteArray jhw, jint port)
{
    daemon_t *d = (daemon_t *)calloc(1, sizeof(*d));
    if (!d) return 0;

    (*env)->GetJavaVM(env, &d->jvm);
    d->java_obj = (*env)->NewGlobalRef(env, thiz);

    const char *iface    = (*env)->GetStringUTFChars(env, jiface, NULL);
    const char *hostname = (*env)->GetStringUTFChars(env, jhost,  NULL);

    uint8_t hw[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    if (jhw && (*env)->GetArrayLength(env, jhw) >= 6) {
        jbyte *raw = (*env)->GetByteArrayElements(env, jhw, NULL);
        memcpy(hw, raw, 6);
        (*env)->ReleaseByteArrayElements(env, jhw, raw, JNI_ABORT);
    }

    /* mDNS */
    d->mdns = mdns_init(iface);
    if (d->mdns) {
        mdns_announce(d->mdns, hostname, hw, (uint16_t)port);
        pthread_create(&d->mdns_thread, NULL, mdns_thread_fn, d);
    } else {
        LOGE("mdns_init failed for iface %s", iface);
    }

    /* RTSP */
    rtsp_callbacks_t cb = {
        .on_connected    = cb_on_connected,
        .on_disconnected = cb_on_disconnected,
        .on_track_info   = cb_on_track_info,
        .on_progress     = cb_on_progress,
        .on_play_state   = cb_on_play_state,
        .on_artwork      = cb_on_artwork,
        .on_thread_exit  = cb_on_thread_exit,
        .userdata        = d
    };
    d->rtsp = rtsp_server_start((uint16_t)port, hw, &cb);
    if (!d->rtsp) LOGE("rtsp_server_start failed on port %d", port);


    (*env)->ReleaseStringUTFChars(env, jiface, iface);
    (*env)->ReleaseStringUTFChars(env, jhost,  hostname);

    LOGI("daemon started handle=%p", (void*)d);
    return (jlong)(uintptr_t)d;
}

JNIEXPORT void JNICALL
Java_com_splendid_airplay_jni_AirPlayNative_nativeStop(
        JNIEnv *env, jobject thiz, jlong handle)
{
    (void)thiz;
    daemon_t *d = (daemon_t *)(uintptr_t)handle;
    if (!d) return;
    if (d->rtsp)  rtsp_server_stop(d->rtsp);
    if (d->mdns) { mdns_stop(d->mdns); pthread_join(d->mdns_thread, NULL); }
    (*env)->DeleteGlobalRef(env, d->java_obj);
    free(d);
    LOGI("daemon stopped");
}

JNIEXPORT void JNICALL
Java_com_splendid_airplay_jni_AirPlayNative_nativeSendCommand(
        JNIEnv *env, jobject thiz, jlong handle, jint cmd)
{
    (void)env; (void)thiz;
    daemon_t *d = (daemon_t *)(uintptr_t)handle;
    if (!d || !d->rtsp) return;
    /* Forward command to RTSP server which sends it to active session */
    rtsp_server_send_command(d->rtsp, (int)cmd);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)vm; (void)reserved;
    return JNI_VERSION_1_6;
}
