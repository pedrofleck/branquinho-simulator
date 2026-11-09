# Roadmap — Branquinho: Rota do Vale

Guia de implementação do trabalho final de INF01047. Cada fase termina com o jogo
compilando e rodando. Regras obrigatórias (ver AGENTS.md): só funções de
`matrices.h` para matrizes, tudo baseado em Δt, colisões só em `collisions.cpp`,
commits de IA com `IA:` + prompt em `PROMPTS.md`, não mexer na linha
`Correcao_KeyCallback(...)`.

## Situação atual

- `main.cpp` (~1600 linhas) ainda é o template do laboratório: carrega
  `sphere.obj`/`bunny.obj` sem usar, mantém variáveis de exercícios antigos
  (ângulos de Euler, antebraço/torso) e desenha o ônibus **duas vezes**.
- O chão é um quad de 2×2 m com textura em `CLAMP_TO_EDGE`; `farplane = -10`.
- O ônibus está em `y = -1.2` e o chão em `y = -1.1` (ônibus enterrado), escala 0.25.
- `branquinho.obj` já está **em metros** (~11,5 m de comprimento, ~4,5 m de
  altura) com a base em z≈0 (coordenadas Blender, Z-up). A bbox em x vai de
  -3,47 a 1,44 (4,9 m de largura → algum objeto fora do lugar ou espelhos) e o
  pivô não está centrado.
- O OBJ tem 193 objetos nomeados, incluindo `_wheel_fl/fr/rl/rr_*`, `_rdoor_*`,
  `_steering_wheel_`, `_transparent_*` e `*_emissive_` (luzes): base para
  hierarquia de rodas/portas, vidros e luzes.

## Estrutura de arquivos proposta

```
cgvis-base-opengl/
├── include/
│   ├── matrices.h        (existente) transformações e projeções
│   ├── utils.h           (existente) glCheckError
│   ├── game_state.h      struct GameState { Input, Bus, Camera, ... }  ← estado compartilhado
│   ├── obj_model.h       struct ObjModel + ComputeNormals
│   ├── scene.h           SceneObject, g_VirtualScene, Build/DrawVirtualObject
│   ├── shaders.h         carregamento de shaders + struct com uniforms
│   ├── textures.h        LoadTextureImage
│   ├── textrendering.h   declarações hoje soltas no topo do main.cpp
│   ├── controls.h        callbacks GLFW + struct InputState
│   ├── camera.h          câmera orbital (look-at) e câmera de cabine (livre)
│   ├── bus.h             estado e cinemática do ônibus
│   ├── terrain.h         malha do terreno + GetTerrainHeight(x, z)
│   ├── world.h           cenário: instâncias de árvores, postes, prédios
│   ├── hud.h             FPS, velocímetro, mensagens
│   ├── collisions.h      testes de intersecção (obrigatório separado)
│   ├── bezier.h          curvas de Bézier cúbicas
│   ├── pedestrians.h     pedestres andando em Bézier
│   └── particles.h       fumaça dos pneus
└── src/
    ├── main.cpp          só: init GLFW/GLAD, carga de assets, loop (Δt → update → render)
    ├── obj_model.cpp     ObjModel, ComputeNormals, PrintObjModelInfo
    ├── scene.cpp         BuildTrianglesAndAddToVirtualScene, DrawVirtualObject, Push/PopMatrix
    ├── shaders.cpp       LoadShader*, CreateGpuProgram, LoadShadersFromFiles
    ├── textures.cpp      LoadTextureImage
    ├── controls.cpp      KeyCallback, MouseButton/CursorPos/Scroll/Framebuffer callbacks
    ├── camera.cpp
    ├── bus.cpp           UpdateBus(dt, input) + DrawBus(...)
    ├── terrain.cpp
    ├── world.cpp
    ├── hud.cpp           (move TextRendering_ShowFramesPerSecond etc.)
    ├── collisions.cpp
    ├── bezier.cpp
    ├── pedestrians.cpp
    ├── particles.cpp
    ├── textrendering.cpp (existente)
    ├── correcao.cpp      (existente — não alterar)
    └── glad.c, stb_image.cpp, tiny_obj_loader.cpp (existentes)
```

**Estado compartilhado:** em vez de dezenas de globais `g_*`, um `GameState`
único criado no `main()` e registrado com `glfwSetWindowUserPointer(window, &state)`;
os callbacks recuperam com `glfwGetWindowUserPointer`. Funções de update recebem
`GameState&` e `dt`.

Cada `.cpp` novo deve ser adicionado à lista `SOURCES` de
`cgvis-base-opengl/CMakeLists.txt`.

---

## Fase 1 — Limpeza e separação do `main.cpp` (sem mudar comportamento)

Objetivo: mesma imagem na tela, código organizado.

1. Remover o que não usamos:
   - carga de `sphere.obj`, `bunny.obj` e o bloco `argv[1]`;
   - `g_AngleX/Y/Z`, `g_ForearmAngle*`, `g_TorsoPosition*`, botões direito/meio do mouse;
   - teclas X, Y, Z, Espaço, O/P (projeção ortográfica);
   - `TextRendering_ShowModelViewProjection`, `TextRendering_ShowEulerAngles`, `TextRendering_ShowProjection`;
   - ramos `SPHERE`/`BUNNY` em `shader_fragment.glsl`; `src/buildtriangles.cpp`
     (sobra de laboratório, nem está no CMake).
2. Desenhar o ônibus **uma vez só**, com `glDisable(GL_CULL_FACE)` antes e `glEnable` depois.
3. Mover funções para os arquivos da estrutura acima (recortar/colar + headers).
   Manter `R` (recarregar shaders), `H` (texto), `ESC`.
4. Trocar `#define SPHERE/BUNNY/PLANE/BUS` por um único header/enum compartilhado
   de IDs (`BUS`, `TERRAIN`, …) com valores iguais aos do shader.
5. Título da janela: `"Branquinho: Rota do Vale"`.
6. (Opcional) Unificar assets em `data/` na raiz (`data/models`, `data/textures`)
   e definir o caminho base via `target_compile_definitions(... DATA_DIR=...)`
   no CMake, eliminando os `../../` e `../../../`.

**Pronto quando:** compila, roda igual, `main.cpp` < ~200 linhas.

## Fase 2 — Terreno grande e ônibus no chão

1. Convenção: **1 unidade = 1 metro, chão em y = 0**. O modelo do ônibus já está
   em metros com base em z≈0 → usar escala 1.0 e posição y = 0.
   (Atualizar a seção 3 do AGENTS.md, que hoje sugere `g_BusPos.y = -0.4`.)
2. Centralizar o pivô do ônibus: aplicar `Matrix_Translate(-centro)` no espaço do
   modelo (centro calculado da bbox dos shapes da carroceria) ou corrigir a origem
   no Blender. Idealmente o pivô fica no eixo traseiro (melhora a curva).
   Investigar a largura de 4,9 m (algum objeto fora do lugar?).
3. Terreno: `Matrix_Scale(500, 1, 500)` no `the_plane`, sampler com `GL_REPEAT`
   e repetição de UV no shader (`uv * tiling`, ex. 100) para a textura não esticar.
4. Projeção: `nearplane = -0.1`, `farplane = -1000` (ou proporcional ao terreno).
5. Céu: cor de fundo azul-céu (névoa vem na Fase 8).

**Pronto quando:** ônibus em tamanho real apoiado no chão, textura do chão nítida
até o horizonte.

## Fase 3 — Controles WASD e cinemática do ônibus (`controls.cpp`, `bus.cpp`)

1. `InputState { bool w, a, s, d; }` atualizado no `KeyCallback` com
   `GLFW_PRESS`/`GLFW_RELEASE` (após a linha `Correcao_KeyCallback`).
2. Loop principal: `dt = glfwGetTime() - t_anterior`, com clamp (ex. `dt ≤ 0.1`)
   para evitar saltos ao arrastar a janela.
3. `struct Bus { glm::vec4 pos; float yaw, speed, steer; }` e `UpdateBus(bus, input, dt)`:
   - `W` acelera, `S` freia e depois dá ré, sem tecla → atrito proporcional a `dt`;
     velocidade limitada (ex. 60 km/h ≈ 16,7 m/s; ré ≈ 3 m/s).
   - `A`/`D` mudam o ângulo de esterço `steer` (com retorno ao centro);
     modelo de bicicleta: `yaw += speed * tan(steer) / entre_eixos * dt`
     (sem giro parado).
   - `f = (sin yaw, 0, cos yaw, 0)`; `pos += f * speed * dt`.
4. Matriz do ônibus:
   `Translate(pos) * Rotate_Y(yaw) * Rotate_X(-π/2) * Translate(-pivô)`.
   Conferir para que lado a frente aponta após a rotação; se apontar para −Z,
   somar π ao yaw na matriz.

**Pronto quando:** dirigir pelo chão com WASD, velocidade independente do FPS.

## Fase 4 — Câmeras (`camera.cpp`)

1. **Orbital (look-at)** seguindo o ônibus: alvo `l = bus.pos + (0, h, 0)`,
   posição em esféricas (θ, φ, r) com θ relativo ao `yaw` (fica atrás do ônibus);
   mouse esquerdo gira, scroll faz zoom (limites de r e φ). Opcional: suavizar
   o θ de volta para trás do ônibus com Δt.
2. **Cabine (câmera livre / primeira pessoa):** posição = `M_bus * banco_local`,
   direção = `M_bus * (0,0,1,0)` + offset do mouse (olhar para os lados).
   Plano B do SPEC: bumper cam à frente do para-choque.
3. Tecla `C` alterna câmeras. View sempre via `Matrix_Camera_View`.

## Fase 5 — Materiais, texturas e iluminação

1. Usar os materiais do `branquinho.mtl` (`material_ids` por face): agrupar
   triângulos por material em `BuildTrianglesAndAddToVirtualScene` e enviar
   `Kd`, `Ks`, `Ns` e textura `map_Kd` como uniforms por draw.
2. Shader: Lambert + Blinn-Phong com luz direcional (sol). Conferir no enunciado
   os modelos de iluminação/interpolação exigidos (ex.: Gouraud em um objeto,
   Phong nos demais) e garantir que cada um apareça.
3. Evoluir para Disney BRDF (SPEC) depois que Blinn-Phong estiver estável.
4. Texturas do cenário a partir de fotos do Campus do Vale.

## Fase 6 — Cenário do Campus do Vale (`terrain.cpp`, `world.cpp`)

1. Terreno em grade procedural N×N com relevo (heightmap ou função),
   `GetTerrainHeight(x, z)` e normal do terreno.
2. Ônibus segue o relevo: `pos.y = altura`, pitch/roll a partir das alturas
   sob os eixos (subidas, quebra-molas).
3. Estrada (faixa texturizada sobre o terreno) e rota do circular.
4. Instâncias: árvores e postes desenhados N vezes com a mesma malha e matrizes
   diferentes (requisito "instâncias de objetos").
5. Modelos: prédios, pórtico de entrada, paradas de ônibus.

## Fase 7 — Colisões e checkpoints (`collisions.cpp`)

1. Ônibus como OBB (ou AABB no espaço local) × obstáculos AABB (prédios, postes)
   → resolver penetração e zerar/inverter velocidade.
2. Esfera/cilindro × ponto (checkpoints / paradas).
3. Raio × terreno ou raio × AABB (ex. sensor de chão, mira do mouse).
4. Gameplay: parar nas paradas com a porta aberta, embarcar passageiros, HUD.

## Fase 8 — Animações e efeitos

1. Hierarquia do OBJ: rodas giram com `speed`, rodas dianteiras esterçam,
   volante gira, portas abrem (tecla), cada parte com transformação local.
2. Pedestres em curvas de Bézier cúbicas parametrizadas por tempo.
3. Partículas de fumaça nos pneus em arrancadas/freadas/curvas fortes
   (billboards com vida útil, velocidade, escala, alpha).
4. Pedidos do professor: névoa volumétrica, postes iluminados (luzes pontuais
   com atenuação), vidros transparentes (blending, desenhar por último em ordem
   de distância), espelhos retrovisores (render-to-texture com câmera extra).

## Checklist de requisitos × fase

| Requisito                                   | Fase |
|---------------------------------------------|------|
| Malhas complexas (ônibus, prédios)          | 2, 6 |
| Transformações controladas pelo usuário     | 3    |
| Câmera look-at e câmera livre               | 4    |
| Instâncias de objetos                       | 6    |
| Testes de intersecção em `collisions.cpp`   | 7    |
| Modelos de iluminação / interpolação        | 5    |
| Texturas em todos os objetos                | 5, 6 |
| Curva de Bézier cúbica                      | 8    |
| Animações baseadas em Δt                    | 3, 8 |
| Extra: partículas de fumaça                 | 8    |

## Como testar a cada fase

```
cmake --preset opengl-mingw
cmake --build --preset build-opengl
cd cgvis-base-opengl/bin/Debug && ./main.exe
```

Verificar: sem erros no terminal (shaders, `glCheckError`), FPS no HUD, e o
comportamento descrito em "Pronto quando" de cada fase. Para checar
independência de FPS, comparar o tempo para atingir a velocidade máxima com
VSync ligado/desligado.
