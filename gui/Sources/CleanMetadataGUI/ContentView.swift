import SwiftUI
import AppKit

struct ContentView: View {
    @StateObject private var state = AppState()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            Divider()
            if state.items.isEmpty {
                emptyState
            } else {
                resultsList
            }
            Divider()
            footer
        }
        .padding(16)
        .frame(minWidth: 600, minHeight: 440)
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 12) {
            Button("Choose Folder…") {
                state.chooseFolder()
            }
            .disabled(state.isScanning || state.isDeleting)

            if let root = state.rootURL {
                Text(root.path)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .foregroundStyle(.secondary)
            } else {
                Text("No folder selected")
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button {
                Task { await state.scan() }
            } label: {
                if state.isScanning {
                    ProgressView()
                        .controlSize(.small)
                        .frame(width: 40)
                } else {
                    Text("Scan")
                        .frame(width: 40)
                }
            }
            .disabled(state.rootURL == nil || state.isScanning || state.isDeleting)
        }
    }

    // MARK: - Empty state

    private var emptyState: some View {
        VStack {
            Spacer()
            if let error = state.errorMessage {
                Label(error, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.red)
                    .multilineTextAlignment(.center)
                    .padding()
            } else if let summary = state.lastSummary {
                Label(summary, systemImage: "checkmark.circle")
                    .foregroundStyle(.secondary)
            } else {
                Text("Choose a folder and click Scan to look for hidden metadata files (.DS_Store, .Spotlight-V100, Thumbs.db, and similar clutter).")
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .padding()
            }
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    // MARK: - Results

    private var resultsList: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("\(state.items.count) item(s) found — \(state.selectedCount) selected")
                    .font(.headline)
                Spacer()
                Button("Select All") { state.selectAll(true) }
                    .disabled(state.isDeleting)
                Button("Select None") { state.selectAll(false) }
                    .disabled(state.isDeleting)
            }

            List(state.items) { item in
                MatchRow(item: item) { selected in
                    state.setSelected(selected, for: item.id)
                }
            }
            .listStyle(.inset(alternatesRowBackgrounds: true))
        }
    }

    // MARK: - Footer

    private var footer: some View {
        HStack(alignment: .top) {
            if let error = state.errorMessage, !state.items.isEmpty {
                Label(error, systemImage: "exclamationmark.triangle")
                    .foregroundStyle(.red)
                    .lineLimit(2)
            } else if let summary = state.lastSummary {
                Text(summary)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            Button(role: .destructive) {
                confirmAndRemove()
            } label: {
                if state.isDeleting {
                    ProgressView().controlSize(.small)
                } else {
                    Text("Remove Selected (\(state.selectedCount))")
                }
            }
            .disabled(state.selectedCount == 0 || state.isDeleting || state.isScanning)
        }
    }

    private func confirmAndRemove() {
        let alert = NSAlert()
        alert.messageText = "Delete \(state.selectedCount) item(s)?"
        alert.informativeText = "This permanently deletes the selected files and folders (recursively, where applicable). This cannot be undone."
        alert.alertStyle = .warning
        alert.addButton(withTitle: "Delete")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        Task { await state.removeSelected() }
    }
}

// MARK: - Row

private struct MatchRow: View {
    let item: MatchItem
    let onToggle: (Bool) -> Void

    var body: some View {
        HStack(spacing: 10) {
            Toggle("", isOn: Binding(
                get: { item.isSelected },
                set: { onToggle($0) }
            ))
            .labelsHidden()
            .disabled(item.status != .pending)

            Image(systemName: item.isDirectory ? "folder" : "doc")
                .foregroundStyle(.secondary)
                .frame(width: 16)

            VStack(alignment: .leading, spacing: 1) {
                Text(item.displayName)
                Text(item.path)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer()

            statusView
        }
        .padding(.vertical, 2)
        .opacity(item.status == .removed ? 0.5 : 1.0)
    }

    @ViewBuilder
    private var statusView: some View {
        switch item.status {
        case .pending:
            EmptyView()
        case .removing:
            ProgressView().controlSize(.small)
        case .removed:
            Image(systemName: "checkmark.circle.fill")
                .foregroundStyle(.green)
        case .failed(let message):
            Image(systemName: "xmark.circle.fill")
                .foregroundStyle(.red)
                .help(message)
        }
    }
}
