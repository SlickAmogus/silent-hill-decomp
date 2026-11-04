/* Browser shell for the Silent Hill PC Port (web_port).
 *
 * Owns everything outside the game: picking and remembering the player's disc
 * image, the persistent filesystem, fullscreen, and crash reporting. The game
 * itself is SilentHillPC.js/.wasm, built by web_port/build_web.sh.
 *
 * Filesystem layout the C side expects (pc_port/src/main_pc.c):
 *   /game  working directory, IndexedDB-backed: config.cfg, gamedata/save/N.MCD
 *   /disc  the player's .bin, memory only -- never copied into IndexedDB
 *   /port  the port's own shipped files (fonts, lang packs), preloaded by the
 *          build and copied into /game/ on every start so updates reach it
 *   /tmp   the session log
 */
(function () {
	"use strict";

	var $ = function (id) { return document.getElementById(id); };
	var ui = {
		launcher: $("launcher"), file: $("file"), disc: $("disc"), discName: $("discName"),
		discMeta: $("discMeta"), remember: $("remember"), play: $("play"),
		progress: $("progress"), bar: $("progress").firstElementChild, status: $("status"),
		platformNote: $("platformNote"), canvas: $("canvas"), fsbtn: $("fsbtn"),
		crash: $("crash"), crashMsg: $("crashMsg"), crashDetail: $("crashDetail"), crashLog: $("crashLog")
	};

	var SECTOR = 2352;
	var BUILD = "__BUILD__";
	var isIOS = /iPhone|iPad|iPod/.test(navigator.userAgent) ||
		(navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
	/* A coarse primary pointer is a phone or tablet. A touchscreen laptop has a
	 * fine one and stays on desktop controls; the game still brings touch up
	 * there if a finger actually lands (pc_touch.c). */
	var isTouch = window.matchMedia("(pointer: coarse)").matches || isIOS;
	/* Development only, like ?disc= below: force the phone path on a desktop. */
	if (/^(localhost|127\.0\.0\.1)$/.test(location.hostname) &&
		new URLSearchParams(location.search).get("touch") === "1")
		isTouch = true;

	document.body.classList.add(isTouch ? "touch" : "desktop");

	var devMap = /^(localhost|127\.0\.0\.1)$/.test(location.hostname) &&
		(new URLSearchParams(location.search).get("map") || "").replace(/[^a-z0-9_]/g, "");

	var chosen = null;   /* File or Blob with a .name */
	var Module = null;
	var running = false;

	function setStatus(msg, cls) {
		ui.status.textContent = msg || "";
		ui.status.className = cls || "";
	}

	function fmtSize(n) {
		return (n / (1024 * 1024)).toFixed(0) + " MB";
	}

	/* ---------------- Remembered disc (IndexedDB) ---------------- */

	var DB_NAME = "sh1-web", DB_STORE = "kv";

	function openDb() {
		return new Promise(function (resolve, reject) {
			var req = indexedDB.open(DB_NAME, 1);
			req.onupgradeneeded = function () { req.result.createObjectStore(DB_STORE); };
			req.onsuccess = function () { resolve(req.result); };
			req.onerror = function () { reject(req.error); };
		});
	}

	function dbOp(mode, fn) {
		return openDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var tx = db.transaction(DB_STORE, mode);
				var req = fn(tx.objectStore(DB_STORE));
				tx.oncomplete = function () { resolve(req && req.result); db.close(); };
				tx.onerror = tx.onabort = function () { reject(tx.error); db.close(); };
			});
		});
	}

	function loadRememberedDisc() {
		return dbOp("readonly", function (s) { return s.get("disc"); }).catch(function () { return null; });
	}

	/* Asks the browser not to clear this site's storage (the saves, the
	 * remembered disc) when the disk runs low. Chrome decides silently; Firefox
	 * may show its own prompt once, so it is only asked from a click. */
	function protectStorage() {
		if (!navigator.storage || !navigator.storage.persist) return;
		var p = navigator.storage.persisted ? navigator.storage.persisted() : Promise.resolve(false);
		p.then(function (already) {
			if (!already) return navigator.storage.persist();
		}).catch(function () {});
	}

	function rememberDisc(file) {
		protectStorage();
		return dbOp("readwrite", function (s) {
			return s.put({ name: file.name, size: file.size, blob: file }, "disc");
		});
	}

	function forgetDisc() {
		return dbOp("readwrite", function (s) { return s.delete("disc"); }).catch(function () {});
	}

	/* ---------------- Disc selection ---------------- */

	function showDisc(file, remembered) {
		chosen = file;
		ui.discName.textContent = file.name;
		ui.disc.classList.add("ready");
		var problems = [];
		if (!/\.bin$/i.test(file.name))
			problems.push("This does not look like a .bin file.");
		if (file.size % SECTOR !== 0)
			problems.push("Its size is not a whole number of CD sectors, so it may not be a raw .bin rip.");
		if (file.size < 100 * 1024 * 1024)
			problems.push("It is much smaller than a Silent Hill disc.");
		ui.discMeta.innerHTML = "";
		ui.discMeta.appendChild(document.createTextNode(
			fmtSize(file.size) + (remembered ? " · remembered on this device" : "")));
		if (problems.length) {
			var w = document.createElement("div");
			w.className = "warn";
			w.textContent = problems.join(" ");
			ui.discMeta.appendChild(w);
		}
		if (remembered) {
			var forget = document.createElement("a");
			forget.href = "#";
			forget.textContent = " · Forget";
			forget.onclick = function (e) {
				e.preventDefault();
				forgetDisc();
				chosen = null;
				ui.discName.textContent = "No disc chosen";
				ui.disc.classList.remove("ready");
				ui.discMeta.textContent = "A .bin rip of your own Silent Hill disc.";
				ui.play.disabled = true;
			};
			ui.discMeta.firstChild.after(forget);
		}
		ui.play.disabled = false;
	}

	ui.file.addEventListener("change", function () {
		var f = ui.file.files && ui.file.files[0];
		if (f) showDisc(f, false);
	});

	/* Drag-and-drop onto the launcher is the obvious desktop gesture. */
	ui.launcher.addEventListener("dragover", function (e) { e.preventDefault(); });
	ui.launcher.addEventListener("drop", function (e) {
		e.preventDefault();
		var f = e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files[0];
		if (f) showDisc(f, false);
	});

	/* Streamed into one preallocated buffer so the progress bar is real and the
	 * 700 MB image exists exactly once; MEMFS then adopts it (canOwn) instead of
	 * copying. */
	function readDisc(file) {
		var total = file.size, buf = new Uint8Array(total), off = 0;
		ui.progress.style.display = "block";
		if (!file.stream) {
			return file.arrayBuffer().then(function (ab) { return new Uint8Array(ab); });
		}
		var reader = file.stream().getReader();
		function pump() {
			return reader.read().then(function (r) {
				if (r.done) return buf;
				buf.set(r.value, off);
				off += r.value.length;
				ui.bar.style.width = (100 * off / total).toFixed(1) + "%";
				setStatus("Reading disc… " + Math.floor(100 * off / total) + "%");
				return pump();
			});
		}
		return pump();
	}

	/* ---------------- Filesystem ---------------- */

	function mkdirp(FS, path) {
		var parts = path.split("/").filter(Boolean), cur = "";
		parts.forEach(function (p) {
			cur += "/" + p;
			try { FS.mkdir(cur); } catch (e) { /* exists */ }
		});
	}

	function syncFs(populate) {
		return new Promise(function (resolve) {
			Module.FS.syncfs(populate, function (err) {
				if (err) console.warn("[web] syncfs", err);
				resolve();
			});
		});
	}

	/* Port assets change with each build (lang packs, fonts), so they are
	 * refreshed every start. Only files that differ are written, which keeps the
	 * IndexedDB sync that follows small. */
	function stagePortAssets(FS) {
		var copied = 0;
		function walk(dir) {
			FS.readdir(dir).forEach(function (n) {
				if (n === "." || n === "..") return;
				var src = dir + "/" + n, dst = "/game" + src.slice("/port".length);
				var st = FS.stat(src);
				if (FS.isDir(st.mode)) {
					mkdirp(FS, dst);
					walk(src);
					return;
				}
				var same = false;
				try { same = FS.stat(dst).size === st.size; } catch (e) { /* missing */ }
				if (!same) {
					FS.writeFile(dst, FS.readFile(src));
					copied++;
				}
			});
		}
		try { walk("/port"); } catch (e) { console.warn("[web] asset staging", e); }
		return copied;
	}

	var syncing = false, syncTimer = 0, syncAgain = false, syncWaiters = [];

	/* Copy the in-memory files to IndexedDB. A request that arrives while a
	 * copy runs is answered by a fresh copy started after it, so a caller who
	 * just wrote files (the QoL mods installer) is told only once THOSE files
	 * are kept. */
	function persistNow(onDone) {
		if (typeof onDone === "function") syncWaiters.push(onDone);
		if (!Module) return;
		if (syncing) { syncAgain = true; return; }
		syncing = true;
		syncAgain = false;
		var waiters = syncWaiters;
		syncWaiters = [];
		syncFs(false).then(function () {
			syncing = false;
			waiters.forEach(function (f) { try { f(); } catch (e) { console.warn(e); } });
			if (syncAgain || syncWaiters.length) persistNow();
		});
	}

	/* ---------------- Crash / log ---------------- */

	function latestLog() {
		if (!Module) return null;
		try {
			var names = Module.FS.readdir("/tmp").filter(function (n) { return /\.log$/.test(n); }).sort();
			if (!names.length) return null;
			var n = names[names.length - 1];
			return { name: n, data: Module.FS.readFile("/tmp/" + n) };
		} catch (e) { return null; }
	}

	function downloadLog() {
		var log = latestLog();
		if (!log) return;
		var a = document.createElement("a");
		a.href = URL.createObjectURL(new Blob([log.data], { type: "text/plain" }));
		a.download = log.name;
		a.click();
		setTimeout(function () { URL.revokeObjectURL(a.href); }, 5000);
	}

	/* ---------------- Logs kept between sessions ----------------
	 * The game writes its log to /tmp, which is gone with the page. A copy of
	 * it is kept here, in a store of the launcher's own, every 30 seconds and
	 * as the page hides; IDBFS would rewrite the whole growing file every few
	 * seconds instead. The newest LOG_KEEP survive. */
	var LOG_DB = "sh-online-logs", LOG_STORE = "logs", LOG_KEEP = 10;

	function openLogDb() {
		return new Promise(function (resolve, reject) {
			var req = indexedDB.open(LOG_DB, 1);
			req.onupgradeneeded = function () { req.result.createObjectStore(LOG_STORE); };
			req.onsuccess = function () { resolve(req.result); };
			req.onerror = function () { reject(req.error); };
		});
	}

	function logList() {
		return openLogDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var out = [], tx = db.transaction(LOG_STORE, "readonly");
				tx.objectStore(LOG_STORE).openCursor().onsuccess = function (e) {
					var c = e.target.result;
					if (!c) return;
					out.push({ name: String(c.primaryKey), size: c.value && c.value.data ? c.value.data.length : 0 });
					c.continue();
				};
				tx.oncomplete = function () { db.close(); resolve(out.sort(function (a, b) { return a.name < b.name ? 1 : -1; })); };
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	function logGet(name) {
		return openLogDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var tx = db.transaction(LOG_STORE, "readonly"), r = tx.objectStore(LOG_STORE).get(name);
				tx.oncomplete = function () { db.close(); resolve(r.result ? r.result.data : null); };
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	function logKeep() {
		var log = latestLog();
		if (!log || !log.data || !log.data.length) return Promise.resolve();
		return openLogDb().then(function (db) {
			return new Promise(function (resolve) {
				var tx = db.transaction(LOG_STORE, "readwrite"), st = tx.objectStore(LOG_STORE);
				st.put({ data: new Uint8Array(log.data) }, log.name);
				st.getAllKeys().onsuccess = function (e) {
					var keys = (e.target.result || []).map(String).sort();
					keys.slice(0, Math.max(0, keys.length - LOG_KEEP)).forEach(function (k) { st.delete(k); });
				};
				tx.oncomplete = tx.onerror = tx.onabort = function () { db.close(); resolve(); };
			});
		}).catch(function (e) { console.warn("[web] log keep", e); });
	}

	function logClearAll() {
		return openLogDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var tx = db.transaction(LOG_STORE, "readwrite");
				tx.objectStore(LOG_STORE).clear();
				tx.oncomplete = function () { db.close(); resolve(); };
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	function showCrash(msg, detail) {
		if (ui.crash.style.display === "flex") return;
		persistNow();
		logKeep();
		ui.crashMsg.textContent = msg;
		ui.crashDetail.textContent = detail || "";
		ui.crashLog.style.display = latestLog() ? "" : "none";
		ui.crash.style.display = "flex";
		if (document.fullscreenElement) document.exitFullscreen().catch(function () {});
	}

	ui.crashLog.onclick = downloadLog;

	/* For support and automation: the log text without a download. */
	window.SHWeb = {
		logText: function () {
			var l = latestLog();
			return l ? new TextDecoder().decode(l.data) : "";
		},
		downloadLog: downloadLog,
		raDialog: raOpen,
		fs: function () { return Module && Module.FS; },
		audioContexts: function () { return audioContexts; },
		/* Every file the game wrote, with the text of small logs/configs. */
		dumpFiles: function () {
			if (!Module) return "";
			var F = Module.FS, out = [];
			(function walk(d) {
				F.readdir(d).forEach(function (n) {
					if (n === "." || n === "..") return;
					var p = (d === "/" ? "" : d) + "/" + n, st;
					try { st = F.stat(p); } catch (e) { return; }
					if (F.isDir(st.mode)) {
						if (["/dev", "/proc", "/port", "/disc"].indexOf(p) < 0) walk(p);
						return;
					}
					out.push(p + " " + st.size);
					if (/\.(log|txt|cfg)$/i.test(n) && st.size < 400000)
						out.push("----- " + p, new TextDecoder().decode(F.readFile(p)));
				});
			})("/");
			return out.join("\n");
		}
	};

	/* Only a WebAssembly trap stops the game; an abort reports itself through
	 * onAbort. Everything else that lands here is the browser declining
	 * something the page asked for and the game carries on: requestPointerLock
	 * returns a promise that rejects when the click that asked for it came
	 * while focus was still returning from another window. Those are logged,
	 * not shown, or a game running fine underneath gets a crash card on top. */
	function fatal(err) {
		return typeof WebAssembly !== "undefined" && err instanceof WebAssembly.RuntimeError;
	}

	/* Any other error that escaped the game's own frame could still have
	 * stalled it, and the type alone cannot say. Watching whether it keeps
	 * drawing can: count the WebGL draws for a few seconds and report only if
	 * they stop while the page is on screen. */
	var drawWatch = null;
	function confirmStalled(detail) {
		var gl = ui.canvas.getContext("webgl2");
		if (drawWatch || !gl) return;
		drawWatch = { draws: 0, orig: { drawArrays: gl.drawArrays, drawElements: gl.drawElements } };
		["drawArrays", "drawElements"].forEach(function (fn) {
			gl[fn] = function () { drawWatch.draws++; return drawWatch.orig[fn].apply(gl, arguments); };
		});
		setTimeout(function () {
			var stalled = drawWatch.draws === 0 && document.visibilityState === "visible";
			gl.drawArrays = drawWatch.orig.drawArrays;
			gl.drawElements = drawWatch.orig.drawElements;
			drawWatch = null;
			if (stalled) showCrash("The game stopped unexpectedly.", detail);
		}, 3000);
	}

	function onPageError(err, detail) {
		if (!running) return;
		if (fatal(err)) {
			showCrash("The game stopped unexpectedly.", detail);
			return;
		}
		console.warn("[shell] non-fatal error, game still checked:", detail);
		confirmStalled(detail);
	}
	window.addEventListener("error", function (e) {
		onPageError(e.error, String(e.message || "") + "\n" + String(e.error && e.error.stack || ""));
	});
	window.addEventListener("unhandledrejection", function (e) {
		onPageError(e.reason, String(e.reason && (e.reason.stack || e.reason)));
	});

	/* ---------------- Audio lifecycle ---------------- */

	/* Browsers suspend a hidden page's AudioContext (phones always; iOS calls
	 * it "interrupted"), and SDL only resumes it once, when the device opens,
	 * so audio stayed off for good after the first trip to the home screen.
	 * Track every context the game creates and resume it on the way back;
	 * iOS only honours resume() inside a gesture, hence the input listeners. */
	var audioContexts = [];
	(function trackAudioContexts() {
		var Base = window.AudioContext || window.webkitAudioContext;
		if (!Base) return;
		var Tracked = function (opts) {
			var ctx = new Base(opts);
			audioContexts.push(ctx);
			return ctx;
		};
		Tracked.prototype = Base.prototype;
		window.AudioContext = Tracked;
		if (window.webkitAudioContext) window.webkitAudioContext = Tracked;
	})();

	function resumeAudio() {
		if (document.visibilityState === "hidden") return;
		audioContexts.forEach(function (ctx) {
			if (ctx.state !== "running" && ctx.state !== "closed")
				ctx.resume().catch(function () {});
		});
	}

	document.addEventListener("visibilitychange", resumeAudio);
	window.addEventListener("focus", resumeAudio);
	window.addEventListener("pageshow", resumeAudio);
	["pointerdown", "touchend", "keydown"].forEach(function (ev) {
		document.addEventListener(ev, resumeAudio, { capture: true, passive: true });
	});

	/* ---------------- RetroAchievements dialog ---------------- */

	/* Opened by the Achievements row in Web Options (pc_port/src/web_ra.c).
	 * The game keeps running behind it; the row's label follows the result. */
	var ra = {
		root: $("ra"), login: $("raLogin"), logout: $("raLogout"), user: $("raUser"),
		pass: $("raPass"), msg: $("raLogoutMsg"), signOut: $("raSignOut")
	};

	function raClose() {
		ra.root.style.display = "none";
		ra.pass.value = "";
		ui.canvas.focus();
	}

	function raOpen(signedIn, user) {
		ra.login.style.display = signedIn ? "none" : "";
		ra.logout.style.display = signedIn ? "" : "none";
		if (signedIn) {
			ra.msg.textContent = "Signed in as " + user + ". Sign out?";
		} else {
			ra.user.value = user || ra.user.value;
		}
		ra.root.style.display = "flex";
		(signedIn ? ra.signOut : (ra.user.value ? ra.pass : ra.user)).focus();
	}

	/* SDL listens for keys on window and swallows them, so typing here would
	 * both fail and drive the game. Keep every key inside the dialog. */
	["keydown", "keyup", "keypress"].forEach(function (ev) {
		ra.root.addEventListener(ev, function (e) {
			e.stopPropagation();
			if (ev === "keydown" && e.key === "Escape") raClose();
		});
	});

	ra.login.addEventListener("submit", function (e) {
		e.preventDefault();
		var u = ra.user.value.trim(), p = ra.pass.value;
		if (!u || !p || !Module) return;
		Module.ccall("Web_RaLogin", null, ["string", "string"], [u, p]);
		raClose();
	});
	ra.signOut.addEventListener("click", function () {
		if (Module) Module.ccall("Web_RaLogout", null, [], []);
		raClose();
	});
	Array.prototype.forEach.call(document.querySelectorAll(".raCancel"), function (b) {
		b.addEventListener("click", raClose);
	});

	/* ---------------- Fullscreen ---------------- */

	function wantFullscreen() {
		var el = document.documentElement;
		var req = el.requestFullscreen || el.webkitRequestFullscreen;
		if (!req) return Promise.resolve();
		return Promise.resolve(req.call(el, { navigationUI: "hide" })).then(function () {
			if (screen.orientation && screen.orientation.lock)
				return screen.orientation.lock("landscape").catch(function () {});
		}).catch(function () {});
	}

	function updateFsButton() {
		var canFs = !!(document.documentElement.requestFullscreen || document.documentElement.webkitRequestFullscreen);
		var inFs = !!(document.fullscreenElement || document.webkitFullscreenElement);
		ui.fsbtn.style.display = (running && canFs && !inFs && isTouch) ? "block" : "none";
	}

	ui.fsbtn.onclick = function () { wantFullscreen().then(updateFsButton); };
	document.addEventListener("fullscreenchange", updateFsButton);
	document.addEventListener("webkitfullscreenchange", updateFsButton);

	/* ---------------- Landscape only ---------------- */

	/* orientation.lock only works in fullscreen on Android and not at all on
	 * iOS, so a phone can still be turned upright. The game sizes itself from
	 * the canvas, and a run of portrait/landscape resizes leaves it confused,
	 * so in portrait the canvas keeps its last landscape size behind a cover
	 * and the game never sees the turn. */
	var rotateCover = $("rotate");
	var landSize = null;

	function checkOrientation() {
		var portrait = window.innerHeight > window.innerWidth;
		var cover = running && isTouch && portrait;
		if (!portrait) landSize = { w: window.innerWidth, h: window.innerHeight };
		rotateCover.style.display = cover ? "flex" : "none";
		if (cover) {
			var s = landSize || { w: window.innerHeight, h: window.innerWidth };
			ui.canvas.style.width = s.w + "px";
			ui.canvas.style.height = s.h + "px";
		} else {
			ui.canvas.style.width = "";
			ui.canvas.style.height = "";
		}
	}

	window.addEventListener("resize", checkOrientation);
	window.addEventListener("orientationchange", checkOrientation);
	checkOrientation();

	/* ---------------- Launch ---------------- */

	function loadScript(src) {
		return new Promise(function (resolve, reject) {
			var s = document.createElement("script");
			s.src = src;
			s.onload = resolve;
			s.onerror = function () { reject(new Error("Could not load " + src)); };
			document.head.appendChild(s);
		});
	}

	function discFsName(name) {
		/* The C side only autodetects names ending in .bin/.BIN. */
		var base = name.replace(/[\\/:*?"<>|]/g, "_").replace(/\.[^.]*$/, "");
		return (base || "disc") + ".bin";
	}

	function start() {
		if (!chosen) return;
		ui.play.disabled = true;
		ui.file.disabled = true;
		protectStorage();
		/* From here the game owns the save folder; a write from these would be
		 * overwritten by its next sync, or overwrite what it just saved. */
		saveUi.exp.disabled = true;
		saveUi.imp.disabled = true;
		if (isTouch) wantFullscreen();

		var file = chosen.blob || chosen;
		var name = chosen.name;
		var saveDisc = (ui.remember.checked && !chosen.blob)
			? rememberDisc(file).catch(function (e) {
				console.warn("[web] could not remember disc", e);
			})
			: (!ui.remember.checked ? forgetDisc() : Promise.resolve());

		var discBytes;
		readDisc(file).then(function (bytes) {
			discBytes = bytes;
			setStatus("Loading the game…");
			ui.bar.style.width = "100%";
			return loadScript("SilentHillPC.js?v=" + BUILD);
		}).then(function () {
			return createSilentHill({
				canvas: ui.canvas,
				/* Same stamp on every file, so a cached older .js can never pair with
				 * a newer .wasm after an update. */
				locateFile: function (p, dir) { return dir + p + "?v=" + BUILD; },
				preRun: [function (m) {
					/* environ is built during runtime init, after preRun and before
					 * main, so this is the last moment it can be set. */
					m.ENV.SH_WEB_TOUCH = isTouch ? "1" : "0";
				}],
				print: function (t) { console.log(t); },
				printErr: function (t) { console.warn(t); },
				onAbort: function (what) { showCrash("The game stopped unexpectedly.", String(what)); },
				setStatus: function () {}
			});
		}).then(function (m) {
			Module = m;
			m.shPersist = persistNow;
			var FS = m.FS;
			mkdirp(FS, "/game");
			FS.mount(m.IDBFS, {}, "/game");
			return syncFs(true).then(function () {
				mkdirp(FS, "/game/gamedata/save");
				/* Development only: ?log=1 on a local server turns the debug
				 * log on, the same switch a phone tester flips in config.cfg. */
				if (/^(localhost|127\.0\.0\.1)$/.test(location.hostname) &&
					new URLSearchParams(location.search).get("log") === "1") {
					var cfg = "";
					try { cfg = FS.readFile("/game/config.cfg", { encoding: "utf8" }); } catch (e) { /* none yet */ }
					FS.writeFile("/game/config.cfg", cfg + "\nenable_debug_log = 1\ngl_verbose = 1\n");
				}
				/* Development only: ?map=map0_s01 on a local server starts a new
				 * game straight on that map (the desktop -skiptogame switch). */
				if (devMap) {
					var mcfg = "";
					try { mcfg = FS.readFile("/game/config.cfg", { encoding: "utf8" }); } catch (e) { /* none yet */ }
					FS.writeFile("/game/config.cfg", mcfg + "\nmap = " + devMap + "\n");
				}
				stagePortAssets(FS);
				mkdirp(FS, "/disc");
				FS.writeFile("/disc/" + discFsName(name), discBytes, { canOwn: true });
				discBytes = null;
				return syncFs(false);
			});
		}).then(function () {
			return saveDisc;
		}).then(function () {
			ui.launcher.classList.add("hidden");
			ui.progress.style.display = "none";
			running = true;
			checkOrientation();
			updateFsButton();
			ui.canvas.focus();
			syncTimer = setInterval(function () { persistNow(); }, 4000);
			setInterval(logKeep, 30000);
			document.addEventListener("visibilitychange", function () {
				if (document.visibilityState === "hidden") { persistNow(); logKeep(); }
			});
			window.addEventListener("pagehide", function () { persistNow(); logKeep(); });
			/* A reload in the middle of a copy loses what it had not reached
			 * yet; the browser's own "leave this page?" holds it for a moment. */
			window.addEventListener("beforeunload", function (e) {
				if (!syncing && !syncWaiters.length) return;
				e.preventDefault();
				e.returnValue = "";
			});
			try {
				Module.callMain(devMap ? ["-skiptogame"] : []);
			} catch (e) {
				/* ASYNCIFY unwinds main at its first yield by throwing "unwind";
				 * that is normal, anything else is a real failure. */
				if (e !== "unwind" && !(e && e.name === "ExitStatus" && e.status === 0))
					showCrash("The game failed to start.", String(e) + "\n" + String(e && e.stack || ""));
			}
		}).catch(function (e) {
			console.error(e);
			setStatus(String(e && e.message || e), "err");
			ui.play.disabled = false;
			ui.file.disabled = false;
		});
	}

	ui.play.addEventListener("click", start);

	/* ---------------- Saved games (export / import) ---------------- */

	/* The game's /game folder lives in IDBFS's own IndexedDB database, named
	 * after the mount point, one record per path. These buttons sit on the
	 * launcher, before the game has loaded and mounted it, so they read and
	 * write those records directly, in the shape IDBFS stores them; the next
	 * Play loads them like any other file. */
	var IDB_NAME = "/game", IDB_VERSION = 21, IDB_STORE = "FILE_DATA";
	var SAVE_DIR = "/game/gamedata/save";
	var MODE_FILE = 33206, MODE_DIR = 16895;   /* S_IFREG|0666, S_IFDIR|0777: what FS.writeFile/mkdir give */
	var saveUi = { exp: $("saveExport"), imp: $("saveImport"), status: $("saveStatus") };

	function openGameDb() {
		return new Promise(function (resolve, reject) {
			var req = indexedDB.open(IDB_NAME, IDB_VERSION);
			/* Only on a browser that has never run the game: the same store and
			 * index IDBFS would make. */
			req.onupgradeneeded = function () {
				var db = req.result, store;
				store = db.objectStoreNames.contains(IDB_STORE)
					? req.transaction.objectStore(IDB_STORE)
					: db.createObjectStore(IDB_STORE);
				if (!store.indexNames.contains("timestamp"))
					store.createIndex("timestamp", "timestamp", { unique: false });
			};
			req.onsuccess = function () { resolve(req.result); };
			req.onerror = function () { reject(req.error); };
		});
	}

	/* Every N.MCD on the save folder: [{ name, path, contents }]. */
	function readCards() {
		return openGameDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var out = [], tx = db.transaction(IDB_STORE, "readonly");
				var range = IDBKeyRange.bound(SAVE_DIR + "/", SAVE_DIR + "/￿");
				tx.objectStore(IDB_STORE).openCursor(range).onsuccess = function (e) {
					var c = e.target.result;
					if (!c) return;
					var name = String(c.primaryKey).slice(SAVE_DIR.length + 1);
					if (/^\d+\.mcd$/i.test(name) && c.value && c.value.contents)
						out.push({ name: name, path: c.primaryKey, contents: new Uint8Array(c.value.contents) });
					c.continue();
				};
				tx.oncomplete = function () { db.close(); resolve(out); };
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	function writeCards(cards) {
		return openGameDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var tx = db.transaction(IDB_STORE, "readwrite"), store = tx.objectStore(IDB_STORE);
				var now = new Date();
				["/game", "/game/gamedata", SAVE_DIR].forEach(function (d) {
					store.get(d).onsuccess = function (e) {
						if (!e.target.result) store.put({ timestamp: now, mode: MODE_DIR }, d);
					};
				});
				cards.forEach(function (c) {
					store.put({ timestamp: now, mode: MODE_FILE, contents: c.contents }, c.path);
				});
				tx.oncomplete = function () { db.close(); resolve(); };
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	/* ---- Minimal zip: stored entries out, stored or deflated entries in ---- */

	var crcTable = null;
	function crc32(b) {
		if (!crcTable) {
			crcTable = new Uint32Array(256);
			for (var n = 0; n < 256; n++) {
				var c = n;
				for (var k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
				crcTable[n] = c >>> 0;
			}
		}
		var crc = 0xFFFFFFFF;
		for (var i = 0; i < b.length; i++) crc = crcTable[(crc ^ b[i]) & 0xFF] ^ (crc >>> 8);
		return (crc ^ 0xFFFFFFFF) >>> 0;
	}

	function makeZip(files) {
		var d = new Date();
		var dosTime = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1);
		var dosDate = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
		var parts = [], central = [], off = 0, enc = new TextEncoder();
		files.forEach(function (f) {
			var name = enc.encode(f.name), crc = crc32(f.data), len = f.data.length;
			var h = new DataView(new ArrayBuffer(30));
			h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true);
			h.setUint16(10, dosTime, true); h.setUint16(12, dosDate, true);
			h.setUint32(14, crc, true); h.setUint32(18, len, true); h.setUint32(22, len, true);
			h.setUint16(26, name.length, true);
			parts.push(new Uint8Array(h.buffer), name, f.data);
			var c = new DataView(new ArrayBuffer(46));
			c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true);
			c.setUint16(12, dosTime, true); c.setUint16(14, dosDate, true);
			c.setUint32(16, crc, true); c.setUint32(20, len, true); c.setUint32(24, len, true);
			c.setUint16(28, name.length, true); c.setUint32(42, off, true);
			central.push(new Uint8Array(c.buffer), name);
			off += 30 + name.length + len;
		});
		var cdSize = central.reduce(function (s, p) { return s + p.length; }, 0);
		var e = new DataView(new ArrayBuffer(22));
		e.setUint32(0, 0x06054b50, true);
		e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
		e.setUint32(12, cdSize, true); e.setUint32(16, off, true);
		return new Blob(parts.concat(central, [new Uint8Array(e.buffer)]), { type: "application/zip" });
	}

	function inflateRaw(bytes) {
		if (typeof DecompressionStream === "undefined")
			return Promise.reject(new Error("This browser cannot open compressed .zip files. Import the .MCD files one at a time instead."));
		var s = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
		return new Response(s).arrayBuffer().then(function (ab) { return new Uint8Array(ab); });
	}

	/* [{ name (full path inside the zip), data }] for every file entry. */
	function readZip(buf) {
		var b = new Uint8Array(buf), v = new DataView(buf), eocd = -1;
		for (var i = b.length - 22; i >= Math.max(0, b.length - 22 - 65535); i--) {
			if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
		}
		if (eocd < 0) return Promise.reject(new Error("That file is not a .zip."));
		var count = v.getUint16(eocd + 10, true), p = v.getUint32(eocd + 16, true), dec = new TextDecoder();
		var jobs = [];
		for (var n = 0; n < count; n++) {
			if (v.getUint32(p, true) !== 0x02014b50) break;
			var method = v.getUint16(p + 10, true), csize = v.getUint32(p + 20, true);
			var nlen = v.getUint16(p + 28, true), xlen = v.getUint16(p + 30, true), clen = v.getUint16(p + 32, true);
			var loc = v.getUint32(p + 42, true);
			var name = dec.decode(b.subarray(p + 46, p + 46 + nlen));
			var start = loc + 30 + v.getUint16(loc + 26, true) + v.getUint16(loc + 28, true);
			var raw = b.subarray(start, start + csize);
			p += 46 + nlen + xlen + clen;
			if (/\/$/.test(name)) continue;
			jobs.push((function (name, method, raw) {
				if (method === 0) return Promise.resolve({ name: name, data: raw.slice() });
				if (method === 8) return inflateRaw(raw).then(function (d) { return { name: name, data: d }; });
				return Promise.reject(new Error(name + " uses a compression this page cannot open."));
			})(name, method, raw));
		}
		return Promise.all(jobs);
	}

	/* ---- The buttons ---- */

	function saveMsg(text, cls) {
		saveUi.status.textContent = text;
		saveUi.status.className = "note" + (cls ? " " + cls : "");
	}

	/* ---- Logs: the switch lives in config.cfg (enable_debug_log), read and
	 * written straight from IDBFS's record like the saves above. ---- */
	var logUi = { box: $("logs"), on: $("logEnable"), pick: $("logPick"), dl: $("logDownload"),
		clear: $("logClear"), status: $("logStatus") };
	var CFG_PATH = "/game/config.cfg";

	function cfgRead() {
		return openGameDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var tx = db.transaction(IDB_STORE, "readonly"), r = tx.objectStore(IDB_STORE).get(CFG_PATH);
				tx.oncomplete = function () {
					db.close();
					resolve(r.result && r.result.contents ? new TextDecoder().decode(new Uint8Array(r.result.contents)) : "");
				};
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	function cfgWrite(text) {
		return openGameDb().then(function (db) {
			return new Promise(function (resolve, reject) {
				var tx = db.transaction(IDB_STORE, "readwrite"), st = tx.objectStore(IDB_STORE), now = new Date();
				st.get("/game").onsuccess = function (e) {
					if (!e.target.result) st.put({ timestamp: now, mode: MODE_DIR }, "/game");
				};
				st.put({ timestamp: now, mode: MODE_FILE, contents: new TextEncoder().encode(text) }, CFG_PATH);
				tx.oncomplete = function () { db.close(); resolve(); };
				tx.onerror = tx.onabort = function () { db.close(); reject(tx.error); };
			});
		});
	}

	function logMsg(text, cls) {
		logUi.status.textContent = text;
		logUi.status.className = "note" + (cls ? " " + cls : "");
	}

	function logRefresh() {
		return logList().then(function (logs) {
			logUi.pick.innerHTML = "";
			logs.forEach(function (l) {
				var o = document.createElement("option");
				o.value = l.name;
				o.textContent = l.name.replace(/^SilentHill_/, "").replace(/\.log$/, "") + "  (" +
					Math.max(1, Math.round(l.size / 1024)) + " KB)";
				logUi.pick.appendChild(o);
			});
			logUi.dl.disabled = logUi.clear.disabled = logUi.pick.disabled = !logs.length;
			if (!logs.length) {
				var o = document.createElement("option");
				o.textContent = "No logs yet";
				logUi.pick.appendChild(o);
			}
		}).catch(function (e) { console.warn("[web] logs", e); });
	}

	cfgRead().then(function (cfg) {
		logUi.on.checked = /^\s*enable_debug_log\s*=\s*1/m.test(cfg);
		logUi.box.classList.toggle("enabled", logUi.on.checked);
		if (logUi.on.checked) logRefresh();
	}).catch(function (e) { console.warn("[web] config", e); });

	logUi.on.addEventListener("change", function () {
		var on = logUi.on.checked;
		cfgRead().then(function (cfg) {
			var line = "enable_debug_log = " + (on ? "1" : "0");
			cfg = /^\s*enable_debug_log\s*=.*$/m.test(cfg)
				? cfg.replace(/^\s*enable_debug_log\s*=.*$/mg, line)
				: cfg + (cfg && !/\n$/.test(cfg) ? "\n" : "") + line + "\n";
			return cfgWrite(cfg);
		}).then(function () {
			logUi.box.classList.toggle("enabled", on);
			logMsg(on ? "Logging is on from the next Play." : "Logging is off from the next Play.");
			if (on) logRefresh();
		}).catch(function (e) {
			logUi.on.checked = !on;
			logMsg("Could not change the setting: " + (e && e.message || e), "warn");
		});
	});

	logUi.dl.addEventListener("click", function () {
		var name = logUi.pick.value;
		logGet(name).then(function (data) {
			if (!data) { logMsg("That log is gone.", "warn"); return logRefresh(); }
			var a = document.createElement("a");
			a.href = URL.createObjectURL(new Blob([data], { type: "text/plain" }));
			a.download = name;
			a.click();
			setTimeout(function () { URL.revokeObjectURL(a.href); }, 5000);
		}).catch(function (e) { logMsg("Could not read the log: " + (e && e.message || e), "warn"); });
	});

	logUi.clear.addEventListener("click", function () {
		if (!confirm("Delete every log kept in this browser?")) return;
		logClearAll().then(function () {
			logMsg("Logs cleared.");
			return logRefresh();
		}).catch(function (e) { logMsg("Could not clear the logs: " + (e && e.message || e), "warn"); });
	});

	function cardName(n) { return n.replace(/\.mcd$/i, "").replace(/^0+(?=\d)/, "") + ".MCD"; }

	saveUi.exp.addEventListener("click", function () {
		readCards().then(function (cards) {
			if (!cards.length) { saveMsg("There are no saved games in this browser yet.", "warn"); return; }
			var zip = makeZip(cards.map(function (c) { return { name: "save/" + c.name, data: c.contents }; }));
			var a = document.createElement("a");
			var d = new Date(), pad = function (x) { return (x < 10 ? "0" : "") + x; };
			a.href = URL.createObjectURL(zip);
			a.download = "SilentHill-saves-" + d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + ".zip";
			document.body.appendChild(a);
			a.click();
			a.remove();
			setTimeout(function () { URL.revokeObjectURL(a.href); }, 10000);
			saveMsg("Exported " + cards.map(function (c) { return c.name; }).join(", ") + ".");
		}).catch(function (e) {
			console.warn("[web] save export", e);
			saveMsg("Could not read the saved games: " + (e && e.message || e), "err");
		});
	});

	saveUi.imp.addEventListener("change", function () {
		var f = saveUi.imp.files && saveUi.imp.files[0];
		saveUi.imp.value = "";
		if (!f) return;
		protectStorage();
		var incoming;
		f.arrayBuffer().then(function (buf) {
			if (/\.mcd$/i.test(f.name)) {
				var base = f.name.split(/[\\/]/).pop();
				if (!/^\d+\.mcd$/i.test(base))
					throw new Error("Name the file after the card it replaces, 0.MCD or 8.MCD, then import it again.");
				return [{ name: base, data: new Uint8Array(buf) }];
			}
			return readZip(buf).then(function (entries) {
				/* Any N.MCD in the zip, at any depth: save/0.MCD as exported, or
				 * a zip someone made of the gamedata folder or the cards alone. */
				return entries.filter(function (e) { return /(^|\/)\d+\.mcd$/i.test(e.name); })
					.map(function (e) { return { name: e.name.split("/").pop(), data: e.data }; });
			});
		}).then(function (files) {
			if (!files.length) throw new Error("No memory card files (0.MCD, 8.MCD) were found in that .zip.");
			incoming = files.filter(function (x) { return x.data.length > 0; }).map(function (x) {
				var n = cardName(x.name);
				return { name: n, path: SAVE_DIR + "/" + n, contents: x.data };
			});
			if (!incoming.length) throw new Error("Those memory card files are empty.");
			return readCards();
		}).then(function (existing) {
			var replaced = incoming.filter(function (c) {
				return existing.some(function (e) { return e.name.toUpperCase() === c.name.toUpperCase(); });
			});
			if (replaced.length && !window.confirm(
				"This will overwrite " + replaced.map(function (c) { return c.name; }).join(" and ") +
				" in this browser, and every save on " + (replaced.length > 1 ? "those cards" : "that card") +
				" will be replaced. This cannot be undone. Export first if you want to keep them.\n\nAre you sure?")) {
				saveMsg("Import cancelled. Nothing was changed.");
				return;
			}
			/* A differently cased name from an older import would load instead
			 * of the new one on a case-sensitive filesystem; the new one wins. */
			existing.forEach(function (e) {
				incoming.forEach(function (c) {
					if (e.name !== c.name && e.name.toUpperCase() === c.name.toUpperCase()) c.stale = e.path;
				});
			});
			return writeCards(incoming).then(function () {
				var stale = incoming.filter(function (c) { return c.stale; });
				if (!stale.length) return;
				return openGameDb().then(function (db) {
					return new Promise(function (resolve) {
						var tx = db.transaction(IDB_STORE, "readwrite");
						stale.forEach(function (c) { tx.objectStore(IDB_STORE).delete(c.stale); });
						tx.oncomplete = tx.onerror = tx.onabort = function () { db.close(); resolve(); };
					});
				});
			}).then(function () {
				saveMsg("Imported " + incoming.map(function (c) { return c.name; }).join(", ") + ". They load when you press Play.");
			});
		}).catch(function (e) {
			console.warn("[web] save import", e);
			saveMsg(String(e && e.message || e), "err");
		});
	});

	/* ---------------- Capability checks ---------------- */

	(function checks() {
		var notes = [];
		var gl = null;
		try { gl = document.createElement("canvas").getContext("webgl2"); } catch (e) { /* none */ }
		if (!gl) {
			setStatus("This browser does not support WebGL 2, which the game needs.", "err");
			ui.play.disabled = true;
			ui.file.disabled = true;
			return;
		}
		if (typeof WebAssembly !== "object") {
			setStatus("This browser does not support WebAssembly.", "err");
			return;
		}
		if (isTouch) {
			notes.push("Touch controls appear once the game starts. A Bluetooth controller works too.");
			if (isIOS)
				notes.push("On iPhone, use Share → Add to Home Screen for a full-screen game without the browser bars.");
			if (navigator.deviceMemory && navigator.deviceMemory < 4)
				notes.push("This device reports under 4 GB of memory; the browser may close the game. The app versions use far less.");
		}
		ui.platformNote.textContent = notes.join(" ");

		/* Development only: ?disc=<url> on a local server skips the picker, so
		 * the page can be driven by automation. Inert on any real host. */
		var devDisc = new URLSearchParams(location.search).get("disc");
		if (devDisc && /^(localhost|127\.0\.0\.1)$/.test(location.hostname)) {
			setStatus("Fetching dev disc…");
			fetch(devDisc).then(function (r) { return r.blob(); }).then(function (b) {
				var f = new File([b], devDisc.split("/").pop(), { type: "application/octet-stream" });
				ui.remember.checked = false;
				showDisc(f, false);
				setStatus("Dev disc ready.");
			});
			return;
		}

		loadRememberedDisc().then(function (rec) {
			if (rec && rec.blob && !chosen) {
				var f = rec.blob;
				showDisc({ name: rec.name, size: rec.size, blob: f }, true);
			}
		});
	})();
})();
