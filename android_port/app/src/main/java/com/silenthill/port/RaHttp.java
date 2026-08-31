package com.silenthill.port;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * The RetroAchievements transport for Android.
 *
 * pc_ra_http.c has exactly two backends -- WinHTTP and libcurl -- and the NDK
 * ships neither, which is the only reason RetroAchievements was ever in the
 * desktop-only feature bucket. rcheevos itself is libc-only and always could
 * have built. HttpURLConnection needs no third-party library and comes with the
 * platform's own trust store, so retroachievements.org TLS works with nothing
 * bundled.
 *
 * Called from the RA worker thread, never the game thread: every call here
 * blocks on the network, which is also why Android would refuse it on the main
 * thread outright (NetworkOnMainThreadException).
 */
public final class RaHttp {

    private static final String TAG = "SilentHill";

    private RaHttp() { }

    /**
     * GET when {@code post} is null or empty, otherwise a form-encoded POST.
     *
     * Returns four bytes of status code, big-endian, then the body verbatim.
     * Null means the request could not be made at all, which the caller reports
     * as status 0.
     *
     * BYTES, not a String. This used to hand back "status
body" as text, which
     * serves the JSON API and destroys everything else: achievement badges are
     * PNGs, and new String(bytes, "UTF-8") replaces every byte that is not
     * valid UTF-8 with U+FFFD. The body still arrived non-empty, so the fetch
     * looked successful and the decoder just refused it -- on screen, a browser
     * full of achievements with no pictures. A length-prefixed array has no
     * encoding to get wrong.
     */
    public static byte[] request(String url, String post) {
        HttpURLConnection c = null;

        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            c.setRequestProperty("User-Agent", "SilentHillPC/1.0");
            c.setInstanceFollowRedirects(true);

            if (post != null && post.length() > 0) {
                byte[] body = post.getBytes("UTF-8");

                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded");
                c.setFixedLengthStreamingMode(body.length);

                OutputStream out = c.getOutputStream();
                try {
                    out.write(body);
                } finally {
                    out.close();
                }
            }

            int status = c.getResponseCode();

            /* A 4xx from the RA API still carries a JSON body saying what was
             * wrong ("bad credentials"), and the caller wants to show it --
             * getInputStream() throws on those, so the error stream is the one
             * to read. */
            InputStream in = (status >= 400) ? c.getErrorStream() : c.getInputStream();
            ByteArrayOutputStream bos = new ByteArrayOutputStream();

            if (in != null) {
                byte[] buf = new byte[8192];
                int n;
                try {
                    while ((n = in.read(buf)) > 0) {
                        bos.write(buf, 0, n);
                    }
                } finally {
                    in.close();
                }
            }

            byte[] body = bos.toByteArray();
            byte[] out  = new byte[4 + body.length];

            out[0] = (byte) (status >>> 24);
            out[1] = (byte) (status >>> 16);
            out[2] = (byte) (status >>> 8);
            out[3] = (byte) status;
            System.arraycopy(body, 0, out, 4, body.length);

            return out;
        } catch (Throwable t) {
            /* Throwable, not Exception: a DNS failure on some vendor builds
             * surfaces as an Error, and letting anything propagate into JNI
             * would abort the process rather than fail one request. */
            Log.w(TAG, "RA request failed: " + t);
            return null;
        } finally {
            if (c != null) {
                c.disconnect();
            }
        }
    }
}
