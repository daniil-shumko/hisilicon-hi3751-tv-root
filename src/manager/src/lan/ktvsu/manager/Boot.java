package lan.ktvsu.manager;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;

public class Boot extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent i) {
        try {
            Intent s = new Intent(c, Watch.class);
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(s);
            else c.startService(s);
        } catch (Throwable ignored) {
        }
    }
}
