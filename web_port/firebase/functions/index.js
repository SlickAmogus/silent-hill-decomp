/* RetroAchievements relay for the browser build (sh1pc.com/play/).
 *
 * retroachievements.org sends no CORS headers, so a page cannot call its API.
 * firebase.json rewrites /ra/** on the site to this function, which makes the
 * game's requests same-origin and forwards them. Only dorequest.php is
 * relayed -- the one endpoint rcheevos talks to -- so this is not an open
 * proxy. Badge images come from media.retroachievements.org, which does send
 * CORS headers, and never pass through here.
 */
const { onRequest } = require("firebase-functions/v2/https");

const RA_ORIGIN = "https://retroachievements.org/";
/* The desktop build identifies as SilentHillPC/1.0 (pc_ra_http.c); a browser
 * cannot set User-Agent itself, so the relay does it for the web build. */
const USER_AGENT = "SilentHillPC/1.0 (web)";

exports.raRelay = onRequest(
    { region: "us-central1", memory: "128MiB", timeoutSeconds: 30, maxInstances: 10 },
    async (req, res) => {
        const path = req.path.replace(/^\/ra\//, "");
        if (path !== "dorequest.php" || (req.method !== "GET" && req.method !== "POST")) {
            res.status(404).send("not found");
            return;
        }

        const q = req.originalUrl.indexOf("?");
        const url = RA_ORIGIN + path + (q >= 0 ? req.originalUrl.slice(q) : "");
        const init = { method: req.method, headers: { "User-Agent": USER_AGENT } };
        if (req.method === "POST") {
            init.headers["Content-Type"] = req.get("content-type") || "application/x-www-form-urlencoded";
            init.body = req.rawBody;
        }

        try {
            const r = await fetch(url, init);
            const body = Buffer.from(await r.arrayBuffer());
            res.status(r.status)
                .set("Content-Type", r.headers.get("content-type") || "application/json")
                .set("Cache-Control", "no-store")
                .send(body);
        } catch (e) {
            res.status(502).json({ Success: false, Error: "RetroAchievements could not be reached." });
        }
    });

/* Essential QoL Mods relay (pc_qol_mods.c, web build). GitHub release downloads
 * send no CORS headers, so the browser fetches the qol-mods release through
 * here. Called directly (not via a Hosting rewrite, whose 60 s limit a large
 * pack would hit), so it sends its own CORS header. Only plain file names from
 * that one release are relayed, streamed rather than buffered. */
const { Readable } = require("stream");
const QOL_BASE = "https://github.com/SlickAmogus/silent-hill-decomp/releases/download/qol-mods/";

exports.qolMods = onRequest(
    { region: "us-central1", memory: "256MiB", timeoutSeconds: 900, maxInstances: 10 },
    async (req, res) => {
        res.set("Access-Control-Allow-Origin", "*");
        const name = req.path.replace(/^\/+/, "").replace(/^qolMods\//, "");
        if (req.method !== "GET" || !/^[A-Za-z0-9._-]+$/.test(name) || name.includes("..")) {
            res.status(404).send("not found");
            return;
        }
        try {
            const r = await fetch(QOL_BASE + name, { headers: { "User-Agent": "SilentHillPC/1.0 (web)" } });
            if (!r.ok || !r.body) {
                res.status(r.status || 502).send("not found");
                return;
            }
            res.status(200).set("Content-Type", r.headers.get("content-type") || "application/octet-stream");
            const len = r.headers.get("content-length");
            if (len) res.set("Content-Length", len);
            Readable.fromWeb(r.body).pipe(res);
        } catch (e) {
            res.status(502).send("GitHub could not be reached.");
        }
    });
