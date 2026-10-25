import SwiftUI

@main struct RecompIAApp: App {
    @StateObject private var model = AppModel()
    var body: some Scene {
        WindowGroup("RecompIA · Recomp") { LibraryView().environmentObject(model) }
            .defaultSize(width: 1180, height: 780).windowStyle(.hiddenTitleBar)
            .commands { CommandGroup(replacing: .newItem) {} }
    }
}
