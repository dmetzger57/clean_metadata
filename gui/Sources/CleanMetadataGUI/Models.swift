import Foundation

/// Lifecycle of a single matched item as the user approves and (optionally)
/// removes it.
enum ItemStatus: Equatable {
    case pending
    case removing
    case removed
    case failed(String)
}

/// One file or folder matched by `clean_metadata --scan`.
struct MatchItem: Identifiable, Equatable {
    /// The absolute path is unique per scan, so it doubles as the identity.
    var id: String { path }

    let path: String
    var isSelected: Bool = true
    var status: ItemStatus = .pending

    var displayName: String {
        (path as NSString).lastPathComponent
    }

    var isDirectory: Bool {
        var isDir: ObjCBool = false
        FileManager.default.fileExists(atPath: path, isDirectory: &isDir)
        return isDir.boolValue
    }
}
