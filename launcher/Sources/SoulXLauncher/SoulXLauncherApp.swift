import SwiftUI
import AppKit

@main
struct SoulXLauncherApp: App {
    @StateObject private var model = AppModel()
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        WindowGroup("SoulX") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 760, minHeight: 520)
                .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
                    model.shutdown()
                }
        }
        .defaultSize(width: 900, height: 640)
    }
}

/// Run from `swift run` (no app bundle) the process would start as a background tool: make it a regular app
/// with a Dock icon and bring it to the front.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
