import SwiftUI
import SoulXCore

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var updates: UpdateModel
    @State private var creating = false
    @State private var importing: ImportRequest?

    /// Until the Android runtime is installed, setup is the only page: nothing else can be picked.
    private var setupNeeded: Bool { model.repairing || (model.runtime.map { !$0.ready } ?? false) }
    @State private var confirmRepair = false

    var body: some View {
        NavigationSplitView {
            List(selection: Binding(get: { setupNeeded ? nil : model.selection },
                                    set: { v in
                                        if !setupNeeded { model.selection = v; return }
                                        // the list highlighted the row itself: push "nothing selected" back to it
                                        DispatchQueue.main.async { model.selection = nil }
                                    })) {
                Section {
                    ForEach(model.instances) { inst in
                        InstanceRow(instance: inst, busy: model.busy[inst.name])
                            .tag(Optional(inst.name))
                            .selectionDisabled(setupNeeded)
                            .disabled(setupNeeded)
                            .opacity(setupNeeded ? 0.4 : 1)
                    }
                } header: {
                    HStack(alignment: .center) {
                        Text(L("Instances"))
                        Spacer()
                        // header-sized, centred on the heading and under the sidebar button at the top
                        Menu {
                            Button(L("New instance…")) { creating = true }
                            Button(L("Import existing instance…")) { importInstance() }
                        } label: {
                            Image(systemName: "plus")
                                .font(.system(size: 11, weight: .semibold))
                                .frame(width: 20, height: 20)
                                .contentShape(Rectangle())
                        }
                        .menuStyle(.button)
                        .buttonStyle(.plain)
                        .menuIndicator(.hidden)
                        .fixedSize()
                        .foregroundStyle(.secondary)
                        .padding(.trailing, 11)
                            .help(L("New instance"))
                            .accessibilityLabel(L("New instance"))
                            .disabled(model.cli == nil || setupNeeded)
                            .opacity(setupNeeded ? 0.35 : 1)
                    }
                }
                Section(L("AI")) {
                    ForEach([mcpSelection], id: \.self) { item in
                        let on = model.mcp?.enabled == true
                        SidebarRow(systemName: "sparkles", tint: .blue, active: on,
                                   title: L("AI control"), subtitle: "MCP · " + (on ? L("On") : L("Off")))
                            .tag(Optional(item))
                            .selectionDisabled(setupNeeded)
                            .disabled(setupNeeded)
                            .opacity(setupNeeded ? 0.4 : 1)
                            .help(setupNeeded ? L("Install the Android runtime first.") : "")
                    }
                }
            }
            .safeAreaInset(edge: .top) { brand }
            .safeAreaInset(edge: .bottom) {
                VStack(spacing: 10) {
                    UpdateBanner()
                    macCard
                }
                .padding(.horizontal, 12).padding(.bottom, 12)
            }
            .navigationSplitViewColumnWidth(min: 220, ideal: 250)
        } detail: {
            detail
        }
        .sheet(isPresented: $creating) { CreateInstanceView(isPresented: $creating) }
        .sheet(item: $importing) { req in
            ImportSheet(url: req.url, manifest: req.manifest,
                        isPresented: Binding(get: { importing != nil }, set: { if !$0 { importing = nil } }))
        }
        .sheet(isPresented: Binding(get: { model.transfer != nil }, set: { if !$0 { model.transfer = nil } })) {
            if let t = model.transfer { TransferSheet(transfer: t) }
        }
        .sheet(isPresented: $updates.showSheet) { UpdateSheet().interactiveDismissDisabled(updates.busy) }
        .alert(L("Error"), isPresented: Binding(get: { model.alert != nil }, set: { if !$0 { model.alert = nil } })) {
            Button(L("OK")) { model.alert = nil }
        } message: { Text(model.alert ?? "") }
        .task { await model.loadLanguage(); await model.loadMcp(); model.startPolling(); updates.start() }
        .id(model.language)                         // redraw every text in the new language
    }

    private var brand: some View {
        HStack(spacing: 10) {
            AppMark(size: 30)
            VStack(alignment: .leading, spacing: 1) {
                Text("SoulX").font(.system(size: 13, weight: .bold))
                Text(L("Android emulator")).font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Spacer()
        }
        .padding(.horizontal, 16).padding(.top, 4).padding(.bottom, 6)
    }

    /// This Mac's cores and memory, with the update and language menus.
    private var macCard: some View {
        HStack(spacing: 10) {
            Image(systemName: "desktopcomputer").foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text(L("This Mac")).font(.system(size: 12, weight: .semibold)).lineLimit(1).fixedSize()
                if let host = model.host {
                    Text("\(host.cpus) \(L("cores")) · \(host.memoryMB / 1024) GB")
                        .font(.system(size: 11)).foregroundStyle(.secondary).monospacedDigit()
                        .lineLimit(1).fixedSize()
                }
            }
            Spacer(minLength: 4)
            Button { confirmRepair = true } label: { Image(systemName: "wrench.and.screwdriver") }
                .buttonStyle(.borderless)
                .help(model.instances.contains(where: \.running) ? L("Stop every instance to repair the Android runtime.")
                                                                   : L("Repair the Android runtime…"))
                .disabled(model.cli == nil || setupNeeded || model.instances.contains(where: \.running))
                .confirmationDialog(L("Repair the Android runtime?"), isPresented: $confirmRepair) {
                    Button(L("Repair")) { model.installRuntime(force: true) }
                    Button(L("Cancel"), role: .cancel) {}
                } message: {
                    Text(String(format: L("SoulX downloads the engine, adb and Android 17 again (about %@) and reinstalls them. Your instances, apps and game data are kept."),
                                ByteCountFormatter.string(fromByteCount: model.runtime?.packages
                                    .filter { $0.kind != "image" || $0.key == model.runtime?.defaultImage }
                                    .reduce(0) { $0 + $1.downloadBytes } ?? 0, countStyle: .file)))
                }
            UpdateMenu()
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
        .padding(.leading, 12).padding(.trailing, 8).padding(.vertical, 10)
        .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(Color.primary.opacity(0.07)))
    }

    /// "Import existing instance…": pick a .soulxvm, read what it holds, then confirm name and import.
    private func importInstance() {
        guard let url = chooseImportFile() else { return }
        Task {
            if let m = await model.inspectArchive(url) { importing = ImportRequest(url: url, manifest: m) }
        }
    }

    @ViewBuilder private var detail: some View {
        if model.cli == nil {
            ContentUnavailableView(L("Error"), systemImage: "exclamationmark.triangle",
                description: Text(L("The soulx command was not found. Build it (scripts/build_macos.sh) or set SOULX_CLI.")))
        } else if let rt = model.runtime, !rt.ready || model.repairing {
            SetupView(runtime: rt)                       // first, whatever was selected before
        } else if model.selection == mcpSelection {
            McpView()
        } else if model.cli == nil {
            ContentUnavailableView(L("Error"), systemImage: "exclamationmark.triangle",
                description: Text(L("The soulx command was not found. Build it (scripts/build_macos.sh) or set SOULX_CLI.")))
        } else if let rt = model.runtime, !rt.ready {
            SetupView(runtime: rt)
        } else if let inst = model.instance(model.selection) {
            InstanceDetailView(instance: inst).id(inst.name)
        } else if model.instances.isEmpty {
            WelcomeView(create: { creating = true }, importInstance: importInstance)
        } else {
            ContentUnavailableView(L("Select an instance."), systemImage: "iphone")
        }
    }
}

/// A sidebar entry: a tinted icon tile, a title and a status line.
struct SidebarRow: View {
    let systemName: String
    let tint: Color
    let active: Bool
    let title: String
    let subtitle: String
    var busy = false

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: systemName)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(active ? tint : Color.secondary)
                .frame(width: 22, height: 22)
                .background((active ? tint : Color.primary).opacity(active ? 0.16 : 0.07),
                            in: RoundedRectangle(cornerRadius: 9, style: .continuous))
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.system(size: 12, weight: .semibold)).lineLimit(1)
                Text(subtitle).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 4)
            if busy {
                ProgressView().controlSize(.small)
            } else if active && tint == .green {
                Circle().fill(Color.green).frame(width: 7, height: 7)
            }
        }
        .padding(.vertical, 2)
    }
}

struct InstanceRow: View {
    let instance: Instance
    let busy: String?

    var body: some View {
        let label = busy.flatMap { $0.isEmpty ? nil : $0 }
        SidebarRow(systemName: "iphone.gen3", tint: .green, active: instance.running,
                   title: instance.name,
                   subtitle: label ?? "\(instance.android ?? L("Android")) · \(instance.running ? L("Running") : L("Stopped"))",
                   busy: label != nil)
    }
}

struct InstanceDetailView: View {
    @EnvironmentObject var model: AppModel
    let instance: Instance
    @State private var edited: InstanceSettings
    @State private var note: String?
    @State private var confirmDelete = false
    @State private var volume: VolumeInfo?
    @State private var volumeLevel: Double = 0          // Android's steps (0...max), like the SoulX window's slider
    @State private var draggingVolume = false

    init(instance: Instance) {
        self.instance = instance
        _edited = State(initialValue: instance.settings)
    }

    private var busy: Bool { model.busy[instance.name] != nil }
    private var dirty: Bool { edited != instance.settings }

    /// One gap between all cards, so the columns below line up with the figures above.
    private static let gap: CGFloat = 12

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: Self.gap) {
                header
                if instance.running { stats }
                // Two equal columns whose edge lines up with the gap between the 2nd and 3rd figure: what you use
                // on the left, the settings on the right. Each block sizes itself from the width it is offered
                // (like CSS grid / flex-wrap), so a narrow window gets one column, never a clipped page.
                SplitRow(ratio: 1, spacing: Self.gap, minWidth: 820) {
                    VStack(spacing: Self.gap) {
                        AppsSection(instance: instance)
                        ScriptsSection(instance: instance)
                        SystemCard(settings: $edited, locked: instance.running)
                    }
                    VStack(spacing: Self.gap) {
                        BasicSettingsCard(settings: $edited,
                                          recommended: Profile.recommended(hostMemoryGB: model.hostMemoryGB),
                                          locked: instance.running)
                        HardwareCard(settings: $edited, minDataGB: instance.dataGB, locked: instance.running)
                        DisplayCard(settings: $edited, locked: instance.running)
                    }
                }
            }
            .padding(.horizontal, 20).padding(.top, 4).padding(.bottom, 20)
            .frame(maxWidth: 1600)
            .frame(maxWidth: .infinity)
        }
        .safeAreaInset(edge: .bottom) {
            if !instance.running { applyBar.padding(.bottom, 16) }
        }
        .navigationTitle(instance.name)
        .hiddenToolbarTitle()
        .onChange(of: instance) { _, new in
            // follow changes made elsewhere; once it runs, unapplied edits are dropped (they could not apply now)
            if edited == instance.settings || new.running { edited = new.settings }
            if new.running { note = nil }
        }
        .task(id: instance.running) { if instance.running { await followVolume() } }
        .task(id: instance.running) { if instance.running { await followUsage() } }
        .confirmationDialog(L("Delete this instance and everything on it (apps and game data)?"),
                            isPresented: $confirmDelete) {
            Button(L("Delete"), role: .destructive) { Task { await model.delete(instance.name) } }
            Button(L("Cancel"), role: .cancel) {}
        } message: {
            Text(L("Its scripts and network captures are moved to the Trash."))
        }
    }

    // MARK: Header — name, state and the instance's actions

    /// Title and actions on one line; when that does not fit, the recording button drops its words; when that
    /// still does not fit, the actions wrap under the title (flex-wrap).
    private var header: some View {
        ViewThatFits(in: .horizontal) {
            HStack(alignment: .center, spacing: 16) { headerTitle; Spacer(minLength: 16); actions(compact: false) }
            HStack(alignment: .center, spacing: 16) { headerTitle; Spacer(minLength: 16); actions(compact: true) }
            VStack(alignment: .leading, spacing: 12) {
                headerTitle
                ViewThatFits(in: .horizontal) { actions(compact: false); actions(compact: true) }
            }
        }
        .padding(.vertical, 6)
    }

    private var headerTitle: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
                Text(instance.name).font(.system(size: 17, weight: .bold)).lineLimit(1)
                statusPill
            }
            Text(specLine).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(1)
        }
        .fixedSize()
    }

    @ViewBuilder private var statusPill: some View {
        if let label = model.busy[instance.name], !label.isEmpty {
            StatusPill(text: label, color: .orange)
        } else if instance.running {
            StatusPill(text: L("Running"), color: .green)
        } else {
            StatusPill(text: L("Stopped"), color: .secondary)
        }
    }

    private var specLine: String {
        let s = instance.settings
        let res = s.resolution == .custom ? "\(s.width)×\(s.height)" : s.resolution.rawValue
        return "\(instance.android ?? L("Android")) · \(res) \(s.landscape ? L("Landscape") : L("Portrait"))"
            + " · \(s.cores) \(L("cores")) · \(s.ramMB / 1024) GB"
    }

    @ViewBuilder private func actions(compact: Bool) -> some View {
        if let label = model.busy[instance.name], !label.isEmpty {
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text(label).foregroundStyle(.secondary)
            }
        } else if instance.running {
            HStack(spacing: 10) {
                if let v = volume, v.audio { volumeCapsule(v) }
                ControlCapsule {
                    captureButton(compact: compact)
                    Rectangle().fill(Color.primary.opacity(0.14)).frame(width: 1, height: 18)
                    Button { Task { await model.stop(instance.name) } } label: {
                        Label(L("Stop"), systemImage: "stop.fill")
                    }
                    .buttonStyle(CapsuleItemStyle())
                    .help(L("Stop"))
                }
                Button { Task { await model.openWindow(instance.name) } } label: {
                    Label(L("Open window"), systemImage: "macwindow")
                }
                .buttonStyle(.pillPrimary)
            }
        } else {
            HStack(spacing: 10) {
                Menu {
                    Button(L("Start in background")) { Task { await model.start(instance.name, headless: true) } }
                    Button(L("Export…")) { chooseExportFile(for: instance.name, model: model) }
                    if let dir = instance.filesDir {
                        Button(String(format: L("Show “%@” Files in Finder"), instance.name)) {
                            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: dir)])
                        }
                    }
                    Divider()
                    Button(L("Delete…"), role: .destructive) { confirmDelete = true }
                } label: {
                    Image(systemName: "ellipsis")
                }
                .menuStyle(.button)
                .buttonStyle(PillButtonStyle())
                .menuIndicator(.hidden)
                .fixedSize()
                .help(L("More"))
                Button { Task { await model.start(instance.name) } } label: {
                    Label(L("Start"), systemImage: "play.fill")
                }
                .buttonStyle(.pillPrimary)
            }
        }
    }

    @ViewBuilder private func captureButton(compact: Bool) -> some View {
        if let path = instance.networkCapturePath {
            Button { Task { await model.stopNetworkCapture(instance.name) } } label: {
                Label(L("Stop Recording"), systemImage: "stop.circle.fill").foregroundStyle(.red)
            }
            .buttonStyle(CapsuleItemStyle())
            .help(String(format: L("Recording network traffic to %@. Click to stop and reveal it in Finder."),
                         (path as NSString).lastPathComponent))
        } else {
            Button { Task { await model.startNetworkCapture(instance.name) } } label: {
                HStack(spacing: 6) {
                    Image(systemName: "record.circle").foregroundStyle(.red)
                    if !compact { Text(L("Record Network Traffic")) }     // icon only when room is short
                }
            }
            .buttonStyle(CapsuleItemStyle())
            .help(L("Record network traffic to a .pcap file (open it in Wireshark)"))
        }
    }

    private func volumeCapsule(_ v: VolumeInfo) -> some View {
        let pct = Int((volumeLevel * 100 / Double(max(1, v.max))).rounded())
        return ControlCapsule {
            Button {
                Task { volume = await model.setVolume(instance.name, mute: !v.muted) ?? volume }
            } label: {
                Image(systemName: v.muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
                    .font(.system(size: Metrics.body)).frame(width: 22, height: 22).contentShape(Circle())
            }
            .buttonStyle(.plain)
            .help(L("Muted"))
            Slider(value: $volumeLevel, in: 0...Double(max(1, v.max))) { editing in
                draggingVolume = editing
                if !editing {
                    volumeLevel = volumeLevel.rounded()          // Android's steps
                    let p = Int((volumeLevel * 100 / Double(max(1, v.max))).rounded())
                    Task { volume = await model.setVolume(instance.name, percent: p) ?? volume }
                }
            }
            .controlSize(.small)
            .frame(width: 112)
            .accessibilityLabel(L("Volume"))
            Text(v.muted ? "—" : "\(pct)%")
                .font(.system(size: Metrics.body, weight: .medium)).foregroundStyle(.secondary).monospacedDigit()
                .frame(width: 36, alignment: .trailing).padding(.trailing, 8)
        }
    }

    // MARK: Live figures

    private var stats: some View {
        let s = model.stats[instance.name]
        let u = model.usage[instance.name]
        let cores = max(1, instance.cores)
        let cpu = s?.cpuPercent ?? 0                     // the engine counts one core as 100%
        let share = cpu / Double(cores)                  // shown against all of the instance's cores
        return AutoGrid(minItem: 170, spacing: Self.gap) {
            StatCard(icon: "cpu", label: L("CPU"), note: "\(cores) \(L("cores"))",
                     value: s == nil ? "—" : "\(Int(share.rounded()))", unit: s == nil ? nil : "%",
                     fraction: s == nil ? nil : share / 100,
                     help: s == nil ? nil : String(format: "%.0f%% (%@)", cpu, L("of one core")))
            usageCard(icon: "memorychip", label: L("Memory"), used: u?.memUsedMB, total: u?.memTotalMB,
                      host: s?.memoryMB)
            usageCard(icon: "internaldrive", label: L("Storage"), used: u?.storageUsedMB, total: u?.storageTotalMB,
                      host: u?.hostStorageMB)
            StatCard(icon: "clock", label: L("Uptime"), note: L("This session"),
                     value: s?.uptimeS.map(uptime) ?? "—")
        }
    }

    /// Memory or storage: the phone's own use against its total; the Mac-side footprint as the tooltip.
    private func usageCard(icon: String, label: String, used: Int?, total: Int?, host: Int?) -> some View {
        let known = used != nil && (total ?? 0) > 0
        let fraction = known ? Double(used!) / Double(total!) : nil
        return StatCard(icon: icon, label: label,
                        note: fraction.map { "\(Int(($0 * 100).rounded()))%" },
                        value: known ? String(format: "%.1f", Double(used!) / 1024) : "—",
                        unit: known ? String(format: "/ %.1f GB", Double(total!) / 1024) : nil,
                        fraction: fraction,
                        help: host.map { String(format: L("Uses %@ on your Mac"), Self.sizeText($0)) })
    }

    private static func sizeText(_ mb: Int) -> String {
        mb >= 1024 ? String(format: "%.1f GB", Double(mb) / 1024) : "\(mb) MB"
    }

    private func uptime(_ s: Int) -> String { String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60) }

    // MARK: Pending settings (a stopped instance)

    private var applyBar: some View {
        HStack(spacing: 14) {
            Circle().fill(dirty ? Color.orange : Color.green).frame(width: 7, height: 7)
            Text(note ?? (dirty ? L("Unsaved changes") : L("All settings saved")))
                .font(.system(size: Metrics.body, weight: .medium)).foregroundStyle(.secondary).lineLimit(1)
            Button(L("Revert")) { edited = instance.settings; note = nil }
                .buttonStyle(PillButtonStyle())
                .disabled(!dirty)
            Button(L("Apply")) {
                Task { note = await model.apply(instance.name, from: instance.settings, to: edited) }
            }
            .buttonStyle(PillButtonStyle(kind: dirty ? .primary : .normal))
            .keyboardShortcut(.defaultAction)
            .disabled(!dirty || busy)
        }
        .padding(.leading, 18).padding(.trailing, 7).padding(.vertical, 7)
        .glassPanel(cornerRadius: 23)
        .overlay(Capsule().strokeBorder(Color.primary.opacity(0.12)))
        .shadow(color: .black.opacity(0.25), radius: 16, y: 8)
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

    // The phone's RAM/storage change slowly and each read is an adb call: poll every 8 s while the page is shown.
    private func followUsage() async {
        while !Task.isCancelled {
            if let u = await model.usageInfo(instance.name) { model.usage[instance.name] = u }
            try? await Task.sleep(nanoseconds: 8_000_000_000)
        }
    }
}

struct CreateInstanceView: View {
    @EnvironmentObject var model: AppModel
    @Binding var isPresented: Bool
    @State private var name = ""
    @State private var settings = InstanceSettings.defaults(profile: .balanced)
    @State private var working = false
    @FocusState private var nameFocused: Bool

    private var nameProblem: String? {
        if name.isEmpty { return nil }
        if !isValidInstanceName(name) { return L("Lowercase letters, digits, - and _ (up to 32).") }
        if model.instance(name) != nil { return L("An instance with this name already exists.") }
        return nil
    }

    /// The line beside the buttons: what is missing, or what happens next.
    private var hint: String {
        nameProblem ?? (name.isEmpty ? L("Give the instance a name first.") : L("You can start it once it is created."))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            nameField.padding(.horizontal, 22).padding(.top, 18)
            // The settings in two columns, grouped under small headings, as in the design.
            SplitRow(ratio: 1, spacing: 16, minWidth: 600) {
                VStack(spacing: 14) {
                    BasicSettingsCard(settings: $settings,
                                      recommended: Profile.recommended(hostMemoryGB: model.hostMemoryGB))
                    HardwareCard(settings: $settings)
                }
                VStack(spacing: 14) {
                    DisplayCard(settings: $settings)
                    SystemCard(settings: $settings)
                }
            }
            .environment(\.groupedCards, true)
            .padding(.horizontal, 22).padding(.top, 16).padding(.bottom, 18)
            footer
        }
        .frame(width: 760)
        .fixedSize(horizontal: false, vertical: true)
        .onAppear {
            settings = .defaults(profile: Profile.recommended(hostMemoryGB: model.hostMemoryGB))
            if model.instances.isEmpty { name = "default" }
            nameFocused = true
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            AppMark(size: 36)
            VStack(alignment: .leading, spacing: 2) {
                Text(L("New instance")).font(.system(size: 15, weight: .bold))
                Text(L("A new Android phone. You can change its settings whenever it is stopped."))
                    .font(.system(size: 11)).foregroundStyle(.secondary)
            }
            Spacer(minLength: 12)
            RoundIconButton(systemName: "xmark", help: L("Cancel")) { isPresented = false }
        }
        .padding(.leading, 22).padding(.trailing, 16).padding(.top, 20)
    }

    private var nameField: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(L("Name")).font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary)
            TextField("", text: $name, prompt: Text("default"))
                .textFieldStyle(.plain)
                .font(.system(size: Metrics.body))
                .focused($nameFocused)
                .padding(.horizontal, 12).frame(height: Metrics.control + 4)
                .background(Color.black.opacity(0.22), in: Capsule())
                .overlay(Capsule().strokeBorder(nameBorder))
                .shadow(color: nameFocused && nameProblem == nil ? Color.accentColor.opacity(0.25) : .clear, radius: 3)
        }
    }

    private var nameBorder: Color {
        if nameProblem != nil { return Color.red.opacity(0.7) }
        return nameFocused ? Color.accentColor : Color.primary.opacity(0.12)
    }

    private var footer: some View {
        HStack(spacing: 10) {
            Image(systemName: nameProblem == nil ? "info.circle" : "exclamationmark.triangle.fill")
                .foregroundStyle(nameProblem == nil ? Color.secondary : Color.red)
            Text(hint).foregroundStyle(nameProblem == nil ? Color.secondary : Color.red)
            Spacer(minLength: 12)
            Button(L("Cancel")) { isPresented = false }
                .buttonStyle(.pill)
                .keyboardShortcut(.cancelAction)
            Button(L("Create")) {
                working = true
                Task {
                    if await model.create(name, settings) { isPresented = false }
                    working = false
                }
            }
            .buttonStyle(.pillPrimary)
            .keyboardShortcut(.defaultAction)
            .disabled(name.isEmpty || nameProblem != nil || working)
        }
        .font(.system(size: 11))
        .padding(.horizontal, 22).padding(.vertical, 14)
        .overlay(alignment: .top) { Rectangle().fill(Color.primary.opacity(0.07)).frame(height: 1) }
    }
}

struct SetupView: View {
    @EnvironmentObject var model: AppModel
    let runtime: RuntimeInfo
    @State private var accepted = false

    private var packages: [RuntimeInfo.Package] {
        runtime.packages.filter { $0.kind != "image" || $0.key == runtime.defaultImage }
    }

    /// SoulX was set up before (it has instances, or a repair was asked for): this page repairs, not welcomes.
    private var repair: Bool { model.repairing || !model.instances.isEmpty }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                header
                if !repair { FirstRunSteps(current: 0) }
                packagesCard
                if model.settingUp || !model.setupLog.isEmpty { progressCard }
            }
            .padding(.horizontal, 20).padding(.top, 4).padding(.bottom, 20)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
        .hiddenToolbarTitle()
    }

    // Same header as an instance page: title with a state pill, a caption under it.
    private var header: some View {
        HStack(alignment: .center, spacing: 16) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 8) {
                    Text(repair ? L("Repair the Android runtime") : L("Welcome to SoulX"))
                        .font(.system(size: 17, weight: .bold))
                    if model.settingUp {
                        StatusPill(text: repair ? L("Repairing…") : L("Installing…"), color: .orange)
                    } else if repair {
                        StatusPill(text: L("Needs repair"), color: .orange, systemImage: "wrench.and.screwdriver")
                    } else {
                        StatusPill(text: L("Setup needed"), color: .blue)
                    }
                }
                Text(repair ? L("Some runtime files are missing or damaged. Repair downloads them again; your instances, apps and game data are kept.")
                            : L("SoulX needs its Android runtime (engine, adb and Android 17) before the first start."))
                    .font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
            }
            Spacer(minLength: 16)
        }
        .padding(.vertical, 6)
    }

    private var packagesCard: some View {
        SectionCard(L("Android runtime"), icon: "shippingbox", count: packages.count) {
            if runtime.missingBytes > 0 {
                Text(String(format: L("%@ to download"),
                            ByteCountFormatter.string(fromByteCount: runtime.missingBytes, countStyle: .file)))
                    .font(.system(size: Metrics.caption)).foregroundStyle(.secondary).monospacedDigit()
            }
        } content: {
            ForEach(packages, id: \.key) { packageRow($0) }
            // the license, then the one action of the page
            HStack(spacing: 12) {
                Toggle("", isOn: $accepted).labelsHidden().toggleStyle(.switch).controlSize(.small)
                VStack(alignment: .leading, spacing: 2) {
                    Text(L("I accept the Android SDK License Agreement"))
                        .font(.system(size: Metrics.body, weight: .medium))
                    Link(L("Read the license"), destination: URL(string: "https://developer.android.com/studio/terms")!)
                        .font(.system(size: Metrics.caption))
                }
                Spacer(minLength: 12)
                Button { model.installRuntime() } label: {
                    Label(repair ? L("Repair") : L("Install"), systemImage: repair ? "wrench.and.screwdriver" : "arrow.down.circle")
                }
                    .buttonStyle(.pillPrimary)
                    .disabled(!accepted || model.settingUp)
                    .keyboardShortcut(.defaultAction)
                    .help(accepted ? L("Install") : L("Accept the license first."))
            }
            .cardLine(vertical: 12)
        }
    }

    /// A package, laid out like an app in an instance's apps list.
    private func packageRow(_ p: RuntimeInfo.Package) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon(for: p.kind))
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(p.installed ? Color.green : Color.accentColor)
                .frame(width: 24, height: 24)
                .background((p.installed ? Color.green : Color.accentColor).opacity(0.14),
                            in: RoundedRectangle(cornerRadius: 7, style: .continuous))
            VStack(alignment: .leading, spacing: 2) {
                Text(p.description ?? p.key).font(.system(size: Metrics.listTitle, weight: .semibold)).lineLimit(1)
                Text(p.key + (p.version.isEmpty ? "" : " · " + p.version))
                    .font(.system(size: Metrics.listDetail, design: .monospaced)).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 12)
            if p.installed {
                StatusPill(text: L("Installed"), color: .green)
            } else {
                Text(ByteCountFormatter.string(fromByteCount: p.downloadBytes, countStyle: .file))
                    .font(.system(size: Metrics.caption)).foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .padding(.leading, 16).padding(.trailing, 12)
        .frame(height: AppsSection.rowHeight)
        .overlay(alignment: .top) { Rectangle().fill(Color.primary.opacity(0.07)).frame(height: 1) }
    }

    private func icon(for kind: String) -> String {
        switch kind {
        case "image": return "iphone"
        case "emulator", "engine": return "cpu"
        case "platform-tools", "tools": return "wrench.and.screwdriver"
        default: return "shippingbox"
        }
    }

    private var progressCard: some View {
        SectionCard(L("Progress"), icon: "arrow.down.circle") {
            if model.settingUp {
                Button(L("Cancel")) { model.cancelRuntimeInstall() }
                    .buttonStyle(.pill)
                    .keyboardShortcut(.cancelAction)
            }
        } content: {
            VStack(alignment: .leading, spacing: 10) {
                if let step = stepLine { Text(step).font(.system(size: Metrics.body, weight: .medium)) }
                if let (pct, text) = progress, model.settingUp {
                    MeterBar(fraction: Double(pct) / 100)
                    Text(text).font(.system(size: Metrics.caption)).foregroundStyle(.secondary).monospacedDigit()
                } else if model.settingUp {
                    HStack { ProgressView().controlSize(.small); Text(L("Installing…")).foregroundStyle(.secondary) }
                        .font(.system(size: Metrics.body))
                } else if let last = model.setupLog.last {
                    Text(clean(last)).font(.system(size: Metrics.caption))
                }
                DisclosureGroup(L("Details")) {
                    ScrollView {
                        Text(model.setupLog.suffix(200).map(clean).joined(separator: "\n"))
                            .font(.system(size: Metrics.listDetail, design: .monospaced))
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .textSelection(.enabled)
                    }
                    .frame(minHeight: 140, maxHeight: 220)
                }
                .font(.system(size: Metrics.body))
            }
            .cardLine(vertical: 12)
        }
    }

    private func clean(_ line: String) -> String {
        line.hasPrefix("soulx: ") ? String(line.dropFirst(7)) : line
    }

    /// "[2/3] Android SDK Platform-Tools ..." : which package is being installed.
    private var stepLine: String? {
        model.setupLog.last(where: { clean($0).hasPrefix("[") }).map(clean)
    }

    /// The latest "...  45% (187 MB / 416 MB)" line and its percentage.
    private var progress: (Int, String)? {
        guard let line = model.setupLog.last, let r = line.range(of: #"(\d{1,3})%"#, options: .regularExpression),
              let pct = Int(line[r].dropLast()) else { return nil }
        return (min(pct, 100), clean(line))
    }
}

/// The three steps from a fresh install to playing: done ones ticked, the current one in the accent colour.
struct FirstRunSteps: View {
    let current: Int          // 0 install the runtime, 1 create an instance, 2 start it and install a game

    private let steps: [(String, String)] = [
        ("Install the Android runtime", "Engine, adb and Android 17, once."),
        ("Create an instance", "One Android phone, with its own apps and data."),
        ("Start it and install your game", "Drop an APK on its window, or install from the instance page."),
    ]

    var body: some View {
        HStack(alignment: .top, spacing: 0) {
            ForEach(steps.indices, id: \.self) { i in
                HStack(alignment: .top, spacing: 10) {
                    mark(i)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(L(steps[i].0)).font(.system(size: Metrics.body, weight: .semibold))
                            .foregroundStyle(i <= current ? Color.primary : Color.secondary)
                        Text(L(steps[i].1)).font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 14).padding(.vertical, 12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .overlay(alignment: .leading) {
                    if i > 0 { Rectangle().fill(Color.primary.opacity(0.07)).frame(width: 1) }
                }
            }
        }
        .surface()
    }

    @ViewBuilder private func mark(_ i: Int) -> some View {
        ZStack {
            if i < current {
                Circle().fill(Color.green.opacity(0.18))
                Image(systemName: "checkmark").font(.system(size: 10, weight: .bold)).foregroundStyle(.green)
            } else if i == current {
                Circle().fill(Color.accentColor)
                Text("\(i + 1)").font(.system(size: 11, weight: .bold)).foregroundStyle(.white)
            } else {
                Circle().strokeBorder(Color.primary.opacity(0.25), lineWidth: 1.5)
                Text("\(i + 1)").font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary)
            }
        }
        .frame(width: 22, height: 22)
    }
}

/// What a new user sees once the runtime is in and no instance exists yet.
struct WelcomeView: View {
    let create: () -> Void
    let importInstance: () -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                HStack(alignment: .center, spacing: 16) {
                    VStack(alignment: .leading, spacing: 3) {
                        HStack(spacing: 8) {
                            Text(L("Welcome to SoulX")).font(.system(size: 17, weight: .bold))
                            StatusPill(text: L("Runtime ready"), color: .green)
                        }
                        Text(L("An instance is one Android phone. Create one, start it, then install your game from its window."))
                            .font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 16)
                }
                .padding(.vertical, 6)
                FirstRunSteps(current: 1)
                // a large, quiet call to action where the instance page would be
                VStack(spacing: 12) {
                    AppMark(size: 44)
                    VStack(spacing: 4) {
                        Text(L("No instances yet.")).font(.system(size: 14, weight: .semibold))
                        Text(L("It takes a few seconds; you can change its settings whenever it is stopped."))
                            .font(.system(size: Metrics.caption)).foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    Button(action: create) { Label(L("Create instance"), systemImage: "plus") }
                        .buttonStyle(.pillPrimary)
                        .keyboardShortcut(.defaultAction)
                    Button(L("Or import an existing instance…"), action: importInstance)
                        .buttonStyle(.link)
                        .font(.system(size: Metrics.caption))
                }
                .padding(.vertical, 36)
                .frame(maxWidth: .infinity)
                .surface()
            }
            .padding(.horizontal, 20).padding(.top, 4).padding(.bottom, 20)
            .frame(maxWidth: 820)
            .frame(maxWidth: .infinity)
        }
        .hiddenToolbarTitle()
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

    /// A small panel surface: Liquid Glass on macOS 26, a plain material before it.
    @ViewBuilder func glassPanel(cornerRadius: CGFloat) -> some View {
        if #available(macOS 26.0, *) {
            self.glassEffect(.regular, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        } else {
            self.background(.regularMaterial, in: RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
        }
    }

    /// Pages that put their own title in the content leave the window's toolbar title out.
    @ViewBuilder func hiddenToolbarTitle() -> some View {
        if #available(macOS 15.0, *) {
            self.toolbar(removing: .title)
        } else {
            self
        }
    }
}

/// A picked .soulxvm and what it holds, for the import sheet.
struct ImportRequest: Identifiable {
    let url: URL
    let manifest: VmManifest
    var id: URL { url }
}
