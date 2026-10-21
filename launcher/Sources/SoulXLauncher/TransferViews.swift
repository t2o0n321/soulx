import AppKit
import SwiftUI
import UniformTypeIdentifiers
import SoulXCore

/// The .soulxvm file type (an Apple Archive; see vm_archive.c).
let soulxVmType = UTType(filenameExtension: "soulxvm") ?? .data

/// Asks where to save an instance's export, then starts it.
@MainActor func chooseExportFile(for name: String, model: AppModel) {
    let panel = NSSavePanel()
    panel.title = String(format: L("Export “%@”"), name)
    panel.nameFieldStringValue = name                 // the panel adds .soulxvm itself
    panel.allowedContentTypes = [soulxVmType]
    panel.canCreateDirectories = true
    panel.directoryURL = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first
    guard panel.runModal() == .OK, var url = panel.url else { return }
    // "x.soulxvm.soulxvm" (a name typed with the extension, for a type the system does not know): one is enough
    while url.pathExtension == "soulxvm", url.deletingPathExtension().pathExtension == "soulxvm" {
        url = url.deletingPathExtension()
    }
    model.exportInstance(name, to: url)
}

/// Asks for a .soulxvm file to import.
@MainActor func chooseImportFile() -> URL? {
    let panel = NSOpenPanel()
    panel.title = L("Import an existing instance")
    panel.allowedContentTypes = [soulxVmType]
    panel.allowsMultipleSelection = false
    panel.canChooseDirectories = false
    return panel.runModal() == .OK ? panel.url : nil
}

/// What a file holds, and under which name it comes in.
struct ImportSheet: View {
    @EnvironmentObject var model: AppModel
    let url: URL
    let manifest: VmManifest
    @Binding var isPresented: Bool
    @State private var name = ""
    @FocusState private var focused: Bool

    private var problem: String? {
        if name.isEmpty { return nil }
        if !isValidInstanceName(name) { return L("Lowercase letters, digits, - and _ (up to 32).") }
        if model.instance(name) != nil { return L("An instance with this name already exists.") }
        return nil
    }

    private var hint: String {
        if !manifest.imageKnown { return L("This file needs an Android version this SoulX does not have. Update SoulX first.") }
        if let problem { return problem }
        if name.isEmpty { return L("Give the instance a name first.") }
        return L("Your apps, game progress and scripts come with it.")
    }

    private var ready: Bool { manifest.imageKnown && !name.isEmpty && problem == nil }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 12) {
                AppMark(size: 36)
                VStack(alignment: .leading, spacing: 2) {
                    Text(L("Import an existing instance")).font(.system(size: 15, weight: .bold))
                    Text(url.lastPathComponent).font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
                        .lineLimit(1).truncationMode(.middle)
                }
                Spacer(minLength: 12)
                RoundIconButton(systemName: "xmark", help: L("Cancel")) { isPresented = false }
            }
            .padding(.leading, 22).padding(.trailing, 16).padding(.top, 20)

            VStack(alignment: .leading, spacing: 6) {
                Text(L("Name")).font(.system(size: Metrics.caption, weight: .semibold)).foregroundStyle(.secondary)
                TextField("", text: $name, prompt: Text(manifest.suggestedName))
                    .textFieldStyle(.plain)
                    .font(.system(size: Metrics.body))
                    .focused($focused)
                    .padding(.horizontal, 12).frame(height: Metrics.control + 4)
                    .background(Color.black.opacity(0.22), in: Capsule())
                    .overlay(Capsule().strokeBorder(problem != nil ? Color.red.opacity(0.7)
                                                    : (focused ? Color.accentColor : Color.primary.opacity(0.12))))
            }
            .padding(.horizontal, 22).padding(.top, 18)

            // what the file holds, grouped like the create sheet
            VStack(alignment: .leading, spacing: 6) {
                Text(L("In this file")).font(.system(size: Metrics.caption, weight: .semibold)).foregroundStyle(.secondary)
                    .padding(.leading, 4)
                VStack(spacing: 0) {
                    infoRow(L("Exported instance"), manifest.name)
                    infoRow(L("Android"), manifest.android.isEmpty ? manifest.image : manifest.android)
                    infoRow(L("Size"), ByteCountFormatter.string(fromByteCount: manifest.bytes, countStyle: .file))
                    infoRow(L("Scripts"), "\(manifest.scripts)")
                    infoRow(L("Exported"), exportedText)
                }
                .background(Color.primary.opacity(0.045), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
                .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.primary.opacity(0.08)))
            }
            .padding(.horizontal, 22).padding(.top, 16).padding(.bottom, 18)

            HStack(spacing: 10) {
                Image(systemName: ready || name.isEmpty ? "info.circle" : "exclamationmark.triangle.fill")
                    .foregroundStyle(ready || name.isEmpty ? Color.secondary : Color.red)
                Text(hint).foregroundStyle(ready || name.isEmpty ? Color.secondary : Color.red)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 12)
                Button(L("Cancel")) { isPresented = false }
                    .buttonStyle(.pill)
                    .keyboardShortcut(.cancelAction)
                Button(L("Import")) {
                    model.importArchive(url, as: name)
                    isPresented = false
                }
                .buttonStyle(.pillPrimary)
                .keyboardShortcut(.defaultAction)
                .disabled(!ready)
            }
            .font(.system(size: Metrics.caption))
            .padding(.horizontal, 22).padding(.vertical, 14)
            .overlay(alignment: .top) { Rectangle().fill(Color.primary.opacity(0.07)).frame(height: 1) }
        }
        .frame(width: 520)
        .fixedSize(horizontal: false, vertical: true)
        .onAppear {
            name = manifest.suggestedName
            focused = true
        }
    }

    private var exportedText: String {
        let iso = ISO8601DateFormatter()
        guard let d = iso.date(from: manifest.exported) else { return manifest.exported }
        return d.formatted(date: .abbreviated, time: .shortened) + " · SoulX " + manifest.soulxVersion
    }

    private func infoRow(_ label: String, _ value: String) -> some View {
        HStack(spacing: 12) {
            Text(label).font(.system(size: Metrics.body, weight: .medium))
            Spacer(minLength: 12)
            Text(value).font(.system(size: Metrics.body)).foregroundStyle(.secondary).monospacedDigit()
                .lineLimit(1).truncationMode(.middle)
        }
        .cardLine(vertical: 9)
    }
}

/// An export or import in progress, then its result.
struct TransferSheet: View {
    @EnvironmentObject var model: AppModel
    let transfer: VmTransfer

    private var title: String {
        let done = transfer.finished && transfer.error == nil
        switch transfer.kind {
        case .export: return String(format: L(done ? "Exported “%@”" : "Exporting “%@”"), transfer.name)
        case .import: return String(format: L(done ? "Imported “%@”" : "Importing “%@”"), transfer.name)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 12) {
                Image(systemName: transfer.kind == .export ? "square.and.arrow.up" : "square.and.arrow.down")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(Color.accentColor)
                    .frame(width: 36, height: 36)
                    .background(Color.accentColor.opacity(0.14), in: RoundedRectangle(cornerRadius: 11, style: .continuous))
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.system(size: 15, weight: .bold))
                    Text(transfer.file.lastPathComponent).font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
                        .lineLimit(1).truncationMode(.middle)
                }
            }
            if let error = transfer.error {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.system(size: Metrics.body)).foregroundStyle(.red)
                    .fixedSize(horizontal: false, vertical: true)
                Text(transfer.kind == .export ? L("The instance itself was not changed.")
                                              : L("Nothing was added; your instances are as they were."))
                    .font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
            } else if transfer.finished {
                Label(transfer.kind == .export ? L("Exported. Copy the file to another Mac and import it there.")
                                               : L("Imported. Start it from its page."),
                      systemImage: "checkmark.circle.fill")
                    .font(.system(size: Metrics.body)).foregroundStyle(.green)
            } else {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text(transfer.phase.isEmpty ? L("Preparing") : transfer.phase)
                            .font(.system(size: Metrics.body, weight: .medium))
                        Spacer()
                        if let f = transfer.fraction {
                            Text("\(Int((f * 100).rounded()))%").font(.system(size: Metrics.body))
                                .foregroundStyle(.secondary).monospacedDigit()
                        }
                    }
                    if let f = transfer.fraction { MeterBar(fraction: f) } else { ProgressView().controlSize(.small) }
                    Text(transfer.kind == .export ? L("Keep the instance stopped until this is done.")
                                                  : L("Every file is checked before the instance is added."))
                        .font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
                }
            }
            HStack(spacing: 10) {
                Spacer()
                if transfer.finished {
                    if transfer.kind == .export && transfer.error == nil {
                        Button(L("Show in Finder")) {
                            NSWorkspace.shared.activateFileViewerSelecting([transfer.file])
                        }
                        .buttonStyle(.pill)
                    }
                    Button(L("Done")) { model.transfer = nil }
                        .buttonStyle(.pillPrimary)
                        .keyboardShortcut(.defaultAction)
                } else {
                    Button(L("Cancel")) { model.cancelTransfer() }
                        .buttonStyle(.pill)
                        .keyboardShortcut(.cancelAction)
                }
            }
        }
        .padding(22)
        .frame(width: 460)
        .interactiveDismissDisabled(!transfer.finished)
    }
}
