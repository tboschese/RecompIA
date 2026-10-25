import Foundation
import CryptoKit

public struct PortBuilder: Sendable {
    public init() {}
    public static func filename(_ title: String) -> String {
        let invalid = CharacterSet(charactersIn: "/:\\").union(.controlCharacters)
        let name = title.components(separatedBy: invalid).joined(separator: "-").trimmingCharacters(in: .whitespacesAndNewlines)
        return name.isEmpty || name == "." || name == ".." ? "Jogo" : String(name.prefix(100))
    }
    public static func contained(_ path: String, in project: URL) throws -> URL {
        guard !path.isEmpty, !path.hasPrefix("/") else { throw PortError.message("Use um caminho relativo dentro do projeto: \(path)") }
        let base = project.resolvingSymlinksInPath().standardizedFileURL
        // Resolve existing ancestors individually: Foundation may otherwise retain a
        // symlink when the final output file does not exist yet.
        var url = base
        for component in path.components(separatedBy: "/") {
            url = url.appendingPathComponent(component).standardizedFileURL.resolvingSymlinksInPath()
            guard url.path == base.path || url.path.hasPrefix(base.path + "/") else {
                throw PortError.message("O caminho sai da pasta do projeto: \(path)")
            }
        }
        guard url.path == base.path || url.path.hasPrefix(base.path + "/") else { throw PortError.message("O caminho sai da pasta do projeto: \(path)") }
        return url
    }
    public static func hash(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url); defer { try? handle.close() }
        var hash = SHA256()
        while let chunk = try handle.read(upToCount: 1024 * 1024), !chunk.isEmpty { hash.update(data: chunk) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
    public static func nativeExecutable(_ app: URL) throws -> URL {
        guard app.pathExtension == "app", let bundle = Bundle(url: app), let executable = bundle.executableURL,
              FileManager.default.isExecutableFile(atPath: executable.path) else {
            throw PortError.message("O projeto ainda não gerou um aplicativo macOS válido.")
        }
        let output = try run("/usr/bin/file", ["-b", executable.path])
        guard output.contains("Mach-O"), output.contains("executable") else {
            throw PortError.message("A saída precisa ser um executável Mach-O para macOS. Scripts e atalhos de emuladores não são aceitos como port nativo.")
        }
        return executable
    }
    public func compile(project: URL) throws -> URL {
        let recipe = try RecompRecipe.load(project: project)
        guard !recipe.steps.isEmpty else {
            throw PortError.message("Este jogo ainda não tem uma receita de recompilação. Vamos implementar o port dele antes de compilar.")
        }
        // Validate the whole plan before running the first step.
        let app = try Self.contained(recipe.outputApp, in: project)
        for step in recipe.steps {
            guard step.executable.hasPrefix("/"), FileManager.default.isExecutableFile(atPath: step.executable) else {
                throw PortError.message("Ferramenta não encontrada: \(step.executable). Use o caminho absoluto do executável.")
            }
            _ = try Self.contained(step.directory, in: project)
        }
        if !recipe.input.isEmpty {
            let input = URL(fileURLWithPath: recipe.input, relativeTo: project)
            guard !recipe.inputSHA256.isEmpty else { throw PortError.message("Registre o SHA-256 da versão do jogo na receita antes de compilar.") }
            guard try Self.hash(input) == recipe.inputSHA256.lowercased() else {
                throw PortError.message("O arquivo do jogo não corresponde à versão esperada (SHA-256 diferente).")
            }
        }
        let log = project.appendingPathComponent("build.log")
        FileManager.default.createFile(atPath: log.path, contents: nil)
        let handle = try FileHandle(forWritingTo: log); defer { try? handle.close() }
        try handle.truncate(atOffset: 0)
        for step in recipe.steps {
            try handle.write(contentsOf: Data("\n[\(step.name)] \(step.executable) \(step.arguments.joined(separator: " "))\n".utf8))
            let process = Process(); process.executableURL = URL(fileURLWithPath: step.executable)
            process.arguments = step.arguments.map {
                $0.replacingOccurrences(of: "{project}", with: project.path)
                    .replacingOccurrences(of: "{input}", with: recipe.input.isEmpty ? "" : URL(fileURLWithPath: recipe.input, relativeTo: project).path)
            }
            process.currentDirectoryURL = try Self.contained(step.directory, in: project)
            process.standardOutput = handle; process.standardError = handle
            try process.run(); process.waitUntilExit()
            guard process.terminationStatus == 0 else {
                throw PortError.message("A etapa ‘\(step.name)’ falhou (\(process.terminationStatus)). Consulte build.log na pasta do projeto.")
            }
        }
        _ = try Self.nativeExecutable(app)
        // A successful build does not establish gameplay or controller compatibility.
        return app
    }
    public func export(project: URL, destination: URL) throws -> URL {
        let recipe = try RecompRecipe.load(project: project)
        let source = try Self.contained(recipe.outputApp, in: project)
        _ = try Self.nativeExecutable(source)
        guard !FileManager.default.fileExists(atPath: destination.path) else { throw PortError.message("Já existe um app nesse local. Escolha outro nome.") }
        let parent = destination.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: parent, withIntermediateDirectories: true)
        let staging = parent.appendingPathComponent(".port-\(UUID().uuidString).app")
        defer { try? FileManager.default.removeItem(at: staging) }
        try FileManager.default.copyItem(at: source, to: staging)
        // The per-title build must bundle and sign its own dependencies. Only the outer seal is renewed here.
        _ = try Self.run("/usr/bin/codesign", ["--force", "--sign", "-", staging.path])
        _ = try Self.run("/usr/bin/codesign", ["--verify", "--deep", "--strict", staging.path])
        try FileManager.default.moveItem(at: staging, to: destination)
        return destination
    }
    @discardableResult static func run(_ executable: String, _ arguments: [String]) throws -> String {
        let process = Process(); process.executableURL = URL(fileURLWithPath: executable); process.arguments = arguments
        let pipe = Pipe(); process.standardOutput = pipe; process.standardError = pipe
        try process.run(); let output = pipe.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
        let text = String(decoding: output, as: UTF8.self)
        guard process.terminationStatus == 0 else { throw PortError.message("\(executable): \(text)") }
        return text
    }
}
