import AppKit
import SwiftUI
import UniformTypeIdentifiers
import SoulXCore

/// An instance's scripts: list, import, rename, show in Finder, move to the Trash. Running them is the job of
/// SoulX's window (or `soulx run`), not the launcher's.
struct ScriptsSection: View {
    @EnvironmentObject var model: AppModel
    let instance: String
    @State private var list: ScriptList?
    @State private var importing = false
    @State private var renaming: ScriptList.Script?
    @State private var newName = ""
    @State private var removing: ScriptList.Script?

    var body: some View {
        Section {
            if let list, !list.scripts.isEmpty {
                ForEach(list.scripts) { s in row(s) }
            } else if list != nil {
                Text(L("No scripts yet. Import a .lua file or a script folder."))
                    .foregroundStyle(.secondary)
            } else {
                ProgressView().controlSize(.small)
            }
            HStack {
                Text(L("Run scripts from the instance's window."))
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button(L("Open folder")) { openFolder() }
                Button(L("Import…")) { importing = true }
            }
        } header: {
            Text(L("Scripts"))
        }
        .task(id: instance) { await reload() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "lua") ?? .plainText, .folder],
                      allowsMultipleSelection: true) { result in
            guard case .success(let urls) = result else { return }
            Task {
                // files picked in the panel are security-scoped: hold access while the CLI copies them
                let scoped = urls.map { ($0, $0.startAccessingSecurityScopedResource()) }
                defer { scoped.forEach { if $0.1 { $0.0.stopAccessingSecurityScopedResource() } } }
                if let l = await model.importScripts(instance, urls) { list = l }
            }
        }
        .alert(L("Rename script"), isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField(L("Name"), text: $newName)
            Button(L("Cancel"), role: .cancel) { renaming = nil }
            Button(L("Rename")) {
                guard let s = renaming else { return }
                let title = newName.trimmingCharacters(in: .whitespaces)
                renaming = nil
                guard !title.isEmpty else { return }
                Task { if let l = await model.renameScript(instance, file: s.file, to: title) { list = l } }
            }
        } message: { Text(L("The name shown in the window's script list.")) }
        .confirmationDialog(removeQuestion, isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })) {
            Button(L("Move to Trash"), role: .destructive) {
                guard let s = removing else { return }
                removing = nil
                Task { if let l = await model.removeScript(instance, entry: s.entry) { list = l } }
            }
            Button(L("Cancel"), role: .cancel) { removing = nil }
        }
    }

    private var removeQuestion: String {
        guard let s = removing else { return "" }
        return s.package ? String(format: L("Move the folder \"%@\" (the whole script package) to the Trash?"), s.entry)
                         : String(format: L("Move \"%@\" to the Trash?"), s.title)
    }

    private func row(_ s: ScriptList.Script) -> some View {
        HStack {
            Image(systemName: s.package ? "folder" : "doc.text")
                .foregroundStyle(.secondary)
                .frame(width: 18)
            VStack(alignment: .leading, spacing: 2) {
                Text(s.title)
                Text(s.file).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if s.name == nil {
                Text(L("no name: the window will not list it")).font(.caption).foregroundStyle(.orange)
            }
            Menu {
                Button(L("Rename…")) { newName = s.name ?? ""; renaming = s }
                Button(L("Show in Finder")) {
                    NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: s.path)])
                }
                Divider()
                Button(L("Move to Trash…"), role: .destructive) { removing = s }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
        }
        .contextMenu {
            Button(L("Rename…")) { newName = s.name ?? ""; renaming = s }
            Button(L("Show in Finder")) { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: s.path)]) }
            Button(L("Move to Trash…"), role: .destructive) { removing = s }
        }
    }

    private func reload() async {
        list = await model.scripts(instance)
    }

    private func openFolder() {
        Task {
            // `soulx scripts folder` creates it when missing
            if let r = await model.cli?.run(["scripts", "-i", instance, "folder"]), r.ok {
                NSWorkspace.shared.open(URL(fileURLWithPath: r.stdout.trimmingCharacters(in: .whitespacesAndNewlines)))
            }
        }
    }
}
