import AppKit
import SwiftUI
import UniformTypeIdentifiers
import SoulXCore

/// An instance's scripts: list, import, rename, show in Finder, move to the Trash, and — while the instance is
/// running — run one directly (and stop it again). The running script sorts first; the list scrolls inside the
/// card like the apps list beside it.
struct ScriptsSection: View {
    @EnvironmentObject var model: AppModel
    let instance: Instance
    @State private var list: ScriptList?
    @State private var importing = false
    @State private var renaming: ScriptList.Script?
    @State private var newName = ""
    @State private var removing: ScriptList.Script?
    @State private var runningOnly = false
    @State private var hovered: String?               // the row under the pointer (highlighted, like the apps list)

    private var scripts: [ScriptList.Script] { list?.scripts ?? [] }
    private var runningPath: String? { instance.running ? instance.runningScript : nil }

    private var shown: [ScriptList.Script] {
        let running = scripts.filter { $0.path == runningPath }
        return runningOnly ? running : running + scripts.filter { $0.path != runningPath }
    }

    var body: some View {
        SectionCard(L("Scripts"), icon: "terminal", count: scripts.isEmpty ? nil : scripts.count) {
            HStack(spacing: 8) {
                SegmentedPill(selection: $runningOnly,
                              options: [(false, L("All")), (true, "● " + L("Running") + (runningPath == nil ? "" : " 1"))])
                Button { importing = true } label: { Label(L("Import…"), systemImage: "square.and.arrow.down") }
                    .buttonStyle(.pill)
            }
        } content: {
            ZStack {
                ScrollView {
                    LazyVStack(spacing: 0) { ForEach(shown) { row($0) } }
                }
                if let message = emptyMessage {
                    Text(message).font(.system(size: 11)).foregroundStyle(.secondary)
                        .multilineTextAlignment(.center).padding(24)
                } else if list == nil {
                    ProgressView().controlSize(.small)
                }
            }
            .frame(minHeight: fitHeight, idealHeight: fitHeight,
                   maxHeight: max(fitHeight, AppsSection.listHeight(rows: AppsSection.maxRows)))
            .overlay(alignment: .top) { Rectangle().fill(Color.primary.opacity(0.07)).frame(height: 1) }
        }
        .task(id: instance.name) { await reload() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType(filenameExtension: "lua") ?? .plainText, .folder],
                      allowsMultipleSelection: true) { result in
            guard case .success(let urls) = result else { return }
            Task {
                // files picked in the panel are security-scoped: hold access while the CLI copies them
                let scoped = urls.map { ($0, $0.startAccessingSecurityScopedResource()) }
                defer { scoped.forEach { if $0.1 { $0.0.stopAccessingSecurityScopedResource() } } }
                if let l = await model.importScripts(instance.name, urls) { list = l }
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
                Task { if let l = await model.renameScript(instance.name, file: s.file, to: title) { list = l } }
            }
        } message: { Text(L("The name shown in the window's script list.")) }
        .confirmationDialog(removeQuestion, isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })) {
            Button(L("Move to Trash"), role: .destructive) {
                guard let s = removing else { return }
                removing = nil
                Task { if let l = await model.removeScript(instance.name, entry: s.entry) { list = l } }
            }
            Button(L("Cancel"), role: .cancel) { removing = nil }
        }
    }

    private var fitHeight: CGFloat { AppsSection.listHeight(rows: shown.count) }

    private var emptyMessage: String? {
        guard list != nil else { return nil }
        if scripts.isEmpty { return L("No scripts yet. Import a .lua file or a script folder.") }
        if shown.isEmpty { return L("No script is running.") }
        return nil
    }

    private var removeQuestion: String {
        guard let s = removing else { return "" }
        return s.package ? String(format: L("Move the folder \"%@\" (the whole script package) to the Trash?"), s.entry)
                         : String(format: L("Move \"%@\" to the Trash?"), s.title)
    }

    private func row(_ s: ScriptList.Script) -> some View {
        let isRunning = s.path == runningPath
        return HStack(spacing: 12) {
            TagAvatar(text: "LUA", size: 24)
                .activeDot(isRunning, help: L("Running"))
            VStack(alignment: .leading, spacing: 2) {
                Text(s.title).font(.system(size: Metrics.listTitle, weight: .semibold)).lineLimit(1)
                Text(s.name == nil ? L("no name: the window will not list it") : s.file)
                    .font(.system(size: Metrics.listDetail, design: s.name == nil ? .default : .monospaced))
                    .foregroundStyle(s.name == nil ? Color.orange : Color.secondary)
                    .lineLimit(1).truncationMode(.middle)
            }
            Spacer(minLength: 12)
            if !instance.running {
                Text(L("Runs once started")).font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
            }
            Menu { scriptMenu(s) } label: {
                Image(systemName: "ellipsis").frame(width: 22, height: 22).contentShape(Circle())
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .foregroundStyle(.secondary)
            .help(L("More"))
        }
        .padding(.leading, 16).padding(.trailing, 8)
        .frame(height: AppsSection.rowHeight)
        .background(hovered == s.path ? Color.primary.opacity(0.06) : Color.clear)
        .overlay(alignment: .bottom) { Rectangle().fill(Color.primary.opacity(0.05)).frame(height: 1) }
        .contentShape(Rectangle())
        .onHover { hovered = $0 ? s.path : (hovered == s.path ? nil : hovered) }
        .onTapGesture(count: 2) { run(s) }                // like double-clicking an app opens it
        .contextMenu { scriptMenu(s) }
        .help(instance.running ? (runningPath == nil ? L("Double-click to run") : "") : L("Runs once started"))
    }

    @ViewBuilder private func scriptMenu(_ s: ScriptList.Script) -> some View {
        if s.path == runningPath {
            Button(L("Stop")) { model.stopScript(instance.name) }
        } else {
            Button(L("Run")) { run(s) }
                .disabled(!instance.running || runningPath != nil)
        }
        Divider()
        Button(L("Rename…")) { newName = s.name ?? ""; renaming = s }
        Button(L("Show in Finder")) { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: s.path)]) }
        Divider()
        Button(L("Move to Trash…"), role: .destructive) { removing = s }
    }

    /// Runs a script, when the instance runs and no other script does.
    private func run(_ s: ScriptList.Script) {
        guard instance.running, runningPath == nil else { return }
        Task { await model.runScript(instance.name, path: s.path) }
    }

    private func reload() async {
        list = await model.scripts(instance.name)
    }
}
