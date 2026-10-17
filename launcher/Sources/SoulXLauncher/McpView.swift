import AppKit
import SwiftUI
import SoulXCore

/// Sidebar selection value for the AI control page (not a valid instance name, so it never collides).
let mcpSelection = "__ai_control__"

/// AI control (MCP): the on/off switch, which instances and actions an AI may use, how to connect a client,
/// and what it did recently. Everything goes through `soulx mcp config` / `soulx mcp log`.
struct McpView: View {
    @EnvironmentObject var model: AppModel
    @State private var log: [McpLogEntry] = []
    @State private var copied = false
    @State private var clients: [McpClientApp] = []
    @State private var note: String?
    @State private var busyClient: String?

    var body: some View {
        Form {
            if let c = model.mcp {
                Section {
                    Toggle(isOn: Binding(get: { c.enabled }, set: { on in Task { await model.setMcp([on ? "--enable" : "--disable"]) } })) {
                        Text(L("Let AI clients control SoulX (MCP)"))
                        Text(L("An AI app (an MCP client) can look at the screen and tap, swipe and type in the instances you allow. Turning this off stops it at once."))
                    }
                    Label(L("Local only: the AI app starts SoulX and talks to it directly. No network port is opened."),
                          systemImage: "lock.shield")
                        .font(.callout).foregroundStyle(.secondary)
                }
                Section {
                    let found = clients.filter(\.installed)
                    let temporary = c.command.contains("/AppTranslocation/") || c.command.hasPrefix("/Volumes/")
                    if temporary {
                        Label(L("Move SoulX to your Applications folder first, then open it from there. Running from Downloads or the disk image, macOS gives it a temporary location that AI apps could not find later."),
                              systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    }
                    if found.isEmpty {
                        Text(L("No supported AI app was found on this Mac. Install one (for example an AI desktop app or code editor with MCP support), or set it up by hand below."))
                            .foregroundStyle(.secondary)
                    }
                    ForEach(found) { app in clientRow(app) }.disabled(temporary)
                    if let note {
                        Label(note, systemImage: "arrow.clockwise.circle").foregroundStyle(.blue)
                    }
                    DisclosureGroup(L("Set up by hand (advanced)")) {
                        connectRow(L("Command and argument (stdio)"), "\"\(c.command)\" mcp")
                        connectRow(L("As JSON (most AI apps)"),
                                   "\"soulx\": { \"command\": \"\(c.command)\", \"args\": [\"mcp\"] }")
                    }
                } header: {
                    Text(L("Connect an AI app"))
                } footer: {
                    Text(L("SoulX adds itself to the app's own settings and keeps everything else in them (the old file is backed up first)."))
                }
                Section(L("Instances the AI may use")) {
                    Picker(L("Instances the AI may use"), selection: Binding(
                        get: { c.allInstances }, set: { all in Task { await model.setMcp(["--instances", all ? "*" : (model.instances.first?.name ?? "default")]) } })) {
                        Text(L("All instances")).tag(true)
                        Text(L("Only these")).tag(false)
                    }
                    .pickerStyle(.radioGroup)
                    .labelsHidden()
                    if !c.allInstances {
                        ForEach(model.instances) { inst in
                            Toggle(inst.name, isOn: Binding(get: { c.instanceList.contains(inst.name) }, set: { on in
                                var list = Set(c.instanceList)
                                if on { list.insert(inst.name) } else { list.remove(inst.name) }
                                let value = list.sorted().joined(separator: ",")   // empty: no instance at all
                                Task { await model.setMcp(["--instances", value]) }
                            }))
                        }
                    }
                }
                Section(L("What the AI may do")) {
                    LabeledContent(L("Look and touch")) { Text(L("Always (screenshots, text, taps, swipes, keys, apps)")).foregroundStyle(.secondary) }
                    permission(L("Start and stop instances"), nil, "start-stop", c.allowStartStop)
                    permission(L("Install apps"), L("Installs .apk files from your Mac. Leave off unless you need it."), "install", c.allowInstall)
                    permission(L("Run your scripts"), L("Only the scripts in each instance's own scripts folder."), "scripts", c.allowScripts)
                    permission(L("Run Lua code it writes"), L("Always sandboxed: it can operate the instance but cannot read or change files on your Mac."), "lua", c.allowLua)
                    LabeledContent(L("Never")) { Text(L("Create, delete or change instances; setup")).foregroundStyle(.secondary) }
                }
                Section(L("Recent activity")) {
                    if log.isEmpty {
                        Text(L("No AI activity yet.")).foregroundStyle(.secondary)
                    } else {
                        ForEach(log.reversed().prefix(30)) { e in
                            HStack(alignment: .firstTextBaseline) {
                                Image(systemName: e.ok ? "checkmark.circle" : "xmark.octagon")
                                    .foregroundStyle(e.ok ? Color.green : Color.orange)
                                Text(e.time).monospacedDigit().foregroundStyle(.secondary)
                                Text(e.tool).bold()
                                if e.instance != "-" { Text(e.instance).foregroundStyle(.secondary) }
                                Text(e.detail).lineLimit(1).truncationMode(.tail).foregroundStyle(.secondary)
                            }
                            .font(.caption)
                        }
                    }
                }
            } else {
                ProgressView()
            }
        }
        .formStyle(.grouped)
        .navigationTitle(L("AI control"))
        .task {
            await model.loadMcp()
            while !Task.isCancelled {
                log = await model.mcpLog()
                clients = await model.mcpClients()
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    private func permission(_ title: String, _ note: String?, _ key: String, _ on: Bool) -> some View {
        Toggle(isOn: Binding(get: { on }, set: { v in Task { await model.setMcp([v ? "--allow" : "--deny", key]) } })) {
            Text(title)
            if let note { Text(note) }
        }
    }

    private func clientRow(_ app: McpClientApp) -> some View {
        HStack {
            Image(systemName: "app.connected.to.app.below.fill").foregroundStyle(.secondary).frame(width: 20)
            VStack(alignment: .leading, spacing: 2) {
                Text(app.name)
                Text(stateText(app.state)).font(.caption)
                    .foregroundStyle(app.state == "connected" ? Color.green : app.state == "unreadable" ? Color.orange : Color.secondary)
            }
            Spacer()
            if busyClient == app.id {
                ProgressView().controlSize(.small)
            } else if app.state == "connected" {
                Button(L("Disconnect")) { run(app, connect: false) }
            } else if app.state == "other" {
                Button(L("Remove")) { run(app, connect: false) }
                Button(L("Repair")) { run(app, connect: true) }.buttonStyle(.borderedProminent)
            } else if app.state != "unreadable" {
                Button(L("Connect")) { run(app, connect: true) }.buttonStyle(.borderedProminent)
            }
        }
    }

    private func stateText(_ s: String) -> String {
        switch s {
        case "connected": return L("Connected")
        case "other": return L("Connected to another copy of SoulX")
        case "unreadable": return L("Its settings file is damaged; SoulX does not touch it")
        default: return L("Not connected")
        }
    }

    private func run(_ app: McpClientApp, connect: Bool) {
        busyClient = app.id
        Task {
            if let msg = await model.connectClient(app.id, connect: connect) {
                note = String(format: L(connect ? "%@ is connected. Restart it to start using SoulX." : "SoulX was removed from %@. Restart it to apply."), app.name)
                _ = msg
            }
            clients = await model.mcpClients()
            busyClient = nil
        }
    }

    private func connectRow(_ title: String, _ text: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
            HStack {
                Text(text)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .lineLimit(2)
                Spacer()
                Button(L("Copy")) {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(text, forType: .string)
                }
            }
        }
    }
}
