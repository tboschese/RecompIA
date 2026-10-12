# rom2apk

Transforma ROMs de videogame em APKs Android que abrem direto no jogo, com imagem melhorada, controles na tela e suporte a controles Bluetooth/USB.

```bash
python3 -m rom2apk build MeuJogo.sfc
# → dist/MeuJogo.apk
```

## Consoles suportados

| Console | Extensões | Núcleo libretro | Melhorias de imagem |
| --- | --- | --- | --- |
| NES | `.nes` `.fds` `.unf` | fceumm | shader HD, sem limite de sprites (menos piscadas) |
| Super Nintendo | `.sfc` `.smc` | snes9x | shader HD |
| Game Boy / Color | `.gb` `.gbc` | gambatte | shader HD, cores automáticas em jogos de GB |
| Game Boy Advance | `.gba` | mgba | shader HD |
| Mega Drive, Master System, Game Gear | `.md` `.gen` `.smd` `.bin` `.sms` `.gg` | genesis_plus_gx | shader HD, sem limite de sprites |
| PlayStation | `.cue` `.chd` `.pbp` `.exe` | pcsx_rearmed | resolução interna 2x |
| Nintendo 64 | `.n64` `.z64` `.v64` | mupen64plus_next_gles3 | resolução interna até 4x (1280×960) |

ROMs dentro de `.zip` também funcionam. Para PS1 com várias faixas, passe o `.cue`; as faixas `.bin` são incluídas automaticamente.

## Requisitos

- Python 3.9+
- JDK 17+
- Android SDK com `platforms;android-35` e `build-tools;35+` (o Android Studio já instala)

Confira o ambiente com:

```bash
python3 -m rom2apk doctor
```

## Uso

```bash
# Uma ROM
python3 -m rom2apk build Jogo.gba --name "Meu Jogo" --icon capa.png

# Uma pasta inteira (inclusive subpastas), um APK por ROM
python3 -m rom2apk build ~/roms --out ~/apks

# N64 em 4x e PS1 com filtro de TV de tubo
python3 -m rom2apk build Jogo.z64 --scale 4
python3 -m rom2apk build Jogo.cue --video crt --bios scph1001.bin
```

| Opção | Descrição |
| --- | --- |
| `--video hd\|sharp\|crt\|lcd\|original` | perfil de imagem inicial (padrão `hd`); o jogador pode trocar no menu |
| `--scale 1-4` | resolução interna para PS1 (até 2x) e N64 (até 4x) |
| `--console` | força o console quando a extensão é ambígua |
| `--name`, `--icon`, `--package` | nome, ícone e identificador do app (só com uma ROM) |
| `--bios` | inclui um arquivo de BIOS (opcional para PS1) |
| `--abi` | arquitetura alvo; padrão `arm64-v8a` (repita para incluir `armeabi-v7a` em aparelhos antigos) |
| `--version-code` | aumente ao gerar uma nova versão de um APK já instalado |
| `--keystore …` | assina com a sua própria chave em vez da chave gerada automaticamente |

`python3 -m rom2apk consoles` lista os consoles e `python3 -m rom2apk build --help` mostra todas as opções.

## O que vem no APK

- **Controles na tela** com layout de cada console (analógico e botões C no N64, △○✕□ no PS1), multitoque e deslizar entre botões.
- **Controles físicos** Bluetooth/USB (Xbox, PlayStation, 8BitDo…), com vários jogadores, gatilhos analógicos e o analógico esquerdo funcionando como direcional em consoles 2D. Os controles da tela somem quando um controle é conectado.
- **Menu** (botão ☰, voltar ou botão central do controle): 3 slots de save state, troca de filtro de imagem, controles na tela automático/sempre/nunca, reiniciar e sair.
- **Saves do jogo** (bateria/memory card) gravados automaticamente.

## Como funciona

```
rom2apk/      CLI em Python: detecta o console, baixa o núcleo e monta o APK
template/     app Android (Kotlin + LibretroDroid) usado como base de todo APK
```

O CLI copia `template/` para `~/.rom2apk/workspace`, coloca a ROM em `assets/game/`, o núcleo do console em `jniLibs/`, grava `assets/game.json` com nome, console, filtro e opções do núcleo, e roda o Gradle. O primeiro build demora alguns minutos; os seguintes levam poucos segundos.

Os núcleos são baixados do [buildbot oficial do libretro](https://buildbot.libretro.com/nightly/android/latest/) e ficam em cache em `~/.rom2apk/cores` (`--refresh-cores` baixa de novo).

Os APKs são assinados com uma chave criada em `~/.rom2apk/rom2apk.keystore`. **Faça backup dela**: sem a mesma chave não é possível atualizar um APK já instalado.

## Aviso legal

Use apenas ROMs de jogos que você possui (extraídas dos seus cartuchos/discos) ou homebrew com licença livre. Não distribua APKs com jogos comerciais.

Os núcleos libretro têm licenças próprias, a maioria GPL, então os APKs gerados herdam essas obrigações. O snes9x proíbe uso comercial.
