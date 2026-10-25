import XCTest
@testable import PortKit

final class PortKitTests: XCTestCase {
    var temp: URL!
    override func setUpWithError() throws {
        temp = FileManager.default.temporaryDirectory.appendingPathComponent("RecompIA tests \(UUID().uuidString)")
        try FileManager.default.createDirectory(at: temp, withIntermediateDirectories: true)
    }
    override func tearDownWithError() throws { try FileManager.default.removeItem(at: temp) }
    func fixture() throws -> (LibraryGame, URL, RecompRecipe) {
        let store = LibraryStore(root: temp)
        let game = try store.createProject(title: "Native build fixture", console: .switchConsole)
        let project = URL(fileURLWithPath: game.project)
        return (game, project, try RecompRecipe.load(project: project))
    }
    func nativeFixture() throws -> (LibraryGame, URL, RecompRecipe) {
        let (game, project, initial) = try fixture()
        var recipe = initial
        let app = project.appendingPathComponent(recipe.outputApp)
        let macos = app.appendingPathComponent("Contents/MacOS")
        try FileManager.default.createDirectory(at: macos, withIntermediateDirectories: true)
        let info: [String: Any] = ["CFBundleExecutable": "Game", "CFBundleIdentifier": "test.recompia.fixture", "CFBundlePackageType": "APPL"]
        try PropertyListSerialization.data(fromPropertyList: info, format: .xml, options: 0).write(to: app.appendingPathComponent("Contents/Info.plist"))
        let source = project.appendingPathComponent("main.c")
        try "int main(void) { return 0; }".write(to: source, atomically: true, encoding: .utf8)
        recipe.steps = [
            BuildStep(name: "Literal arguments", executable: "/usr/bin/printf", arguments: ["%s", "$(touch SHOULD_NOT_EXIST)"]),
            BuildStep(name: "Compile native fixture", executable: "/usr/bin/cc", arguments: [source.path, "-o", macos.appendingPathComponent("Game").path]),
        ]
        try recipe.save(project: project)
        return (game, project, recipe)
    }
    func testScopeContainsOnlyRequestedPlatformsAndStartsUnimplemented() throws {
        XCTAssertEqual(Console.allCases.map(\.rawValue), ["switch", "ps2", "ps3", "xbox360"])
        let (_, project, recipe) = try fixture()
        XCTAssertEqual(recipe.stage, .planning)
        XCTAssertTrue(recipe.steps.isEmpty)
        XCTAssertThrowsError(try PortBuilder().compile(project: project))
    }
    func testLibraryRoundTripAndReimportPreserveProject() throws {
        let (game, project, _) = try fixture()
        let store = LibraryStore(root: temp)
        var document = LibraryDocument(); document.games = [game]
        try store.save(document)
        XCTAssertEqual(try store.load().games, [game])
        XCTAssertEqual(try store.importProject(project).id, game.id)
        XCTAssertThrowsError(try store.createProject(title: "Overwrite", console: .ps2, destination: project))
        XCTAssertThrowsError(try store.createProject(title: "  ", console: .switchConsole))
    }
    func testCorruptAndFutureLibrariesAreNotReset() throws {
        let store = LibraryStore(root: temp)
        try Data("invalid json".utf8).write(to: temp.appendingPathComponent("library.json"))
        XCTAssertThrowsError(try store.load())
        var document = LibraryDocument(); document.version = 999
        try store.save(document)
        XCTAssertThrowsError(try store.load())
    }
    func testBuildExportAndRunNativeMachOWithoutShellExpansion() throws {
        let (_, project, recipe) = try nativeFixture()
        let app = try PortBuilder().compile(project: project)
        XCTAssertEqual(try RecompRecipe.load(project: project).stage, .planning, "Build success must not claim gameplay validation")
        XCTAssertTrue(try String(contentsOf: project.appendingPathComponent("build.log")).contains("$(touch SHOULD_NOT_EXIST)"))
        XCTAssertFalse(FileManager.default.fileExists(atPath: project.appendingPathComponent("SHOULD_NOT_EXIST").path))
        let executable = try PortBuilder.nativeExecutable(app)
        XCTAssertTrue(FileManager.default.isExecutableFile(atPath: executable.path))
        let destination = temp.appendingPathComponent("Exported Game.app")
        _ = try PortBuilder().export(project: project, destination: destination)
        let process = Process(); process.executableURL = try PortBuilder.nativeExecutable(destination)
        try process.run(); process.waitUntilExit(); XCTAssertEqual(process.terminationStatus, 0)
        XCTAssertThrowsError(try PortBuilder().export(project: project, destination: destination))
        XCTAssertEqual(recipe.console, .switchConsole)
    }
    func testInputVersionMismatchStopsBeforeCommandsRun() throws {
        let (_, project, initial) = try fixture()
        var recipe = initial
        let input = project.appendingPathComponent("original.nso")
        try Data("original".utf8).write(to: input)
        recipe.input = "original.nso"; recipe.inputSHA256 = try PortBuilder.hash(input)
        recipe.steps = [BuildStep(name: "Must not run", executable: "/usr/bin/touch", arguments: ["ran.txt"])]
        try Data("different version".utf8).write(to: input)
        try recipe.save(project: project)
        XCTAssertThrowsError(try PortBuilder().compile(project: project))
        XCTAssertFalse(FileManager.default.fileExists(atPath: project.appendingPathComponent("ran.txt").path))
    }
    func testPathTraversalAndSymlinkEscapeAreRejected() throws {
        let (_, project, _) = try fixture()
        XCTAssertThrowsError(try PortBuilder.contained("../outside.app", in: project))
        XCTAssertThrowsError(try PortBuilder.contained("/tmp/outside.app", in: project))
        try FileManager.default.createSymbolicLink(at: project.appendingPathComponent("escape"), withDestinationURL: temp)
        XCTAssertThrowsError(try PortBuilder.contained("escape/outside.app", in: project))
        XCTAssertEqual(try PortBuilder.contained(".", in: project).path, project.path)
    }
    func testMissingToolFailsBeforeEarlierCommands() throws {
        let (_, project, initial) = try fixture()
        var recipe = initial
        recipe.steps = [BuildStep(name: "First", executable: "/usr/bin/touch", arguments: ["ran.txt"]),
                        BuildStep(name: "Missing tool", executable: "/nonexistent/compiler", arguments: [])]
        try recipe.save(project: project)
        XCTAssertThrowsError(try PortBuilder().compile(project: project))
        XCTAssertFalse(FileManager.default.fileExists(atPath: project.appendingPathComponent("ran.txt").path))
    }
    func testShellLauncherDoesNotQualifyAsNativePort() throws {
        let app = temp.appendingPathComponent("Launcher.app")
        let macos = app.appendingPathComponent("Contents/MacOS")
        try FileManager.default.createDirectory(at: macos, withIntermediateDirectories: true)
        let script = macos.appendingPathComponent("Player")
        try "#!/bin/sh\nexit 0\n".write(to: script, atomically: true, encoding: .utf8)
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script.path)
        try PropertyListSerialization.data(fromPropertyList: ["CFBundleExecutable": "Player", "CFBundlePackageType": "APPL"], format: .xml, options: 0)
            .write(to: app.appendingPathComponent("Contents/Info.plist"))
        XCTAssertThrowsError(try PortBuilder.nativeExecutable(app))
    }
}
