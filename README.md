# RemakeIA

Versões refeitas de jogos clássicos para telas modernas, com qualidade acima de um emulador. O código original é analisado e recompilado para código nativo, e a partir daí o jogo ganha:
- **pixel perfect** em qualquer tela;
- **movimento fluido a 90/120 Hz**, interpolando sprites e câmera entre os quadros do jogo;
- **profundidade**: camadas com paralaxe e iluminação.

Os jogos saem primeiro para **macOS**, para testar rápido, e depois para **Android**.

O repositório guarda **só código e anotações**. ROMs e tudo que é extraído delas ficam fora do Git: cada projeto lê a sua própria cópia do jogo na hora de compilar.

## Estrutura

```
games/<jogo>/      um projeto por jogo: versão esperada da ROM, opções do app, pontos de entrada, nomes de rotinas
engine/            o motor (C + SDL2 + OpenGL) que roda os jogos: macOS hoje, Android depois
engine/shaders/    visuais: pixel perfect (padrão) e CRT
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

O app é independente: leva dentro dele o motor, o núcleo, o SDL, a base de controles e a sua ROM. Ele continua de onde você parou e guarda os saves e os controles em `~/Library/Application Support/RemakeIA/<jogo>/`. O núcleo e o motor são compilados do código-fonte na primeira vez e ficam guardados em `~/.snesre`.

**Qualidade — Chrono Trigger 0.4:**
- **Pixel perfect:** cada pixel do jogo vira um bloco exato de pixels da tela, sempre na maior escala inteira que cabe na janela, sem nenhuma suavização.
- **Mode 7 em HD:** renderização nativa 4x com correção de perspectiva (bsnes-hd).
- **Sem limite de sprites:** nada pisca.
- **Movimento liso:** o jogo é travado na taxa da tela com vsync (60 Hz, ou 120 Hz no ProMotion), sem engasgos nem cortes na imagem. O áudio se ajusta em até 0,5% para acompanhar, sem estalos.
- **Áudio com interpolação cúbica:** mais agudos que o som abafado do console.

**Todo jogo já nasce com:**
- **Controles USB e Bluetooth nativos:** é só conectar, inclusive com o jogo aberto. Os controles conhecidos (a base do SDL mais a [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB), com milhares de modelos) já entram no layout do SNES. Qualquer outro funciona depois de mapeado no menu. O segundo controle é o jogador 2.
- **Teclado** com todas as teclas remapeáveis.
- **Menu de pausa** (Esc, botão Home do controle ou Select+Start) com: continuar, salvar estado, carregar estado, controles, visual (pixel perfect ou CRT), tela cheia e sair.
- **8 slots de save state**, cada um com uma miniatura da cena, data e hora. Além disso, o jogo salva sozinho ao fechar e continua dali ao abrir.

Teclado padrão: setas, Z/X/A/S = B/A/Y/X, Q/W = L/R, Enter = Start, Shift direito = Select. Atalhos: Esc = menu, Tab (segurar) = acelerar, F2 = visual, F5/F7 = salvar/carregar no slot atual, F11 ou Cmd+F = tela cheia.

## Projeto atual: Chrono Trigger (USA)

1. **Descoberta do código** — desmontagem a partir dos vetores da CPU, seguindo chamadas e desvios com rastreamento das flags M/X. *(feito)*
2. **Cobertura completa** — tabelas de salto, código na RAM e registros de execução (do nosso motor ou CDL do Mesen 2), mais anotações manuais em `game.json`. *(em andamento: falta jogar e descomprimir o código que o jogo descompacta na RAM)*
3. **Recompilador** — cada função 65816 vira uma função C. O jogo roda nativamente e é comparado quadro a quadro com um emulador, para garantir que se comporta igual.
4. **Renderizador próprio** — o motor desenha a cena a partir do estado gráfico do jogo (tiles, sprites, rolagem e paleta), não da imagem pronta do console. É isso que permite:
   - interpolar posições para 90/120 Hz;
   - separar camadas para profundidade e paralaxe.
5. **Plataformas** — macOS *(feito: app pixel perfect com HD Mode 7 e vsync)*; Android com SDL2 + NDK, controles na tela e Bluetooth (os do rom2apk).

Para mapear o jogo, jogue pelo motor com rastreamento e depois rode a análise:

```bash
export PYTHONPATH=tools/snesre

# Jogar registrando o código que roda (snes9x com rastreador; registro em build/chrono-trigger/trace.bin)
python3 -m snesre play games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"

# Analisar: junta a desmontagem estática com os registros de execução (play e .cdl do Mesen 2)
python3 -m snesre analyze games/chrono-trigger/game.json "Chrono Trigger (USA).sfc"
# → build/chrono-trigger/disassembly.asm e analysis.json
```

A ROM precisa ser exatamente a versão esperada (o SHA-1 está em `games/chrono-trigger/game.json`). Use apenas um dump do seu próprio cartucho.
