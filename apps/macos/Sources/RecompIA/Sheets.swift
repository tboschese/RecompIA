import SwiftUI
import AppKit
import PortKit

struct NewProjectSheet: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) var dismiss
    @State var console: Console
    @State private var title = ""
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            Label("Novo projeto de recompilação", systemImage: "hammer").font(.title2.weight(.semibold))
            Form {
                TextField("Nome do jogo", text: $title)
                Picker("Plataforma", selection: $console) { ForEach(Console.allCases) { Text($0.name).tag($0) } }
            }.formStyle(.grouped).frame(height: 125)
            Text("O projeto começa em planejamento. Vamos implementar a análise, a recompilação e o runtime deste jogo antes de gerar um app jogável.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer(); Button("Cancelar") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Escolher pasta…") { dismiss(); model.add(title: title, console: console) }
                    .buttonStyle(.borderedProminent).keyboardShortcut(.defaultAction).disabled(title.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }.padding(28).frame(width: 490).preferredColorScheme(.dark)
    }
}

struct PipelineSheet: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) var dismiss
    let game: LibraryGame
    private var recipe: RecompRecipe? { model.recipes[game.id] }
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack { VStack(alignment: .leading, spacing: 5) {
                Text(game.title).font(.title2.weight(.semibold)); Text(game.console.architecture).font(.caption).foregroundStyle(.secondary)
            }; Spacer(); Button("Concluir") { dismiss() }.keyboardShortcut(.cancelAction) }
            Divider()
            HStack(spacing: 8) {
                ForEach(PortStage.allCases) { stage in
                    VStack(alignment: .leading, spacing: 7) {
                        Capsule().fill(stage == recipe?.stage ? Color.accentColor : .white.opacity(0.12)).frame(height: 3)
                        Text(stage.title).font(.system(size: 9)).foregroundStyle(stage == recipe?.stage ? .primary : .secondary)
                    }
                }
            }
            if let recipe {
                Text(recipe.notes).font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                Label(recipe.controllerSupport, systemImage: "gamecontroller").font(.caption)
                if recipe.steps.isEmpty {
                    Text("Receita ainda não implementada. A criação do projeto não recompila o jogo. Vamos desenvolver as etapas específicas deste título aqui no repositório.")
                        .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                } else {
                    Text("Etapas da receita").font(.headline)
                    ScrollView {
                        VStack(alignment: .leading, spacing: 13) {
                            ForEach(Array(recipe.steps.enumerated()), id: \.offset) { _, step in
                                VStack(alignment: .leading, spacing: 5) {
                                    Text(step.name).font(.caption.weight(.semibold))
                                    Text(([step.executable] + step.arguments).joined(separator: " ")).font(.system(size: 10, design: .monospaced)).textSelection(.enabled)
                                }
                            }
                        }.frame(maxWidth: .infinity, alignment: .leading)
                    }.frame(maxHeight: 160)
                    Text("Compilar executa as etapas acima. O projeto precisa fornecer um .app nativo completo, com suas bibliotecas. Uma compilação concluída não marca a jogabilidade como validada.")
                        .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
                HStack {
                    Button("Abrir receita") { NSWorkspace.shared.open(URL(fileURLWithPath: game.project).appendingPathComponent("recomp.json")) }
                    Button("Abrir pasta") { NSWorkspace.shared.open(URL(fileURLWithPath: game.project)) }
                    Spacer(); Button("Compilar port") { model.compile(game); dismiss() }.buttonStyle(.borderedProminent).disabled(recipe.steps.isEmpty || model.busy != nil)
                }
            } else { Text("O projeto não foi encontrado ou a receita está inválida.").foregroundStyle(.orange) }
        }.padding(28).frame(width: 660).preferredColorScheme(.dark)
    }
}

struct InfoSheet: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) var dismiss
    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            HStack { Text("Ports nativos, jogo por jogo").font(.title2.weight(.semibold)); Spacer(); Button("Concluir") { dismiss() }.keyboardShortcut(.cancelAction) }
            Text("Switch é a primeira plataforma. PS2, PS3 e Xbox 360 ficam no escopo. Cada port precisará de um recompilador, tradução gráfica, áudio, arquivos e controles adequados ao jogo.")
                .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Text("Estado atual: biblioteca e execução de receitas de build. Não há recompiladores de consoles integrados nem jogos comerciais validados nesta versão.")
                .font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
            Divider()
            Label("Controle no menu", systemImage: "gamecontroller").font(.headline)
            Text(model.controllerNames.isEmpty ? "Conecte um controle reconhecido pelo macOS. O direcional seleciona projetos e A/sul abre uma build já existente." : model.controllerNames.joined(separator: ", "))
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Text("O suporte a controle dentro dos jogos será implementado e testado no runtime de cada port.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }.padding(28).frame(width: 570).preferredColorScheme(.dark)
    }
}
