import SwiftUI
import UniformTypeIdentifiers
import SoulXCore

/// An instance's apps: install (.apk/.xapk/.apks/.apkm, from a picker or dropped on the card), open, force-stop
/// and uninstall. Android must be running for all of that, but a stopped instance still shows the apps it had
/// the last time it ran (AppModel.appsCache), read-only. The list scrolls inside the card, so a phone with many
/// apps does not stretch the page; a search field and a list / grid switch help find one.
struct AppsSection: View {
    @EnvironmentObject var model: AppModel
    let instance: Instance
    @State private var apps: [InstalledApp]?
    @State private var importing = false
    @State private var installing: String?          // what is being installed, shown with a spinner
    @State private var removing: InstalledApp?
    @State private var dropTargeted = false
    @State private var query = ""
    @AppStorage("appsGrid") private var grid = false
    @AppStorage("appsFrontFirst") private var frontFirst = false

    /// The list grows with what is installed, up to `maxRows` rows; beyond that it scrolls inside the card.
    static let rowHeight: CGFloat = 40
    static let maxRows = 6

    /// The list's height for `rows` rows, at most `maxRows`; an empty list keeps two rows for its message.
    static func listHeight(rows: Int) -> CGFloat { CGFloat(min(rows == 0 ? 2 : rows, maxRows)) * rowHeight }
    private static let types: [UTType] = ["apk", "xapk", "apks", "apkm"].compactMap { UTType(filenameExtension: $0) }

    private var all: [InstalledApp] { instance.running ? (apps ?? []) : (model.appsCache[instance.name] ?? []) }

    private var shown: [InstalledApp] {
        let q = query.trimmingCharacters(in: .whitespaces).lowercased()
        let found = q.isEmpty ? all : all.filter { $0.title.lowercased().contains(q) || $0.package.lowercased().contains(q) }
        let byName = found.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
        return frontFirst ? byName.filter(\.running) + byName.filter { !$0.running } : byName
    }

    var body: some View {
        SectionCard(L("Apps"), icon: "shippingbox", count: all.isEmpty ? nil : all.count) {
            Button { importing = true } label: { Label(L("Install APK…"), systemImage: "plus") }
                .buttonStyle(.pill)
                .disabled(!instance.running || installing != nil)
                .help(L(".apk, .xapk, .apks or .apkm; you can also drop files here."))
        } content: {
            toolbar.cardLine(vertical: 10)
            ZStack {
                ScrollView {
                    if grid { gridBody } else { listBody }
                }
                if let message = emptyMessage {
                    Text(message).font(.system(size: 11)).foregroundStyle(.secondary)
                        .multilineTextAlignment(.center).padding(24)
                }
            }
            // grows with the apps up to the cap, and up to the cap again when the card beside it is taller
            .frame(minHeight: fitHeight, idealHeight: fitHeight, maxHeight: max(fitHeight, Self.listHeight(rows: Self.maxRows)))
            .overlay(alignment: .top) { Rectangle().fill(Color.primary.opacity(0.07)).frame(height: 1) }
        }
        .overlay {
            if dropTargeted && instance.running {
                RoundedRectangle(cornerRadius: 20, style: .continuous).strokeBorder(Color.accentColor, lineWidth: 2)
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            guard instance.running, installing == nil else { return false }
            Task { await install(await urls(from: providers)) }
            return true
        }
        .task(id: instance.running) {
            guard instance.running else { apps = nil; return }
            while !Task.isCancelled {
                await reload()
                try? await Task.sleep(nanoseconds: 2_500_000_000)   // keeps the "in front" badge in sync
            }
        }
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

    // MARK: Search, sort, view

    private var toolbar: some View {
        HStack(spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass").font(.system(size: 11)).foregroundStyle(.secondary)
                TextField("", text: $query, prompt: Text(L("Search name or package")))
                    .textFieldStyle(.plain).font(.system(size: Metrics.body))
                if !query.isEmpty {
                    Button { query = "" } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.plain).foregroundStyle(.secondary)
                        .help(L("Clear"))
                }
            }
            .padding(.horizontal, 12).frame(height: Metrics.control)
            .background(Color.primary.opacity(0.06), in: Capsule())
            .overlay(Capsule().strokeBorder(Color.primary.opacity(0.10)))
            ValueMenu(frontFirst ? L("In front first") : L("By name")) {
                Picker("", selection: $frontFirst) {
                    Text(L("By name")).tag(false)
                    Text(L("In front first")).tag(true)
                }
                .pickerStyle(.inline).labelsHidden()
            }
            IconSegmented(selection: $grid, off: ("list.bullet", L("List")), on: ("square.grid.2x2", L("Grid")))
        }
    }

    /// Rows in the list (or grid, three tiles a row), counting the "installing" line.
    private var fitHeight: CGFloat {
        if grid {
            let rows = min(max(1, (shown.count + 2) / 3), 4)
            return max(Self.listHeight(rows: 2), CGFloat(rows) * 44 + CGFloat(rows - 1) * 10 + 28)
        }
        return Self.listHeight(rows: shown.count + (installing == nil ? 0 : 1))
    }

    private var emptyMessage: String? {
        if !all.isEmpty { return shown.isEmpty ? L("No app matches.") : nil }
        if !instance.running { return L("Start the instance to install and open apps.") }
        if apps == nil { return nil }
        return L("No apps yet. Install an APK, or drop one here.")
    }

    @ViewBuilder private var listBody: some View {
        LazyVStack(spacing: 0) {
            if instance.running, apps == nil {
                ProgressView().controlSize(.small).padding(.top, 24)
            }
            if let installing {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(String(format: L("Installing %@…"), installing)).foregroundStyle(.secondary)
                    Spacer()
                }
                .padding(.horizontal, 16).frame(height: Self.rowHeight)
            }
            ForEach(shown) { app in
                AppListRow(app: app, live: instance.running,
                           open: { Task { await model.appAction(instance.name, "launch", app.package) } },
                           stop: { Task { await model.appAction(instance.name, "stop-app", app.package) } },
                           uninstall: { removing = app })
            }
        }
    }

    private var gridBody: some View {
        LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10), count: 3), spacing: 10) {
            ForEach(shown) { app in
                Button { if instance.running { Task { await model.appAction(instance.name, "launch", app.package) } } } label: {
                    HStack(spacing: 10) {
                        LetterAvatar(title: app.title, seed: app.package, size: 24)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(app.title).font(.system(size: Metrics.listTitle, weight: .semibold)).lineLimit(1)
                            Text(app.package).font(.system(size: Metrics.listDetail, design: .monospaced)).foregroundStyle(.secondary)
                                .lineLimit(1).truncationMode(.middle)
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(10)
                    .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Color.primary.opacity(0.07)))
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .opacity(instance.running ? 1 : 0.6)
                .help(instance.running ? L("Open") : app.package)
                .contextMenu { if instance.running { appMenu(app) } }
            }
        }
        .padding(14)
    }

    @ViewBuilder private func appMenu(_ app: InstalledApp) -> some View {
        Button(L("Open")) { Task { await model.appAction(instance.name, "launch", app.package) } }
        Button(L("Force stop")) { Task { await model.appAction(instance.name, "stop-app", app.package) } }
        Divider()
        Button(L("Uninstall…"), role: .destructive) { removing = app }
    }

    // MARK: Doing things

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

/// One app in the list. "Open" shows while the pointer is over the row, so a long list stays quiet.
private struct AppListRow: View {
    let app: InstalledApp
    let live: Bool
    let open: () -> Void
    let stop: () -> Void
    let uninstall: () -> Void
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 12) {
            LetterAvatar(title: app.title, seed: app.package, size: 24)
                .opacity(live ? 1 : 0.55)
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 8) {
                    Text(app.title).font(.system(size: Metrics.listTitle, weight: .semibold)).lineLimit(1)
                    if live && app.running { StatusPill(text: L("In front"), color: .green) }
                }
                if app.title != app.package {
                    Text(app.package).font(.system(size: Metrics.listDetail, design: .monospaced)).foregroundStyle(.secondary)
                        .lineLimit(1).truncationMode(.middle)
                }
            }
            Spacer(minLength: 12)
            if live {
                if hovering { Button(L("Open"), action: open).buttonStyle(.pill) }
                Menu {
                    Button(L("Open"), action: open)
                    Button(L("Force stop"), action: stop)
                    Divider()
                    Button(L("Uninstall…"), role: .destructive, action: uninstall)
                } label: {
                    Image(systemName: "ellipsis").frame(width: 22, height: 22).contentShape(Circle())
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .foregroundStyle(.secondary)
                .help(L("More"))
            }
        }
        .padding(.leading, 16).padding(.trailing, 8)
        .frame(height: AppsSection.rowHeight)
        .background(hovering ? Color.primary.opacity(0.06) : Color.clear)
        .overlay(alignment: .bottom) { Rectangle().fill(Color.primary.opacity(0.05)).frame(height: 1) }
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture(count: 2) { if live { open() } }
    }
}
