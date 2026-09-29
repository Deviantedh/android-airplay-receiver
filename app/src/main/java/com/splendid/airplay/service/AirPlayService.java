package com.splendid.airplay.service;

import android.app.Notification;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.WifiManager.MulticastLock;
import android.net.wifi.WifiManager.WifiLock;
import android.os.Handler;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.PowerManager.WakeLock;
import android.util.Log;

import com.splendid.airplay.jni.AirPlayNative;
import com.splendid.airplay.ui.MainActivity;

/**
 * AirPlayService – Foreground Service running the AirPlay native daemon.
 *
 * Stays alive with:
 *   - START_STICKY (auto-restart if killed)
 *   - PARTIAL_WAKE_LOCK (CPU stays on when screen off)
 *   - MulticastLock (Wi-Fi passes multicast UDP)
 *   - startForeground() with persistent notification
 *
 * Broadcasts to UI:
 *   ACTION_TRACK_INFO    – title, artist, album, duration
 *   ACTION_PROGRESS      – elapsed, duration (fired every second)
 *   ACTION_ARTWORK       – byte[] JPEG/PNG
 *   ACTION_CONNECTED     – AirPlay client connected
 *   ACTION_DISCONNECTED  – AirPlay client disconnected
 *   ACTION_PLAY_STATE    – isPlaying boolean
 *
 * Receives from UI:
 *   ACTION_CMD_PLAY_PAUSE / ACTION_CMD_PREV / ACTION_CMD_NEXT
 *   ACTION_REQUEST_STATE  – re-broadcast current state to newly resumed Activity
 */
public class AirPlayService extends Service implements AirPlayNative.Listener {

    private static final String TAG       = "AirPlayService";
    private static final int    NOTIF_ID  = 1;
    private static final int    RTSP_PORT = 5000;
    private static final String HOSTNAME  = "AirPlay";


    /* ── Public broadcast action constants ─────────────────────────── */
    public static final String ACTION_TRACK_INFO    = "com.splendid.airplay.TRACK_INFO";
    public static final String ACTION_PROGRESS      = "com.splendid.airplay.PROGRESS";
    public static final String ACTION_ARTWORK       = "com.splendid.airplay.ARTWORK";
    public static final String ACTION_CONNECTED     = "com.splendid.airplay.CONNECTED";
    public static final String ACTION_DISCONNECTED  = "com.splendid.airplay.DISCONNECTED";
    public static final String ACTION_PLAY_STATE    = "com.splendid.airplay.PLAY_STATE";
    public static final String ACTION_REQUEST_STATE = "com.splendid.airplay.REQUEST_STATE";
    public static final String ACTION_CMD_PLAY_PAUSE= "com.splendid.airplay.CMD_PLAY_PAUSE";
    public static final String ACTION_CMD_PREV      = "com.splendid.airplay.CMD_PREV";
    public static final String ACTION_CMD_NEXT      = "com.splendid.airplay.CMD_NEXT";

    public static final String EXTRA_TITLE    = "title";
    public static final String EXTRA_ARTIST   = "artist";
    public static final String EXTRA_ALBUM    = "album";
    public static final String EXTRA_DURATION = "duration";
    public static final String EXTRA_ELAPSED  = "elapsed";
    public static final String EXTRA_ARTWORK  = "artwork";
    public static final String EXTRA_MIME     = "mime";
    public static final String EXTRA_PLAYING  = "playing";

    /* ── State ─────────────────────────────────────────────────────── */

    private AirPlayNative  nativeDaemon;
    private WakeLock       wakeLock;
    private WifiLock       wifiLock;
    private MulticastLock  multicastLock;
    private final Handler  handler = new Handler();

    // Last known track info (for re-broadcasting to newly resumed Activity)
    private String  lastTitle = "", lastArtist = "", lastAlbum = "";
    private byte[]  lastArtwork = null;
    private String  lastMime    = null;
    private int     durationSec = 0;
    private int     elapsedSec  = 0;
    private boolean playing     = false;

    /* ── Progress ticker ────────────────────────────────────────────── */

    private final Runnable progressTick = new Runnable() {
        @Override
        public void run() {
            if (playing) {
                elapsedSec++;
                if (durationSec > 0 && elapsedSec > durationSec) {
                    elapsedSec = durationSec;
                }
                Intent pi = new Intent(ACTION_PROGRESS);
                pi.putExtra(EXTRA_ELAPSED,  elapsedSec);
                pi.putExtra(EXTRA_DURATION, durationSec);
                sendBroadcast(pi);
            }
            handler.postDelayed(this, 1000);
        }
    };

    /* ── UI command receiver ────────────────────────────────────────── */

    private final BroadcastReceiver cmdReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            final String action = intent.getAction();
            if (ACTION_REQUEST_STATE.equals(action)) {
                rebroadcastState();
                return;
            }

            final int cmd;
            if (ACTION_CMD_PLAY_PAUSE.equals(action)) {
                cmd = AirPlayNative.CMD_PLAY_PAUSE;
            } else if (ACTION_CMD_PREV.equals(action)) {
                cmd = AirPlayNative.CMD_PREV;
            } else if (ACTION_CMD_NEXT.equals(action)) {
                cmd = AirPlayNative.CMD_NEXT;
            } else {
                return;
            }

            new Thread(new Runnable() {
                @Override
                public void run() {
                    nativeDaemon.sendCommand(cmd);
                }
            }).start();
        }
    };

    /* ── Lifecycle ──────────────────────────────────────────────────── */

    @Override
    public void onCreate() {
        super.onCreate();

        // Wake lock: keep CPU alive when screen off
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, TAG);
        wakeLock.acquire();

        // Multicast lock: Android Wi-Fi drops mDNS packets without this
        WifiManager wm = (WifiManager) getSystemService(WIFI_SERVICE);
        multicastLock = wm.createMulticastLock(TAG);
        multicastLock.setReferenceCounted(false);
        multicastLock.acquire();

        // Wifi lock: prevent Gingerbread Wi-Fi sleep policy from dropping connection
        wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL, TAG);
        wifiLock.setReferenceCounted(false);
        wifiLock.acquire();

        // Register UI command receiver
        IntentFilter f = new IntentFilter();
        f.addAction(ACTION_CMD_PLAY_PAUSE);
        f.addAction(ACTION_CMD_PREV);
        f.addAction(ACTION_CMD_NEXT);
        f.addAction(ACTION_REQUEST_STATE);
        registerReceiver(cmdReceiver, f);

        // Detect Wi-Fi address and MAC
        String ifaceAddr = getWifiIpAddress(wm);
        byte[] hwAddr    = getWifiMac(wm);
        Log.i(TAG, "Wi-Fi addr=" + ifaceAddr);

        // Start native daemon
        nativeDaemon = new AirPlayNative(this);
        nativeDaemon.start(ifaceAddr, HOSTNAME, hwAddr, RTSP_PORT);

        // Start progress ticker
        handler.postDelayed(progressTick, 1000);

        startForeground(NOTIF_ID, buildNotification("○ Ожидание AirPlay…", null, null));
        Log.i(TAG, "Service started");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    @Override
    public void onDestroy() {
        handler.removeCallbacks(progressTick);
        unregisterReceiver(cmdReceiver);
        nativeDaemon.stop();
        if (wifiLock != null && wifiLock.isHeld())           wifiLock.release();
        if (multicastLock != null && multicastLock.isHeld()) multicastLock.release();
        if (wakeLock != null && wakeLock.isHeld())           wakeLock.release();
        stopForeground(true);
        super.onDestroy();
    }

    /* ── AirPlayNative.Listener ─────────────────────────────────────── */

    @Override
    public void onConnected() {
        playing = true;
        updateNotif("● Подключено", lastTitle, lastArtist);
        sendBroadcast(new Intent(ACTION_CONNECTED));
    }

    @Override
    public void onDisconnected() {
        playing = false;
        updateNotif("○ Ожидание AirPlay…",
                    lastTitle.isEmpty() ? null : lastTitle,
                    lastArtist.isEmpty() ? null : lastArtist);
        sendBroadcast(new Intent(ACTION_DISCONNECTED));
    }

    @Override
    public void onTrackInfo(String title, String artist, String album,
                            int durationSeconds) {
        if (title != null && !title.isEmpty()) {
            if (!title.equals(lastTitle)) {
                elapsedSec = 0;
            }
            lastTitle = title;
        }
        if (artist != null && !artist.isEmpty()) {
            lastArtist = artist;
        }
        if (album != null && !album.isEmpty()) {
            lastAlbum = album;
        }
        if (durationSeconds > 0) {
            durationSec = durationSeconds;
        }
        playing = true;

        updateNotif("▶ " + lastTitle, lastTitle, lastArtist);

        Intent i = new Intent(ACTION_TRACK_INFO);
        i.putExtra(EXTRA_TITLE,    lastTitle);
        i.putExtra(EXTRA_ARTIST,   lastArtist);
        i.putExtra(EXTRA_ALBUM,    lastAlbum);
        i.putExtra(EXTRA_DURATION, durationSec);
        sendBroadcast(i);

        Intent ps = new Intent(ACTION_PLAY_STATE);
        ps.putExtra(EXTRA_PLAYING, true);
        sendBroadcast(ps);
    }

    @Override
    public void onPlayState(boolean isPlaying) {
        playing = isPlaying;
        updateNotif(isPlaying ? "▶ " + lastTitle : "○ Пауза",
                    lastTitle.isEmpty() ? null : lastTitle,
                    lastArtist.isEmpty() ? null : lastArtist);
        Intent ps = new Intent(ACTION_PLAY_STATE);
        ps.putExtra(EXTRA_PLAYING, isPlaying);
        sendBroadcast(ps);
    }

    @Override
    public void onProgress(int elapsed, int duration) {
        elapsedSec  = elapsed;
        durationSec = duration;
        Intent i = new Intent(ACTION_PROGRESS);
        i.putExtra(EXTRA_ELAPSED,  elapsed);
        i.putExtra(EXTRA_DURATION, duration);
        sendBroadcast(i);
    }

    @Override
    public void onArtwork(byte[] data, String mimeType) {
        lastArtwork = data;
        lastMime    = mimeType;
        Intent i = new Intent(ACTION_ARTWORK);
        i.putExtra(EXTRA_ARTWORK, data);
        i.putExtra(EXTRA_MIME,    mimeType);
        sendBroadcast(i);
    }

    /* ── Re-broadcast current state when Activity resumes ──────────── */

    private void rebroadcastState() {
        if (lastTitle != null && !lastTitle.isEmpty()) {
            Intent ti = new Intent(ACTION_TRACK_INFO);
            ti.putExtra(EXTRA_TITLE,    lastTitle);
            ti.putExtra(EXTRA_ARTIST,   lastArtist);
            ti.putExtra(EXTRA_ALBUM,    lastAlbum);
            ti.putExtra(EXTRA_DURATION, durationSec);
            ti.putExtra(EXTRA_ELAPSED,  elapsedSec);
            sendBroadcast(ti);
        }

        if (lastArtwork != null) {
            Intent ai = new Intent(ACTION_ARTWORK);
            ai.putExtra(EXTRA_ARTWORK, lastArtwork);
            ai.putExtra(EXTRA_MIME,    lastMime);
            sendBroadcast(ai);
        }

        Intent pi = new Intent(ACTION_PROGRESS);
        pi.putExtra(EXTRA_ELAPSED,  elapsedSec);
        pi.putExtra(EXTRA_DURATION, durationSec);
        sendBroadcast(pi);

        Intent ps = new Intent(ACTION_PLAY_STATE);
        ps.putExtra(EXTRA_PLAYING, playing);
        sendBroadcast(ps);
    }

    /* ── Notification ───────────────────────────────────────────────── */

    @SuppressWarnings("deprecation")
    private Notification buildNotification(String ticker, String title, String text) {
        Intent pi = new Intent(this, MainActivity.class);
        pi.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pending = PendingIntent.getActivity(this, 0, pi, 0);

        // API 10 only has the 4-argument Notification constructor:
        // Notification(int icon, CharSequence tickerText, long when)
        // Then set contentIntent and contentView manually.
        Notification n = new Notification(
            android.R.drawable.ic_media_play,
            ticker,
            System.currentTimeMillis()
        );
        // setLatestEventInfo is deprecated in API 11+ but is the ONLY way on API 10
        n.setLatestEventInfo(
            this,
            title  != null ? title  : "AirPlay Receiver",
            text   != null ? text   : ticker,
            pending
        );
        n.flags |= Notification.FLAG_ONGOING_EVENT;
        return n;
    }


    private void updateNotif(final String ticker, final String title, final String text) {
        handler.post(new Runnable() {
            @Override
            public void run() {
                android.app.NotificationManager nm =
                    (android.app.NotificationManager) getSystemService(NOTIFICATION_SERVICE);
                nm.notify(NOTIF_ID, buildNotification(ticker, title, text));
            }
        });
    }

    /* ── Wi-Fi helpers ──────────────────────────────────────────────── */

    private static String getWifiIpAddress(WifiManager wm) {
        int ip = wm.getConnectionInfo().getIpAddress();
        return String.format("%d.%d.%d.%d",
            ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
    }

    @SuppressWarnings("deprecation")
    private static byte[] getWifiMac(WifiManager wm) {
        try {
            String mac = wm.getConnectionInfo().getMacAddress();
            if (mac == null) return new byte[]{(byte)0xAA,0x1B,0x2C,0x3D,0x4E,0x5F};
            String[] parts = mac.split(":");
            byte[] b = new byte[6];
            for (int i = 0; i < 6; i++)
                b[i] = (byte) Integer.parseInt(parts[i], 16);
            return b;
        } catch (Exception e) {
            return new byte[]{(byte)0xAA,0x1B,0x2C,0x3D,0x4E,0x5F};
        }
    }
}
