import SwiftUI
import UniformTypeIdentifiers
import SoulXCore

/// An instance's apps: install (.apk/.xapk/.apks/.apkm, from a picker or dropped on the section), open, force-stop
/// and uninstall. Android must be running for any of it.
struct AppsSection: View {
    @EnvironmentObject var model: AppModel
    let instance: Instance
    @State private var apps: [InstalledApp]?
    @State private var importing = false
    @State private var installing: String?          // what is being installed, shown with a spinner
    @State private var removing: InstalledApp?
    @State private var dropTargeted = false

    private static let types: [UTType] = ["apk", "xapk", "apks", "apkm"].compactMap { UTType(filenameExtension: $0) }

    var body: some View {
        Section {
            if !instance.running {
                Label(L("Start the instance to install and open apps."), systemImage: "power")
                    .foregroundStyle(.secondary)
            } else {
                if let installing {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text(String(format: L("Installing %@…"), installing)).foregroundStyle(.secondary)
                    }
                }
                if let apps, !apps.isEmpty {
                    ForEach(apps) { row($0) }
                } else if apps != nil {
                    Text(L("No apps yet. Install an APK, or drop one here.")).foregroundStyle(.secondary)
                } else {
                    ProgressView().controlSize(.small)
                }
            }
            HStack {
                Text(L(".apk, .xapk, .apks or .apkm; you can also drop files here."))
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button(L("Install APK…")) { importing = true }
                    .disabled(!instance.running || installing != nil)
            }
        } header: {
            Text(L("Apps"))
        }
        .overlay {
            if dropTargeted {
                RoundedRectangle(cornerRadius: 10).strokeBorder(Color.accentColor, lineWidth: 2)
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            guard instance.running, installing == nil else { return false }
            Task { await install(await urls(from: providers)) }
            return true
        }
        .task(id: instance.running) { if instance.running { await reload() } else { apps = nil } }
        .fileImporter(isPresented: $importing, allowedContentTypes: Self.types.isEmpty ? [.data] : Self.types,
                      allowsMultipleSelection: true) { result in
            guard case .success(let urls) = result else { return }
            Task {
                let scoped = urls.map { ($0, $0.startAccessingSecurityScopedResource()) }
                defer { scoped.forEach { if $0.1 { $0.0.stopAccessingSecurityScopedResource() } } }
                await install(urls)
            }
        }
        .confirmationDialog(removing.map { String(format: L("Uninstall %@ and delete its data?"), $0.title) } ?? "",
                            isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })) {
            Button(L("Uninstall"), role: .destructive) {
                guard let app = removing else { return }
                removing = nil
                Task { await model.appAction(instance.name, "uninstall", app.package); await reload() }
            }
            Button(L("Cancel"), role: .cancel) { removing = nil }
        }
    }

    private func row(_ app: InstalledApp) -> some View {
        HStack {
            Image(systemName: "app").foregroundStyle(.secondary).frame(width: 18)
            VStack(alignment: .leading, spacing: 2) {
                Text(app.title).lineLimit(1)
                if app.title != app.package {
                    Text(app.package).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        .textSelection(.enabled)
                }
            }
            Spacer()
            Button(L("Open")) { Task { await model.appAction(instance.name, "launch", app.package) } }
            Menu {
                Button(L("Force stop")) { Task { await model.appAction(instance.name, "stop-app", app.package) } }
                Divider()
                Button(L("Uninstall…"), role: .destructive) { removing = app }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
        }
    }

    private func install(_ urls: [URL]) async {
        let files = urls.filter { ["apk", "xapk", "apks", "apkm"].contains($0.pathExtension.lowercased()) }
        guard !files.isEmpty else {
            model.alert = L("Only .apk, .xapk, .apks and .apkm files can be installed.")
            return
        }
        installing = files.count == 1 ? files[0].lastPathComponent : String(format: L("%d apps"), files.count)
        _ = await model.installApps(instance.name, files)
        installing = nil
        await reload()
    }

    private func urls(from providers: [NSItemProvider]) async -> [URL] {
        var out: [URL] = []
        for p in providers {
            if let url = await withCheckedContinuation({ (c: CheckedContinuation<URL?, Never>) in
                _ = p.loadObject(ofClass: URL.self) { url, _ in c.resume(returning: url) }
            }) { out.append(url) }
        }
        return out
    }

    private func reload() async {
        apps = await model.apps(instance.name) ?? []
    }
}
