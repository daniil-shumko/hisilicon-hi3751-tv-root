package lan.ktvsu.manager;

import android.net.LocalSocket;
import android.net.LocalSocketAddress;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/**
 * Tiny client for the ktvsud admin socket.
 *
 * The daemon authenticates us by SO_PEERCRED against /data/misc/ktvsu/admin_uid,
 * so there is no token to leak. One request per connection.
 */
public final class Admin {

    static final String SOCKET = "ktvsuadm";
    static final int TIMEOUT_MS = 4000;

    private Admin() { }

    /** Send one JSON line, return the response line, or null. Blocking. */
    public static String call(String json) {
        LocalSocket s = new LocalSocket();
        try {
            s.connect(new LocalSocketAddress(SOCKET, LocalSocketAddress.Namespace.ABSTRACT));
            s.setSoTimeout(TIMEOUT_MS);

            OutputStream os = s.getOutputStream();
            os.write((json + "\n").getBytes("UTF-8"));
            os.flush();

            InputStream is = s.getInputStream();
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            int c;
            while ((c = is.read()) != -1) {
                if (c == '\n') break;
                bos.write(c);
                if (bos.size() > 65536) break;
            }
            return bos.toString("UTF-8");
        } catch (Throwable t) {
            android.util.Log.w("KTVSU", "admin call failed: " + t);
            return null;
        } finally {
            try { s.close(); } catch (Throwable ignored) { }
        }
    }

    /* ---------------- minimal JSON helpers ---------------- */

    public static String str(String json, String key) {
        if (json == null) return null;
        String pat = "\"" + key + "\":";
        int i = json.indexOf(pat);
        if (i < 0) return null;
        i += pat.length();
        while (i < json.length() && (json.charAt(i) == ' ')) i++;
        if (i >= json.length() || json.charAt(i) != '"') return null;
        i++;
        StringBuilder sb = new StringBuilder();
        while (i < json.length()) {
            char c = json.charAt(i++);
            if (c == '\\' && i < json.length()) {
                char e = json.charAt(i++);
                if (e == 'n') sb.append('\n');
                else if (e == 'r') sb.append('\r');
                else if (e == 't') sb.append('\t');
                else sb.append(e);
            } else if (c == '"') {
                break;
            } else {
                sb.append(c);
            }
        }
        return sb.toString();
    }

    public static long num(String json, String key) {
        if (json == null) return -1;
        String pat = "\"" + key + "\":";
        int i = json.indexOf(pat);
        if (i < 0) return -1;
        i += pat.length();
        int j = i;
        while (j < json.length() && (Character.isDigit(json.charAt(j)) || json.charAt(j) == '-')) j++;
        try {
            return Long.parseLong(json.substring(i, j));
        } catch (Throwable t) {
            return -1;
        }
    }

    public static boolean bool(String json, String key) {
        if (json == null) return false;
        String pat = "\"" + key + "\":";
        int i = json.indexOf(pat);
        if (i < 0) return false;
        return json.startsWith("true", i + pat.length());
    }

    /** Return each top-level {} object inside "key":[ ... ] as its own string. */
    public static List<String> objects(String json, String key) {
        List<String> out = new ArrayList<>();
        if (json == null) return out;
        String pat = "\"" + key + "\":[";
        int i = json.indexOf(pat);
        if (i < 0) return out;
        i += pat.length();

        int depth = 0, start = -1;
        boolean inStr = false, esc = false;
        for (; i < json.length(); i++) {
            char c = json.charAt(i);
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == '{') {
                if (depth == 0) start = i;
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0 && start >= 0) {
                    out.add(json.substring(start, i + 1));
                    start = -1;
                }
            } else if (c == ']' && depth == 0) {
                break;
            }
        }
        return out;
    }
}
