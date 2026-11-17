# Guia Técnico de Contexto — Terreno, Máscaras Editáveis (PSD), Iluminação e Colisões

> **Nota para Agentes de IA**: Este arquivo documenta todas as decisões técnicas, coordenadas, ferramentas e fluxos construídos até o momento no projeto **"Branquinho: Rota do Vale"** (INF01047 - UFRGS), servindo como referência para continuar o trabalho em qualquer computador sem perder contexto.
> Consulte também [AGENTS.md](AGENTS.md) para as regras mandatórias da disciplina (`matrices.h`, `Delta t`, módulo `collisions.cpp`/`collisions.h` e `Correcao_KeyCallback`).

---

## 1. Arquitetura Física e Métrica do Terreno (QGIS -> OpenGL)

- **Projeção original**: SIRGAS 2000 / UTM zone 22S (`EPSG:31982`).
- **Escala espacial**: **1 unidade OpenGL = 1.0 metro** (`1 pixel` do heightmap = `1.0 m` em X e Z).
- **Arquivos de elevação (16-bit grayscale, `0..65535`)**:
  - `data/vale.png` (`1930 x 1703`): recorte topográfico original do QGIS.
  - `data/vale_processed.png` (`1930 x 1703`): versão processada com nivelamento transversal de pistas e da bacia da Lagoa do Vale. Carregado por `BuildHeightmapAndAddToVirtualScene("../../../data/vale_processed.png", ...)` em `cgvis-base-opengl/src/main.cpp` (com fallback automático para `data/vale.png`).
- **Sistema de coordenadas no mundo**:
  - **X** cresce para Leste: `[-965.0 m, +965.0 m]` (`W = 1930.0f`).
  - **Z** cresce para Sul: `[-851.5 m, +851.5 m]` (`H = 1703.0f`).
  - **Y** (altitude em metros):
    $$y = 33.0 + \frac{\text{pixel\_uint16}}{65535.0} \times 104.043$$
    (mínimo real `33.0 m`, máximo real `137.043 m`, desnível total `deltaH = 104.043 m`).
- **Recorte de bordas válidas**:
  - `data/terrain/valid_mask.png` (`1930 x 1703`, gerado a partir da área não-preta de `tools/bruto_2012_mascara.tiff`): células completamente pretas são omitidas da malha 3D na geração em `cgvis-base-opengl/src/scene.cpp` e os splatmaps zeram o peso fora da máscara (`if (w_sum < 0.02) discard;` em `shader_fragment.glsl`).
- **Consulta de altura na CPU (`O(1)`)**:
  - `GetTerrainHeight(float world_x, float world_z)` em `cgvis-base-opengl/include/scene.h` / `scene.cpp` faz interpolação bilinear sobre a grade carregada e é usada pelo ônibus (`g_Bus.UpdatePos`), postes (`Lighting::AddStreetLamp`) e árvores (`trees.txt`).
- **Normais e Malha**:
  - Calculadas por diferenças finitas centrais (`vec4(hL - hR, 2.0f * grid_spacing, hU - hD, 0.0f)` normalizado com `norm()` de `matrices.h`).
  - Enviadas à GPU em um único VBO intercalado (`TerrainVertex`: `vec3 position`, `vec3 normal`, `vec2 uv`) + EBO de `GLuint` (`GL_TRIANGLES`).
  - `stbi_set_flip_vertically_on_load(false)` é obrigatório para manter `vale.png`, `valid_mask.png`, `splatmap_0/1.png`, `macro_albedo.png` e `satelite_2012_low.png` alinhados pixel a pixel.

---

## 2. Sistema de Materiais Multi-Textura (Splatmaps + Shader)

Em `cgvis-base-opengl/src/scene.cpp` (`LoadTerrainTextures`) e `shader_fragment.glsl` (`object_id == PLANE`):

| Unidade GPU | Uniform / Arquivo | Conteúdo / Função |
| :---: | :--- | :--- |
| `0` | `TextureImage0` | Textura base (`red_brick_diff_1k.jpg`) |
| `1` | `TextureImage1` | Detalhe de solo fallback (`rocky_terrain_02_diff_1k.jpg`) |
| `2` | `TextureImage2` (`data/satelite_2012_low.png`) | Satélite `1930x1703` (`1 px/m`), usado apenas para depuração visual ao apertar **F10** |
| `3` | `TerrainSplat0` (`data/terrain/splatmap_0.png`) | Pesos RGBA (`2048x2048` linear, `GL_RGBA8`) dos canais 0–3 |
| `4` | `TerrainSplat1` (`data/terrain/splatmap_1.png`) | Pesos RGBA (`2048x2048` linear, `GL_RGBA8`) dos canais 4–7 |
| `5` | `TerrainMacroAlbedo` (`data/terrain/macro_albedo.png`) | Modulador cromático (`4096x4096`) aplicado somente em grama/mata/terra |
| `6` | `TerrainMaterials` (`GL_TEXTURE_2D_ARRAY`) | 8 texturas `512x512` seamless (`mat_0` a `mat_7`) amostradas em escala métrica |
| `7–8` | `TreeBarkTex`, `TreeFoliageTex` | Texturas das árvores 3D (`data/tree.obj` + `data/terrain/trees.txt`) |

### Mapeamento dos 8 Canais

| Índice | Canal no Splatmap | Textura (`data/terrain/`) | Descrição |
| :---: | :--- | :--- | :--- |
| `0` | `splatmap_0.r` | `mat_0_asphalt_main.png` | **Asfalto Principal** (Anel Viário, Av. Bento Gonçalves) |
| `1` | `splatmap_0.g` | `mat_1_asphalt_sec.png` | **Asfalto Secundário** / Estacionamentos / Vias menores |
| `2` | `splatmap_0.b` | `mat_2_sidewalk.png` | **Calçadas** / Pátios pavimentados |
| `3` | `splatmap_0.a` | `mat_3_grass.png` | **Grama Aberta** / Canteiros |
| `4` | `splatmap_1.r` | `mat_4_forest.png` | **Solo de Mata** / Folhagem rasteira |
| `5` | `splatmap_1.g` | `mat_5_dirt.png` | **Terra Exposta** / Taludes / Saibro |
| `6` | `splatmap_1.b` | `mat_6_water.png` | **Água** (Lagoa do Vale) |
| `7` | `splatmap_1.a` | `mat_7_building.png` | **Fundação / Base de Prédios** |

---

## 3. Edição Visual das Máscaras no Affinity Photo (`terreno_editavel.psd`)

Como editar pesos RGBA com canal Alpha manualmente em editores de imagem é difícil, o repositório possui uma ferramenta em C++ puro (`tools/terrain_psd_tool.cpp` -> `tools/terrain_psd_tool.exe`) que converte ida e volta entre os dois splatmaps e um arquivo PSD multicamadas (`data/terrain/terreno_editavel.psd`, `2048 x 2048`).

### 3.1 Pilha de Camadas em `data/terrain/terreno_editavel.psd` (Topo -> Fundo)
1. **`SATELITE_GUIA_TOPO`** (`45%` opacidade): imagem de satélite por cima para guiar o traçado das ruas; não vira material.
2. **`0_Asfalto_Principal`** (Vermelho `#E53935`) -> `splatmap_0.r`
3. **`1_Asfalto_Secundario`** (Laranja `#FB8C00`) -> `splatmap_0.g`
4. **`2_Calcadas`** (Ciano `#00ACC1`) -> `splatmap_0.b`
5. **`7_Predios`** (Roxo `#8E24AA`) -> `splatmap_1.a`
6. **`6_Agua_Lagoa`** (Azul `#1E88E5`) -> `splatmap_1.b`
7. **`5_Terra_Barro`** (Marrom `#8D6E63`) -> `splatmap_1.g`
8. **`4_Mata_Arvores`** (Verde Escuro `#1B5E20`) -> `splatmap_1.r`
9. **`3_Grama_Base`** (Verde Claro `#7CB342`) -> `splatmap_0.a` (preenche `Alpha = 255` em toda a área válida para servir de base ao apagar camadas superiores).
10. **`SATELITE_BASE_FUNDO`** (`100%` opacidade): imagem de satélite completa no fundo.

### 3.2 Como Funciona a Edição e Conversão (`--init` e `--apply`)
- **Regra de Composição (`Porter-Duff over` de cima para baixo)**:
  - No `--apply`, o algoritmo percorre as camadas de cima (`0_Asfalto_Principal`) para baixo (`3_Grama_Base`), lendo `canal Alpha * opacidade global da camada` (ignorando a cor RGB usada para pintar e respeitando a visibilidade da camada).
  - Camadas superiores têm prioridade automática: pintar asfalto em `0_Asfalto_Principal` cobre a grama/mata abaixo sem precisar apagar manualmente as camadas inferiores; usar a Borracha (`E`) em `0_Asfalto_Principal` revela a camada abaixo (`3_Grama_Base`).
  - Se todas as 8 camadas de material estiverem transparentes (`Alpha = 0`), o pixel permanece vazio (fora da área válida, descartado pelo shader).
- **Árvores 3D**:
  - Na primeira execução do `--apply`, a lista original de árvores é preservada em `data/terrain/trees_base.txt`.
  - A cada `--apply`, `data/terrain/trees.txt` é reexportado removendo qualquer árvore num raio de ~4m sobre pixels onde `(asphalt_main + asphalt_sec) > 0.25`. Se um trecho de asfalto errado for apagado no PSD, as árvores originais daquele local voltam na próxima atualização.
- **Compatibilidade PSD**:
  - `ReadPSD` suporta compressão `0` (Raw), `1` (PackBits RLE), `2` (ZIP) e `3` (ZIP + Predictor), bounding boxes recortadas por camada, nomes Pascal ou Unicode (`luni`), e User Layer Masks (`channel_id == -2`).

### 3.3 Sincronização Automática & Comandos em Outro Computador
- **Automático ao abrir o simulador**: Em `cgvis-base-opengl/src/scene.cpp` (`LoadTerrainTextures()`), no Windows, se `data/terrain/terreno_editavel.psd` tiver timestamp (`st_mtime`) mais recente que `data/terrain/splatmap_0.png` e `tools/terrain_psd_tool.exe` existir, o simulador executa `..\..\..\tools\terrain_psd_tool.exe --apply` automaticamente antes de carregar os splatmaps e as árvores.
- **Execução manual** (na raiz do repositório):
  ```powershell
  # Aplicar edições do Affinity Photo -> splatmap_0.png, splatmap_1.png e trees.txt:
  .\tools\terrain_psd_tool.exe --apply

  # (Apenas se precisar recriar o .psd do zero a partir dos splatmaps atuais - SOBRESCREVE edições no PSD!):
  .\tools\terrain_psd_tool.exe --init
  ```
- **Recompilando e gerando o PSD em outro PC** (`*.exe` e `terreno_editavel.psd` ficam no `.gitignore` para evitar bloat e falsos positivos do GitHub Secret Scanning; usa `stb_image.h` do projeto e `zlib` estática do MSYS2 UCRT64):
  ```powershell
  g++ -O3 -Wall -static tools/terrain_psd_tool.cpp -Icgvis-base-opengl/include -lz -o tools/terrain_psd_tool.exe
  .\tools\terrain_psd_tool.exe --init
  ```
- *(Opcional)* `tools/generate_terrain_assets.py` contém o pipeline Python offline original (requer `numpy`, `Pillow`, `rasterio` e `tools/bruto_2012_mascara.tiff`). Caso novas pistas sejam desenhadas e seja necessário nivelar o relevo (`vale_processed.png`) sob novas estradas fora dos corredores originais, o nivelamento pode ser adicionado ao Python ou em C++ usando o tensor de estrutura do asfalto sobre `data/vale.png`.

---

## 4. Controles Atuais do Simulador (Debug e Condução)

Definidos em `cgvis-base-opengl/src/main.cpp`, `bus.cpp`, `camera.cpp` e `lighting.cpp`:
- `W`, `A`, `S`, `D`: Dirigem o ônibus (nos modos `Orbit` / `Bumper`) ou voam com a câmera livre (quando em modo `Free`).
- `C`: Alterna câmera entre `Orbit` e `Bumper` (para-choque).
- `F11`: Ativa/desativa a câmera livre (`Free`, velocidade `120 m/s`, desliga o fog temporariamente e restaura ao sair).
- `F10`: Alterna o terreno entre o modo jogável (8 materiais dos splatmaps) e `satelite_2012_low.png`.
- `L`: Ciclo de 3 estágios dos faróis frontais (`Desligados -> Luz menor do meio (_tex_electric__emissive_) -> Farol principal maior assimétrico (_tex_low_beam__emissive_) -> Desligados`), com filtragem espacial no shader (`HEADLIGHT_LENS`) para não acender marcadores laterais.
- `P`: Farol separado de luz alta (`_tex_high_beam__emissive_`) — toque curto pisca; segurar por `>= 0.25s` trava aceso até pressionar `P` novamente.
- `J` / `K`: Altera o ângulo do Sol (hora do dia / iluminação / névoa).
- `O`: Alterna projeção Perspectiva / Ortográfica (`farplane = -3500.0f`).
- `V`: Liga/desliga V-Sync.
- `R`: Hot-reload dos shaders (`shader_vertex.glsl` / `shader_fragment.glsl`).

---

## 5. Próximo Passo Planejado: Módulo de Colisões (`collisions.h` / `collisions.cpp`)

O usuário já alinhou a estratégia para iniciar as colisões assim que o traçado do asfalto em `splatmap_0.png` estiver revisado via Affinity Photo:

1. **Onde implementar**: Exclusivamente em `cgvis-base-opengl/include/collisions.h` e `cgvis-base-opengl/src/collisions.cpp` (conforme regra 3 de `AGENTS.md`), usando apenas funções matemáticas de `matrices.h`.
2. **Parede Invisível na Borda do Asfalto (Área Jogável)**:
   - Reutilizar a máscara de asfalto (`splatmap_0.r + splatmap_0.g`, com interpolação bilinear em escala real `1930 x 1703`) carregada na CPU.
   - Modelar a carroceria do ônibus (`data/branquinho.obj`: `X ∈ [-1.28, +1.28]`, `Z ∈ [-7.95, +3.25]`) como uma **OBB** orientada pelo `yaw` do ônibus.
   - Testar os 4 cantos (ou perímetro) da OBB contra o peso de asfalto (`asphalt_weight < threshold`): quando um canto tentar sair do asfalto, calcular a normal da borda via diferenças centrais no campo de distância/peso do asfalto, corrigir a penetração empurrando `g_Bus.position` de volta para a pista e zerar apenas a componente normal da velocidade para permitir deslizamento suave ao raspar nas curvas.
3. **Outras Intersecções da Disciplina**:
   - **OBB vs OBB (SAT - Separating Axis Theorem)**: para colisão com futuros outros veículos e obstáculos do cenário.
   - **Esfera vs OBB / AABB**: para detecção de paradas de ônibus e *checkpoints* do percurso.
