# Relatório Técnico: Meio-Fio 3D Geométrico, Rebaixamento Viário e Iluminação

Este documento registra tudo o que foi implementado na branch de meio-fio geométrico (`feat/meio-fio-3d`) para consulta em outros dispositivos e para orientar qualquer ajuste futuro por agentes ou desenvolvedores.

---

## 1. Resumo Executivo do que foi Implementado

1. **Geometria 3D Real e Retilínea do Meio-Fio (`object_id == CURB`)** — em `cgvis-base-opengl/src/scene.cpp` (`BuildRoadCurbsAndAddToVirtualScene` e `DrawRoadCurbs`):
   - Extração sub-pixel da borda da camada vermelha (`splatmap_0.r`, Asfalto Principal) usando **Marching Squares** (`iso = 0.48`) após filtragem Gaussiana leve (`2` passagens 3×3) para remover serrilhado de pixel sem deslocar a via.
   - **Recorte automático em encontros com Asfalto Secundário (`splatmap_0.g`)**: células dentro de `~1,88 m` (dilatação `5×5`) do asfalto secundário têm `s0.g > 45` e não recebem meio-fio, deixando entradas/cruzamentos abertos com tampas verticais fechando as pontas.
   - **Prevenção de fechamento nas bordas externas do recorte do mapa**: `valid_mask.png` é erodida em `5×5` para não fechar pistas que saem da área válida (como extremidades da Av. Bento Gonçalves).
   - **Linhas retas e limpas (sem efeito serrilhado)**: as cadeias extraídas passam por **12 iterações de suavização Taubin (`λ = 0.50, μ = -0.52`)** seguidas de **simplificação Ramer-Douglas-Peucker (`ε = 0.16 m`)**, colapsando trechos retos em segmentos únicos longos sem zigue-zague.
   - **Quinas rígidas de 90° (`non-shared normals`)**: cada trecho gera 3 planos com vértices independentes (`AddCurbQuad` com verificação automática de winding CCW):
     - **Topo horizontal** (`20 cm` de largura: `-0.03 m` interno a `+0.17 m` externo com *miter* limitado a `1.6×` em curvas), normal ortogonal à rampa longitudinal da via e à parede lateral;
     - **Parede lateral interna vertical (`90°`)** voltada exatamente para o asfalto e para os faróis do ônibus (`normal = (n_in.x, 0, n_in.z)`);
     - **Parede lateral externa** ancorada abaixo do terreno externo;
     - **Tampas de fechamento** nas extremidades abertas onde encontra vias secundárias.

2. **Topo na Altura do Terreno e Asfalto Rebaixado em `-10 cm`** — em `cgvis-base-opengl/src/scene.cpp` e `tools/generate_terrain_assets.py`:
   - `SmoothTerrainAlongRoads` (em C++) e `process_heightmap_16bit` (em Python) executam **35 iterações de difusão geodésica 3×3** restritas estritamente aos pixels internos do asfalto (`w_asph > 0.45`, ignorando degraus > `0.85 m/px`), sem modificar nenhum centímetro das encostas fora do asfalto e sem misturar pistas paralelas em níveis distintos.
   - Uma cópia das alturas suavizadas **antes** do rebaixamento (`unrecessed_heights`) é mantida para definir a cota superior `Y_top` do meio-fio (`max(h_edge, h_outer) + 0.015 m` flush anti-Z-fighting, suavizada em `5` passagens 1D longitudinais).
   - Somente os vértices internos ao asfalto recebem o rebaixamento físico de **`-0.10 m` (`10 cm`)**, fazendo o ônibus sentir o degrau da pista via `GetTerrainHeight` enquanto o topo do meio-fio acompanha exatamente o terreno ao redor.
   - `unapply_recess_first` em `scene.cpp` desfaz e reaplica o `-0.10 m` de forma idempotente quando carrega `data/vale_processed.png`.

3. **Material Branco Isolado e Iluminação Realista no Shader** — em `cgvis-base-opengl/src/shader_fragment.glsl` e `cgvis-base-opengl/include/scene.h`:
   - Definido `#define CURB 9`.
   - No `shader_fragment.glsl` (`else if (object_id == CURB)`):
     - Cor branca pré-moldada (`vec3(0.84, 0.85, 0.83)`) aplicada **exclusivamente** à geometria do meio-fio (zero vazamento de branco sobre grama, calçada ou asfalto);
     - Granulação fina de concreto amostrada fora das linhas de rejunte do `mat_2_sidewalk.png`, juntas de dilatação sutis a cada **`1.0 m`** ao longo da guia (`fract(texcoords.x)`) e leve escurecimento de contato apenas na base inferior da parede vertical (`texcoords.y` em `[0.80..0.99]`);
     - Parametrização fotométrica (`Ks = vec3(0.22, 0.22, 0.23)`, `q = 36.0`) passando pelo cálculo completo de luz solar, ambiente, postes e **faróis do ônibus (`CalcPointLight`)** — como a parede lateral vertical possui normal horizontal apontada para a pista, o facho do farol (`L`/`P`) ilumina e reflete realisticamente ao longo da guia.
   - Em `main.cpp`, `DrawRoadCurbs()` é chamado logo após o terreno quando `!g_SatelliteTerrainMode` (desligado automaticamente no modo `F10` satélite para manter a comparação pura).

---

## 2. Arquivos Modificados / Criados nesta Entrega

- `cgvis-base-opengl/include/scene.h` (`CURB 9`, `DrawRoadCurbs()`)
- `cgvis-base-opengl/src/scene.cpp` (`SmoothTerrainAlongRoads`, `BuildRoadCurbsAndAddToVirtualScene`, `DrawRoadCurbs`)
- `cgvis-base-opengl/src/main.cpp` (chamada condicional `if (!g_SatelliteTerrainMode) DrawRoadCurbs();`)
- `cgvis-base-opengl/src/shader_fragment.glsl` (`#define CURB 9` e material/iluminação do meio-fio)
- `tools/generate_terrain_assets.py` (rebaixamento `-0.10 m` no asfalto em `process_heightmap_16bit` + flag `--textures-only`)
- `data/vale_processed.png` e `data/terrain/trees.txt` / `data/terrain/instances.json` regenerados preservando `splatmap_0.png` e `splatmap_1.png`
- `PROMPTS.md` (histórico técnico dos prompts atualizado)
- `MEIO_FIO_RELATORIO.md` (este documento)

---

## 3. Guia Rápido de Ajustes para Agentes ou Edição Futura

| O que ajustar | Onde está | Valor atual | Como alterar |
| :--- | :--- | :--- | :--- |
| **Largura do meio-fio** | `scene.cpp` (`BuildRoadCurbsAndAddToVirtualScene`, `p_in`/`p_out`) | `-0.03f` a `+0.17f` (`20 cm`) | Altere os offsets em metros ao longo de `mdir` |
| **Altura do rebaixamento do asfalto** | `scene.cpp` (`SmoothTerrainAlongRoads`) e `generate_terrain_assets.py` (`process_heightmap_16bit`) | `-0.10f` (`10 cm`) | Altere em **ambos** os arquivos (e no `+0.10f` de `unapply_recess_first`) e rode `--heightmap-only` |
| **Retidão / simplificação das curvas** | `scene.cpp` (`Taubin` iters e `RDPSimplifyRange` eps) | `12 iters`, `eps = 0.16f` (`16 cm`) | Aumente `eps` (`0.22f`) para retas ainda mais longas; reduza (`0.08f`) para curvas muito apertadas |
| **Folga no encontro com asfalto secundário** | `scene.cpp` (dilatação de `g_dil` e limiar `> 45`) | Raio `2 px` (`5×5`, `~1.88 m`), `g_dil > 45` | Troque `dx,dy in -2..2` por `-1..1` para fechar mais perto da esquina, ou `-3..3` para abrir mais |
| **Cor, juntas e brilho ao farol** | `shader_fragment.glsl` (`else if (object_id == CURB)`) | `vec3(0.84, 0.85, 0.83)`, junta `1.0 m`, `Ks = 0.22`, `q = 36.0` | Salve o shader e pressione **`R`** com o jogo aberto (Hot-Reload) |
| **Suavidade vertical do topo** | `scene.cpp` (loop `p < 5` em `y_top` e lip `+0.015f`) | `5` passagens, `+1.5 cm` flush | Ajuste `+0.015f` para subir/descer ligeiramente o topo relativo ao entorno |

---

## 4. Comandos Úteis após Editar o PSD ou Códigos

```powershell
# 1. Após editar data/terrain/terreno_editavel.psd e exportar sobrescrevendo o PSD:
.\tools\terrain_psd_tool.exe --apply

# 2. Para rebaixar o asfalto novo em data/vale_processed.png e ajustar alturas das árvores sem perder os splatmaps:
python tools/generate_terrain_assets.py --heightmap-only
python tools/generate_terrain_assets.py --trees-only

# 3. Para compilar o simulador C++ (também recalcula meio-fio 3D e suavização em float32 no carregamento):
cmake --build build/opengl --config Debug
```
