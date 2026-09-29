package com.splendid.airplay.jni;

import android.util.Log;

/**
 * JNI wrapper around libairplay_core.so.
 *
 * Extended Listener interface:
 *   onTrackInfo(title, artist, album, durationSeconds)
 *   onProgress(elapsed, duration)
 *   onPlayState(isPlaying)
 *   onArtwork(data, mime)
 *   onConnected() / onDisconnected()
 */
public class AirPlayNative {

    private static final String TAG = "AirPlayNative";

    /** Transport command codes passed to nativeSendCommand() */
    public static final int CMD_PLAY_PAUSE = 1;
    public static final int CMD_PREV       = 2;
    public static final int CMD_NEXT       = 3;

    static {
        System.loadLibrary("airplay_core");
    }

    /* ── JNI declarations ─────────────────────────────────────────── */

    private native long nativeStart(String ifaceAddr, String hostname,
                                    byte[] hwAddr, int rtspPort);
    private native void nativeStop(long handle);
    private native void nativeSendCommand(long handle, int cmd);

    /* ── Listener interface ───────────────────────────────────────── */

    public interface Listener {
        void onTrackInfo(String title, String artist, String album,
                         int durationSeconds);
        void onProgress(int elapsed, int duration);
        void onPlayState(boolean isPlaying);
        void onArtwork(byte[] data, String mimeType);
        void onConnected();
        void onDisconnected();
    }

    /* ── State ────────────────────────────────────────────────────── */

    private final Listener listener;
    private long handle = 0;

    public AirPlayNative(Listener listener) {
        this.listener = listener;
    }

    public synchronized void start(String ifaceAddr, String hostname,
                                   byte[] hwAddr, int rtspPort) {
        if (handle != 0) { Log.w(TAG, "already running"); return; }
        handle = nativeStart(ifaceAddr, hostname, hwAddr, rtspPort);
        Log.i(TAG, "started handle=" + handle);
    }

    public synchronized void stop() {
        if (handle == 0) return;
        nativeStop(handle);
        handle = 0;
    }

    public synchronized void sendCommand(int cmd) {
        if (handle == 0) return;
        nativeSendCommand(handle, cmd);
    }

    /* ── Callbacks invoked from native threads ────────────────────── */

    /** Called by airplay_jni.c */
    public void onConnected() {
        if (listener != null) listener.onConnected();
    }

    public void onDisconnected() {
        if (listener != null) listener.onDisconnected();
    }

    public void onTrackInfo(String title, String artist, String album,
                            int duration) {
        if (listener != null)
            listener.onTrackInfo(title, artist, album, duration);
    }

    public void onProgress(int elapsed, int duration) {
        if (listener != null) listener.onProgress(elapsed, duration);
    }

    public void onPlayState(boolean isPlaying) {
        if (listener != null) listener.onPlayState(isPlaying);
    }

    public void onArtwork(byte[] data, String mimeType) {
        if (listener != null) listener.onArtwork(data, mimeType);
    }
}
