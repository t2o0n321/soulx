import SwiftUI
import SoulXCore

/// The instance settings, in two layers: what everyone needs (performance, orientation, resolution) and an
/// "Advanced" group. Used by the create sheet and the instance page.
struct SettingsForm: View {
    @Binding var settings: InstanceSettings
    var recommended: Profile
    var minDataGB: Int = 4             // existing instances: storage only grows
    var allowOrientation = true

    var body: some View {
        Section(L("Settings")) {
            Picker(L("Performance"), selection: profileBinding) {
                ForEach(Profile.allCases) { p in
                    Text(L(p.rawValue) + (p == recommended ? "  · " + L("recommended for this Mac") : "")).tag(Optional(p))
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
        }
        Section {
            DisclosureGroup(L("Advanced")) {
                Stepper("\(L("CPU cores")): \(settings.cores)", value: $settings.cores, in: 1...8)
                Picker(L("Memory"), selection: $settings.ramMB) {
                    ForEach([4096, 6144, 8192, 12288], id: \.self) { Text("\($0 / 1024) GB").tag($0) }
                }
                Stepper("\(L("Storage")): \(settings.dataGB) GB", value: $settings.dataGB, in: minDataGB...64, step: 2)
                if minDataGB > 4 {
                    Text(L("Storage can only grow.")).font(.caption).foregroundStyle(.secondary)
                }
                Picker(L("Graphics"), selection: $settings.gpu) {
                    Text(L("Automatic (Mac GPU)")).tag(GPUMode.host)
                    Text(L("Compatibility (OpenGL ES)")).tag(GPUMode.hostNoVulkan)
                    Text(L("Software (slow)")).tag(GPUMode.software)
                    if settings.gpu == .metal { Text(L("Metal (experimental)")).tag(GPUMode.metal) }
                }
                Picker(L("Frame rate cap"), selection: $settings.fps) {
                    Text("30 FPS").tag(30)
                    Text("45 FPS").tag(45)
                    Text("60 FPS (\(L("no cap")))").tag(60)
                    if ![30, 45, 60].contains(settings.fps) { Text("\(settings.fps) FPS").tag(settings.fps) }
                }
                HStack {
                    TextField(L("DNS servers"), text: $settings.dns, prompt: Text(L("the Mac's")))
                    Button(L("Use public DNS")) { settings.dns = "1.1.1.1,8.8.8.8" }
                }
                Toggle(L("Keep screen on"), isOn: $settings.stayAwake)
                Toggle(L("Lean Android (disable unused Google apps, about 600 MB less)"), isOn: $settings.lean)
                Toggle(L("Sound"), isOn: $settings.audio)
            }
        }
    }

    private var profileBinding: Binding<Profile?> {
        Binding(get: { settings.profile }, set: { if let p = $0 { settings.apply(p) } })
    }

    private var resolutionBinding: Binding<Resolution> {
        Binding(get: { settings.resolution }, set: { settings.setResolution($0) })
    }
}
