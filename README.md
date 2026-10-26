# Photo Mode (Dusklight mod)

Modo foto para o [Dusklight](https://twilitrealm.dev/): congela o jogo, esconde o HUD e solta a câmera. Serve para avaliar mods gráficos de qualquer ângulo, a qualquer hora do dia.

## Controles

| Botão | Ação |
|---|---|
| **L3 + R3** (apertar os dois analógicos) | Entrar / sair do modo foto |
| **B** | Sair do modo foto |
| Analógico esquerdo | Mover a câmera |
| Analógico direito | Olhar |
| R / L | Subir / descer |
| Z (segurar) | Mais rápido |
| X / Y | Inclinar a câmera |
| D-pad ↑ / ↓ | Zoom (campo de visão) |
| **D-pad ← / →** | Mudar a hora do dia |
| A | Resetar inclinação e zoom |

Ao sair, a câmera volta para onde estava. Por padrão, a hora também volta à original. Para manter a hora escolhida, ligue **Keep Time on Exit**.

No painel do mod (menu de mods → Photo Mode → **Settings and Controls**), dá para ajustar a velocidade da câmera, a inversão dos eixos, a velocidade da mudança de hora e a opção de esconder o HUD.

O modo foto não abre durante cutscenes nem com menus abertos.

## Instalação

Baixe o `.dusk` em **Releases** (ou em **Actions → último build → mod-combined**) e copie para a pasta de mods:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

## Como funciona

- **Botões L3 e R3:** o controle de GameCube não tem esses botões. A Aurora (a camada de PC do Dusklight) informa os cliques em `PADStatus::extButton`, e o mod lê esse campo a cada frame.
- **Câmera:** é controlada por um operador do `CameraService`. Como o jogo pausa a câmera junto com o resto, o mod faz um hook em `fopCam_Execute` para mantê-la rodando, do mesmo jeito que a câmera de debug do Dusklight.
- **Hora do dia:** a etapa de atualização do ambiente fica pausada junto com o jogo, então o mod grava a hora diretamente em `g_env_light.daytime`. As cores de luz, céu e névoa são misturadas na hora do desenho (`drawKankyo` → `setLight`), que continua rodando com o jogo congelado.

## Compilar localmente

```
cmake -B build
cmake --build build
```
