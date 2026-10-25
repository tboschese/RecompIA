import Foundation

public enum Console: String, Codable, CaseIterable, Identifiable, Sendable {
    case switchConsole = "switch", ps2, ps3, xbox360
    public var id: String { rawValue }
    public var name: String {
        switch self {
        case .switchConsole: return "Nintendo Switch"
        case .ps2: return "PlayStation 2"
        case .ps3: return "PlayStation 3"
        case .xbox360: return "Xbox 360"
        }
    }
    public var shortName: String {
        switch self {
        case .switchConsole: return "SWITCH"
        case .ps2: return "PS2"
        case .ps3: return "PS3"
        case .xbox360: return "XBOX 360"
        }
    }
    public var architecture: String {
        switch self {
        case .switchConsole: return "AArch64 · NVN / Maxwell"
        case .ps2: return "MIPS R5900 · VU / GS"
        case .ps3: return "PowerPC PPU · SPU / RSX"
        case .xbox360: return "PowerPC Xenon · Xenos"
        }
    }
}

public enum PortStage: String, Codable, CaseIterable, Identifiable, Sendable {
    case planning, analysis, recompilation, runtime, validation, playable
    public var id: String { rawValue }
    public var title: String {
        switch self {
        case .planning: return "Planejamento"
        case .analysis: return "Análise do jogo"
        case .recompilation: return "Recompilação"
        case .runtime: return "Runtime para Mac"
        case .validation: return "Em validação"
        case .playable: return "Jogável · validado"
        }
    }
}

public struct BuildStep: Codable, Equatable, Sendable {
    public var name: String
    public var executable: String
    public var arguments: [String]
    public var directory: String
    public init(name: String, executable: String, arguments: [String], directory: String = ".") {
        self.name = name; self.executable = executable; self.arguments = arguments; self.directory = directory
    }
}

/// A per-title recipe. Empty steps intentionally mean that no recompiler is integrated yet.
public struct RecompRecipe: Codable, Sendable {
    public var version = 1
    public var id: String
    public var title: String
    public var console: Console
    public var stage: PortStage = .planning
    public var input: String = ""
    public var inputSHA256: String = ""
    public var steps: [BuildStep] = []
    public var outputApp: String = "build/Game.app"
    public var controllerSupport: String = "Pendente no runtime do jogo"
    public var notes: String = "Definir a versão do jogo, analisar o executável, integrar o recompilador e implementar o runtime para macOS."
    public init(id: String, title: String, console: Console) { self.id = id; self.title = title; self.console = console }
    public static func load(project: URL) throws -> RecompRecipe {
        let recipe = try JSONDecoder().decode(Self.self, from: Data(contentsOf: project.appendingPathComponent("recomp.json")))
        guard recipe.version == 1, !recipe.id.isEmpty, !recipe.title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw PortError.message("Projeto de recompilação inválido ou de versão incompatível.")
        }
        return recipe
    }
    public func save(project: URL) throws {
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: project.appendingPathComponent("recomp.json"), options: .atomic)
    }
}

public struct LibraryGame: Codable, Identifiable, Equatable, Sendable {
    public var id: String
    public var title: String
    public var console: Console
    public var project: String
    public var cover: String?
    public var favorite = false
    public var addedAt = Date()
    public var lastPlayed: Date?
    public init(recipe: RecompRecipe, project: URL) {
        id = recipe.id; title = recipe.title; console = recipe.console; self.project = project.path
    }
}

public struct LibraryDocument: Codable, Sendable {
    public var version = 2
    public var games: [LibraryGame] = []
    public init() {}
}
public enum PortError: LocalizedError {
    case message(String)
    public var errorDescription: String? { if case .message(let text) = self { return text }; return nil }
}

public struct LibraryStore: Sendable {
    public let root: URL
    public init(root: URL? = nil) {
        self.root = root ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("RecompIA", isDirectory: true)
    }
    public func load() throws -> LibraryDocument {
        let url = root.appendingPathComponent("library.json")
        guard FileManager.default.fileExists(atPath: url.path) else { return LibraryDocument() }
        let document = try JSONDecoder().decode(LibraryDocument.self, from: Data(contentsOf: url))
        guard document.version == 2 else { throw PortError.message("Versão de biblioteca incompatível. O arquivo original foi preservado.") }
        return document
    }
    public func save(_ document: LibraryDocument) throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(document).write(to: root.appendingPathComponent("library.json"), options: .atomic)
    }
    public func createProject(title: String, console: Console, destination: URL? = nil) throws -> LibraryGame {
        let clean = title.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !clean.isEmpty else { throw PortError.message("Informe o nome do jogo.") }
        let id = "\(console.rawValue)-\(UUID().uuidString.lowercased())"
        let project = destination ?? root.appendingPathComponent("Projects/\(id)")
        guard !FileManager.default.fileExists(atPath: project.path) else { throw PortError.message("A pasta de destino já existe. Importe o projeto existente ou escolha outra pasta.") }
        try FileManager.default.createDirectory(at: project, withIntermediateDirectories: true)
        let recipe = RecompRecipe(id: id, title: clean, console: console)
        try recipe.save(project: project)
        return LibraryGame(recipe: recipe, project: project)
    }
    public func importProject(_ project: URL) throws -> LibraryGame {
        LibraryGame(recipe: try RecompRecipe.load(project: project), project: project.standardizedFileURL)
    }
}
