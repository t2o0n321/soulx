import SwiftUI
import SoulXCore

/// The sheet that offers a new version (what changed, update now / later, update automatically next time), and
/// answers "Check for updates".
struct UpdateSheet: View {
    @EnvironmentObject var updates: UpdateModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            header
            if let update = updates.update, showsUpdate {
                if !update.notes.isEmpty {
                    Text(L("What's new")).font(.headline)
                    ScrollView {
                        Text(notes(update.notes))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                    }
                    .frame(minHeight: 80, maxHeight: 220)
                }
            }
            status
            Toggle(L("Update automatically the next time SoulX opens"), isOn: $updates.autoUpdate)
                .disabled(updates.current == nil)
            if updates.update != nil, showsUpdate {
                Text(L("SoulX reopens after the update. Running instances keep running; restart them to use the new version."))
                    .font(.caption).foregroundStyle(.secondary)
            }
            buttons
        }
        .padding(20)
        .frame(width: 500)
    }

    /// The sheet is about an update (rather than the answer to "Check for updates")
    private var showsUpdate: Bool {
        switch updates.phase {
        case .checking, .upToDate: return false
        default: return true
        }
    }

    @ViewBuilder private var header: some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: updates.update != nil && showsUpdate ? "arrow.down.circle.fill" : "arrow.triangle.2.circlepath")
                .font(.system(size: 34))
                .foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 4) {
                if let update = updates.update, showsUpdate {
                    Text(String(format: L("SoulX %@ is available"), update.version.description)).font(.title3.bold())
                    if let current = updates.current {
                        Text(String(format: L("You have %@."), current.description)).foregroundStyle(.secondary)
                    }
                } else {
                    Text(L("Check for updates")).font(.title3.bold())
                    if let current = updates.current {
                        Text("SoulX \(current.description)").foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    @ViewBuilder private var status: some View {
        switch updates.phase {
        case .checking:
            HStack { ProgressView().controlSize(.small); Text(L("Checking for updates…")).foregroundStyle(.secondary) }
        case .upToDate:
            Label(String(format: L("SoulX is up to date (%@)."), updates.current?.description ?? ""),
                  systemImage: "checkmark.circle.fill")
                .foregroundStyle(.green)
        case .downloading(let fraction):
            ProgressView(value: fraction) {
                Text(String(format: L("Downloading… %d%%"), Int((fraction * 100).rounded())))
                    .font(.callout).foregroundStyle(.secondary).monospacedDigit()
            }
        case .installing:
            HStack { ProgressView().controlSize(.small); Text(L("Installing…")).foregroundStyle(.secondary) }
        case .relaunching:
            HStack { ProgressView().controlSize(.small); Text(L("Reopening SoulX…")).foregroundStyle(.secondary) }
        case .failed(let message):
            VStack(alignment: .leading, spacing: 6) {
                Label(message, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                if let page = updates.update?.page {
                    Link(L("Download it from GitHub"), destination: page)
                }
            }
        case .idle, .available:
            EmptyView()
        }
    }

    @ViewBuilder private var buttons: some View {
        HStack {
            if let page = updates.update?.page, showsUpdate {
                Link(L("View on GitHub"), destination: page)
            }
            Spacer()
            if updates.update != nil, showsUpdate {
                Button(L("Later")) { updates.dismiss() }
                    .keyboardShortcut(.cancelAction)
                    .disabled(updates.busy)
                Button(L("Update now")) { Task { await updates.install() } }
                    .keyboardShortcut(.defaultAction)
                    .buttonStyle(.borderedProminent)
                    .disabled(updates.busy)
            } else {
                Button(L("OK")) { updates.dismiss() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(updates.phase == .checking)
            }
        }
    }

    /// The release text keeps its line breaks; **bold** and `code` are shown as such.
    private func notes(_ text: String) -> AttributedString {
        (try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
            ?? AttributedString(text)
    }
}

/// Sidebar, above the Mac's cores and memory: shown while a newer SoulX is available.
struct UpdateBanner: View {
    @EnvironmentObject var updates: UpdateModel

    var body: some View {
        if let update = updates.update {
            Button { updates.showSheet = true } label: {
                Label(String(format: L("Update to %@"), update.version.description), systemImage: "arrow.down.circle.fill")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .controlSize(.regular)
            .help(String(format: L("SoulX %@ is available"), update.version.description))
        }
    }
}

/// Sidebar, next to the language menu: this version, "Check for updates" and the automatic update switch.
struct UpdateMenu: View {
    @EnvironmentObject var updates: UpdateModel

    var body: some View {
        Menu {
            if let current = updates.current {
                Text("SoulX \(current.description)")
                Button(L("Check for updates")) { Task { await updates.check(userAsked: true) } }
                    .disabled(updates.busy)
                Toggle(L("Update automatically the next time SoulX opens"), isOn: $updates.autoUpdate)
            } else {
                Text(L("Development build: no update checks."))
            }
        } label: {
            Image(systemName: "arrow.triangle.2.circlepath")
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help(L("Updates"))
    }
}
