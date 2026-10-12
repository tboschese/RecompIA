# rom2apk

Versões recompiladas e refeitas de jogos clássicos para Android: o código original é analisado e traduzido para código nativo, e a partir daí o jogo ganha melhorias que um emulador não consegue dar — movimento interpolado para telas de 90/120 Hz, camadas com profundidade e paralaxe, widescreen e áudio remasterizado.

O repositório guarda **só código e anotações**. ROMs e tudo que é extraído delas ficam fora do Git: cada projeto lê a sua própria cópia do jogo na hora de compilar.

## Estrutura

```
games/<jogo>/      um projeto por jogo: versão esperada da ROM, pontos de entrada, nomes de rotinas
tools/snesre/      engenharia reversa de SNES (desmontador 65816, descoberta de código)
tools/rom2apk/     o caminho rápido: empacota uma ROM com emulador em um APK
build/             saída das análises (ignorada pelo Git)
```

## Projeto atual: Chrono Trigger (USA)

Recompilação estática: o código 65816 do jogo vira C, que roda junto de uma implementação da PPU e do áudio do SNES. Etapas:

1. **Descoberta do código** — desmontagem a partir dos vetores da CPU, seguindo chamadas e desvios com rastreamento das flags M/X. *(feito: primeira passada)*
2. **Cobertura completa** — resolver tabelas de salto e trechos que rodam da RAM, com logs de execução (do nosso player ou CDL do Mesen 2) e anotações manuais em `game.json`. *(em andamento: tabelas de salto, rastreamento e código na RAM prontos; falta jogar e descomprimir o código que o jogo descompacta na RAM)*
3. **Recompilador** — cada função 65816 vira uma função C; o jogo roda nativamente e é comparado quadro a quadro com um emulador para garantir que se comporta igual.
4. **Base Android** — SDL2 + NDK, controles na tela e Bluetooth (os do rom2apk).
5. **Melhorias** — interpolação de posições de sprites e câmera entre os quadros lógicos, camadas renderizadas separadamente (paralaxe, profundidade, iluminação), widescreen, Mode 7 em alta resolução, áudio remasterizado.

```bash
export PYTHONPATH=tools/snesre

# Jogue: o player registra todo o código que roda e com quais flags (build/chrono-trigger/trace.bin).
# Cada sessão soma ao registro; o save do cartucho continua entre sessões. Precisa de SDL2 (brew install sdl2).
python3 -m snesre play games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"

# Analise: junta a desmontagem estática com o registro (e com qualquer .cdl do Mesen 2 em build/chrono-trigger/).
python3 -m snesre analyze games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"
# → build/chrono-trigger/disassembly.asm e analysis.json
```

O player é um núcleo snes9x com um rastreador nosso (`tools/snesre/tracer/`), compilado do código-fonte na primeira execução e guardado em `~/.snesre`. Controles: setas, Z/X/A/S = B/A/Y/X, Q/W = L/R, Enter = Start, Shift direito = Select, Tab = acelerar, F5/F7 = salvar/carregar estado; controles USB e Bluetooth funcionam no layout do SNES.

A ROM precisa ser exatamente a versão esperada (o SHA-1 está em `games/chrono-trigger/game.json`). Use apenas um dump do seu próprio cartucho.
