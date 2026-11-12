package lan.ktvsu.manager;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Polls the daemon for pending root requests and raises the prompt.
 *
 * A foreground service rather than anything cleverer: on a TV nothing is
 * guaranteed to be running, and a missed request means an app silently gets
 * no root after a 120 s timeout. The notification is the cheapest way to keep
 * the process alive.
 */
public class Watch extends Service {

    static final String TAG = "KTVSU";
    static final String CHANNEL = "ktvsu";

    volatile boolean running = false;
    Thread worker;
    final Set<String> seen = new HashSet<>();

    @Override
    public void onCreate() {
        super.onCreate();
        startForeground(1, notif("watching for root requests"));
        start();
    }

    @Override
    public int onStartCommand(Intent i, int flags, int id) {
        start();
        return START_STICKY;
    }

    Notification notif(String text) {
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            nm.createNotificationChannel(
                    new NotificationChannel(CHANNEL, "Superuser", NotificationManager.IMPORTANCE_MIN));
        }
        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL)
                : new Notification.Builder(this);
        b.setContentTitle("KTV Superuser")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.ic_lock_lock)
                .setOngoing(true);
        return b.build();
    }

    synchronized void start() {
        if (running) return;
        running = true;
        worker = new Thread(new Runnable() {
            public void run() {
                loop();
            }
        });
        worker.start();
    }

    void loop() {
        while (running) {
            try {
                String resp = Admin.call("{\"op\":\"pending\"}");
                if (resp == null) {
                    sleep(1500);
                    continue;
                }
                List<String> pend = Admin.objects(resp, "pending");
                Set<String> alive = new HashSet<>();

                for (String p : pend) {
                    String id = Admin.str(p, "id");
                    if (id == null) continue;
                    alive.add(id);
                    if (seen.contains(id)) continue;
                    seen.add(id);
                    /* Do not startActivity here: Android 14 refuses background
                     * activity starts from a foreground service (BAL_BLOCK) and
                     * SYSTEM_ALERT_WINDOW does not reliably lift it. The root
                     * daemon raises the prompt instead -- a start from uid 0 is
                     * allowlisted. We only keep this process warm so answering
                     * is instant, and dedupe ids we have already reported. */
                }
                seen.retainAll(alive);
                sleep(pend.isEmpty() ? 900 : 400);
            } catch (Throwable t) {
                Log.w(TAG, "loop: " + t);
                sleep(1500);
            }
        }
    }

    void raise(String id, String pkg, String cmd, long uid) {
        try {
            Intent i = new Intent(this, PromptActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                    | Intent.FLAG_ACTIVITY_CLEAR_TOP
                    | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            i.putExtra("id", id);
            i.putExtra("pkg", pkg == null ? ("uid " + uid) : pkg);
            i.putExtra("cmd", cmd == null ? "" : cmd);
            i.putExtra("uid", uid);
            startActivity(i);
        } catch (Throwable t) {
            // Android 10+ blocks background activity starts without an
            // overlay permission. The install script grants it; if it is
            // missing the request simply times out.
            Log.e(TAG, "cannot raise prompt: " + t);
        }
    }

    static void sleep(long ms) {
        try { Thread.sleep(ms); } catch (Throwable ignored) { }
    }

    @Override
    public void onDestroy() {
        running = false;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent i) {
        return null;
    }
}
