import SwiftUI
import SoulXCore

/// The instance settings, in two layers: what everyone needs (performance, orientation, resolution) and an
/// "Advanced" group. Used by the create sheet and the instance page.
struct SettingsForm: View {
    @Binding var settings: InstanceSettings
    var recommended: Profile
    var minDataGB: Int = 4             // existing instances: storage only grows
    var allowOrientation = true
    @AppStorage("showAdvancedSettings") private var showAdvanced = false

    var body: some View {
        Section(L("Settings")) {
            Picker(L("Performance"), selection: profileBinding) {
                ForEach(Profile.allCases) { p in
                    Text(L(p.rawValue) + (p == recommended ? " · " + L("recommended for this Mac") : "")).tag(Optional(p))
                }
                if settings.profile == nil { Text(L("Custom")).tag(Optional<Profile>.none) }
            }
            if allowOrientation {
                Picker(L("Orientation"), selection: $settings.landscape) {
                    Text(L("Portrait")).tag(false)
                    Text(L("Landscape")).tag(true)
                }
                .pickerStyle(.segmented)
            }
            Picker(L("Resolution"), selection: resolutionBinding) {
                ForEach(Resolution.presets) { r in Text(r.rawValue).tag(r) }
                if settings.resolution == .custom {
                    Text("\(L("Custom")) \(settings.width)×\(settings.height)").tag(Resolution.custom)
                }
            }
            Button {
                withAnimation(.easeInOut(duration: 0.2)) { showAdvanced.toggle() }
            } label: {
                HStack {
                    Text(L("Advanced settings"))
                    Spacer()
                    Image(systemName: "chevron.right")
                        .rotationEffect(.degrees(showAdvanced ? 90 : 0))
                        .foregroundStyle(.secondary)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
        }
        if showAdvanced {
            Section(L("Hardware")) {
                Picker(L("CPU cores"), selection: $settings.cores) {
                    ForEach(1...8, id: \.self) { Text("\($0)").tag($0) }
                }
                Picker(L("Memory"), selection: $settings.ramMB) {
                    ForEach([4096, 6144, 8192, 12288], id: \.self) { Text("\($0 / 1024) GB").tag($0) }
                }
                Picker(selection: $settings.dataGB) {
                    ForEach(storageChoices, id: \.self) { Text("\($0) GB").tag($0) }
                } label: {
                    Text(L("Storage"))
                    if minDataGB > 4 { Text(L("Storage can only grow.")) }
                }
            }
            Section(L("Display and performance")) {
                Picker(selection: $settings.gpu) {
                    Text(L("Automatic (Mac GPU)")).tag(GPUMode.host)
                    Text(L("Compatibility (OpenGL ES)")).tag(GPUMode.hostNoVulkan)
                    Text(L("Software (slow)")).tag(GPUMode.software)
                    if settings.gpu == .metal { Text(L("Metal (experimental)")).tag(GPUMode.metal) }
                } label: {
                    Text(L("Graphics"))
                    Text(L("Try Compatibility if a game shows black or broken graphics."))
                }
                Picker(selection: $settings.fps) {
                    Text("30 FPS").tag(30)
                    Text("45 FPS").tag(45)
                    Text("60 FPS").tag(60)
                    if ![30, 45, 60].contains(settings.fps) { Text("\(settings.fps) FPS").tag(settings.fps) }
                } label: {
                    Text(L("Frame rate cap"))
                    Text(L("30 FPS uses about 30% less CPU, good for idle farming."))
                }
            }
            Section(L("System")) {
                LabeledContent {
                    HStack {
                        TextField("", text: $settings.dns, prompt: Text(L("the Mac's")))
                            .multilineTextAlignment(.trailing)
                            .frame(maxWidth: 180)
                        Button(L("Use public DNS")) { settings.dns = "1.1.1.1,8.8.8.8" }
                            .controlSize(.small)
                    }
                } label: {
                    Text(L("DNS servers"))
                    Text(L("Change this when Android has no internet (VPN or company network)."))
                }
                Toggle(isOn: $settings.stayAwake) { Text(L("Keep screen on")) }
                Toggle(isOn: $settings.lean) {
                    Text(L("Lean Android"))
                    Text(L("Disables preinstalled Google apps games never use; about 600 MB less memory."))
                }
                Toggle(isOn: $settings.audio) {
                    Text(L("Sound"))
                    Text(L("Off removes the sound device; saves a little CPU."))
                }
            }
        }
    }

    /// Sizes offered for storage: common ones, never below what the instance already has (it only grows).
    private var storageChoices: [Int] {
        Set([8, 16, 24, 32, 48, 64, settings.dataGB, minDataGB]).filter { $0 >= minDataGB }.sorted()
    }

    private var profileBinding: Binding<Profile?> {
        Binding(get: { settings.profile }, set: { if let p = $0 { settings.apply(p) } })
    }

    private var resolutionBinding: Binding<Resolution> {
        Binding(get: { settings.resolution }, set: { settings.setResolution($0) })
    }
}
