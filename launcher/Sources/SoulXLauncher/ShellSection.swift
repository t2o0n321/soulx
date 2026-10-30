import AppKit
import SwiftUI
import SoulXCore

/// A quick way to open a macOS terminal wired to the instance's Android shell (`soulx shell`), and — when the
/// instance is rooted — a root shell (`su`). Both need the instance running, since they talk to it over adb.
struct ShellSection: View {
    @EnvironmentObject var model: AppModel
    let instance: Instance

    private var hint: String {
        instance.running ? L("Open a terminal connected to this instance.")
                         : L("Start the instance to open a shell.")
    }

    var body: some View {
        SectionCard(L("Shell"), icon: "apple.terminal") {
            Text(hint)
                .font(.system(size: 11)).foregroundStyle(.secondary)
                .lineLimit(1).truncationMode(.tail)
        } content: {
            HStack(spacing: 8) {
                Button { model.openShell(instance.name, root: false) } label: {
                    Label(L("Open Shell"), systemImage: "terminal")
                }
                .buttonStyle(.pill)
                .disabled(!instance.running)

                if instance.rooted {
                    Button { model.openShell(instance.name, root: true) } label: {
                        Label(L("Open Root Shell"), systemImage: "lock.open")
                    }
                    .buttonStyle(.pill)
                    .disabled(!instance.running)
                    .help(L("Opens a root shell (su) via Magisk."))
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16)
            .overlay(alignment: .top) { Rectangle().fill(Color.primary.opacity(0.07)).frame(height: 1) }
        }
    }
}
