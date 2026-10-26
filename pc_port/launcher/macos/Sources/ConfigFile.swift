import Foundation

/// config.cfg reader/writer with the Windows launcher's contract
/// (ConfigManager.cs): only keys the launcher set are written back, and the
/// file is re-read right before saving so edits made by the game or a text
/// editor since the launcher started are kept.
final class ConfigFile {
    let url: URL
    private(set) var values: [String: String] = [:]
    private var dirty: [String: String] = [:]

    init(url: URL) {
        self.url = url
        load()
    }

    func load() {
        values = [:]
        dirty = [:]
        for line in readLines() {
            if let (k, v) = Self.split(line) { values[k] = v }
        }
    }

    func get(_ key: String, _ fallback: String = "") -> String {
        values[key] ?? fallback
    }

    func set(_ key: String, _ value: String) {
        guard values[key] != value else { return }
        values[key] = value
        dirty[key] = value
    }

    func save() throws {
        guard !dirty.isEmpty else { return }
        var lines = readLines()
        var pending = dirty
        for i in lines.indices {
            guard let (k, _) = Self.split(lines[i]), let v = pending[k] else { continue }
            lines[i] = "\(k) = \(v)"
            pending[k] = nil
        }
        for (k, v) in pending.sorted(by: { $0.key < $1.key }) {
            lines.append("\(k) = \(v)")
        }
        var text = lines.joined(separator: "\n")
        if !text.hasSuffix("\n") { text += "\n" }
        try text.write(to: url, atomically: true, encoding: .utf8)
        dirty = [:]
        for line in lines {
            if let (k, v) = Self.split(line) { values[k] = v }
        }
    }

    private func readLines() -> [String] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        var lines = text.components(separatedBy: "\n").map {
            $0.hasSuffix("\r") ? String($0.dropLast()) : $0
        }
        if lines.last == "" { lines.removeLast() }
        return lines
    }

    private static func split(_ line: String) -> (String, String)? {
        if line.trimmingCharacters(in: .whitespaces).hasPrefix("#") { return nil }
        guard let eq = line.firstIndex(of: "=") else { return nil }
        let key = line[..<eq].trimmingCharacters(in: .whitespaces)
        let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
        return key.isEmpty ? nil : (key, value)
    }
}
