import Foundation
import AppKit

/// Outcome of a `--scan` run. Plain `String` can't be used as `Result`'s
/// failure type (it doesn't conform to `Error`), so this stands in for a
/// `Result<[String], String>`.
enum ScanOutcome {
    case success([String])
    case failure(String)
}

/// Outcome of deleting a single item.
enum DeleteOutcome {
    case success
    case failure(String)
}

/// Drives the `clean_metadata` CLI (scanning) and performs deletion of
/// approved items natively. The CLI's fast, multithreaded scanner does the
/// expensive recursive walk; this app is a thin, interactive approval layer
/// on top of it.
@MainActor
final class AppState: ObservableObject {
    @Published var rootURL: URL?
    @Published var items: [MatchItem] = []
    @Published var isScanning = false
    @Published var isDeleting = false
    @Published var errorMessage: String?
    @Published var lastSummary: String?

    private lazy var cliBinary: URL? = AppState.locateCLIBinary()

    var selectedCount: Int {
        items.filter { $0.isSelected && $0.status == .pending }.count
    }

    // MARK: - User actions

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.title = "Choose a Folder to Scan"
        panel.message = "clean_metadata will recursively scan the chosen folder for hidden metadata files."
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.canCreateDirectories = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Scan"

        guard panel.runModal() == .OK, let url = panel.url else { return }
        rootURL = url
        items = []
        errorMessage = nil
        lastSummary = nil
    }

    func scan() async {
        guard let root = rootURL else { return }
        guard let binary = cliBinary else {
            errorMessage = "Could not find the clean_metadata command-line tool. Build it (make) in the repo root first — see the README — then relaunch this app."
            return
        }

        isScanning = true
        errorMessage = nil
        lastSummary = nil
        items = []
        defer { isScanning = false }

        switch await AppState.runScan(binary: binary, path: root.path) {
        case .success(let paths):
            items = paths.sorted().map { MatchItem(path: $0) }
            if items.isEmpty {
                lastSummary = "No target housekeeping files or directories found."
            }
        case .failure(let message):
            errorMessage = message
        }
    }

    func setSelected(_ selected: Bool, for id: String) {
        guard let idx = items.firstIndex(where: { $0.id == id }) else { return }
        items[idx].isSelected = selected
    }

    func selectAll(_ selected: Bool) {
        for idx in items.indices where items[idx].status == .pending {
            items[idx].isSelected = selected
        }
    }

    func removeSelected() async {
        let toRemove = items.filter { $0.isSelected && $0.status == .pending }
        guard !toRemove.isEmpty else { return }

        isDeleting = true
        defer { isDeleting = false }

        for item in toRemove {
            setStatus(.removing, for: item.id)
        }

        let results = await AppState.deleteItems(toRemove)

        var succeeded = 0
        var failed = 0
        for item in toRemove {
            switch results[item.path] {
            case .success:
                setStatus(.removed, for: item.id)
                succeeded += 1
            case .failure(let message):
                setStatus(.failed(message), for: item.id)
                failed += 1
            case .none:
                setStatus(.failed("No result reported"), for: item.id)
                failed += 1
            }
        }

        if let root = rootURL, succeeded > 0 {
            AppState.touchMarker(in: root)
        }

        lastSummary = failed == 0
            ? "Removed \(succeeded) item(s)."
            : "Removed \(succeeded) item(s), \(failed) failed. See status icons below for details."
    }

    private func setStatus(_ status: ItemStatus, for id: String) {
        guard let idx = items.firstIndex(where: { $0.id == id }) else { return }
        items[idx].status = status
    }
}

// MARK: - CLI interop & filesystem work (kept off the main actor)

extension AppState {
    /// Looks for the `clean_metadata` CLI binary: first alongside this app
    /// when bundled (Contents/Resources/clean_metadata), then a handful of
    /// locations useful during local development, then $PATH.
    nonisolated static func locateCLIBinary() -> URL? {
        let fm = FileManager.default
        var candidates: [URL] = []

        if let resourceURL = Bundle.main.resourceURL {
            candidates.append(resourceURL.appendingPathComponent("clean_metadata"))
        }

        let cwd = URL(fileURLWithPath: fm.currentDirectoryPath)
        candidates.append(cwd.appendingPathComponent("clean_metadata"))
        candidates.append(cwd.deletingLastPathComponent().appendingPathComponent("clean_metadata"))
        candidates.append(fm.homeDirectoryForCurrentUser.appendingPathComponent("bin/clean_metadata"))
        candidates.append(URL(fileURLWithPath: "/usr/local/bin/clean_metadata"))

        if let pathEnv = ProcessInfo.processInfo.environment["PATH"] {
            for dir in pathEnv.split(separator: ":") {
                candidates.append(URL(fileURLWithPath: String(dir)).appendingPathComponent("clean_metadata"))
            }
        }

        return candidates.first { fm.isExecutableFile(atPath: $0.path) }
    }

    /// Runs `clean_metadata --scan <path>` and parses its NUL-delimited
    /// stdout. Stdout/stderr are drained concurrently with waiting for the
    /// process to exit, to avoid deadlocking on a full pipe buffer for
    /// large scans.
    nonisolated static func runScan(binary: URL, path: String) async -> ScanOutcome {
        let process = Process()
        process.executableURL = binary
        process.arguments = ["--scan", path]
        process.standardInput = FileHandle.nullDevice

        let outPipe = Pipe()
        let errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe

        do {
            try process.run()
        } catch {
            return .failure("Failed to launch clean_metadata: \(error.localizedDescription)")
        }

        async let outData = readAll(outPipe.fileHandleForReading)
        async let errData = readAll(errPipe.fileHandleForReading)

        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            process.terminationHandler = { _ in continuation.resume() }
        }

        let out = await outData
        let err = await errData

        if process.terminationStatus == 0 {
            let paths = out.split(separator: 0)
                .compactMap { String(decoding: $0, as: UTF8.self) }
                .filter { !$0.isEmpty }
            return .success(paths)
        } else {
            let message = String(decoding: err, as: UTF8.self)
                .trimmingCharacters(in: .whitespacesAndNewlines)
            return .failure(message.isEmpty
                ? "clean_metadata exited with status \(process.terminationStatus)"
                : message)
        }
    }

    nonisolated static func readAll(_ handle: FileHandle) async -> Data {
        var data = Data()
        do {
            for try await byte in handle.bytes {
                data.append(byte)
            }
        } catch {
            // Best-effort: return whatever was read before the error.
        }
        return data
    }

    /// Deletes every given item concurrently via FileManager (which
    /// recursively removes directories in a single call), reporting a
    /// per-path result.
    nonisolated static func deleteItems(_ items: [MatchItem]) async -> [String: DeleteOutcome] {
        await withTaskGroup(of: (String, DeleteOutcome).self) { group in
            for item in items {
                group.addTask {
                    do {
                        try FileManager.default.removeItem(atPath: item.path)
                        return (item.path, .success)
                    } catch {
                        return (item.path, .failure(error.localizedDescription))
                    }
                }
            }
            var results: [String: DeleteOutcome] = [:]
            for await (path, result) in group {
                results[path] = result
            }
            return results
        }
    }

    /// Mirrors the CLI's index-inhibition marker: create (don't truncate)
    /// `.metadata_never_index` at the scanned root after a successful
    /// cleanup.
    nonisolated static func touchMarker(in root: URL) {
        let markerPath = root.appendingPathComponent(".metadata_never_index").path
        if !FileManager.default.fileExists(atPath: markerPath) {
            FileManager.default.createFile(atPath: markerPath, contents: nil)
        }
    }
}
