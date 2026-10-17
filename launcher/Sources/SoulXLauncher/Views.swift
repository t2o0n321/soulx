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
                if let host = model.host {
                    Section(L("Mac")) {
                        Text("\(host.cpus) \(L("cores")) · \(host.memoryMB / 1024) GB").foregroundStyle(.secondary)
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 200, ideal: 230)
            .toolbar {
                ToolbarItem {
                    Button { creating = true } label: { Label(L("New instance"), systemImage: "plus") }
                        .disabled(model.cli == nil)
                }
                ToolbarItem {
                    Button { Task { await model.refresh() } } label: {
                        Label(L("Refresh"), systemImage: "arrow.clockwise")
                    }
                }
            }
        } detail: {
            if model.cli == nil {
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
        .sheet(isPresented: $creating) { CreateInstanceView(isPresented: $creating) }
        .alert(L("Error"), isPresented: Binding(get: { model.alert != nil }, set: { if !$0 { model.alert = nil } })) {
            Button(L("OK")) { model.alert = nil }
        } message: { Text(model.alert ?? "") }
        .task { model.startPolling() }
    }
}

struct InstanceRow: View {
    let instance: Instance
    let busy: String?

    var body: some View {
        HStack {
            Circle().fill(instance.running ? Color.green : Color.secondary.opacity(0.4)).frame(width: 8, height: 8)
            VStack(alignment: .leading) {
                Text(instance.name)
                Text(busy ?? (instance.running ? L("Running") : L("Stopped")))
                    .font(.caption).foregroundStyle(.secondary)
            }
            if busy != nil && busy != "" { Spacer(); ProgressView().controlSize(.small) }
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
    @State private var volumeValue: Double = 50

    init(instance: Instance) {
        self.instance = instance
        _edited = State(initialValue: instance.settings)
    }

    private var busy: Bool { model.busy[instance.name] != nil }

    var body: some View {
        Form {
            Section {
                HStack(alignment: .firstTextBaseline) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(instance.name).font(.title2.bold())
                        Text("\(instance.android ?? L("Android")) · \(instance.running ? L("Running") : L("Stopped"))")
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    actions
                }
                if let s = model.stats[instance.name], instance.running {
                    LabeledContent(L("CPU"), value: String(format: "%.0f%% (%@)", s.cpuPercent ?? 0, L("of one core")))
                    LabeledContent(L("Memory"), value: "\((s.memoryMB ?? 0)) MB")
                    LabeledContent(L("Uptime"), value: uptime(s.uptimeS ?? 0))
                }
            }
            if instance.running { volumeSection }
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
        .onChange(of: instance) { _, new in if edited == instance.settings { edited = new.settings } }
        .task(id: instance.running) { if instance.running { await loadVolume() } }
        .confirmationDialog(L("Delete this instance and everything on it (apps and game data)?"),
                            isPresented: $confirmDelete) {
            Button(L("Delete"), role: .destructive) { Task { await model.delete(instance.name) } }
            Button(L("Cancel"), role: .cancel) {}
        }
    }

    @ViewBuilder private var actions: some View {
        if let label = model.busy[instance.name], !label.isEmpty {
            ProgressView().controlSize(.small)
            Text(label).foregroundStyle(.secondary)
        } else if instance.running {
            Button(L("Open window")) { Task { await model.openWindow(instance.name) } }
            Button(L("Stop")) { Task { await model.stop(instance.name) } }
        } else {
            Menu(L("Start")) {
                Button(L("Start")) { Task { await model.start(instance.name) } }
                Button(L("Start in background")) { Task { await model.start(instance.name, headless: true) } }
            } primaryAction: { Task { await model.start(instance.name) } }
            .fixedSize()
            Button(L("Delete…"), role: .destructive) { confirmDelete = true }
        }
    }

    @ViewBuilder private var volumeSection: some View {
        Section(L("Volume")) {
            if let v = volume, !v.audio {
                Text(L("No sound device (turn Sound on in Settings)")).foregroundStyle(.secondary)
            } else if let v = volume {
                HStack {
                    Image(systemName: v.muted ? "speaker.slash" : "speaker.wave.2")
                    Slider(value: $volumeValue, in: 0...100, step: 5) { editing in
                        if !editing { Task { volume = await model.setVolume(instance.name, percent: Int(volumeValue)) ?? volume } }
                    }
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

    // right after a start Android may still be booting and the call fails: keep trying while the page is shown
    private func loadVolume() async {
        for _ in 0..<30 {
            if let v = await model.volume(instance.name) {
                volume = v
                volumeValue = Double(v.volume)
                return
            }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            if Task.isCancelled { return }
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
