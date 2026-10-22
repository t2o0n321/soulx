import SwiftUI
import SoulXCore

// The instance settings as four cards: what everyone needs (performance, orientation, resolution), hardware,
// display and performance, and system. The instance page lays them out in its two columns; the create sheet
// stacks them (SettingsForm). `locked`: a running instance, whose settings change only once it is stopped.

struct BasicSettingsCard: View {
    @Binding var settings: InstanceSettings
    var recommended: Profile
    var allowOrientation = true
    var locked = false

    var body: some View {
        SectionCard(L("Settings"), icon: "slider.horizontal.3") {
            if locked { lockMark }
        } content: {
            Group {
                SettingRow(L("Performance")) {
                    ValueMenu(profileLabel) {
                        Picker("", selection: profileBinding) {
                            ForEach(Profile.allCases) { p in Text(label(for: p)).tag(Optional(p)) }
                            if settings.profile == nil { Text(L("Custom")).tag(Optional<Profile>.none) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
                if allowOrientation {
                    SettingRow(L("Orientation")) {
                        SegmentedPill(selection: $settings.landscape,
                                      options: [(false, L("Portrait")), (true, L("Landscape"))])
                    }
                }
                SettingRow(L("Resolution")) {
                    ValueMenu(resolutionLabel) {
                        Picker("", selection: resolutionBinding) {
                            ForEach(Resolution.presets) { r in Text(r.rawValue).tag(r) }
                            if settings.resolution == .custom { Text(resolutionLabel).tag(Resolution.custom) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
            }
            .disabled(locked)
        }
    }

    private func label(for p: Profile) -> String {
        L(p.rawValue) + (p == recommended ? " · " + L("recommended for this Mac") : "")
    }

    private var profileLabel: String { settings.profile.map(label(for:)) ?? L("Custom") }

    private var resolutionLabel: String {
        settings.resolution == .custom ? "\(L("Custom")) \(settings.width)×\(settings.height)" : settings.resolution.rawValue
    }

    private var profileBinding: Binding<Profile?> {
        Binding(get: { settings.profile }, set: { if let p = $0 { settings.apply(p) } })
    }

    private var resolutionBinding: Binding<Resolution> {
        Binding(get: { settings.resolution }, set: { settings.setResolution($0) })
    }
}

struct HardwareCard: View {
    @Binding var settings: InstanceSettings
    var minDataGB = 4                  // existing instances: storage only grows
    var locked = false

    var body: some View {
        SectionCard(L("Hardware"), icon: "cpu") {
            if locked { lockMark }
        } content: {
            Group {
                SettingRow(L("CPU cores")) {
                    ValueMenu("\(settings.cores) \(L("cores"))") {
                        Picker("", selection: $settings.cores) {
                            ForEach(1...8, id: \.self) { Text("\($0) \(L("cores"))").tag($0) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
                SettingRow(L("Memory")) {
                    ValueMenu("\(settings.ramMB / 1024) GB") {
                        Picker("", selection: $settings.ramMB) {
                            ForEach([4096, 6144, 8192, 12288], id: \.self) { Text("\($0 / 1024) GB").tag($0) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
                SettingRow(L("Storage"), caption: L("Storage can only grow.")) {
                    ValueMenu("\(settings.dataGB) GB") {
                        Picker("", selection: $settings.dataGB) {
                            ForEach(storageChoices, id: \.self) { Text("\($0) GB").tag($0) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
            }
            .disabled(locked)
        }
    }

    /// Sizes offered for storage: common ones, never below what the instance already has.
    private var storageChoices: [Int] {
        Set([8, 16, 24, 32, 48, 64, settings.dataGB, minDataGB]).filter { $0 >= minDataGB }.sorted()
    }
}

struct DisplayCard: View {
    @Binding var settings: InstanceSettings
    var locked = false

    var body: some View {
        SectionCard(L("Display and performance"), icon: "gauge.with.dots.needle.33percent") {
            if locked { lockMark }
        } content: {
            Group {
                SettingRow(L("Graphics"), caption: L("Try Compatibility if a game shows black or broken graphics.")) {
                    ValueMenu(gpuLabel(settings.gpu)) {
                        Picker("", selection: $settings.gpu) {
                            Text(gpuLabel(.host)).tag(GPUMode.host)
                            Text(gpuLabel(.hostNoVulkan)).tag(GPUMode.hostNoVulkan)
                            Text(gpuLabel(.software)).tag(GPUMode.software)
                            if settings.gpu == .metal { Text(gpuLabel(.metal)).tag(GPUMode.metal) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
                SettingRow(L("Frame rate cap"), caption: L("30 FPS uses about 30% less CPU, good for idle farming.")) {
                    ValueMenu("\(settings.fps) FPS") {
                        Picker("", selection: $settings.fps) {
                            Text("30 FPS").tag(30)
                            Text("45 FPS").tag(45)
                            Text("60 FPS").tag(60)
                            if ![30, 45, 60].contains(settings.fps) { Text("\(settings.fps) FPS").tag(settings.fps) }
                        }
                        .pickerStyle(.inline).labelsHidden()
                    }
                }
            }
            .disabled(locked)
        }
    }

    private func gpuLabel(_ g: GPUMode) -> String {
        switch g {
        case .host: return L("Automatic (Mac GPU)")
        case .hostNoVulkan: return L("Compatibility (OpenGL ES)")
        case .software: return L("Software (slow)")
        case .metal: return L("Metal (experimental)")
        }
    }
}

struct SystemCard: View {
    @Binding var settings: InstanceSettings
    var locked = false
    var instance: Instance?
    @EnvironmentObject var model: AppModel

    var body: some View {
        SectionCard(L("System"), icon: "server.rack") {
            if locked { lockMark }
        } content: {
            Group {
                SettingRow(L("DNS servers"), caption: L("Change this when Android has no internet (VPN or company network).")) {
                    HStack(spacing: 8) {
                        TextField("", text: $settings.dns, prompt: Text(L("Use the Mac's DNS")))
                            .textFieldStyle(.plain)
                            .font(.system(size: Metrics.body).monospacedDigit())
                            .padding(.horizontal, 10).frame(width: 150, height: Metrics.control)
                            .background(Color.primary.opacity(0.06), in: Capsule())
                            .overlay(Capsule().strokeBorder(Color.primary.opacity(0.10)))
                        Button(L("Use public DNS")) { settings.dns = "1.1.1.1,8.8.8.8" }
                            .buttonStyle(.pill)
                    }
                }
                SettingRow(L("Keep screen on"), caption: L("The screen never turns off while idle.")) {
                    Toggle("", isOn: $settings.stayAwake).labelsHidden().toggleStyle(.switch).controlSize(.small)
                }
                SettingRow(L("Lean Android"), caption: L("Disables preinstalled Google apps games never use; about 600 MB less memory.")) {
                    Toggle("", isOn: $settings.lean).labelsHidden().toggleStyle(.switch).controlSize(.small)
                }
                SettingRow(L("Sound"), caption: L("Off removes the sound device; saves a little CPU.")) {
                    Toggle("", isOn: $settings.audio).labelsHidden().toggleStyle(.switch).controlSize(.small)
                }
            }
            .disabled(locked)
            if let inst = instance {
                rootToggle(inst)
            }
        }
    }

    @ViewBuilder private func rootToggle(_ inst: Instance) -> some View {
        let rooted = inst.rooted
        let busy = model.busy[inst.name] != nil
        SettingRow(L("Root (Magisk)"), caption: rootCaption(inst)) {
            Toggle("", isOn: Binding(
                get: { rooted },
                set: { enable in
                    Task { await model.applyRoot(inst.name, enable: enable) }
                }
            ))
            .labelsHidden().toggleStyle(.switch).controlSize(.small)
            .disabled(busy || inst.running)
        }
    }

    private func rootCaption(_ inst: Instance) -> String {
        if model.busy[inst.name] != nil { return L("Grants root access via Magisk. Downloads and patches on first enable.") }
        switch inst.rootStatus {
        case "active": return L("Magisk root is active.")
        case "ready": return L("Magisk root is ready. Start the instance to use it.")
        case "inactive": return L("Root is enabled but Magisk is not active. Try restarting.")
        case "missing": return L("Root is enabled but the ramdisk is missing. Toggle off and on to repair.")
        default: return L("Grants root access via Magisk. Downloads and patches on first enable.")
        }
    }
}

/// The lock shown on a settings card's header while the instance runs.
private var lockMark: some View {
    Image(systemName: "lock.fill").font(.system(size: 11)).foregroundStyle(.secondary)
        .help(L("Settings cannot change while the instance runs. Stop it to change them."))
}
