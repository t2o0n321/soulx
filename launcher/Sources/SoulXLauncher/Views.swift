import SwiftUI
import SoulXCore

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @State private var creating = false

    var body: some View {
        NavigationSplitView {
            List(selection: $model.selection) {
                Section(L("Instances")) {
                    ForEach(model.instances) { inst in
                        InstanceRow(instance: inst, busy: model.busy[inst.name])
                            .tag(Optional(inst.name))
                    }
                }
                Section(L("AI")) {
                    ForEach([mcpSelection], id: \.self) { item in
                        Label {
                            HStack {
                                Text(L("AI control"))
                                Spacer()
                                Text(model.mcp?.enabled == true ? L("On") : L("Off"))
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        } icon: {
                            Image(systemName: "sparkles")
                                .foregroundStyle(model.mcp?.enabled == true ? Color.accentColor : Color.secondary)
                        }
                        .tag(Optional(item))
                    }
                }
            }
            .safeAreaInset(edge: .bottom) {
                HStack {
                    if let host = model.host {
                        Label("\(host.cpus) \(L("cores")) · \(host.memoryMB / 1024) GB", systemImage: "desktopcomputer")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Menu {
                        Picker(L("Language"), selection: Binding(get: { model.language },
                                                                 set: { c in Task { await model.setLanguage(c) } })) {
                            ForEach(LanguageChoice.allCases) { Text($0.label).tag($0) }
                        }
                        .pickerStyle(.inline)
                    } label: { Image(systemName: "globe") }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .help(L("Language"))
                    .disabled(model.cli == nil)
                }
                .padding(.horizontal, 18).padding(.vertical, 12)
            }
            .navigationSplitViewColumnWidth(min: 200, ideal: 230)
            .toolbar {
                ToolbarItem {
                    Button { creating = true } label: { Label(L("New instance"), systemImage: "plus") }
                        .disabled(model.cli == nil)
                }
            }
        } detail: {
            detail
        }
        .sheet(isPresented: $creating) { CreateInstanceView(isPresented: $creating) }
        .alert(L("Error"), isPresented: Binding(get: { model.alert != nil }, set: { if !$0 { model.alert = nil } })) {
            Button(L("OK")) { model.alert = nil }
        } message: { Text(model.alert ?? "") }
        .task { await model.loadLanguage(); await model.loadMcp(); model.startPolling() }
        .id(model.language)                         // redraw every text in the new language
    }

    @ViewBuilder private var detail: some View {
        if model.cli != nil, model.selection == mcpSelection {
            McpView()
        } else if model.cli == nil {
            ContentUnavailableView(L("Error"), systemImage: "exclamationmark.triangle",
                description: Text(L("The soulx command was not found. Build it (scripts/build_macos.sh) or set SOULX_CLI.")))
        } else if let rt = model.runtime, !rt.ready {
            SetupView(runtime: rt)
        } else if let inst = model.instance(model.selection) {
            InstanceDetailView(instance: inst).id(inst.name)
        } else {
            ContentUnavailableView(model.instances.isEmpty ? L("No instances yet.") : L("Select an instance."),
                                   systemImage: "iphone")
        }
    }
}

struct InstanceRow: View {
    let instance: Instance
    let busy: String?

    var body: some View {
        Label {
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text(instance.name)
                    Text(busy.flatMap { $0.isEmpty ? nil : $0 } ?? (instance.running ? L("Running") : L("Stopped")))
                        .font(.caption).foregroundStyle(.secondary)
                }
                if busy != nil && busy != "" { Spacer(); ProgressView().controlSize(.small) }
            }
        } icon: {
            Image(systemName: "iphone.gen3")
                .foregroundStyle(instance.running ? Color.green : Color.secondary)
        }
    }
}

struct InstanceDetailView: View {
    @EnvironmentObject var model: AppModel
    let instance: Instance
    @State private var edited: InstanceSettings
    @State private var note: String?
    @State private var needsRestart = false
    @State private var confirmDelete = false
    @State private var volume: VolumeInfo?
    @State private var volumeLevel: Double = 0          // Android's steps (0...max), like the SoulX window's slider
    @State private var draggingVolume = false

    init(instance: Instance) {
        self.instance = instance
        _edited = State(initialValue: instance.settings)
    }

    private var busy: Bool { model.busy[instance.name] != nil }

    var body: some View {
        Form {
            if instance.running {
                Section(L("Status")) {
                    if let s = model.stats[instance.name] {
                        LabeledContent(L("CPU"), value: String(format: "%.0f%% (%@)", s.cpuPercent ?? 0, L("of one core")))
                        LabeledContent(L("Memory"), value: "\((s.memoryMB ?? 0)) MB")
                        LabeledContent(L("Uptime"), value: uptime(s.uptimeS ?? 0))
                    } else {
                        ProgressView().controlSize(.small)
                    }
                    volumeRow
                }
            }
            ScriptsSection(instance: instance.name)
            SettingsForm(settings: $edited, recommended: Profile.recommended(hostMemoryGB: model.hostMemoryGB),
                         minDataGB: instance.dataGB)
            Section {
                HStack {
                    if let note { Text(note).font(.callout).foregroundStyle(.secondary) }
                    Spacer()
                    Button(L("Revert")) { edited = instance.settings; note = nil }
                        .disabled(edited == instance.settings)
                    Button(L("Apply")) {
                        let wasRunning = instance.running
                        Task {
                            note = await model.apply(instance.name, from: instance.settings, to: edited)
                            needsRestart = wasRunning && note != nil
                        }
                    }
                    .keyboardShortcut(.defaultAction)
                    .disabled(edited == instance.settings || busy)
                    if instance.running && needsRestart {
                        Button(L("Restart")) { Task { await model.restart(instance.name); note = nil; needsRestart = false } }.disabled(busy)
                    }
                }
            }
        }
        .formStyle(.grouped)
        .navigationTitle(instance.name)
        .navigationSubtitle("\(instance.android ?? L("Android")) · \(instance.running ? L("Running") : L("Stopped"))")
        .toolbar { toolbarActions }
        .onChange(of: instance) { _, new in if edited == instance.settings { edited = new.settings } }
        .task(id: instance.running) { if instance.running { await followVolume() } }
        .confirmationDialog(L("Delete this instance and everything on it (apps and game data)?"),
                            isPresented: $confirmDelete) {
            Button(L("Delete"), role: .destructive) { Task { await model.delete(instance.name) } }
            Button(L("Cancel"), role: .cancel) {}
        }
    }

    /// Toolbar: a secondary action (Stop, or the ⋯ menu) and, apart from it, the main one (Open window, or Start).
    @ToolbarContentBuilder private var toolbarActions: some ToolbarContent {
        ToolbarItem(placement: .primaryAction) { secondaryAction }
        if #available(macOS 26.0, *) {
            ToolbarSpacer(.fixed, placement: .primaryAction)
        }
        ToolbarItem(placement: .primaryAction) { mainAction }
    }

    @ViewBuilder private var secondaryAction: some View {
        if let label = model.busy[instance.name], !label.isEmpty {
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text(label).foregroundStyle(.secondary)
            }
            .padding(.horizontal, 10)
        } else if instance.running {
            Button { Task { await model.stop(instance.name) } } label: {
                Label(L("Stop"), systemImage: "stop.fill")
            }
            .help(L("Stop"))
        } else {
            Menu {
                Button(L("Start in background")) { Task { await model.start(instance.name, headless: true) } }
                Divider()
                Button(L("Delete…"), role: .destructive) { confirmDelete = true }
            } label: {
                Label(L("More"), systemImage: "ellipsis")
            }
            .menuIndicator(.hidden)
            .help(L("More"))
        }
    }

    @ViewBuilder private var mainAction: some View {
        if let label = model.busy[instance.name], !label.isEmpty {
            EmptyView()
        } else if instance.running {
            Button { Task { await model.openWindow(instance.name) } } label: {
                Label(L("Open window"), systemImage: "macwindow").labelStyle(.titleAndIcon)
            }
            .prominentToolbarButton()
            .help(L("Open window"))
        } else {
            Button { Task { await model.start(instance.name) } } label: {
                Label(L("Start"), systemImage: "play.fill").labelStyle(.titleAndIcon)
            }
            .prominentToolbarButton()
            .help(L("Start"))
        }
    }

    @ViewBuilder private var volumeRow: some View {
        LabeledContent(L("Volume")) {
            if let v = volume, !v.audio {
                Text(L("No sound device (turn Sound on in Settings)")).foregroundStyle(.secondary)
            } else if let v = volume {
                HStack(spacing: 10) {
                    Image(systemName: v.muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                        .foregroundStyle(.secondary).frame(width: 22)
                    Slider(value: $volumeLevel, in: 0...Double(max(1, v.max))) { editing in
                        draggingVolume = editing
                        if !editing {
                            volumeLevel = volumeLevel.rounded()          // Android's steps
                            let pct = Int((volumeLevel * 100 / Double(max(1, v.max))).rounded())
                            Task { volume = await model.setVolume(instance.name, percent: pct) ?? volume }
                        }
                    }
                    .frame(width: 240)
                    Toggle(L("Muted"), isOn: Binding(get: { v.muted }, set: { m in
                        Task { volume = await model.setVolume(instance.name, mute: m) ?? volume }
                    }))
                    .toggleStyle(.checkbox)
                }
            } else {
                ProgressView().controlSize(.small)
            }
        }
    }

    // The volume can change elsewhere (the SoulX window, a game, Android itself), and right after a start Android may
    // still be booting: read it every 3 s while the page is shown, except while the user drags the slider.
    private func followVolume() async {
        while !Task.isCancelled {
            if !draggingVolume, let v = await model.volume(instance.name), !draggingVolume {
                volume = v
                volumeLevel = Double(v.level)
            }
            try? await Task.sleep(nanoseconds: 3_000_000_000)
        }
    }

    private func uptime(_ s: Int) -> String { String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) }
}

struct CreateInstanceView: View {
    @EnvironmentObject var model: AppModel
    @Binding var isPresented: Bool
    @State private var name = ""
    @State private var settings = InstanceSettings.defaults(profile: .balanced)
    @State private var working = false

    private var nameProblem: String? {
        if name.isEmpty { return nil }
        if !isValidInstanceName(name) { return L("Lowercase letters, digits, - and _ (up to 32).") }
        if model.instance(name) != nil { return L("An instance with this name already exists.") }
        return nil
    }

    var body: some View {
        VStack(spacing: 0) {
            Text(L("New instance")).font(.title3.bold())
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 20).padding(.top, 18)
            Form {
                Section {
                    TextField(L("Name"), text: $name)
                    if let p = nameProblem { Text(p).font(.caption).foregroundStyle(.red) }
                }
                SettingsForm(settings: $settings, recommended: Profile.recommended(hostMemoryGB: model.hostMemoryGB))
            }
            .formStyle(.grouped)
            HStack {
                Spacer()
                Button(L("Cancel")) { isPresented = false }.keyboardShortcut(.cancelAction)
                Button(L("Create")) {
                    working = true
                    Task {
                        if await model.create(name, settings) { isPresented = false }
                        working = false
                    }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(name.isEmpty || nameProblem != nil || working)
            }
            .padding()
        }
        .frame(width: 520, height: 560)
        .onAppear {
            settings = .defaults(profile: Profile.recommended(hostMemoryGB: model.hostMemoryGB))
            if model.instances.isEmpty { name = "default" }
        }
    }
}

struct SetupView: View {
    @EnvironmentObject var model: AppModel
    let runtime: RuntimeInfo
    @State private var accepted = false

    var body: some View {
        Form {
            Section(L("Android runtime")) {
                Text(L("SoulX needs its Android runtime (engine, adb and Android 17) before the first start."))
                LabeledContent(L("Download size"),
                               value: ByteCountFormatter.string(fromByteCount: runtime.missingBytes, countStyle: .file))
                ForEach(runtime.packages.filter { $0.kind != "image" || $0.key == runtime.defaultImage }, id: \.key) { p in
                    LabeledContent(p.description ?? p.key, value: p.installed ? "✓" : "—")
                }
                Toggle(L("I accept the Android SDK License Agreement"), isOn: $accepted)
                Link(L("Read the license"), destination: URL(string: "https://developer.android.com/studio/terms")!)
                HStack {
                    Spacer()
                    if model.settingUp { ProgressView().controlSize(.small); Text(L("Installing…")) }
                    Button(L("Install")) { model.installRuntime() }
                        .disabled(!accepted || model.settingUp)
                        .keyboardShortcut(.defaultAction)
                }
            }
            if !model.setupLog.isEmpty {
                Section {
                    ScrollView {
                        Text(model.setupLog.suffix(200).joined(separator: "\n"))
                            .font(.system(.caption, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                    }
                    .frame(minHeight: 160)
                }
            }
        }
        .formStyle(.grouped)
    }
}

extension View {
    /// The toolbar's main button: Liquid Glass's prominent style on macOS 26 (sized for the toolbar), else bordered.
    @ViewBuilder func prominentToolbarButton() -> some View {
        if #available(macOS 26.0, *) {
            self.buttonStyle(.glassProminent)
        } else {
            self.buttonStyle(.borderedProminent)
        }
    }
}
