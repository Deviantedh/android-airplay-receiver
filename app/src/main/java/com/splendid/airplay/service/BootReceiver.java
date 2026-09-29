package com.splendid.airplay.service;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * BootReceiver – starts AirPlayService automatically on device boot.
 * Declared in AndroidManifest.xml with BOOT_COMPLETED intent-filter.
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) {
            context.startService(new Intent(context, AirPlayService.class));
        }
    }
}
