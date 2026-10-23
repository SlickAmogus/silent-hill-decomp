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

	function rememberDisc(file) {
		if (navigator.storage && navigator.storage.persist)
			navigator.storage.persist().catch(function () {});
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

	var syncing = false, syncTimer = 0;

	function persistNow() {
		if (!Module || syncing) return;
		syncing = true;
		syncFs(false).then(function () { syncing = false; });
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

	function showCrash(msg, detail) {
		if (ui.crash.style.display === "flex") return;
		persistNow();
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

	window.addEventListener("error", function (e) {
		if (running) showCrash("The game stopped unexpectedly.", String(e.message || "") + "\n" + String(e.error && e.error.stack || ""));
	});
	window.addEventListener("unhandledrejection", function (e) {
		if (running) showCrash("The game stopped unexpectedly.", String(e.reason && (e.reason.stack || e.reason)));
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
			syncTimer = setInterval(persistNow, 4000);
			document.addEventListener("visibilitychange", function () {
				if (document.visibilityState === "hidden") persistNow();
			});
			window.addEventListener("pagehide", persistNow);
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
