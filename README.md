# RemakeIA

Versões recompiladas e refeitas de jogos clássicos: o código original é analisado e traduzido para código nativo, e a partir daí o jogo ganha melhorias que um emulador não consegue dar — movimento interpolado para telas de 90/120 Hz, camadas com profundidade e paralaxe, widescreen e áudio remasterizado. Os jogos saem primeiro para macOS (para testar rápido) e depois para Android.

O repositório guarda **só código e anotações**. ROMs e tudo que é extraído delas ficam fora do Git: cada projeto lê a sua própria cópia do jogo na hora de compilar.

## Estrutura

```
games/<jogo>/      um projeto por jogo: versão esperada da ROM, opções do app, pontos de entrada, nomes de rotinas
engine/            o motor (C + SDL2) que roda os jogos: macOS hoje, Android depois
tools/remakeia/    gera os aplicativos (macOS: .app)
tools/snesre/      engenharia reversa de SNES (desmontador 65816, rastreamento de execução)
tools/rom2apk/     o caminho rápido: empacota uma ROM com emulador em um APK
build/, dist/      saída das análises e aplicativos gerados (ignorados pelo Git)
```

## Jogar no Mac

```bash
brew install sdl2   # uma vez
PYTHONPATH=tools/remakeia python3 -m remakeia mac games/chrono-trigger "Chrono Trigger (USA).sfc"
open "dist/Chrono Trigger.app"
```

O app é independente (motor, núcleo, SDL e a sua ROM dentro dele). Ele continua de onde você parou e guarda o save do cartucho em `~/Library/Application Support/RemakeIA/<jogo>/`, junto com o registro de execução — então **jogar o app também alimenta a análise** do jogo.

Controles: setas, Z/X/A/S = B/A/Y/X, Q/W = L/R, Enter = Start, Shift direito = Select, Tab (segurar) = acelerar, F5/F7 = salvar/carregar estado, F11 ou Cmd+F = tela cheia. Controles USB e Bluetooth funcionam no layout do SNES.

## Projeto atual: Chrono Trigger (USA)

Recompilação estática: o código 65816 do jogo vira C, que roda junto de uma implementação da PPU e do áudio do SNES. Hoje o motor ainda usa o snes9x como hardware do SNES; à medida que o recompilador avança, as funções do jogo passam a rodar nativas no lugar da CPU emulada. Etapas:

1. **Descoberta do código** — desmontagem a partir dos vetores da CPU, seguindo chamadas e desvios com rastreamento das flags M/X. *(feito)*
2. **Cobertura completa** — tabelas de salto, código na RAM e registros de execução (do nosso motor ou CDL do Mesen 2), mais anotações manuais em `game.json`. *(em andamento: falta jogar e descomprimir o código que o jogo descompacta na RAM)*
3. **Recompilador** — cada função 65816 vira uma função C; o jogo roda nativamente e é comparado quadro a quadro com um emulador para garantir que se comporta igual.
4. **Plataformas** — macOS *(feito: app com HD Mode 7, escala nítida, tela cheia, controles)*; Android com SDL2 + NDK, controles na tela e Bluetooth (os do rom2apk).
5. **Melhorias** — interpolação de posições de sprites e câmera entre os quadros lógicos, camadas renderizadas separadamente (paralaxe, profundidade, iluminação), widescreen, áudio remasterizado.

```bash
export PYTHONPATH=tools/snesre

# Jogar sem gerar o app (mesmo motor; registro em build/chrono-trigger/trace.bin)
python3 -m snesre play games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"

# Analisar: junta a desmontagem estática com os registros de execução (do app, do play e .cdl do Mesen 2)
python3 -m snesre analyze games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"
# → build/chrono-trigger/disassembly.asm e analysis.json
```

O núcleo snes9x com o rastreador (`tools/snesre/tracer/`) e o motor são compilados do código-fonte na primeira execução e guardados em `~/.snesre`.

A ROM precisa ser exatamente a versão esperada (o SHA-1 está em `games/chrono-trigger/game.json`). Use apenas um dump do seu próprio cartucho.
