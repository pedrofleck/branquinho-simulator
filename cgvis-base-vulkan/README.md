# Código base do Trabalho Final em Vulkan — INF01047 (CGVis I)

Esta pasta contém uma adaptação do
[código base do trabalho final](https://github.com/cgvis-inf-ufrgs/cgvis-base-trabalho-final)
da disciplina, originalmente escrito em **OpenGL 3.3**, para a API gráfica
**Vulkan**.

A aplicação de exemplo é exatamente a mesma do código base: uma esfera, um
coelho e um plano texturizados, com câmera look-at controlada pelo mouse,
projeção perspectiva/ortográfica alternável, e texto informativo na tela.

> [!WARNING]
> Este arquivo documenta o **código base**. Ele **não** é o relatório final
> exigido pelo enunciado. O `README.md` da entrega (na raiz do repositório) é
> um relatório que, segundo o enunciado, **não pode ser escrito com auxílio de
> ferramentas de IA**.

## O que a especificação exige, e como este código base atende

| Exigência do enunciado | Como é atendida aqui |
| --- | --- |
| Somente APIs gráficas de baixo nível | Vulkan é uma das cinco APIs explicitamente permitidas. Nenhum motor gráfico é usado. |
| Matrizes Model/View/Projection implementadas por você | `include/matrices.h`, inalterado em relação ao código base. Nenhuma função de biblioteca pronta (`glm::lookAt()`, `glm::perspective()`, `glm::rotate()`, ...) é usada — o `#include <glm/gtc/matrix_transform.hpp>` foi inclusive **removido**, para deixar isso explícito. A GLM é usada apenas como container de `vec4`/`mat4`. |
| Testes de colisão em arquivo separado `collisions.cpp` | `src/collisions.cpp` e `include/collisions.h`, com testes esfera×esfera, AABB×AABB, esfera×AABB, esfera×plano, ponto×AABB e raio×AABB (este último útil para *picking*). |
| Interação em tempo real | Apresentação em modo `MAILBOX` quando disponível (sem *tearing* e sem travar a aplicação), com *double buffering* de comandos (`MAX_FRAMES_IN_FLIGHT = 2`). |
| Interação por mouse e teclado | Callbacks da GLFW, idênticos aos do código base. |

Os demais requisitos (malhas complexas, duas câmeras distintas, instâncias,
Bézier, iluminação, animação por Δt, funcionalidade extra) são o **seu**
trabalho: este é apenas o ponto de partida.

## Estrutura

```
cgvis-base-vulkan/
+-- CMakeLists.txt          Compila o código E os shaders (GLSL -> SPIR-V)
+-- COMPILACAO.md           Como instalar o Vulkan SDK, compilar e executar
+-- SPEC.md                 Template de especificação (preencher)
+-- data/                   Modelos .obj e texturas
+-- include/
|   +-- matrices.h          Matrizes Model/View/Projection feitas à mão
|   +-- vkcontext.h         Interface do "boilerplate" do Vulkan
|   +-- collisions.h        Testes de intersecção
|   +-- utils.h             Macro VK_CHECK() para verificação de erros
|   +-- dejavufont.h        Atlas de glifos para rasterização de texto
|   +-- GLFW/ glm/ ...      Bibliotecas de terceiros
+-- src/
    +-- main.cpp            Laço principal, cena, câmera, texturas, pipeline
    +-- vkcontext.cpp       Instância, GPU, swapchain, render pass, sincronização
    +-- collisions.cpp      Testes de intersecção
    +-- textrendering.cpp   Rasterização de texto
    +-- correcao.cpp        Não modificar (correção automatizada)
    +-- shader_vertex.vert  Vertex Shader da cena
    +-- shader_fragment.frag  Fragment Shader da cena
    +-- shader_text.vert    Vertex Shader do texto
    +-- shader_text.frag    Fragment Shader do texto
```

Todo o "boilerplate" do Vulkan foi isolado em `src/vkcontext.cpp` **de
propósito**: assim, `src/main.cpp` continua tendo praticamente a mesma
estrutura, os mesmos nomes de função e os mesmos comentários (com referências
aos slides da disciplina) do código base em OpenGL.

## De OpenGL para Vulkan: o mapa

| Código base (OpenGL) | Esta versão (Vulkan) |
| --- | --- |
| `glfwMakeContextCurrent()` + `gladLoadGLLoader()` | `VulkanContext_Init()` — instância, GPU, dispositivo lógico, swapchain, render pass, command buffers, semáforos e fences |
| `glClearColor()` + `glClear()` | `loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR` no render pass, com os valores passados a `VulkanContext_BeginFrame()` |
| `glfwSwapBuffers()` | `VulkanContext_EndFrame()` — `vkQueueSubmit()` + `vkQueuePresentKHR()` |
| `glViewport()` | `vkCmdSetViewport()` (estado dinâmico, regravado a cada quadro) |
| Programa de GPU (`glCreateProgram`, `glUseProgram`) | `VkPipeline` + `vkCmdBindPipeline()`. O pipeline também fixa estados que no OpenGL eram globais |
| `glEnable(GL_DEPTH_TEST)` | `VkPipelineDepthStencilStateCreateInfo` |
| `glEnable(GL_CULL_FACE)`, `glCullFace`, `glFrontFace` | `VkPipelineRasterizationStateCreateInfo` |
| `glEnable(GL_BLEND)` + `glBlendFunc()` | `VkPipelineColorBlendAttachmentState` |
| VAO (`glBindVertexArray`) | `struct GpuModel` + `vkCmdBindVertexBuffers()` / `vkCmdBindIndexBuffer()` |
| VBO (`glGenBuffers` + `glBufferData`) | `Vulkan_CreateBufferWithData()` (staging buffer + cópia para memória da GPU) |
| `glDrawElements()` | `vkCmdDrawIndexed()` |
| `glUniformMatrix4fv(model)`, `glUniform1i(object_id)`, `glUniform4f(bbox_*)` | *Push constants* (`struct ObjectConstants`) — mudam a cada objeto |
| `glUniformMatrix4fv(view/projection)` | Uniform Buffer Object (`struct SceneUniforms`) — muda uma vez por quadro |
| `glGenTextures` + `glTexImage2D` + `glGenerateMipmap` | `Vulkan_CreateImage()` + `Vulkan_CopyBufferToImage()` + `Vulkan_GenerateMipmaps()` |
| `glGenSamplers` + `glSamplerParameteri` | `VkSampler` |
| Unidade de textura + `glUniform1i("TextureImage0", 0)` | *Descriptor set* com `binding` 1, 2 e 3 |
| Shaders GLSL compilados pelo driver | GLSL compilado para SPIR-V pelo `glslc` (feito pelo CMake) |
| `glGetError()` / `glCheckError()` | Macro `VK_CHECK()` + *validation layers* |

## Três detalhes que costumam causar bugs

### 1. O cubo NDC do Vulkan é diferente

Em OpenGL o volume canônico é `[-1,1]³` com o eixo Y **para cima**. Em Vulkan
é `[-1,1]² × [0,1]` com o eixo Y **para baixo**.

As matrizes `Matrix_Perspective()` e `Matrix_Orthographic()` **não foram
alteradas** — elas continuam exatamente como vistas em aula. A conversão é
feita por uma matriz extra, `Matrix_Vulkan_Clip()`, aplicada por último:

```cpp
projection = Matrix_Vulkan_Clip() * Matrix_Perspective(fov, ratio, near, far);
```

### 2. Por isso, o *front face* é `CLOCKWISE`

Como o eixo Y é invertido, a orientação dos triângulos em coordenadas de tela
também inverte. Para obter o mesmo resultado visual que `glFrontFace(GL_CCW)`,
o pipeline usa `VK_FRONT_FACE_CLOCKWISE`. Se seus objetos sumirem, é quase
sempre isso.

### 3. Correção gama

O swapchain é criado com um formato **UNORM** (e não `_SRGB`) de propósito: as
texturas usam `VK_FORMAT_R8G8B8A8_SRGB` (a GPU converte para linear ao ler,
como fazia o `GL_SRGB8`), e a correção gama final continua sendo feita na mão
no fragment shader, com `pow(color.rgb, vec3(1.0)/2.2)` — igual ao código base.
Um swapchain `_SRGB` aplicaria a correção duas vezes, e a imagem ficaria lavada.

## Diferenças de interface em relação ao código base

Quase tudo foi preservado. As três exceções:

1. `DrawVirtualObject()` recebe um parâmetro a mais, o `VkCommandBuffer` onde
   os comandos serão gravados. Em Vulkan não existe um "estado global" para
   onde desenhar.

2. `SceneObject::rendering_mode` agora é um `VkPrimitiveTopology`. Atenção: em
   Vulkan a topologia faz parte do *pipeline*, e não do comando de desenho —
   para desenhar linhas ou pontos é preciso criar um segundo `VkPipeline`.

3. Os arquivos de shader mudaram de extensão (`.glsl` → `.vert` / `.frag`),
   porque o `glslc` usa a extensão para descobrir o estágio do shader.

O atalho **`R`**, que recarrega os shaders sem reiniciar o programa, continua
funcionando: ele chama o `glslc` em tempo de execução e depois recria o
pipeline.

## Controles

| Tecla / mouse | Ação |
| --- | --- |
| Botão esquerdo + arrastar | Gira a câmera look-at |
| Roda do mouse | Aproxima/afasta a câmera (zoom) |
| `X`, `Y`, `Z` (com `Shift` para inverter) | Ângulos de Euler |
| `Espaço` | Zera os ângulos e posições |
| `P` / `O` | Projeção perspectiva / ortográfica |
| `H` | Mostra/esconde o texto informativo |
| `R` | Recarrega os shaders |
| `ESC` | Fecha a janela |

## Compilação

Veja [COMPILACAO.md](COMPILACAO.md). Resumo: instale o Vulkan SDK e rode

```bash
cmake --workflow --preset configure-build-run
```

## Créditos e licenças

- Código base original: Prof. Eduardo Gastal, INF/UFRGS —
  <https://github.com/cgvis-inf-ufrgs/cgvis-base-trabalho-final>
- Texturas em `data/`: veja [data/FONTES.txt](data/FONTES.txt) (Poly Haven, CC0)
- Bibliotecas de terceiros incluídas: GLFW, GLM, `stb_image`,
  `tinyobjloader`, `dejavufont` (atlas gerado com `freetype-gl`)
