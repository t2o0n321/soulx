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
                Section(L("Connect an AI app")) {
                    connectRow(L("Command and argument (stdio)"), "\"\(c.command)\" mcp")
                    connectRow(L("As JSON (most AI apps)"),
                               "\"soulx\": { \"command\": \"\(c.command)\", \"args\": [\"mcp\"] }")
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
        .task {
            await model.loadMcp()
            while !Task.isCancelled {
                log = await model.mcpLog()
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
