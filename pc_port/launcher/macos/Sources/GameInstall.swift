import Foundation

/// The game resolves config.cfg, gamedata/ and maps/ against its working
/// directory, and the .app bundle must stay read-only (writing into it breaks
/// the code signature and fails outright under App Translocation). So the game
/// runs from a writable folder in Application Support whose shipped files are
/// symlinks back into the bundle, refreshed on every start so an updated app
/// takes effect. A real file at a linked path is left alone: that is how a
/// user overrides or mods a shipped asset.
enum GameInstall {
    static let bundleGameDir = Bundle.main.resourceURL!.appendingPathComponent("game")
    static let gameExecutable = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/SilentHillPC")

    static let dataDir: URL = FileManager.default
        .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("SilentHillPC", isDirectory: true)
    static var gamedataDir: URL { dataDir.appendingPathComponent("gamedata", isDirectory: true) }
    static var configURL: URL { dataDir.appendingPathComponent("config.cfg") }

    static var isTranslocated: Bool {
        Bundle.main.bundlePath.contains("/AppTranslocation/")
    }

    static func prepare() throws {
        let fm = FileManager.default
        try fm.createDirectory(at: gamedataDir.appendingPathComponent("save"), withIntermediateDirectories: true)

        if !fm.fileExists(atPath: configURL.path) {
            try fm.copyItem(at: bundleGameDir.appendingPathComponent("config.cfg"), to: configURL)
        }

        pruneDanglingLinks()
        try linkTree(from: bundleGameDir, to: dataDir, skip: ["config.cfg"])
    }

    /// Strip the download quarantine flag from our own bundle, so the map
    /// .dylibs the game dlopen()s don't each raise a Gatekeeper prompt.
    /// Pointless (and impossible) while translocated.
    static func clearQuarantine() {
        guard !isTranslocated else { return }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/xattr")
        p.arguments = ["-dr", "com.apple.quarantine", Bundle.main.bundlePath]
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        try? p.run()
        p.waitUntilExit()
    }

    private static func linkTree(from src: URL, to dst: URL, skip: Set<String>) throws {
        let fm = FileManager.default
        let items = try fm.contentsOfDirectory(at: src, includingPropertiesForKeys: [.isDirectoryKey])
        for item in items where !skip.contains(item.lastPathComponent) {
            let target = dst.appendingPathComponent(item.lastPathComponent)
            let isDir = (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            let existingLink = try? fm.destinationOfSymbolicLink(atPath: target.path)

            if isDir {
                if existingLink != nil { try fm.removeItem(at: target) }
                try fm.createDirectory(at: target, withIntermediateDirectories: true)
                try linkTree(from: item, to: target, skip: [])
                continue
            }

            if let existingLink {
                if existingLink == item.path { continue }
                try fm.removeItem(at: target)
            } else if fm.fileExists(atPath: target.path) {
                continue
            }
            try fm.createSymbolicLink(at: target, withDestinationURL: item)
        }
    }

    /// Links left behind by a previous launch from a translocated path that no
    /// longer exists, or into another copy of the app (an older version still
    /// sitting in Downloads) whose files this version may not ship.
    private static func pruneDanglingLinks() {
        let fm = FileManager.default
        let ours = bundleGameDir.path + "/"
        guard let e = fm.enumerator(at: dataDir, includingPropertiesForKeys: [.isSymbolicLinkKey]) else { return }
        for case let url as URL in e {
            guard (try? url.resourceValues(forKeys: [.isSymbolicLinkKey]).isSymbolicLink) == true,
                  let dest = try? fm.destinationOfSymbolicLink(atPath: url.path) else { continue }
            let foreignBundle = dest.contains(".app/Contents/Resources/game/") && !dest.hasPrefix(ours)
            if foreignBundle || !fm.fileExists(atPath: url.path) {
                try? fm.removeItem(at: url)
            }
        }
    }
}
