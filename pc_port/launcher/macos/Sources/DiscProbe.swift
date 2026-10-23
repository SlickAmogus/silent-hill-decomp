import CryptoKit
import Foundation

/// Swift port of the Windows launcher's DiscProbe.cs, which mirrors the game's
/// Pc_DetectRegionFromBin (main_pc.c). Keep all three in agreement: a bin the
/// launcher reports must be one the game would accept.
struct Disc: Identifiable, Hashable {
    var id: String { fileName }
    let url: URL
    let fileName: String
    let serial: String
    let region: String
    let regionLabel: String
    let supported: Bool
    let unsupportedReason: String?
    let modified: Bool
}

enum DiscProbe {
    private static let sectorSize: UInt64 = 2352
    private static let dataOffset: UInt64 = 24
    private static let jpRev12TextSize: UInt32 = 0x13800

    private static let regionMap: [(prefix: String, region: String, label: String)] = [
        ("SLUS", "USA", "USA / NTSC-U"),
        ("SLES", "PAL", "PAL / Europe (En,Fr,De,Es,It)"),
        ("SLPS", "JAP", "Japan / NTSC-J"),
        ("SLPM", "JAP", "Japan / NTSC-J"),
        ("SIPS", "JAP", "Japan / NTSC-J"),
    ]

    private static let bodyprogMarker: [String: (lba: UInt64, sha1: String)] = [
        "USA": (0x0CF, "10f14642a565fb8d502397626734f71f270c4e9a"),
        "PAL": (0x0D0, "b2f3679c645650a72805a8afb3c5d68bdde61ebe"),
        "JAP": (0x0CF, "a467247d5f73b83a6ac424ab9c08ec50e874e85c"),
    ]
    private static let markerSectors: [UInt64] = [0, 2, 274]

    static func scan(_ folder: URL) -> [Disc] {
        let files = (try? FileManager.default.contentsOfDirectory(
            at: folder, includingPropertiesForKeys: nil)) ?? []
        return files
            .filter { $0.pathExtension.lowercased() == "bin" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
            .compactMap(probe)
    }

    static func probe(_ url: URL) -> Disc? {
        guard let f = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? f.close() }

        guard let pvd = readSector(f, 16) else { return nil }
        let rootLba = UInt64(le32(pvd, 158))
        guard let root = readSector(f, rootLba) else { return nil }

        var o = 0
        while o + 33 < 2048 {
            let recLen = Int(root[o])
            if recLen == 0 { break }
            let nameLen = Int(root[o + 32])
            if o + 33 + nameLen <= 2048, nameLen >= 4,
               let name = String(bytes: root[(o + 33)..<(o + 33 + nameLen)], encoding: .ascii),
               let entry = regionMap.first(where: { name.hasPrefix($0.prefix) })
            {
                var supported = true
                var reason: String?
                if entry.region == "JAP" {
                    let tSize = exeTextSize(f, UInt64(le32(root, o + 2)))
                    if tSize != jpRev12TextSize {
                        supported = false
                        reason = tSize == 0 ? "boot exe unreadable" : "NTSC-J first print (unsupported build)"
                    }
                }
                return Disc(
                    url: url,
                    fileName: url.lastPathComponent,
                    serial: formatSerial(name),
                    region: entry.region,
                    regionLabel: entry.label,
                    supported: supported,
                    unsupportedReason: reason,
                    modified: isBodyprogModified(f, entry.region))
            }
            o += recLen
        }
        return nil
    }

    private static func readSector(_ f: FileHandle, _ lba: UInt64) -> [UInt8]? {
        do {
            try f.seek(toOffset: lba * sectorSize + dataOffset)
            guard let d = try f.read(upToCount: 2048), d.count == 2048 else { return nil }
            return [UInt8](d)
        } catch {
            return nil
        }
    }

    private static func le32(_ b: [UInt8], _ o: Int) -> UInt32 {
        UInt32(b[o]) | UInt32(b[o + 1]) << 8 | UInt32(b[o + 2]) << 16 | UInt32(b[o + 3]) << 24
    }

    private static func exeTextSize(_ f: FileHandle, _ lba: UInt64) -> UInt32 {
        guard let hdr = readSector(f, lba),
              String(bytes: hdr[0..<8], encoding: .ascii) == "PS-X EXE" else { return 0 }
        return le32(hdr, 0x1C)
    }

    private static func isBodyprogModified(_ f: FileHandle, _ region: String) -> Bool {
        guard let marker = bodyprogMarker[region] else { return false }
        var sha = Insecure.SHA1()
        for rel in markerSectors {
            guard let sec = readSector(f, marker.lba + rel) else { return false }
            sha.update(data: sec)
        }
        let hex = sha.finalize().map { String(format: "%02x", $0) }.joined()
        return hex != marker.sha1
    }

    /// "SLES_015.14;1" -> "SLES-01514".
    private static func formatSerial(_ bootName: String) -> String {
        let base = bootName.split(separator: ";", maxSplits: 1).first.map(String.init) ?? bootName
        return base.replacingOccurrences(of: "_", with: "-").replacingOccurrences(of: ".", with: "")
    }

    /// The .bin a .cue sheet points at (Silent Hill is a single-track disc).
    static func binForCue(_ cue: URL) -> URL? {
        guard let text = (try? String(contentsOf: cue, encoding: .utf8))
                ?? (try? String(contentsOf: cue, encoding: .isoLatin1))
        else { return nil }
        for line in text.components(separatedBy: .newlines) {
            let t = line.trimmingCharacters(in: .whitespaces)
            guard t.uppercased().hasPrefix("FILE"),
                  let q1 = t.firstIndex(of: "\""),
                  let q2 = t[t.index(after: q1)...].firstIndex(of: "\"") else { continue }
            let name = String(t[t.index(after: q1)..<q2])
            return cue.deletingLastPathComponent().appendingPathComponent(name)
        }
        return nil
    }
}
