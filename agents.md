# Diretrizes de Desenvolvimento para Agentes (AGENTS.MD)

Contexto do projeto: Simulador do ônibus circular da UFRGS ("Branquinho") desenvolvido como trabalho final da disciplina INF01047 (Computação Gráfica e Visualização I - UFRGS).

---

## 1. Regras e Restrições Mandatórias da Disciplina

1. **Biblioteca de Álgebra Linear Própria**:
   - É estritamente proibido o uso de funções utilitárias prontas do GLM ou OpenGL para cálculo de matrizes de câmera e transformações (ex.: `glm::translate`, `glm::rotate`, `glm::scale`, `glm::lookAt`, `glm::perspective`, `glm::ortho`, `gluLookAt`, etc.).
   - Deve-se usar exclusivamente as funções implementadas no header `cgvis-base-opengl/include/matrices.h` (`Matrix_Translate`, `Matrix_Rotate_X/Y/Z`, `Matrix_Scale`, `Matrix_Camera_View`, `Matrix_Perspective`, etc.).
2. **Atualização Baseada em Tempo ($\Delta t$)**:
   - Toda cinemática, atualização de estado, animação e câmera devem usar $\Delta t = t_{atual} - t_{anterior}$ computado via `glfwGetTime()`.
   - Nenhuma velocidade ou deslocamento pode ser fixado por frame.
3. **Módulo de Colisões Separado**:
   - Algoritmos de intersecção e colisão devem residir exclusivamente em `collisions.cpp` / `collisions.h`.
4. **Convenções de Commit e IA**:
   - Commits automáticos/gerados por IA devem iniciar com `IA: <descrição>`.
   - Os prompts utilizados devem ser registrados em `PROMPTS.md`.
5. **Callback de Correção**:
   - A função `KeyCallback` deve manter inalterada a linha inicial `Correcao_KeyCallback(key, action, mod);`.

---

## 2. Mapa do Projeto e Arquivos Críticos

- `CMakeLists.txt`: Configuração raiz (default: backend OpenGL em `cgvis-base-opengl`).
- `cgvis-base-opengl/src/main.cpp`: Ponto de entrada, loop de renderização, callbacks GLFW e gerenciamento de cena virtual.
- `cgvis-base-opengl/src/shader_vertex.glsl`: Vertex shader GLSL 330 core.
- `cgvis-base-opengl/src/shader_fragment.glsl`: Fragment shader com modelo de iluminação e mapeamento UV.
- `cgvis-base-opengl/include/matrices.h`: Funções manuais de transformações afins e projeções.
- `data/branquinho.obj` e `data/branquinho.mtl`: Modelo do ônibus Mascarello Gran Midi.
- `SPEC.md`: Especificação aprovada pela disciplina.

---

## 3. Especificação Técnica da Etapa 1: Cinemática do Ônibus e Câmera

### 3.1 Limpeza de `main.cpp`
- Remover carregamento de malhas de teste não utilizadas (`sphere.obj`, `bunny.obj`).
- Remover variáveis globais legadas de exercícios anteriores:
  - Euler angles (`g_AngleX`, `g_AngleY`, `g_AngleZ`).
  - Hierarquia de braço/torso (`g_ForearmAngleX/Z`, `g_TorsoPositionX/Y`).
- Limpar callbacks:
  - Em `MouseButtonCallback` e `CursorPosCallback`, remover controles do botão direito e botão do meio.
  - Em `KeyCallback`, remover manipulação das teclas X, Y, Z e Espaço.
  - Remover funções de debug de matrizes de texto da tela (`TextRendering_ShowModelViewProjection`, `TextRendering_ShowEulerAngles`).
- Eliminar renderização redundante do ônibus (remover chamadas duplicadas com `glDisable(GL_CULL_FACE)` consecutivas).

### 3.2 Ajustes de Parâmetros de Cena
- Escalar o plano do chão (`the_plane`) para um valor amplo (sugestão: $50 \times 1 \times 50$ ou superior).
- Expandir o plano de corte traseiro (`farplane`) para evitar culling de profundidade a distâncias curtas (mínimo de $-150.0f$).

### 3.3 Tratamento de Input do Teclado
- Implementar flags de estado:
  ```cpp
  bool g_KeyW = false;
  bool g_KeyA = false;
  bool g_KeyS = false;
  bool g_KeyD = false;
  ```
- Atualizar flags no `KeyCallback` com `GLFW_PRESS` / `GLFW_RELEASE`.

### 3.4 Cinemática e Orientação do Veículo
- **Orientação original do modelo**: O modelo `.obj` exportado do Blender possui convenção $Z$-up. A conversão padrão usa rotação de $-\pi/2$ em $X$.
- **Variáveis de estado**:
  ```cpp
  glm::vec4 g_BusPos = glm::vec4(0.0f, -0.4f, 0.0f, 1.0f);
  float g_BusYaw = 0.0f;       // Orientação em radianos no plano horizontal
  float g_BusSpeed = 0.0f;     // Velocidade escalar (m/s)
  ```
- **Cinemática**:
  - Aceleração: ao pressionar `W`, incrementar velocidade considerando taxa de aceleração e $\Delta t$.
  - Frenagem / Marcha ré: tecla `S` desacelera / inverte velocidade.
  - Atrito: quando nem `W` nem `S` estiverem ativas, aplicar desaceleração por amortecimento proporcional a $\Delta t$.
  - Esterçamento: teclas `A` / `D` alteram `g_BusYaw` proporcionalmente à velocidade e direção (impedir rotação pura em repouso estático).
  - Vetor diretor frontal: derivado da orientação $\theta = \text{yaw}$.
    $$\vec{f} = \begin{pmatrix} \sin(\text{yaw}) \\ 0 \\ \cos(\text{yaw}) \\ 0 \end{pmatrix}$$
  - Atualização de posição: $\vec{p}_{novo} = \vec{p} + \vec{f} \cdot (\text{speed} \cdot \Delta t)$.
- **Composição da Matriz Model do Ônibus**:
  $$M_{bus} = \text{Translate}(g\_BusPos) \times \text{Rotate}_Y(g\_BusYaw) \times \text{Rotate}_X(-\pi/2) \times \text{Scale}(s_x, s_y, s_z)$$

### 3.5 Câmera Orbital (Look-At) Acompanhando o Ônibus
- Coordenadas esféricas mantidas por `g_CameraTheta`, `g_CameraPhi` e `g_CameraDistance`.
- Look-at point:
  $$\vec{l} = g\_BusPos + \begin{pmatrix} 0 & h_{offset} & 0 & 0 \end{pmatrix}^T$$
- Posição da câmera:
  $$\vec{c} = \vec{l} + \begin{pmatrix} r \cos(\phi) \sin(\theta) \\ r \sin(\phi) \\ r \cos(\phi) \cos(\theta) \\ 0 \end{pmatrix}$$
- View matrix gerada via `Matrix_Camera_View(\vec{c}, \vec{l} - \vec{c}, \vec{up})`.

---

## 4. Próximos Marcos Arquiteturais (Backlog Técnico)

1. **Câmera em Primeira Pessoa / Cockpit**:
   - Extrair matriz de transformação vinculada ao referencial local do veículo para simular a câmera no banco do motorista.
2. **Sub-hierarquia de Malhas do OBJ**:
   - Particionar a chamada de desenho de `shapes` para manipular separadamente nós de rodas (`_wheel_rr_...`) e portas (`_rdoor_...`) aplicando transformações locais compostas no pipeline.
3. **Shader de Iluminação e Mapeamento de Materiais**:
   - Passar atributos dos materiais de `branquinho.mtl` para uniforms e implementar modelo de iluminação fotométrica compatível com a especificação (BRDF / Blinn-Phong com atenuação).
4. **Colisões**:
   - Implementar em `collisions.cpp` funções de intersecção Ray-AABB, OBB-OBB e Sphere-AABB com resposta de impulso ou resolução de penetração.
5. **Sistema de Partículas**:
   - Buffer de instâncias ou billboard de partículas com vida útil, velocidade e escala para emissores localizados nas coordenadas das rodas.
