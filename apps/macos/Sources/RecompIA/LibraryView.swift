import SwiftUI
import AppKit
import PortKit

private let ink = Color(red: 0.07, green: 0.075, blue: 0.10)
private let panel = Color(red: 0.105, green: 0.11, blue: 0.145)
private let accent = Color(red: 0.66, green: 0.56, blue: 1)

struct LibraryView: View {
    @EnvironmentObject var model: AppModel
    @State private var category = "switch"
    @State private var search = ""
    @State private var selected: String?
    @State private var newProject = false
    @State private var showPipeline = false
    @State private var showInfo = false
    private var games: [LibraryGame] {
        model.document.games.filter {
            (category == "all" || category == "favorites" && $0.favorite || $0.console.rawValue == category)
                && (search.isEmpty || $0.title.localizedCaseInsensitiveContains(search))
        }.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }
    private var current: LibraryGame? { games.first { $0.id == selected } }
    private var title: String { category == "all" ? "Todos os ports" : category == "favorites" ? "Favoritos" : Console(rawValue: category)?.name ?? "Ports" }
    var body: some View {
        HStack(spacing: 0) {
            sidebar
            VStack(spacing: 0) {
                header
                ScrollViewReader { reader in
                    ScrollView {
                        VStack(alignment: .leading, spacing: 28) {
                            welcome
                            HStack {
                                Text(title).font(.title2.weight(.semibold))
                                Text("\(games.count)").font(.caption.monospacedDigit()).padding(.horizontal, 9).padding(.vertical, 4).background(.white.opacity(0.07), in: Capsule())
                                Spacer()
                                Text("RECOMPILAÇÃO NATIVA").font(.system(size: 9, design: .monospaced)).tracking(1.2).foregroundStyle(.secondary)
                            }
                            if games.isEmpty { emptyState }
                            else {
                                LazyVGrid(columns: [GridItem(.adaptive(minimum: 165, maximum: 210), spacing: 22)], alignment: .leading, spacing: 25) {
                                    ForEach(games) { game in
                                        card(game).onTapGesture { selected = game.id }
                                            .contextMenu {
                                                Button("Ver pipeline") { selected = game.id; showPipeline = true }
                                                Button("Abrir pasta do projeto") { NSWorkspace.shared.open(URL(fileURLWithPath: game.project)) }
                                                Button("Jogar build atual") { model.play(game) }
                                                Button("Exportar app…") { model.export(game) }
                                                Button(game.favorite ? "Remover dos favoritos" : "Adicionar aos favoritos") { favorite(game) }
                                                Button("Escolher capa…") { model.addCover(game) }
                                                Divider(); Button("Remover da lista", role: .destructive) { model.remove(game) }
                                            }.id(game.id)
                                    }
                                }
                            }
                            HStack(alignment: .top, spacing: 12) {
                                Image(systemName: "hammer").foregroundStyle(accent)
                                VStack(alignment: .leading, spacing: 5) {
                                    Text("Cada jogo tem seu próprio projeto").font(.caption.weight(.semibold))
                                    Text("Análise do executável → recompilação → runtime para Mac → testes. A biblioteca acompanha o trabalho e compila receitas já implementadas. Os recompiladores ainda não estão integrados.")
                                        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                                }
                            }.padding(18).frame(maxWidth: .infinity, alignment: .leading).background(panel, in: RoundedRectangle(cornerRadius: 10))
                        }.padding(32)
                    }.onChange(of: selected) { value in if let value { withAnimation { reader.scrollTo(value, anchor: .center) } } }
                }
                if let current { selectionBar(current) }
                footer
            }.background(ink)
        }.preferredColorScheme(.dark).tint(accent).frame(minWidth: 960, minHeight: 690)
            .sheet(isPresented: $newProject) { NewProjectSheet(console: Console(rawValue: category) ?? .switchConsole).environmentObject(model) }
            .sheet(isPresented: $showPipeline) { if let current { PipelineSheet(game: current).environmentObject(model) } }
            .sheet(isPresented: $showInfo) { InfoSheet().environmentObject(model) }
            .alert("RecompIA", isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) {
                Button("OK") { model.error = nil }
            } message: { Text(model.error ?? "") }
            .onChange(of: category) { _ in selected = nil }
            .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in model.refresh() }
            .onAppear {
                model.controllerMove = { offset in
                    guard !newProject, !showPipeline, !showInfo, model.error == nil, model.busy == nil, !games.isEmpty else { return }
                    let index = games.firstIndex { $0.id == selected } ?? (offset > 0 ? -1 : games.count)
                    selected = games[min(max(index + offset, 0), games.count - 1)].id
                }
                model.controllerPlay = {
                    guard !newProject, !showPipeline, !showInfo, model.error == nil, let current else { return }
                    model.play(current)
                }
            }
    }
    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 11) {
                Image(systemName: "square.stack.3d.up.fill").font(.title2).foregroundStyle(accent)
                VStack(alignment: .leading, spacing: 3) {
                    Text("RecompIA").font(.headline)
                    Text("RECOMP PARA MAC").font(.system(size: 9, weight: .medium, design: .monospaced)).tracking(1.6).foregroundStyle(.secondary)
                }
            }.padding(.top, 40).padding(.bottom, 30).padding(.horizontal, 22)
            VStack(alignment: .leading, spacing: 5) {
                sectionLabel("BIBLIOTECA")
                row("Todos os ports", icon: "square.grid.2x2", key: "all")
                row("Favoritos", icon: "heart", key: "favorites")
                sectionLabel("PLATAFORMAS").padding(.top, 25)
                ForEach(Console.allCases) { console in row(console.name, icon: "gamecontroller", key: console.rawValue) }
                Text("PRIMEIRO ALVO").font(.system(size: 9, design: .monospaced)).tracking(1.5).foregroundStyle(.secondary).padding(.top, 26).padding(.horizontal, 12)
                Label("Nintendo Switch", systemImage: "scope").font(.caption).foregroundStyle(accent).padding(12)
            }.padding(.horizontal, 12)
            Spacer()
            Button { model.importProject() } label: { Label("Abrir projeto existente…", systemImage: "folder").font(.caption).padding(.horizontal, 22).padding(.vertical, 12) }.buttonStyle(.plain).disabled(model.busy != nil)
            Divider().padding(.top, 8)
            Button { showInfo = true } label: { Label("Sobre os ports e controles", systemImage: "info.circle").font(.caption).foregroundStyle(.secondary).padding(20) }.buttonStyle(.plain)
        }.frame(width: 235).background(panel)
    }
    private func sectionLabel(_ text: String) -> some View {
        Text(text).font(.system(size: 9, weight: .semibold, design: .monospaced)).tracking(1.6).foregroundStyle(.secondary).padding(.horizontal, 12).padding(.bottom, 8)
    }
    private func row(_ text: String, icon: String, key: String) -> some View {
        Button { category = key } label: {
            HStack(spacing: 10) { Image(systemName: icon).frame(width: 18).foregroundStyle(category == key ? accent : .secondary); Text(text).font(.system(size: 12)); Spacer() }
                .padding(.horizontal, 12).padding(.vertical, 11).background(category == key ? accent.opacity(0.13) : .clear, in: RoundedRectangle(cornerRadius: 7))
        }.buttonStyle(.plain)
    }
    private var header: some View {
        HStack(spacing: 16) {
            HStack(spacing: 8) { Image(systemName: "magnifyingglass").foregroundStyle(.secondary); TextField("Buscar nos projetos", text: $search).textFieldStyle(.plain) }
                .font(.system(size: 12)).padding(11).frame(maxWidth: 300).background(.white.opacity(0.045), in: RoundedRectangle(cornerRadius: 8))
            Spacer()
            Label(model.controllerNames.isEmpty ? "Sem controle" : "\(model.controllerNames.count) controle(s)", systemImage: "gamecontroller")
                .font(.caption).foregroundStyle(model.controllerNames.isEmpty ? Color.secondary : accent).help(model.controllerNames.joined(separator: ", "))
            Button { newProject = true } label: { Label("Novo port", systemImage: "plus").font(.system(size: 12, weight: .semibold)).padding(.vertical, 5) }
                .buttonStyle(.borderedProminent).disabled(model.busy != nil).keyboardShortcut("n", modifiers: .command)
        }.padding(.horizontal, 32).padding(.top, 32).padding(.bottom, 20)
    }
    private var welcome: some View {
        HStack {
            VStack(alignment: .leading, spacing: 14) {
                Text("UM JOGO DE CADA VEZ").font(.system(size: 9, weight: .medium, design: .monospaced)).tracking(2).foregroundStyle(accent)
                Text("Do console\npara o seu Mac.").font(.system(size: 34, weight: .semibold, design: .rounded)).tracking(-0.8)
                Text("Um projeto para cada jogo. Código nativo. Uma biblioteca para todos.").font(.system(size: 12)).foregroundStyle(.secondary)
                HStack(spacing: 8) { badge("Switch primeiro"); badge("PS2 · PS3 · Xbox 360") }.padding(.top, 4)
            }
            Spacer(minLength: 8)
            ZStack {
                RoundedRectangle(cornerRadius: 28).fill(accent.opacity(0.04)).frame(width: 160, height: 160).rotationEffect(.degrees(-12))
                RoundedRectangle(cornerRadius: 25).stroke(accent.opacity(0.13), lineWidth: 1).frame(width: 140, height: 140).rotationEffect(.degrees(8))
                Image(systemName: "gamecontroller.fill").font(.system(size: 68, weight: .light)).foregroundStyle(accent.opacity(0.85)).rotationEffect(.degrees(-12))
                Image(systemName: "sparkle").font(.title3).foregroundStyle(accent).offset(x: 65, y: -58)
            }.frame(width: 180)
        }.padding(28).frame(maxWidth: .infinity, alignment: .leading)
            .background(LinearGradient(colors: [Color(red: 0.14, green: 0.12, blue: 0.22), panel.opacity(0.5)], startPoint: .topLeading, endPoint: .bottomTrailing), in: RoundedRectangle(cornerRadius: 16))
            .overlay(RoundedRectangle(cornerRadius: 16).stroke(.white.opacity(0.055)))
    }
    private func badge(_ text: String) -> some View { Text(text).font(.system(size: 9)).foregroundStyle(.secondary).padding(.horizontal, 9).padding(.vertical, 6).background(.white.opacity(0.04), in: Capsule()) }
    private var emptyState: some View {
        VStack(spacing: 13) {
            Image(systemName: "hammer.circle").font(.system(size: 36, weight: .ultraLight)).foregroundStyle(accent.opacity(0.65))
            Text(search.isEmpty ? "O primeiro port começa com um jogo" : "Nenhum projeto encontrado").font(.headline)
            Text(search.isEmpty ? "Crie um projeto para acompanharmos a análise,\na recompilação e os testes no Mac." : "Experimente buscar outro nome.")
                .font(.caption).foregroundStyle(.secondary).multilineTextAlignment(.center).lineSpacing(4)
            if search.isEmpty { Button("Criar projeto…") { newProject = true }.buttonStyle(.bordered).padding(.top, 4).disabled(model.busy != nil) }
        }.frame(maxWidth: .infinity).padding(.vertical, 38)
            .background(RoundedRectangle(cornerRadius: 13).strokeBorder(.white.opacity(0.1), style: StrokeStyle(lineWidth: 1, dash: [5, 5])))
    }
    private func card(_ game: LibraryGame) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            ZStack(alignment: .bottomLeading) {
                if let cover = game.cover, let image = NSImage(contentsOf: model.store.root.appendingPathComponent(cover)) {
                    GeometryReader { proxy in Image(nsImage: image).resizable().scaledToFill().frame(width: proxy.size.width, height: proxy.size.height).clipped() }
                } else {
                    LinearGradient(colors: [accent.opacity(0.24), panel], startPoint: .topLeading, endPoint: .bottomTrailing)
                    VStack(alignment: .leading, spacing: 12) {
                        Text(game.console.shortName).font(.system(size: 9, weight: .medium, design: .monospaced)).tracking(2).foregroundStyle(accent)
                        Spacer(); Image(systemName: "gamecontroller").font(.system(size: 38, weight: .ultraLight)).foregroundStyle(.white.opacity(0.4))
                        Text(game.title).font(.system(size: 21, weight: .bold, design: .rounded)).lineLimit(4)
                        Rectangle().fill(accent.opacity(0.5)).frame(width: 24, height: 2)
                    }.padding(19)
                }
                if game.favorite { Image(systemName: "heart.fill").font(.caption).padding(10).background(.black.opacity(0.5), in: Circle()).padding(9) }
            }.frame(height: 218).clipShape(RoundedRectangle(cornerRadius: 10)).overlay(RoundedRectangle(cornerRadius: 10).stroke(selected == game.id ? accent : .white.opacity(0.07), lineWidth: selected == game.id ? 2 : 1))
            Text(game.title).font(.system(size: 12, weight: .medium)).lineLimit(1)
            Text(model.recipes[game.id]?.stage.title ?? "Projeto indisponível").font(.system(size: 10)).foregroundStyle(.secondary)
        }.contentShape(Rectangle()).accessibilityElement(children: .combine)
    }
    private func selectionBar(_ game: LibraryGame) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(game.title).font(.headline).lineLimit(1)
                Text(model.recipes[game.id]?.stage.title ?? "Projeto indisponível").font(.caption).foregroundStyle(.secondary)
            }
            Spacer(); Button { favorite(game) } label: { Image(systemName: game.favorite ? "heart.fill" : "heart") }.buttonStyle(.borderless)
            Button("Pipeline…") { showPipeline = true }.buttonStyle(.bordered)
            Button("Exportar…") { model.export(game) }.buttonStyle(.bordered)
            Button { model.play(game) } label: { Label("Jogar build", systemImage: "play.fill") }.buttonStyle(.borderedProminent).keyboardShortcut(.return, modifiers: [])
        }.padding(.horizontal, 30).padding(.vertical, 18).background(panel).disabled(model.busy != nil)
    }
    private var footer: some View {
        HStack(spacing: 8) {
            if model.busy != nil { ProgressView().controlSize(.small) } else { Circle().fill(accent.opacity(0.7)).frame(width: 5, height: 5) }
            Text(model.busy ?? model.status).lineLimit(1); Spacer(); Text("macOS  •  Projetos locais").foregroundStyle(.tertiary)
        }.font(.system(size: 10)).foregroundStyle(.secondary).padding(.horizontal, 30).padding(.vertical, 12)
    }
    private func favorite(_ game: LibraryGame) { var value = game; value.favorite.toggle(); model.update(value) }
}
