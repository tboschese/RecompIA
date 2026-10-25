# RecompIA · recompilação nativa para Mac

Ferramenta para desenvolver ports nativos **jogo por jogo**, com biblioteca em SwiftUI inspirada no OpenEmu. O escopo é **Nintendo Switch, PlayStation 2, PlayStation 3 e Xbox 360**. **Switch é a primeira plataforma.**

O produto mudou de direção: o trabalho de SNES, remasterização e Android fica preservado como legado, fora do fluxo da nova interface.

## Estado real

A base da ferramenta já cria e abre projetos por jogo, mostra seu andamento, executa receitas locais de compilação com logs e exporta aplicativos macOS já gerados pelo projeto. A interface reconhece controles pelo framework GameController: direcional seleciona projetos e o botão A/sul abre uma build existente.

**Ainda não há recompiladores de consoles integrados nem jogos comerciais validados.** Criar um projeto não converte o jogo. Cada título precisará de análise do executável, integração da recompilação, runtime de gráficos/áudio/arquivos e testes. O controle dentro de cada jogo também será implementado e testado no respectivo runtime.

## Compilar e abrir

Requisitos para compilar a ferramenta: macOS 13+, Command Line Tools/Xcode com Swift 5.9+ e Python 3. A ferramenta é compilada para a arquitetura do Mac utilizado, sem binário Universal neste momento.

```bash
PYTHONPATH=tools/recompia python3 -m recompia build
open dist/RecompIA.app
```

O aplicativo gerado não depende de Python ou de emuladores para abrir. Cada port terá seus próprios requisitos de desenvolvimento.

## Fluxo por jogo

1. **Novo port**: informe o nome e a plataforma e escolha uma pasta para o projeto.
2. O projeto começa em **Planejamento**, com um arquivo `recomp.json` sem etapas executáveis.
3. Desenvolvemos aqui a análise e o código do port e registramos na receita os comandos de recompilação e build.
4. **Pipeline → Compilar port** executa esses comandos em segundo plano e grava `build.log` na pasta do projeto.
5. A saída deve ser um `.app` com executável Mach-O para macOS. A receita deve empacotar e assinar as bibliotecas do runtime.
6. Testamos a execução e os controles no Mac. Uma compilação concluída não marca automaticamente o jogo como jogável.
7. **Exportar** copia o app gerado, renova a assinatura local e verifica o bundle. Destinos existentes são preservados.

**Abrir projeto existente** adiciona uma pasta que já contém `recomp.json`. Os projetos são referenciados no lugar original, sem copiar imagens de jogos. Remover da biblioteca preserva a pasta e seus arquivos.

## Receita

O arquivo gerado contém a plataforma, o andamento, o arquivo original e seu SHA-256 opcional, os comandos locais, a saída e as notas do port. Quando `input` é preenchido, `inputSHA256` é obrigatório e é conferido antes de executar comandos, para evitar aplicar o trabalho à versão errada do jogo.

Exemplo de uma etapa para **um projeto CMake já implementado**:

```json
{
  "name": "Configurar o projeto nativo",
  "executable": "/caminho/absoluto/para/cmake",
  "arguments": ["-S", "{project}", "-B", "{project}/build", "-DCMAKE_BUILD_TYPE=Release"],
  "directory": "."
}
```

Cada etapa tem `name`, `executable`, `arguments` e `directory`. O executável deve ter caminho absoluto; os argumentos são passados diretamente ao processo. `{project}` e `{input}` são substituídos sem shell. A pasta de trabalho e `outputApp` devem ficar dentro do projeto. A interface mostra as etapas antes de executar **Compilar port**. Só abra e compile projetos cujo código você conhece, como faria com qualquer repositório de desenvolvimento.

O schema inicial utiliza `version: 1`, uma plataforma entre `switch`, `ps2`, `ps3`, `xbox360`, e um estágio entre `planning`, `analysis`, `recompilation`, `runtime`, `validation`, `playable`. O estágio é uma anotação do trabalho, não uma medição automática de compatibilidade. Projetos criados pela interface usam `outputApp: "build/Game.app"`; adapte ao sistema de build real do título.

## Bases a avaliar por título

Estas são referências de pesquisa, **não integrações instaladas ou garantias de execução no Mac**:

| Plataforma | Referência |
| --- | --- |
| Switch | [SwitchRecomp](https://github.com/kaiyao7/SwitchRecomp), framework de pesquisa AArch64 → IR → C |
| PS2 | [PS2Recomp](https://github.com/ran-j/PS2Recomp), recompilador estático e runtime |
| PS3 | [ps3recomp](https://github.com/sp00nznet/ps3recomp), ferramentas e runtime específicos de PS3 |
| Xbox 360 | [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), recompilação de PowerPC para C++ |

Traduzir a CPU não resolve sozinho os gráficos, os serviços do console ou o comportamento específico do título. A escolha do backend e as adaptações para macOS serão feitas depois de definir o jogo e a versão de origem.

## Dados e desenvolvimento

Metadados e capas da biblioteca ficam em `~/Library/Application Support/RecompIA/`. Código, receitas e logs ficam nas pastas dos projetos escolhidas pelo usuário. A assinatura dos apps é ad hoc/local; distribuição pública e notarização ainda não estão configuradas.

```bash
swift build --package-path apps/macos
swift test --package-path apps/macos
```

Os testes usam um programa C mínimo criado no próprio teste para conferir compilação, validação Mach-O, exportação e execução. Esse programa não é um jogo de console nem prova de suporte a recompilação de jogos.

Use `RECOMPIA_LIBRARY=/caminho/isolado` para separar os dados durante o desenvolvimento da interface.

```text
apps/macos/        interface SwiftUI e execução de receitas por jogo
engine/            motor SNES legado, preservado
tools/recompia/     compilação da ferramenta RecompIA
tools/remasteria/   comandos legados de remasterização
tools/snesre/      análise SNES legada
tools/rom2apk/     experimento Android legado
games/             projetos anteriores, preservados
build/, dist/      artefatos locais, fora do Git
```

A [documentação anterior](engine/README.md) descreve o motor e os experimentos antigos. Os comandos legados continuam disponíveis para não apagar o trabalho existente; a nova interface fica restrita às quatro plataformas definidas acima.
