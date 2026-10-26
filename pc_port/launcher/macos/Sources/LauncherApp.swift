import AppKit
import SwiftUI
import UniformTypeIdentifiers

@main
struct LauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var model = LauncherModel()

    var body: some Scene {
        Window("Silent Hill PC", id: "main") {
            LauncherView()
                .environmentObject(model)
        }
        .windowResizability(.contentSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

typealias Option = (label: String, value: String)

@MainActor
final class LauncherModel: ObservableObject {
    @Published var discs: [Disc] = []
    @Published var values: [String: String] = [:]
    @Published var languages: [Option] = []
    @Published var setupError: String?
    @Published var alertMessage: String?
    @Published var statusMessage: String?
    @Published var busy = false
    @Published var gameRunning = false

    private var config: ConfigFile?

    init() {
        GameInstall.clearQuarantine()
        do {
            try GameInstall.prepare()
            config = ConfigFile(url: GameInstall.configURL)
        } catch {
            setupError = "Could not set up the game folder at \(GameInstall.dataDir.path): \(error.localizedDescription)"
        }
        reload()
    }

    func reload() {
        guard let config else { return }
        config.load()
        values = config.values
        discs = DiscProbe.scan(GameInstall.gamedataDir)
        languages = Self.loadLanguages()
    }

    func value(_ key: String, _ fallback: String) -> Binding<String> {
        Binding(
            get: { self.values[key] ?? fallback },
            set: { self.write([key: $0]) })
    }

    func flag(_ key: String, _ fallback: Bool = false) -> Binding<Bool> {
        Binding(
            get: { (self.values[key] ?? (fallback ? "1" : "0")) != "0" },
            set: { self.write([key: $0 ? "1" : "0"]) })
    }

    var resolution: Binding<String> {
        Binding(
            get: {
                if self.values["native_resolution"] == "1" { return "native" }
                return "\(self.values["width"] ?? "640")x\(self.values["height"] ?? "480")"
            },
            set: { v in
                if v == "native" {
                    self.write(["native_resolution": "1"])
                } else {
                    let wh = v.split(separator: "x").map(String.init)
                    guard wh.count == 2 else { return }
                    self.write(["native_resolution": "0", "width": wh[0], "height": wh[1]])
                }
            })
    }

    private func write(_ changes: [String: String]) {
        guard let config else { return }
        for (k, v) in changes {
            config.set(k, v)
            values[k] = v
        }
        do { try config.save() } catch { alertMessage = "Could not save config.cfg: \(error.localizedDescription)" }
    }

    // MARK: Disc selection — mirrors the Windows launcher's Auto rules.

    var selectedDiscName: String { values["disc_image"] ?? "" }

    var bootDisc: Disc? {
        let name = selectedDiscName
        if name.isEmpty { return autoPickDisc() }
        return discs.first { $0.fileName.caseInsensitiveCompare(name) == .orderedSame }
    }

    private func discForRegion(_ region: String) -> Disc? {
        let knownNames = [
            "Silent Hill (USA).bin",
            "Silent Hill (PAL).bin",
            "Silent Hill (Europe) (En,Fr,De,Es,It).bin",
            "Silent Hill (Japan).bin",
        ]
        for kn in knownNames {
            if let d = discs.first(where: {
                $0.region == region && $0.fileName.caseInsensitiveCompare(kn) == .orderedSame
            }) { return d }
        }
        return discs.first { $0.region == region }
    }

    private func autoPickDisc() -> Disc? {
        let pref = ["usa": "USA", "pal": "PAL", "jap": "JAP"][values["region"] ?? "auto"]
        if let pref, let d = discForRegion(pref) { return d }
        for r in ["USA", "PAL", "JAP"] {
            if let d = discForRegion(r) { return d }
        }
        return nil
    }

    var discOptions: [Option] {
        var opts: [Option] = [("Auto", "")]
        opts += discs.map { ($0.fileName + ($0.modified ? "  [modified]" : ""), $0.fileName) }
        let saved = selectedDiscName
        if !saved.isEmpty, !discs.contains(where: { $0.fileName.caseInsensitiveCompare(saved) == .orderedSame }) {
            opts.append((saved + "  [missing]", saved))
        }
        return opts
    }

    func addDisc(_ picked: URL) {
        var src = picked
        if src.pathExtension.lowercased() == "cue" {
            guard let bin = DiscProbe.binForCue(src) else {
                alertMessage = "That .cue sheet doesn't name a .bin file."
                return
            }
            src = bin
        }
        guard let disc = DiscProbe.probe(src) else {
            alertMessage = "\(src.lastPathComponent) isn't a Silent Hill disc image.\n\nUse the raw .bin of the USA, PAL/European or Japanese PlayStation release."
            return
        }

        let dest = GameInstall.gamedataDir.appendingPathComponent(disc.fileName)
        let select = { [weak self] in
            guard let self else { return }
            self.reload()
            if self.discs.count > 1 { self.write(["disc_image": disc.fileName]) }
            self.statusMessage = "Added \(disc.serial) — \(disc.regionLabel)."
        }
        if FileManager.default.fileExists(atPath: dest.path) {
            select()
            return
        }

        busy = true
        statusMessage = "Copying \(disc.fileName)…"
        Task.detached { [src] in
            let result = Result { try FileManager.default.copyItem(at: src, to: dest) }
            await MainActor.run {
                self.busy = false
                if case .failure(let error) = result {
                    self.statusMessage = nil
                    self.alertMessage = "Could not copy the disc image: \(error.localizedDescription)"
                } else {
                    select()
                }
            }
        }
    }

    // MARK: Play

    func play() {
        guard !gameRunning else { return }
        do { try GameInstall.prepare() } catch {
            alertMessage = "Could not refresh the game folder: \(error.localizedDescription)"
            return
        }

        let consoleLog = GameInstall.dataDir.appendingPathComponent("console.log")
        FileManager.default.createFile(atPath: consoleLog.path, contents: nil)
        let out = try? FileHandle(forWritingTo: consoleLog)

        let p = Process()
        p.executableURL = GameInstall.gameExecutable
        p.currentDirectoryURL = GameInstall.dataDir
        p.standardOutput = out ?? FileHandle.nullDevice
        p.standardError = out ?? FileHandle.nullDevice
        p.terminationHandler = { proc in
            let status = proc.terminationStatus
            let crashed = proc.terminationReason == .uncaughtSignal || status != 0
            Task { @MainActor in self.gameExited(crashed: crashed, status: status) }
        }
        do {
            try p.run()
        } catch {
            alertMessage = "Could not start the game: \(error.localizedDescription)"
            return
        }
        gameRunning = true
        statusMessage = nil
        NSApp.hide(nil)
    }

    private func gameExited(crashed: Bool, status: Int32) {
        gameRunning = false
        // The game writes in-game option changes to config.cfg; show them.
        reload()
        NSApp.unhide(nil)
        NSApp.activate(ignoringOtherApps: true)
        if crashed {
            statusMessage = "The game quit unexpectedly (code \(status)). See console.log in the game folder."
        }
    }

    func openGameFolder() {
        NSWorkspace.shared.open(GameInstall.dataDir)
    }

    func editConfig() {
        NSWorkspace.shared.open(
            [GameInstall.configURL],
            withApplicationAt: URL(fileURLWithPath: "/System/Applications/TextEdit.app"),
            configuration: NSWorkspace.OpenConfiguration())
    }

    private static func loadLanguages() -> [Option] {
        var opts: [Option] = [("Automatic", "auto"), ("US English", "en_us")]
        let dir = GameInstall.gamedataDir.appendingPathComponent("lang")
        let files = ((try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "lang" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
        for f in files {
            guard let text = try? String(contentsOf: f, encoding: .utf8) else { continue }
            var code: String?, name: String?
            for line in text.split(separator: "\n", omittingEmptySubsequences: false).prefix(16) {
                let l = line.trimmingCharacters(in: .whitespacesAndNewlines)
                if l.hasPrefix("!code=") { code = String(l.dropFirst(6)) }
                if l.hasPrefix("!name=") { name = String(l.dropFirst(6)) }
            }
            guard let code, !code.isEmpty, !opts.contains(where: { $0.value == code }) else { continue }
            opts.append((name ?? code, code))
        }
        return opts
    }
}

struct OptionPicker: View {
    let title: String
    @Binding var selection: String
    let options: [Option]

    var body: some View {
        Picker(title, selection: $selection) {
            ForEach(allOptions, id: \.value) { Text($0.label).tag($0.value) }
        }
    }

    private var allOptions: [Option] {
        options.contains { $0.value == selection } ? options : options + [("Custom (\(selection))", selection)]
    }
}

struct LauncherView: View {
    @EnvironmentObject var model: LauncherModel
    @State private var dropTargeted = false

    var body: some View {
        VStack(spacing: 0) {
            if let banner = Bundle.main.image(forResource: "launcher") {
                Image(nsImage: banner)
                    .resizable()
                    .aspectRatio(contentMode: .fill)
                    .frame(width: 560, height: 560 * banner.size.height / banner.size.width)
                    .clipped()
            }

            if let err = model.setupError {
                Text(err).foregroundStyle(.red).padding()
            } else {
                settingsForm
            }

            Divider()
            footer
        }
        .frame(width: 560)
        .overlay {
            if dropTargeted {
                RoundedRectangle(cornerRadius: 8)
                    .stroke(Color.accentColor, lineWidth: 4)
                    .padding(4)
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                guard let url else { return }
                Task { @MainActor in model.addDisc(url) }
            }
            return true
        }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            if !model.gameRunning && !model.busy { model.reload() }
        }
        .alert(
            model.alertMessage ?? "",
            isPresented: Binding(get: { model.alertMessage != nil }, set: { if !$0 { model.alertMessage = nil } })
        ) {
            Button("OK", role: .cancel) {}
        }
    }

    private var settingsForm: some View {
        Form {
            Section {
                if GameInstall.isTranslocated {
                    Label("Move Silent Hill PC into your Applications folder, then open it from there.",
                          systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                }
                OptionPicker(title: "Disc", selection: model.value("disc_image", ""), options: model.discOptions)
                    .disabled(model.discs.isEmpty)
                discStatus
                HStack {
                    Button("Add Disc Image…", action: chooseDisc)
                        .disabled(model.busy)
                    if model.busy { ProgressView().controlSize(.small) }
                    Spacer()
                    Text("or drop a .bin / .cue here").font(.caption).foregroundStyle(.secondary)
                }
            } header: {
                Text("Disc")
            }

            Section("Display") {
                OptionPicker(title: "Mode", selection: model.value("fullscreen", "0"), options: [
                    ("Windowed", "0"), ("Fullscreen", "1"), ("Borderless", "2"),
                ])
                OptionPicker(title: "Resolution", selection: model.resolution, options: Self.resolutions)
                OptionPicker(title: "Widescreen", selection: model.value("widescreen_mode", "1"), options: [
                    ("4:3 Pillarbox", "0"), ("Hor+ (wider view)", "1"), ("Stretch", "2"),
                ])
                OptionPicker(title: "VSync", selection: model.value("vsync", "0"), options: [
                    ("Off", "0"), ("On", "1"), ("Adaptive", "-1"),
                ])
                OptionPicker(title: "FPS cap", selection: model.value("fps_cap", "30"), options: [
                    ("30 (PSX)", "30"), ("60", "60"), ("Uncapped", "0"),
                ])
            }

            Section("Graphics") {
                OptionPicker(title: "Texture filtering", selection: model.value("texture_filter", "0"), options: [
                    ("Off (PSX)", "0"), ("Bilinear", "1"), ("Trilinear", "2"),
                    ("Anisotropic 2x", "3"), ("Anisotropic 4x", "4"), ("Anisotropic 8x", "5"), ("Anisotropic 16x", "6"),
                ])
                Toggle("PSX dithering", isOn: model.flag("dithering"))
                OptionPicker(title: "Antialiasing", selection: model.value("msaa", "0"), options: [
                    ("Off", "0"), ("2x MSAA", "2"), ("4x MSAA", "4"), ("8x MSAA", "8"),
                ])
                OptionPicker(title: "Flashlight", selection: model.value("flashlight_mode", "0"), options: [
                    ("Classic", "0"), ("Classic + Shadows", "1"), ("Modern", "2"), ("Modern + Shadows", "3"),
                ])
                OptionPicker(title: "Post-process", selection: model.value("post_process", "0"), options: [
                    ("Off", "0"), ("CRT", "1"), ("Scanlines", "2"), ("Vignette", "3"), ("Color Grade", "4"),
                    ("Film Grain", "5"), ("Sharpen", "6"), ("PSX Retro", "7"), ("Cinematic", "8"),
                ])
                OptionPicker(title: "Tone mapping", selection: model.value("tonemap", "0"), options: [
                    ("Off", "0"), ("Reinhard", "1"), ("ACES", "2"), ("Filmic", "3"),
                ])
            }

            Section("Game") {
                OptionPicker(title: "Language", selection: model.value("language", "auto"), options: model.languages)
                OptionPicker(title: "Intro", selection: model.value("skip_intros", "0"), options: [
                    ("Show logos and movie", "0"), ("Skip to main menu", "1"), ("Skip straight into gameplay", "2"),
                ])
                Toggle("Uncensored (PAL / NTSC-J discs)", isOn: model.flag("uncensored"))
            }
        }
        .formStyle(.grouped)
        .frame(height: 520)
    }

    @ViewBuilder private var discStatus: some View {
        if model.discs.isEmpty {
            Text("No Silent Hill disc image yet. Add the .bin of your USA, PAL/European or Japanese disc.")
                .foregroundStyle(.secondary)
        } else if let d = model.bootDisc {
            if !d.supported {
                Text("\(d.serial) \(d.region) — \(d.unsupportedReason ?? "not supported yet")")
                    .foregroundStyle(.red)
            } else if d.modified {
                Text("\(d.serial) \(d.region) — fan patch").foregroundStyle(.blue)
            } else {
                Text("\(d.serial) — \(d.regionLabel)").foregroundStyle(.secondary)
            }
        } else {
            Text("The selected disc image is missing from the game folder.").foregroundStyle(.red)
        }
    }

    private var footer: some View {
        HStack {
            Button("Open Game Folder") { model.openGameFolder() }
            Button("Edit config.cfg") { model.editConfig() }
            Spacer()
            if let msg = model.statusMessage {
                Text(msg).font(.caption).foregroundStyle(.secondary).lineLimit(2)
            }
            Button(model.gameRunning ? "Running…" : "Play") { model.play() }
                .keyboardShortcut(.defaultAction)
                .controlSize(.large)
                .disabled(!canPlay)
        }
        .padding(12)
    }

    private var canPlay: Bool {
        guard model.setupError == nil, !model.gameRunning, !model.busy, let d = model.bootDisc else { return false }
        return d.supported
    }

    private func chooseDisc() {
        let panel = NSOpenPanel()
        panel.message = "Choose your Silent Hill disc image (.bin or .cue)"
        panel.allowedContentTypes = [UTType(filenameExtension: "bin"), UTType(filenameExtension: "cue")].compactMap { $0 }
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            model.addDisc(url)
        }
    }

    private static let resolutions: [Option] = {
        var res = ["640x480", "1024x768", "1280x720", "1280x960", "1600x1200", "1920x1080", "2560x1440", "3840x2160"]
        if let s = NSScreen.main {
            let px = "\(Int(s.frame.width * s.backingScaleFactor))x\(Int(s.frame.height * s.backingScaleFactor))"
            let pt = "\(Int(s.frame.width))x\(Int(s.frame.height))"
            for r in [pt, px] where !res.contains(r) { res.append(r) }
        }
        let sorted = res.sorted {
            let a = $0.split(separator: "x").compactMap { Int($0) }, b = $1.split(separator: "x").compactMap { Int($0) }
            return a[0] * a[1] < b[0] * b[1]
        }
        return [("Native (PSX)", "native")] + sorted.map { ($0.replacingOccurrences(of: "x", with: " × "), $0) }
    }()
}
