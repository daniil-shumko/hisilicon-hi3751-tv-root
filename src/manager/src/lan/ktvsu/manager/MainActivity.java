package lan.ktvsu.manager;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.List;

/** Rules list + global mode. Everything works with a D-pad. */
public class MainActivity extends Activity {

    LinearLayout list;
    TextView status;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(build());
        startWatcher();
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();   /* rules and mode may have changed while we were away */
    }

    int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                getResources().getDisplayMetrics());
    }

    void startWatcher() {
        try {
            Intent i = new Intent(this, Watch.class);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(i);
            else startService(i);
        } catch (Throwable ignored) {
        }
    }

    View build() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#0b0d12"));
        root.setPadding(dp(44), dp(32), dp(44), dp(32));

        TextView title = new TextView(this);
        title.setText("KTV Superuser");
        title.setTextSize(32);
        title.setTextColor(Color.parseColor("#ffcc44"));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        status = new TextView(this);
        status.setTextSize(18);
        status.setTextColor(Color.parseColor("#7d8aa8"));
        status.setPadding(0, dp(8), 0, dp(18));
        root.addView(status);

        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);

        Button mode = mk("MODE", "#1e3a8a", 16);
        mode.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                cycleMode();
            }
        });
        Button refresh = mk("REFRESH", "#334155", 16);
        refresh.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                refresh();
            }
        });
        bar.addView(mode, weight());
        bar.addView(refresh, weight());
        root.addView(bar);

        TextView hdr = new TextView(this);
        hdr.setText("APPS WITH A SAVED RULE");
        hdr.setTextSize(15);
        hdr.setTextColor(Color.parseColor("#556073"));
        hdr.setPadding(0, dp(24), 0, dp(8));
        root.addView(hdr);

        ScrollView sc = new ScrollView(this);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        sc.addView(list);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(sc, slp);

        TextView hint = new TextView(this);
        hint.setText("Requests appear as a prompt. Back = deny. "
                + "Rules live in /data/misc/ktvsu/policy on the device.");
        hint.setTextSize(14);
        hint.setTextColor(Color.parseColor("#556073"));
        hint.setPadding(0, dp(16), 0, 0);
        root.addView(hint);

        mode.setFocusableInTouchMode(true);
        mode.requestFocus();
        return root;
    }

    LinearLayout.LayoutParams weight() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        p.setMargins(0, 0, dp(10), 0);
        return p;
    }

    Button mk(String text, String color, int size) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(size);
        b.setAllCaps(false);
        b.setBackgroundColor(Color.parseColor(color));
        b.setTextColor(Color.WHITE);
        b.setFocusable(true);
        return b;
    }

    void cycleMode() {
        new Thread(new Runnable() {
            public void run() {
                String cur = Admin.call("{\"op\":\"hello\"}");
                String mode = Admin.str(cur, "mode");
                String next = "ask".equals(mode) ? "allow-all"
                        : "allow-all".equals(mode) ? "deny-all" : "ask";
                Admin.call("{\"op\":\"mode\",\"value\":\"" + next + "\"}");
                runOnUiThread(new Runnable() {
                    public void run() {
                        refresh();
                    }
                });
            }
        }).start();
    }

    void refresh() {
        new Thread(new Runnable() {
            public void run() {
                final String hello = Admin.call("{\"op\":\"hello\"}");
                final String rules = Admin.call("{\"op\":\"rules\"}");
                runOnUiThread(new Runnable() {
                    public void run() {
                        render(hello, rules);
                    }
                });
            }
        }).start();
    }

    void render(String hello, String rules) {
        if (hello == null) {
            status.setText("daemon unreachable — is ktvsud running?");
            status.setTextColor(Color.parseColor("#f87171"));
            list.removeAllViews();
            return;
        }
        String mode = Admin.str(hello, "mode");
        if (mode == null) mode = "unknown";
        status.setText("mode: " + mode + "   ·   v" + Admin.num(hello, "version")
                + "   ·   uid " + android.os.Process.myUid());
        status.setTextColor(Color.parseColor("#7d8aa8"));

        list.removeAllViews();
        List<String> objs = Admin.objects(rules, "rules");
        if (objs.isEmpty()) {
            TextView t = new TextView(this);
            t.setText("(none yet — unknown apps get a prompt)");
            t.setTextColor(Color.parseColor("#556073"));
            t.setTextSize(18);
            t.setPadding(0, dp(10), 0, 0);
            list.addView(t);
            return;
        }
        for (String o : objs) {
            list.addView(row(o));
        }
    }

    View row(final String obj) {
        String pkg = Admin.str(obj, "pkg");
        String policy = Admin.str(obj, "policy");
        long uid = Admin.num(obj, "uid");

        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(8), 0, dp(8));

        TextView t = new TextView(this);
        t.setText(pkg + "  (uid " + uid + ")  ·  " + policy);
        t.setTextSize(19);
        t.setTextColor("allow".equals(policy)
                ? Color.parseColor("#4ade80") : Color.parseColor("#f87171"));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        r.addView(t, tlp);

        Button rev = mk("REVOKE", "#7f1d1d", 15);
        rev.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                final long u = Admin.num(obj, "uid");
                new Thread(new Runnable() {
                    public void run() {
                        Admin.call("{\"op\":\"revoke\",\"uid\":\"" + u + "\"}");
                        runOnUiThread(new Runnable() {
                            public void run() {
                                refresh();
                            }
                        });
                    }
                }).start();
            }
        });
        r.addView(rev);
        return r;
    }
}
