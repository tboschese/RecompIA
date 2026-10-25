import SwiftUI
import AppKit
import PortKit
import GameController

@MainActor final class AppModel: ObservableObject {
    @Published var document = LibraryDocument()
    @Published var error: String?
    @Published var busy: String?
    @Published var status = "Primeiro alvo: Nintendo Switch"
    @Published var controllerNames: [String] = []
    @Published var recipes: [String: RecompRecipe] = [:]
    let store: LibraryStore
    var controllerMove: ((Int) -> Void)?
    var controllerPlay: (() -> Void)?
    private var observers: [NSObjectProtocol] = []
    private var loaded = false
    init() {
        store = LibraryStore(root: ProcessInfo.processInfo.environment["RECOMPIA_LIBRARY"].map { URL(fileURLWithPath: $0) })
        do { document = try store.load(); loaded = true; refresh() }
        catch { self.error = "Não foi possível abrir a biblioteca. \(error.localizedDescription)" }
        for name in [NSNotification.Name.GCControllerDidConnect, .GCControllerDidDisconnect] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor in self?.updateControllers() }
            })
        }
        updateControllers()
    }
    func refresh() {
        recipes = [:]
        for game in document.games {
            if let recipe = try? RecompRecipe.load(project: URL(fileURLWithPath: game.project)) { recipes[game.id] = recipe }
        }
    }
    func persist() {
        guard loaded else { error = "A biblioteca não foi sobrescrita. Corrija library.json e reabra o app."; return }
        do { try store.save(document) } catch { self.error = error.localizedDescription }
    }
    func add(title: String, console: Console) {
        guard loaded else { return }
        let dialog = NSSavePanel(); dialog.title = "Escolher pasta do projeto"
        dialog.nameFieldStringValue = PortBuilder.filename(title); dialog.canCreateDirectories = true
        dialog.message = "A pasta guardará a receita e o código deste port. Nenhum jogo será convertido nesta etapa."
        guard dialog.runModal() == .OK, let destination = dialog.url else { return }
        do {
            let game = try store.createProject(title: title, console: console, destination: destination)
            document.games.append(game); persist(); refresh(); status = "Projeto criado. Próxima etapa: analisar \(title)."
        } catch { self.error = error.localizedDescription }
    }
    func importProject() {
        let dialog = NSOpenPanel(); dialog.title = "Abrir projeto de recompilação"
        dialog.message = "Escolha a pasta que contém recomp.json."
        dialog.canChooseDirectories = true; dialog.canChooseFiles = false
        guard dialog.runModal() == .OK, let url = dialog.url else { return }
        do {
            let game = try store.importProject(url)
            guard !document.games.contains(where: { $0.id == game.id || $0.project == game.project }) else { status = "Este projeto já está na biblioteca."; return }
            document.games.append(game); persist(); refresh()
        } catch { self.error = error.localizedDescription }
    }
    func update(_ game: LibraryGame) {
        if let index = document.games.firstIndex(where: { $0.id == game.id }) { document.games[index] = game; persist() }
    }
    func addCover(_ game: LibraryGame) {
        let dialog = NSOpenPanel(); dialog.allowedContentTypes = [.png, .jpeg, .tiff]
        guard dialog.runModal() == .OK, let source = dialog.url else { return }
        do {
            let folder = store.root.appendingPathComponent("Covers")
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let relative = "Covers/\(UUID().uuidString).\(source.pathExtension)"
            try FileManager.default.copyItem(at: source, to: store.root.appendingPathComponent(relative))
            var value = game; value.cover = relative; update(value)
        } catch { self.error = error.localizedDescription }
    }
    func compile(_ game: LibraryGame) {
        guard busy == nil else { return }
        busy = "Compilando \(game.title)…"
        Task {
            do {
                _ = try await Task.detached(priority: .userInitiated) { try PortBuilder().compile(project: URL(fileURLWithPath: game.project)) }.value
                status = "App compilado. Jogabilidade e controle ainda precisam ser validados."
            } catch { self.error = error.localizedDescription }
            refresh(); busy = nil
        }
    }
    func output(_ game: LibraryGame) throws -> URL {
        let project = URL(fileURLWithPath: game.project)
        let recipe = try RecompRecipe.load(project: project)
        let app = try PortBuilder.contained(recipe.outputApp, in: project)
        _ = try PortBuilder.nativeExecutable(app)
        return app
    }
    func play(_ game: LibraryGame) {
        do {
            let app = try output(game)
            let options = NSWorkspace.OpenConfiguration(); options.activates = true
            NSWorkspace.shared.openApplication(at: app, configuration: options) { [weak self] _, error in
                Task { @MainActor in
                    if let error { self?.error = error.localizedDescription }
                    else { var value = game; value.lastPlayed = Date(); self?.update(value) }
                }
            }
        } catch { self.error = error.localizedDescription }
    }
    func export(_ game: LibraryGame) {
        guard busy == nil else { return }
        do { _ = try output(game) } catch { self.error = error.localizedDescription; return }
        let dialog = NSSavePanel(); dialog.title = "Exportar port para Mac"
        dialog.nameFieldStringValue = PortBuilder.filename(game.title) + ".app"; dialog.canCreateDirectories = true
        guard dialog.runModal() == .OK, let url = dialog.url else { return }
        let destination = url.pathExtension == "app" ? url : url.appendingPathExtension("app")
        busy = "Exportando \(game.title)…"
        Task {
            do {
                let result = try await Task.detached(priority: .userInitiated) {
                    try PortBuilder().export(project: URL(fileURLWithPath: game.project), destination: destination)
                }.value
                NSWorkspace.shared.activateFileViewerSelecting([result]); status = "Port exportado: \(result.lastPathComponent)"
            } catch { self.error = error.localizedDescription }
            busy = nil
        }
    }
    func remove(_ game: LibraryGame) { document.games.removeAll { $0.id == game.id }; persist(); refresh(); status = "Removido da lista. O projeto foi preservado." }
    private func updateControllers() {
        controllerNames = GCController.controllers().map { $0.vendorName ?? "Controle" }
        for controller in GCController.controllers() {
            controller.extendedGamepad?.dpad.valueChangedHandler = { [weak self] _, x, y in
                guard NSApp.isActive else { return }
                let offset = abs(x) > 0.5 ? (x > 0 ? 1 : -1) : (abs(y) > 0.5 ? (y > 0 ? -1 : 1) : 0)
                if offset != 0 { Task { @MainActor in self?.controllerMove?(offset) } }
            }
            controller.extendedGamepad?.buttonA.pressedChangedHandler = { [weak self] _, _, pressed in
                guard pressed, NSApp.isActive else { return }
                Task { @MainActor in self?.controllerPlay?() }
            }
        }
    }
}
