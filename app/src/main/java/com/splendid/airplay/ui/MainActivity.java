package com.splendid.airplay.ui;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.SeekBar;
import android.widget.TextView;

import com.splendid.airplay.R;
import com.splendid.airplay.service.AirPlayService;

/**
 * MainActivity – Classic iPod touch "Now Playing" UI for Android 2.3 (API 10).
 * True edge-to-edge square artwork, translucent glass header and dock.
 */
public class MainActivity extends Activity implements View.OnClickListener {

    private ImageView   artView;
    private TextView    titleView;
    private TextView    artistView;
    private TextView    albumView;
    private SeekBar     seekBar;
    private TextView    timeElapsed;
    private TextView    timeTotal;
    private ImageButton btnPrev;
    private ImageButton btnPlayPause;
    private ImageButton btnNext;
    private TextView    statusView;
    private TextView    btnBack;

    private boolean isPlaying     = false;
    private int     durationSec   = 0;
    private int     elapsedSec    = 0;
    private boolean isUserSeeking = false;

    /* Current displayed state cache for seamless orientation changes */
    private String  curTitle      = "AirPlay";
    private String  curArtist     = "AirPlay Audio";
    private String  curAlbum      = "Receiver";
    private Bitmap  curArtwork    = null;
    private String  curStatusText = "○ Ожидание…";

    /* ── BroadcastReceiver ────────────────────────────────────────────── */

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            final String action = intent.getAction();

            if (AirPlayService.ACTION_TRACK_INFO.equals(action)) {
                String title  = intent.getStringExtra(AirPlayService.EXTRA_TITLE);
                String artist = intent.getStringExtra(AirPlayService.EXTRA_ARTIST);
                String album  = intent.getStringExtra(AirPlayService.EXTRA_ALBUM);
                int newDur    = intent.getIntExtra(AirPlayService.EXTRA_DURATION, 0);
                if (newDur > 0) durationSec = newDur;
                elapsedSec    = intent.getIntExtra(AirPlayService.EXTRA_ELAPSED, 0);

                curTitle  = (title != null && !title.isEmpty()) ? title : "AirPlay";
                curArtist = (artist != null && !artist.isEmpty()) ? artist : "AirPlay Audio";
                curAlbum  = (album != null && !album.isEmpty()) ? album : "Receiver";
                curStatusText = "● В эфире";
                isPlaying = true;

                if (titleView != null)  titleView.setText(curTitle);
                if (artistView != null) artistView.setText(curArtist);
                if (albumView != null)  albumView.setText(curAlbum);
                if (statusView != null) statusView.setText(curStatusText);

                if (durationSec > 0) {
                    seekBar.setMax(durationSec);
                    timeTotal.setText("-" + formatTime(Math.max(0, durationSec - elapsedSec)));
                } else {
                    seekBar.setMax(1000);
                    timeTotal.setText("-:--");
                }
                if (!isUserSeeking) {
                    seekBar.setProgress(elapsedSec);
                    timeElapsed.setText(formatTime(elapsedSec));
                }
                updatePlayPauseIcon();

            } else if (AirPlayService.ACTION_PROGRESS.equals(action)) {
                int elapsed = intent.getIntExtra(AirPlayService.EXTRA_ELAPSED, 0);
                int total   = intent.getIntExtra(AirPlayService.EXTRA_DURATION, 0);
                if (total > 0) durationSec = total;
                elapsedSec  = elapsed;

                if (!isUserSeeking) {
                    if (durationSec > 0) {
                        seekBar.setMax(durationSec);
                        seekBar.setProgress(elapsedSec);
                        timeElapsed.setText(formatTime(elapsedSec));
                        timeTotal.setText("-" + formatTime(Math.max(0, durationSec - elapsedSec)));
                    } else {
                        seekBar.setProgress(elapsedSec);
                        timeElapsed.setText(formatTime(elapsedSec));
                        timeTotal.setText("-:--");
                    }
                }

            } else if (AirPlayService.ACTION_ARTWORK.equals(action)) {
                byte[] data = intent.getByteArrayExtra(AirPlayService.EXTRA_ARTWORK);
                if (data != null && data.length > 0) {
                    Bitmap bmp = decodeSampled(data, 320, 320);
                    if (bmp != null) {
                        curArtwork = bmp;
                        if (artView != null) artView.setImageBitmap(bmp);
                    }
                }

            } else if (AirPlayService.ACTION_CONNECTED.equals(action)) {
                curStatusText = "● В эфире";
                isPlaying = true;
                if (statusView != null) statusView.setText(curStatusText);
                updatePlayPauseIcon();

            } else if (AirPlayService.ACTION_DISCONNECTED.equals(action)) {
                curStatusText = "○ Ожидание…";
                isPlaying = false;
                if (statusView != null) statusView.setText(curStatusText);
                updatePlayPauseIcon();

            } else if (AirPlayService.ACTION_PLAY_STATE.equals(action)) {
                isPlaying = intent.getBooleanExtra(AirPlayService.EXTRA_PLAYING, false);
                curStatusText = isPlaying ? "● В эфире" : "○ Пауза";
                if (statusView != null) statusView.setText(curStatusText);
                updatePlayPauseIcon();
            }
        }
    };

    /* ── View Setup & Rendering ───────────────────────────────────────── */

    private void initViews() {
        artView       = (ImageView)   findViewById(R.id.art);
        titleView     = (TextView)    findViewById(R.id.track_title);
        artistView    = (TextView)    findViewById(R.id.track_artist);
        albumView     = (TextView)    findViewById(R.id.track_album);
        seekBar       = (SeekBar)     findViewById(R.id.seek_bar);
        timeElapsed   = (TextView)    findViewById(R.id.time_elapsed);
        timeTotal     = (TextView)    findViewById(R.id.time_total);
        btnPrev       = (ImageButton) findViewById(R.id.btn_prev);
        btnPlayPause  = (ImageButton) findViewById(R.id.btn_play_pause);
        btnNext       = (ImageButton) findViewById(R.id.btn_next);
        statusView    = (TextView)    findViewById(R.id.status);
        btnBack       = (TextView)    findViewById(R.id.btn_back);

        btnPrev.setOnClickListener(this);
        btnPlayPause.setOnClickListener(this);
        btnNext.setOnClickListener(this);
        if (btnBack != null) btnBack.setOnClickListener(this);

        if (titleView != null)  titleView.setSelected(true);
        if (artistView != null) artistView.setSelected(true);
        if (albumView != null)  albumView.setSelected(true);

        seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                if (fromUser) {
                    elapsedSec = progress;
                    timeElapsed.setText(formatTime(progress));
                    if (durationSec > 0) {
                        timeTotal.setText("-" + formatTime(Math.max(0, durationSec - progress)));
                    }
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar sb) {
                isUserSeeking = true;
            }

            @Override
            public void onStopTrackingTouch(SeekBar sb) {
                isUserSeeking = false;
                elapsedSec = sb.getProgress();
            }
        });
    }

    private void renderCurrentState() {
        if (titleView != null)  titleView.setText(curTitle);
        if (artistView != null) artistView.setText(curArtist);
        if (albumView != null)  albumView.setText(curAlbum);
        if (statusView != null) statusView.setText(curStatusText);

        if (artView != null) {
            if (curArtwork != null) {
                artView.setImageBitmap(curArtwork);
            } else {
                artView.setImageResource(R.drawable.art_placeholder);
            }
        }

        if (durationSec > 0) {
            seekBar.setMax(durationSec);
            timeTotal.setText("-" + formatTime(Math.max(0, durationSec - elapsedSec)));
        } else {
            seekBar.setMax(1000);
            timeTotal.setText("-:--");
        }
        if (!isUserSeeking) {
            seekBar.setProgress(elapsedSec);
            timeElapsed.setText(formatTime(elapsedSec));
        }
        updatePlayPauseIcon();
    }

    /* ── Lifecycle ───────────────────────────────────────────────────── */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Fullscreen immersive mode (no title, no system bar)
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                             WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        setContentView(R.layout.activity_main);
        initViews();
        renderCurrentState();

        // Start AirPlay foreground service
        startService(new Intent(this, AirPlayService.class));
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // Switch between portrait and landscape layouts smoothly without restarting Activity
        setContentView(R.layout.activity_main);
        initViews();
        renderCurrentState();
        sendBroadcast(new Intent(AirPlayService.ACTION_REQUEST_STATE));
    }

    @Override
    protected void onResume() {
        super.onResume();
        sendBroadcast(new Intent(AirPlayService.ACTION_REQUEST_STATE));

        IntentFilter f = new IntentFilter();
        f.addAction(AirPlayService.ACTION_TRACK_INFO);
        f.addAction(AirPlayService.ACTION_ARTWORK);
        f.addAction(AirPlayService.ACTION_PROGRESS);
        f.addAction(AirPlayService.ACTION_CONNECTED);
        f.addAction(AirPlayService.ACTION_DISCONNECTED);
        f.addAction(AirPlayService.ACTION_PLAY_STATE);
        registerReceiver(receiver, f);
    }

    @Override
    protected void onPause() {
        super.onPause();
        unregisterReceiver(receiver);
    }

    /* ── Click handlers ──────────────────────────────────────────────── */

    @Override
    public void onClick(View v) {
        int id = v.getId();
        if (id == R.id.btn_play_pause) {
            isPlaying = !isPlaying;
            updatePlayPauseIcon();
            statusView.setText(isPlaying ? "● В эфире" : "○ Пауза");
            sendBroadcast(new Intent(AirPlayService.ACTION_CMD_PLAY_PAUSE));
        } else if (id == R.id.btn_prev) {
            sendBroadcast(new Intent(AirPlayService.ACTION_CMD_PREV));
        } else if (id == R.id.btn_next) {
            sendBroadcast(new Intent(AirPlayService.ACTION_CMD_NEXT));
        } else if (id == R.id.btn_back) {
            // Re-request state
            sendBroadcast(new Intent(AirPlayService.ACTION_REQUEST_STATE));
        }
    }

    /* ── Helpers ─────────────────────────────────────────────────────── */

    private void updatePlayPauseIcon() {
        btnPlayPause.setImageResource(isPlaying
            ? android.R.drawable.ic_media_pause
            : android.R.drawable.ic_media_play);
    }

    private static String formatTime(int seconds) {
        if (seconds < 0) return "-:--";
        int m = seconds / 60, s = seconds % 60;
        return String.format("%d:%02d", m, s);
    }

    private static Bitmap decodeSampled(byte[] data, int reqW, int reqH) {
        try {
            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(data, 0, data.length, opts);
            opts.inSampleSize       = calcSampleSize(opts, reqW, reqH);
            opts.inJustDecodeBounds = false;
            opts.inPreferredConfig  = Bitmap.Config.RGB_565;
            return BitmapFactory.decodeByteArray(data, 0, data.length, opts);
        } catch (Throwable t) {
            return null;
        }
    }

    private static int calcSampleSize(BitmapFactory.Options opts, int reqW, int reqH) {
        int h = opts.outHeight, w = opts.outWidth, sample = 1;
        if (h > reqH || w > reqW) {
            int halfH = h / 2, halfW = w / 2;
            while ((halfH / sample) > reqH && (halfW / sample) > reqW) {
                sample *= 2;
            }
        }
        return sample;
    }
}
