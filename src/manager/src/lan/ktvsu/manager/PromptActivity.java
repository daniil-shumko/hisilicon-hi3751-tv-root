package lan.ktvsu.manager;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** The approval dialog. D-pad only: focus starts on DENY. */
public class PromptActivity extends Activity {

    String id, pkg, cmd;
    long uid;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        Intent it = getIntent();
        id = it.getStringExtra("id");
        pkg = it.getStringExtra("pkg");
        cmd = it.getStringExtra("cmd");
        uid = it.getLongExtra("uid", -1);

        if (id == null) { finish(); return; }
        setContentView(build());
    }

    /** A second request while the dialog is already up: singleTop means we get
     *  onNewIntent, not a new instance. Without this the dialog would keep
     *  showing the stale request and answering it would answer the wrong id. */
    @Override
    protected void onNewIntent(Intent it) {
        super.onNewIntent(it);
        setIntent(it);
        id = it.getStringExtra("id");
        pkg = it.getStringExtra("pkg");
        cmd = it.getStringExtra("cmd");
        uid = it.getLongExtra("uid", -1);
        if (id == null) { finish(); return; }
        setContentView(build());
    }

    int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    View build() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.parseColor("#0b0d12"));
        root.setPadding(dp(48), dp(40), dp(48), dp(40));

        TextView title = new TextView(this);
        title.setText("Superuser request");
        title.setTextSize(30);
        title.setTextColor(Color.parseColor("#ffcc44"));
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView who = new TextView(this);
        who.setText(pkg + "  (uid " + uid + ")");
        who.setTextSize(26);
        who.setTextColor(Color.parseColor("#e8eefc"));
        who.setPadding(0, dp(18), 0, dp(6));
        root.addView(who);

        TextView lbl = new TextView(this);
        lbl.setText("wants to run as root:");
        lbl.setTextSize(18);
        lbl.setTextColor(Color.parseColor("#7d8aa8"));
        root.addView(lbl);

        TextView code = new TextView(this);
        code.setText(cmd == null || cmd.length() == 0 ? "(shell)" : cmd);
        code.setTextSize(20);
        code.setTextColor(Color.parseColor("#9fd0ff"));
        code.setTypeface(Typeface.MONOSPACE);
        code.setPadding(dp(16), dp(12), dp(16), dp(12));
        code.setBackgroundColor(Color.parseColor("#141a26"));

        ScrollView sc = new ScrollView(this);
        sc.addView(code);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        slp.topMargin = dp(10);
        root.addView(sc, slp);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setPadding(0, dp(24), 0, 0);

        Button deny = mk("DENY", "#7f1d1d");
        Button once = mk("ALLOW ONCE", "#1e3a8a");
        Button always = mk("ALLOW ALWAYS", "#14532d");

        deny.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { send(false, false); }
        });
        once.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { send(true, false); }
        });
        always.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) { send(true, true); }
        });

        row.addView(deny, lp());
        row.addView(once, lp());
        row.addView(always, lp());
        root.addView(row);

        deny.setFocusableInTouchMode(true);
        deny.requestFocus();
        return root;
    }

    LinearLayout.LayoutParams lp() {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        p.setMargins(dp(6), 0, dp(6), 0);
        return p;
    }

    Button mk(String text, String color) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(22);
        b.setAllCaps(false);
        b.setBackgroundColor(Color.parseColor(color));
        b.setTextColor(Color.WHITE);
        b.setPadding(dp(18), dp(18), dp(18), dp(18));
        b.setFocusable(true);
        return b;
    }

    void send(final boolean allow, final boolean remember) {
        new Thread(new Runnable() {
            public void run() {
                Admin.call("{\"op\":\"answer\",\"id\":\"" + id + "\",\"allow\":" + allow
                        + ",\"remember\":" + remember + "}");
            }
        }).start();
        finish();
    }

    @Override
    public void onBackPressed() {
        send(false, false);   /* back = deny, never leave a request hanging */
    }
}
