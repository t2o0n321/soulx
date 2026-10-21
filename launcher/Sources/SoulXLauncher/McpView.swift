import AppKit
import SwiftUI
import SoulXCore

/// Sidebar selection value for the AI control page (not a valid instance name, so it never collides).
let mcpSelection = "__ai_control__"

/// AI control (MCP): the on/off switch, which instances and actions an AI may use, how to connect a client,
/// and what it did recently. Everything goes through `soulx mcp config` / `soulx mcp log`.
struct McpView: View {
    @EnvironmentObject var model: AppModel
    @State private var log = McpLogPage.empty
    @State private var page = 1                     // 1 = the newest; only that page follows new calls
    @State private var clients: [McpClientApp] = []
    @State private var note: String?
    @State private var busyClient: String?
    @State private var showManual = false
    @State private var failedOnly = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                if let c = model.mcp {
                    header(c)
                    masterCard(c)
                    // Everything AI control decides is out of reach while it is off: dimmed and not clickable
                    // (the log below stays readable).
                    SplitRow(ratio: 1, spacing: 12, minWidth: 820, matchHeights: true) {
                        connectCard(c)
                        permissionsCard(c)
                    }
                    .allowsHitTesting(c.enabled)
                    .opacity(c.enabled ? 1 : 0.4)
                    .animation(.easeInOut(duration: 0.15), value: c.enabled)
                    .accessibilityHidden(!c.enabled)
                    activityCard
                } else {
                    ProgressView().frame(maxWidth: .infinity, minHeight: 200)
                }
            }
            .padding(.horizontal, 20).padding(.top, 4).padding(.bottom, 20)
            .frame(maxWidth: 1240)
            .frame(maxWidth: .infinity)
        }
        .navigationTitle(L("AI control"))
        .hiddenToolbarTitle()
        .task {
            await model.loadMcp()
            while !Task.isCancelled {
                if page == 1, let p = await model.mcpLog(page: 1) { log = p }
                clients = await model.mcpClients()
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    private var enabled: Bool { model.mcp?.enabled == true }

    // MARK: Header and master switch

    private func header(_ c: McpConfig) -> some View {
        HStack(alignment: .center, spacing: 16) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 8) {
                    Text(L("AI control")).font(.system(size: 17, weight: .bold))
                    StatusPill(text: "MCP", color: .blue, systemImage: "sparkles")
                }
                Text(L("Let AI apps operate your instances, within what you allow."))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Spacer(minLength: 16)
            Button { showManual = true } label: {
                Label(L("Set up by hand (advanced)"), systemImage: "chevron.left.forwardslash.chevron.right")
            }
            .buttonStyle(PillButtonStyle())
            .popover(isPresented: $showManual, arrowEdge: .bottom) { manualSetup(c) }
        }
        .padding(.vertical, 6)
    }

    private func masterCard(_ c: McpConfig) -> some View {
        // laid out like a card's title line: the small icon tile, the card heading size, a caption under it
        HStack(spacing: 10) {
            HeaderIcon(systemName: "sparkles")
                .foregroundStyle(c.enabled ? Color.blue : Color.secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(L("Let AI clients control SoulX (MCP)")).font(.system(size: 13, weight: .semibold))
                Text(L("An AI app (an MCP client) can look at the screen and tap, swipe and type in the instances you allow. Turning this off stops it at once."))
                    .font(.system(size: Metrics.caption)).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 16)
            Text(c.enabled ? L("On") : L("Off"))
                .font(.system(size: Metrics.body, weight: .medium)).foregroundStyle(.secondary)
            Toggle("", isOn: Binding(get: { c.enabled }, set: { on in Task { await model.setMcp([on ? "--enable" : "--disable"]) } }))
                .labelsHidden().toggleStyle(.switch).controlSize(.small)
        }
        .padding(.leading, 16).padding(.trailing, 12).padding(.vertical, 10)
        .surface()
    }

    // MARK: Clients

    private func connectCard(_ c: McpConfig) -> some View {
        let found = clients.filter(\.installed)
        let temporary = c.command.contains("/AppTranslocation/") || c.command.hasPrefix("/Volumes/")
        return SectionCard(L("Connect an AI app"), icon: "app.connected.to.app.below.fill",
                           count: found.isEmpty ? nil : found.count) {
            if temporary {
                Label(L("Move SoulX to your Applications folder first, then open it from there. Running from Downloads or the disk image, macOS gives it a temporary location that AI apps could not find later."),
                      systemImage: "exclamationmark.triangle.fill")
                    .font(.system(size: 11)).foregroundStyle(.orange)
                    .cardLine()
            }
            if found.isEmpty {
                Text(L("No supported AI app was found on this Mac. Install one (for example an AI desktop app or code editor with MCP support), or set it up by hand below."))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
                    .cardLine()
            }
            ForEach(found) { app in clientRow(app).disabled(temporary) }
            if let note {
                Label(note, systemImage: "arrow.clockwise.circle")
                    .font(.system(size: 11)).foregroundStyle(.blue)
                    .cardLine()
            }
        }
    }

    private func clientRow(_ app: McpClientApp) -> some View {
        // the same row as an app in the instance's apps list
        HStack(spacing: 12) {
            LetterAvatar(title: app.name, seed: app.id, size: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(app.name).font(.system(size: Metrics.listTitle, weight: .semibold)).lineLimit(1)
                Text(stateText(app.state)).font(.system(size: Metrics.listDetail)).foregroundStyle(stateColor(app.state))
                    .lineLimit(1)
            }
            Spacer(minLength: 12)
            if busyClient == app.id {
                ProgressView().controlSize(.small)
            } else if app.state == "connected" {
                if !enabled {
                    Button(L("Turn on")) { Task { await model.setMcp(["--enable"]) } }.buttonStyle(.pillTinted)
                }
                Button(L("Disconnect")) { run(app, connect: false) }.buttonStyle(.pill)
            } else if app.state == "other" {
                Button(L("Remove")) { run(app, connect: false) }.buttonStyle(.pill)
                Button(L("Repair")) { run(app, connect: true) }.buttonStyle(.pillTinted)
            } else if app.state != "unreadable" {
                Button(L("Connect")) { run(app, connect: true) }.buttonStyle(.pill)
            }
        }
        .frame(height: AppsSection.rowHeight)
        .cardLine(vertical: 0)
    }

    private func stateText(_ s: String) -> String {
        switch s {
        case "connected": return enabled ? L("Connected") : L("Added, but AI control is off: the app cannot use SoulX")
        case "other": return L("Connected to another copy of SoulX")
        case "unreadable": return L("Its settings file is damaged; SoulX does not touch it")
        default: return L("Not connected")
        }
    }

    private func stateColor(_ s: String) -> Color {
        switch s {
        case "connected": return enabled ? .green : .orange
        case "unreadable", "other": return .orange
        default: return .secondary
        }
    }

    private func run(_ app: McpClientApp, connect: Bool) {
        busyClient = app.id
        Task {
            if await model.connectClient(app.id, connect: connect) != nil {
                note = String(format: L(connect ? "%@ is connected. Restart it to start using SoulX." : "SoulX was removed from %@. Restart it to apply."), app.name)
            }
            clients = await model.mcpClients()
            busyClient = nil
        }
    }

    private func manualSetup(_ c: McpConfig) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(L("Set up by hand (advanced)")).font(.system(size: 13, weight: .semibold))
            connectRow(L("Command and argument (stdio)"), "\"\(c.command)\" mcp")
            connectRow(L("As JSON (most AI apps)"), "\"soulx\": { \"command\": \"\(c.command)\", \"args\": [\"mcp\"] }")
        }
        .padding(18)
        .frame(width: 460)
    }

    private func connectRow(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: Metrics.body, weight: .medium))
            HStack(alignment: .top) {
                Text(text)
                    .font(.system(size: Metrics.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(10)
                    .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 9, style: .continuous))
                Button(L("Copy")) {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(text, forType: .string)
                }
                .buttonStyle(.pill)
            }
        }
    }

    // MARK: Which instances, which actions

    private func permissionsCard(_ c: McpConfig) -> some View {
        SectionCard(L("AI permissions"), icon: "hand.tap") {
            // which instances first, then what it may do in them
            SettingRow(L("Instances the AI may use")) {
                SegmentedPill(selection: Binding(get: { c.allInstances }, set: { all in
                    Task { await model.setMcp(["--instances", all ? "*" : (model.instances.first?.name ?? "default")]) }
                }), options: [(true, L("All instances")), (false, L("Only these"))])
            }
            if !c.allInstances {
                ForEach(model.instances) { inst in
                    SettingRow(inst.name) {
                        Toggle("", isOn: Binding(get: { c.instanceList.contains(inst.name) }, set: { on in
                            var list = Set(c.instanceList)
                            if on { list.insert(inst.name) } else { list.remove(inst.name) }
                            let value = list.sorted().joined(separator: ",")   // empty: no instance at all
                            Task { await model.setMcp(["--instances", value]) }
                        }))
                        .labelsHidden().toggleStyle(.switch).controlSize(.small)
                    }
                    .padding(.leading, 14)
                }
            }
            SettingRow(L("Look and touch"), caption: L("Always (screenshots, text, taps, swipes, keys, apps)")) {
                StatusPill(text: L("Always allowed"), color: .green)
            }
            permission(L("Start and stop instances"), nil, "start-stop", c.allowStartStop)
            permission(L("Install apps"), L("Installs .apk files from your Mac. Leave off unless you need it."), "install", c.allowInstall)
            permission(L("Run your scripts"), L("Only the scripts in each instance's own scripts folder."), "scripts", c.allowScripts)
            permission(L("Run Lua code it writes"), L("Always sandboxed: it can operate the instance but cannot read or change files on your Mac."), "lua", c.allowLua)
        }
    }

    private func permission(_ title: String, _ caption: String?, _ key: String, _ on: Bool) -> some View {
        SettingRow(title, caption: caption) {
            Toggle("", isOn: Binding(get: { on }, set: { v in Task { await model.setMcp([v ? "--allow" : "--deny", key]) } }))
                .labelsHidden().toggleStyle(.switch).controlSize(.small)
        }
    }

    // MARK: Recent activity

    private var activityCard: some View {
        let failed = log.entries.filter { !$0.ok }
        let rows = failedOnly ? failed : log.entries
        return SectionCard(L("Recent activity"), icon: "clock.arrow.circlepath") {
            if let day = log.entries.first.map({ String($0.time.prefix(10)) }) {
                Text(day).font(.system(size: Metrics.caption)).foregroundStyle(.secondary).monospacedDigit()
                Spacer().frame(width: 8)
            }
            if !log.entries.isEmpty {
                SegmentedPill(selection: $failedOnly,
                              options: [(false, "\(L("All")) \(log.entries.count)"), (true, "\(L("Failed")) \(failed.count)")])
            }
        } content: {
            if log.entries.isEmpty {
                Text(L("No AI activity yet.")).font(.system(size: Metrics.caption)).foregroundStyle(.secondary).cardLine()
            } else {
                HStack(spacing: 14) {
                    Color.clear.frame(width: 16)
                    Text(L("Time")).frame(width: 64, alignment: .leading)
                    Text(L("Action")).frame(width: 128, alignment: .leading)
                    Text(L("Instance")).frame(width: 80, alignment: .leading)
                    Text(L("Details"))
                }
                .font(.system(size: Metrics.caption, weight: .semibold)).foregroundStyle(.secondary)
                .cardLine(vertical: 8)
                ForEach(rows) { e in
                    HStack(spacing: 14) {
                        Image(systemName: e.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                            .foregroundStyle(e.ok ? Color.green : Color.orange).frame(width: 16)
                        Text(String(e.time.suffix(8))).monospacedDigit().foregroundStyle(.secondary)
                            .frame(width: 64, alignment: .leading)
                            .help(e.time)
                        Text(e.tool).font(.system(size: Metrics.listDetail, weight: .medium, design: .monospaced))
                            .frame(width: 128, alignment: .leading)
                        Text(e.instance == "-" ? "—" : e.instance)
                            .frame(width: 80, alignment: .leading)
                        Text(e.detail).foregroundStyle(.secondary).lineLimit(1).truncationMode(.tail)
                    }
                    .font(.system(size: Metrics.listTitle))
                    .frame(height: 32)
                    .cardLine(vertical: 0)
                }
                if log.pages > 1 { pager.cardLine(vertical: 8) }
            }
        }
    }

    private var pager: some View {
        HStack {
            Text(String(format: L("Page %d of %d · %d calls"), log.page, log.pages, log.total))
                .font(.system(size: 11)).foregroundStyle(.secondary).monospacedDigit()
            Spacer()
            RoundIconButton(systemName: "chevron.left.2", help: L("Newest")) { go(1) }.disabled(log.page <= 1)
            RoundIconButton(systemName: "chevron.left", help: L("Newer")) { go(log.page - 1) }.disabled(log.page <= 1)
            RoundIconButton(systemName: "chevron.right", help: L("Older")) { go(log.page + 1) }.disabled(log.page >= log.pages)
        }
    }

    private func go(_ p: Int) {
        page = max(1, min(p, log.pages))
        Task { if let l = await model.mcpLog(page: page) { log = l } }
    }
}
