//     Universidade Federal do Rio Grande do Sul
//             Instituto de Informática
//       Departamento de Informática Aplicada
//
//    INF01047 Computação Gráfica e Visualização I
//               Prof. Eduardo Gastal
//
//     CÓDIGO BASE PARA O TRABALHO FINAL -- VERSÃO VULKAN
//
// Esta é uma adaptação do código base da disciplina (originalmente escrito em
// OpenGL 3.3) para a API gráfica Vulkan. O objetivo foi manter a ESTRUTURA e a
// DIDÁTICA do código original: os nomes das funções, a ordem em que elas são
// chamadas, os comentários e as referências aos slides da disciplina foram
// preservados sempre que possível.
//
// O que mudou, em resumo:
//
//   * Todo o "boilerplate" do Vulkan (escolha da GPU, swapchain, render pass,
//     sincronização, alocação de memória) foi isolado em "src/vkcontext.cpp",
//     para que este arquivo continue legível.
//
//   * Os shaders GLSL agora são compilados para SPIR-V pelo compilador glslc
//     (isso é feito automaticamente pelo CMake), e não mais pelo driver.
//
//   * O "programa de GPU" do OpenGL virou um "pipeline gráfico" (VkPipeline),
//     que também engloba estados que no OpenGL eram globais (teste de
//     profundidade, backface culling, blending, ...).
//
//   * As chamadas glUniform*() viraram "push constants" (para dados que mudam
//     a cada objeto) e um Uniform Buffer Object (para as matrizes de câmera e
//     projeção, que mudam uma vez por quadro).
//
//   * O cubo NDC do Vulkan é diferente do de OpenGL. Isso é resolvido pela
//     matriz Matrix_Vulkan_Clip(), definida em "include/matrices.h". As
//     matrizes de projeção vistas em aula continuam exatamente as mesmas.
//
// IMPORTANTE: assim como no código base original, as matrizes Model, View e
// Projection são implementadas manualmente em "include/matrices.h". Nenhuma
// função pronta de biblioteca (glm::lookAt(), glm::perspective(), ...) é
// utilizada, conforme exige o enunciado do trabalho final.
//

// Arquivos "headers" padrões de C podem ser incluídos em um
// programa C++, sendo necessário somente adicionar o caractere
// "c" antes de seu nome, e remover o sufixo ".h". Exemplo:
//    #include <stdio.h> // Em C
//  vira
//    #include <cstdio> // Em C++
//
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

// Headers abaixo são específicos de C++
#include <set>
#include <map>
#include <stack>
#include <string>
#include <vector>
#include <limits>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

// Headers das bibliotecas Vulkan e GLFW.
//
// GLFW_INCLUDE_VULKAN faz com que a GLFW inclua <vulkan/vulkan.h> por nós e
// exponha as funções glfwCreateWindowSurface() e
// glfwGetRequiredInstanceExtensions().
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>  // Criação de janelas do sistema operacional

// Headers da biblioteca GLM: criação de matrizes e vetores.
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Headers da biblioteca para carregar modelos obj
#include <tiny_obj_loader.h>

#include <stb_image.h>

// Headers locais, definidos na pasta "include/"
#include "utils.h"
#include "matrices.h"
#include "vkcontext.h"
#include "collisions.h"

// Estrutura que representa um modelo geométrico carregado a partir de um
// arquivo ".obj". Veja https://en.wikipedia.org/wiki/Wavefront_.obj_file .
struct ObjModel
{
    tinyobj::attrib_t                 attrib;
    std::vector<tinyobj::shape_t>     shapes;
    std::vector<tinyobj::material_t>  materials;

    // Este construtor lê o modelo de um arquivo utilizando a biblioteca tinyobjloader.
    // Veja: https://github.com/syoyo/tinyobjloader
    ObjModel(const char* filename, const char* basepath = NULL, bool triangulate = true)
    {
        printf("Carregando objetos do arquivo \"%s\"...\n", filename);

        // Se basepath == NULL, então setamos basepath como o dirname do
        // filename, para que os arquivos MTL sejam corretamente carregados caso
        // estejam no mesmo diretório dos arquivos OBJ.
        std::string fullpath(filename);
        std::string dirname;
        if (basepath == NULL)
        {
            auto i = fullpath.find_last_of("/");
            if (i != std::string::npos)
            {
                dirname = fullpath.substr(0, i+1);
                basepath = dirname.c_str();
            }
        }

        std::string warn;
        std::string err;
        bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename, basepath, triangulate);

        if (!err.empty())
            fprintf(stderr, "\n%s\n", err.c_str());

        if (!ret)
            throw std::runtime_error("Erro ao carregar modelo.");

        for (size_t shape = 0; shape < shapes.size(); ++shape)
        {
            if (shapes[shape].name.empty())
            {
                fprintf(stderr,
                        "*********************************************\n"
                        "Erro: Objeto sem nome dentro do arquivo '%s'.\n"
                        "Veja https://www.inf.ufrgs.br/~eslgastal/fcg-faq-etc.html#Modelos-3D-no-formato-OBJ .\n"
                        "*********************************************\n",
                    filename);
                throw std::runtime_error("Objeto sem nome.");
            }
            printf("- Objeto '%s'\n", shapes[shape].name.c_str());
        }

        printf("OK.\n");
    }
};

// ===========================================================================
// Estruturas auxiliares específicas da versão Vulkan
// ===========================================================================

// Conjunto de buffers de um modelo geométrico na memória da GPU. É o
// equivalente, em Vulkan, do "Vertex Array Object" (VAO) do OpenGL: ele agrupa
// os buffers de atributos (posições, normais, coordenadas de textura) e o
// buffer de índices de um mesmo arquivo ".obj".
//
// Diferente do OpenGL, o Vulkan não possui um objeto que guarde esse
// agrupamento: os buffers são ligados explicitamente a cada desenho, pela
// função DrawVirtualObject().
struct GpuModel
{
    VkBuffer       position_buffer;
    VkDeviceMemory position_memory;

    VkBuffer       normal_buffer;
    VkDeviceMemory normal_memory;

    VkBuffer       texcoord_buffer;
    VkDeviceMemory texcoord_memory;

    VkBuffer       index_buffer;
    VkDeviceMemory index_memory;
};

// Uma textura carregada na GPU.
struct Texture
{
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkSampler      sampler;
    uint32_t       mip_levels;
};

// Dados enviados para a GPU uma vez por QUADRO, através de um Uniform Buffer
// Object. Precisa ter exatamente o mesmo leiaute do bloco "SceneUniforms"
// declarado em "shader_vertex.vert" e "shader_fragment.frag".
struct SceneUniforms
{
    glm::mat4 view;
    glm::mat4 projection;
};

// Dados enviados para a GPU a cada OBJETO desenhado, através de "push
// constants". Substituem as chamadas glUniformMatrix4fv() / glUniform1i() /
// glUniform4f() que o código base fazia antes de cada DrawVirtualObject().
//
// Push constants são limitadas a, no mínimo, 128 bytes garantidos pela
// especificação do Vulkan -- por isso só colocamos aqui o que realmente muda a
// cada objeto.
//
// ATENÇÃO: precisa ter exatamente o mesmo leiaute do bloco "ObjectConstants"
// declarado em "shader_vertex.vert" e "shader_fragment.frag".
struct ObjectConstants
{
    glm::mat4 model;      // offset  0, 64 bytes
    glm::vec4 bbox_min;   // offset 64, 16 bytes
    glm::vec4 bbox_max;   // offset 80, 16 bytes
    int       object_id;  // offset 96,  4 bytes
};

// Declaração de funções utilizadas para pilha de matrizes de modelagem.
void PushMatrix(glm::mat4 M);
void PopMatrix(glm::mat4& M);

// Declaração de várias funções utilizadas em main().  Essas estão definidas
// logo após a definição de main() neste arquivo.
void BuildTrianglesAndAddToVirtualScene(ObjModel*); // Constrói representação de um ObjModel como malha de triângulos para renderização
void ComputeNormals(ObjModel* model); // Computa normais de um ObjModel, caso não existam.
void LoadShadersFromFiles(bool recompile_glsl = false); // Carrega os shaders de vértice e fragmento, criando um pipeline gráfico
void LoadTextureImage(const char* filename); // Função que carrega imagens de textura
void DrawVirtualObject(VkCommandBuffer command_buffer, const char* object_name); // Desenha um objeto armazenado em g_VirtualScene
void PrintObjModelInfo(ObjModel*); // Função para debugging

// Funções de inicialização e destruição dos recursos Vulkan da aplicação.
void CreateDescriptorSetLayout();
void CreateUniformBuffers();
void CreateFallbackTexture();
void CreateDescriptorPoolAndSets();
void CreateGraphicsPipeline();
void DestroyGraphicsPipeline();
void DestroyApplicationResources();

// Declaração de funções auxiliares para renderizar texto dentro da janela.
// Estas funções estão definidas no arquivo "textrendering.cpp".
void TextRendering_Init();
void TextRendering_Destroy();
float TextRendering_LineHeight(GLFWwindow* window);
float TextRendering_CharWidth(GLFWwindow* window);
void TextRendering_PrintString(GLFWwindow* window, const std::string &str, float x, float y, float scale = 1.0f);
void TextRendering_PrintMatrix(GLFWwindow* window, glm::mat4 M, float x, float y, float scale = 1.0f);
void TextRendering_PrintVector(GLFWwindow* window, glm::vec4 v, float x, float y, float scale = 1.0f);
void TextRendering_PrintMatrixVectorProduct(GLFWwindow* window, glm::mat4 M, glm::vec4 v, float x, float y, float scale = 1.0f);
void TextRendering_PrintMatrixVectorProductMoreDigits(GLFWwindow* window, glm::mat4 M, glm::vec4 v, float x, float y, float scale = 1.0f);
void TextRendering_PrintMatrixVectorProductDivW(GLFWwindow* window, glm::mat4 M, glm::vec4 v, float x, float y, float scale = 1.0f);

// Funções abaixo renderizam como texto na janela algumas matrizes e outras
// informações do programa. Definidas após main().
void TextRendering_ShowModelViewProjection(GLFWwindow* window, glm::mat4 projection, glm::mat4 view, glm::mat4 model, glm::vec4 p_model);
void TextRendering_ShowEulerAngles(GLFWwindow* window);
void TextRendering_ShowProjection(GLFWwindow* window);
void TextRendering_ShowFramesPerSecond(GLFWwindow* window);

// Funções callback para comunicação com o sistema operacional e interação do
// usuário. Veja mais comentários nas definições das mesmas, abaixo.
void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
void ErrorCallback(int error, const char* description);
void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mode);
void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);

// Definimos uma estrutura que armazenará dados necessários para renderizar
// cada objeto da cena virtual.
struct SceneObject
{
    std::string  name;        // Nome do objeto
    size_t       first_index; // Índice do primeiro vértice dentro do vetor indices[] definido em BuildTrianglesAndAddToVirtualScene()
    size_t       num_indices; // Número de índices do objeto dentro do vetor indices[] definido em BuildTrianglesAndAddToVirtualScene()

    // Modo de rasterização. Em Vulkan, a topologia faz parte do pipeline
    // gráfico, e não é um parâmetro do comando de desenho. Portanto, se você
    // quiser desenhar linhas ou pontos, será necessário criar um SEGUNDO
    // pipeline com outra topologia e ligá-lo antes do desenho.
    VkPrimitiveTopology rendering_mode;

    GpuModel*    gpu_model;   // Buffers deste modelo na GPU ("VAO" do OpenGL)

    glm::vec3    bbox_min;    // Axis-Aligned Bounding Box do objeto
    glm::vec3    bbox_max;
};

// Abaixo definimos variáveis globais utilizadas em várias funções do código.

// A cena virtual é uma lista de objetos nomeados, guardados em um dicionário
// (map).  Veja dentro da função BuildTrianglesAndAddToVirtualScene() como que são incluídos
// objetos dentro da variável g_VirtualScene, e veja na função main() como
// estes são acessados.
std::map<std::string, SceneObject> g_VirtualScene;

// Lista de todos os conjuntos de buffers criados, para que possamos liberá-los
// no encerramento do programa.
std::vector<GpuModel*> g_GpuModels;

// Pilha que guardará as matrizes de modelagem.
std::stack<glm::mat4>  g_MatrixStack;

// Razão de proporção da janela (largura/altura). Veja função FramebufferSizeCallback().
float g_ScreenRatio = 1.0f;

// Ângulos de Euler que controlam a rotação de um dos cubos da cena virtual
float g_AngleX = 0.0f;
float g_AngleY = 0.0f;
float g_AngleZ = 0.0f;

// "g_LeftMouseButtonPressed = true" se o usuário está com o botão esquerdo do mouse
// pressionado no momento atual. Veja função MouseButtonCallback().
bool g_LeftMouseButtonPressed = false;
bool g_RightMouseButtonPressed = false; // Análogo para botão direito do mouse
bool g_MiddleMouseButtonPressed = false; // Análogo para botão do meio do mouse

// Variáveis que definem a câmera em coordenadas esféricas, controladas pelo
// usuário através do mouse (veja função CursorPosCallback()). A posição
// efetiva da câmera é calculada dentro da função main(), dentro do loop de
// renderização.
float g_CameraTheta = 0.0f; // Ângulo no plano ZX em relação ao eixo Z
float g_CameraPhi = 0.0f;   // Ângulo em relação ao eixo Y
float g_CameraDistance = 3.5f; // Distância da câmera para a origem

// Variáveis que controlam rotação do antebraço
float g_ForearmAngleZ = 0.0f;
float g_ForearmAngleX = 0.0f;

// Variáveis que controlam translação do torso
float g_TorsoPositionX = 0.0f;
float g_TorsoPositionY = 0.0f;

// Variável que controla o tipo de projeção utilizada: perspectiva ou ortográfica.
bool g_UsePerspectiveProjection = true;

// Variável que controla se o texto informativo será mostrado na tela.
bool g_ShowInfoText = true;

// ===========================================================================
// Variáveis globais que definem o "programa de GPU" na versão Vulkan
// ===========================================================================

// O pipeline gráfico é o equivalente do "GPU program" (shader program) do
// OpenGL. Além dos shaders, ele também fixa estados que no OpenGL eram globais
// e mutáveis: teste de profundidade, backface culling, blending, topologia dos
// primitivos, e o formato dos atributos de vértice.
VkPipeline            g_GpuProgramID          = VK_NULL_HANDLE;
VkPipelineLayout      g_PipelineLayout        = VK_NULL_HANDLE;
VkDescriptorSetLayout g_DescriptorSetLayout   = VK_NULL_HANDLE;
VkDescriptorPool      g_DescriptorPool        = VK_NULL_HANDLE;
VkDescriptorSet       g_DescriptorSets[MAX_FRAMES_IN_FLIGHT] = {};

// Uniform Buffer Objects com as matrizes "view" e "projection". Existe um por
// quadro "em voo", pois a CPU pode estar escrevendo o quadro N+1 enquanto a
// GPU ainda lê o quadro N. Ficam permanentemente mapeados na memória da CPU.
VkBuffer       g_UniformBuffers[MAX_FRAMES_IN_FLIGHT]        = {};
VkDeviceMemory g_UniformBufferMemories[MAX_FRAMES_IN_FLIGHT] = {};
void*          g_UniformBuffersMapped[MAX_FRAMES_IN_FLIGHT]  = {};

// Dados que serão enviados para a GPU a cada objeto desenhado. Preenchemos o
// campo "model" e "object_id" dentro de main(), da mesma forma que o código
// base chamava glUniformMatrix4fv() e glUniform1i() antes de cada desenho; os
// campos "bbox_*" são preenchidos dentro de DrawVirtualObject().
ObjectConstants g_ObjectConstants = {};

// Número máximo de texturas suportadas pelo Fragment Shader (TextureImage0,
// TextureImage1 e TextureImage2). Se você precisar de mais texturas, adicione
// novos "binding"s em "shader_fragment.frag", em CreateDescriptorSetLayout() e
// em CreateDescriptorPoolAndSets().
#define MAX_TEXTURES 3

Texture  g_Textures[MAX_TEXTURES] = {};

// Número de texturas carregadas pela função LoadTextureImage()
uint32_t g_NumLoadedTextures = 0;

// Textura 1x1 branca, usada para preencher os "bindings" de textura que não
// foram carregados. Ao contrário do OpenGL, o Vulkan exige que TODOS os
// descritores declarados pelo shader estejam preenchidos com recursos válidos.
Texture  g_FallbackTexture = {};

int main(int argc, char* argv[])
{
    // Inicializamos a biblioteca GLFW, utilizada para criar uma janela do
    // sistema operacional, onde poderemos renderizar com Vulkan.
    int success = glfwInit();
    if (!success)
    {
        fprintf(stderr, "ERROR: glfwInit() failed.\n");
        std::exit(EXIT_FAILURE);
    }

    // Definimos o callback para impressão de erros da GLFW no terminal
    glfwSetErrorCallback(ErrorCallback);

    // Ao contrário do código base em OpenGL, aqui pedimos para a GLFW NÃO criar
    // nenhum contexto gráfico: o contexto do Vulkan é criado explicitamente por
    // nós, em VulkanContext_Init().
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    // Criamos uma janela do sistema operacional, com 800 colunas e 600 linhas
    // de pixels, e com título "INF01047 ...".
    GLFWwindow* window;
    window = glfwCreateWindow(800, 600, "INF01047 - Seu Cartao - Seu Nome", NULL, NULL);
    if (!window)
    {
        glfwTerminate();
        fprintf(stderr, "ERROR: glfwCreateWindow() failed.\n");
        std::exit(EXIT_FAILURE);
    }

    // Definimos a função de callback que será chamada sempre que o usuário
    // pressionar alguma tecla do teclado ...
    glfwSetKeyCallback(window, KeyCallback);
    // ... ou clicar os botões do mouse ...
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    // ... ou movimentar o cursor do mouse em cima da janela ...
    glfwSetCursorPosCallback(window, CursorPosCallback);
    // ... ou rolar a "rodinha" do mouse.
    glfwSetScrollCallback(window, ScrollCallback);

    // Definimos a função de callback que será chamada sempre que a janela for
    // redimensionada, por consequência alterando o tamanho do "framebuffer"
    // (região de memória onde são armazenados os pixels da imagem).
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
    FramebufferSizeCallback(window, 800, 600); // Forçamos a chamada do callback acima, para definir g_ScreenRatio.

    // Criamos toda a infraestrutura do Vulkan: instância, dispositivo lógico,
    // swapchain, render pass, command buffers e objetos de sincronização.
    //
    // A camada de validação (que verifica se estamos usando a API corretamente)
    // é habilitada somente em builds de Debug, pois ela custa desempenho.
#ifdef NDEBUG
    const bool enable_validation = false;
#else
    const bool enable_validation = true;
#endif
    VulkanContext_Init(window, enable_validation);

    // Imprimimos no terminal informações sobre a GPU do sistema
    uint32_t api = g_Vk.physical_device_properties.apiVersion;
    printf("GPU: %s, Vulkan %d.%d.%d\n",
           g_Vk.physical_device_properties.deviceName,
           (int)VK_VERSION_MAJOR(api), (int)VK_VERSION_MINOR(api), (int)VK_VERSION_PATCH(api));

    // Descrevemos quais recursos (uniformes e texturas) os shaders esperam
    // receber. No OpenGL isso era descoberto automaticamente pelo driver,
    // através de glGetUniformLocation().
    CreateDescriptorSetLayout();
    CreateUniformBuffers();

    // Carregamos os shaders de vértices e de fragmentos que serão utilizados
    // para renderização, criando o pipeline gráfico.
    LoadShadersFromFiles();

    // Textura de reserva, usada nos "bindings" de textura não utilizados.
    CreateFallbackTexture();

    // Carregamos duas imagens para serem utilizadas como textura
    LoadTextureImage("../../data/red_brick_diff_1k.jpg");        // TextureImage0
    LoadTextureImage("../../data/rocky_terrain_02_diff_1k.jpg"); // TextureImage1

    // Só agora, com as texturas já carregadas, podemos criar os descriptor sets
    // que as ligam ao Fragment Shader.
    CreateDescriptorPoolAndSets();

    // Construímos a representação de objetos geométricos através de malhas de triângulos
    ObjModel spheremodel("../../data/sphere.obj");
    ComputeNormals(&spheremodel);
    BuildTrianglesAndAddToVirtualScene(&spheremodel);

    ObjModel bunnymodel("../../data/bunny.obj");
    ComputeNormals(&bunnymodel);
    BuildTrianglesAndAddToVirtualScene(&bunnymodel);

    ObjModel planemodel("../../data/plane.obj");
    ComputeNormals(&planemodel);
    BuildTrianglesAndAddToVirtualScene(&planemodel);

    if ( argc > 1 )
    {
        ObjModel model(argv[1]);
        ComputeNormals(&model);
        BuildTrianglesAndAddToVirtualScene(&model);
    }

    // Inicializamos o código para renderização de texto.
    TextRendering_Init();

    // NOTE: no código base em OpenGL, era aqui que apareciam as chamadas
    //
    //     glEnable(GL_DEPTH_TEST);              // Z-buffer
    //     glEnable(GL_CULL_FACE);               // Backface Culling
    //     glCullFace(GL_BACK);
    //     glFrontFace(GL_CCW);
    //
    // Em Vulkan, esses estados NÃO são globais: eles fazem parte do pipeline
    // gráfico, e portanto são definidos na função CreateGraphicsPipeline(),
    // logo abaixo. Veja lá os comentários sobre o Z-buffer (slides 104-116 do
    // documento Aula_09_Projecoes.pdf) e sobre o Backface Culling (slides 8-13
    // do documento Aula_02_Fundamentos_Matematicos.pdf, slides 23-34 do
    // documento Aula_13_Clipping_and_Culling.pdf e slides 112-123 do documento
    // Aula_14_Laboratorio_3_Revisao.pdf).

    // Ficamos em um loop infinito, renderizando, até que o usuário feche a janela
    while (!glfwWindowShouldClose(window))
    {
        // Aqui executamos as operações de renderização

        // Definimos a cor do "fundo" do framebuffer como branco.  Tal cor é
        // definida como coeficientes RGBA: Red, Green, Blue, Alpha; isto é:
        // Vermelho, Verde, Azul, Alpha (valor de transparência).
        // Conversaremos sobre sistemas de cores nas aulas de Modelos de Iluminação.
        //
        //                              R     G     B     A
        const float clear_color[4] = { 0.9f, 0.9f, 1.0f, 1.0f };

        // VulkanContext_BeginFrame() faz o papel que, no OpenGL, era feito por
        // glClearColor() + glClear(): ele adquire a imagem onde vamos desenhar
        // e limpa a cor e o Z-buffer. Ele também nos devolve o "command buffer"
        // onde os comandos de desenho deste quadro serão gravados.
        //
        // Ele retorna "false" quando o quadro precisa ser pulado (por exemplo,
        // logo depois de a janela ter sido redimensionada).
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;

        if (!VulkanContext_BeginFrame(window, clear_color, &command_buffer))
        {
            glfwPollEvents();
            continue;
        }

        // Pedimos para a GPU utilizar o pipeline gráfico criado acima (contendo
        // os shaders de vértice e fragmentos). Equivalente ao glUseProgram().
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_GpuProgramID);

        // Computamos a posição da câmera utilizando coordenadas esféricas.  As
        // variáveis g_CameraDistance, g_CameraPhi, e g_CameraTheta são
        // controladas pelo mouse do usuário. Veja as funções CursorPosCallback()
        // e ScrollCallback().
        float r = g_CameraDistance;
        float y = r*sin(g_CameraPhi);
        float z = r*cos(g_CameraPhi)*cos(g_CameraTheta);
        float x = r*cos(g_CameraPhi)*sin(g_CameraTheta);

        // Abaixo definimos as varáveis que efetivamente definem a câmera virtual.
        // Veja slides 195-227 e 229-234 do documento Aula_08_Sistemas_de_Coordenadas.pdf.
        glm::vec4 camera_position_c  = glm::vec4(x,y,z,1.0f); // Ponto "c", centro da câmera
        glm::vec4 camera_lookat_l    = glm::vec4(0.0f,0.0f,0.0f,1.0f); // Ponto "l", para onde a câmera (look-at) estará sempre olhando
        glm::vec4 camera_view_vector = camera_lookat_l - camera_position_c; // Vetor "view", sentido para onde a câmera está virada
        glm::vec4 camera_up_vector   = glm::vec4(0.0f,1.0f,0.0f,0.0f); // Vetor "up" fixado para apontar para o "céu" (eito Y global)

        // Computamos a matriz "View" utilizando os parâmetros da câmera para
        // definir o sistema de coordenadas da câmera.  Veja slides 2-14, 184-190 e 236-242 do documento Aula_08_Sistemas_de_Coordenadas.pdf.
        glm::mat4 view = Matrix_Camera_View(camera_position_c, camera_view_vector, camera_up_vector);

        // Agora computamos a matriz de Projeção.
        glm::mat4 projection;

        // Note que, no sistema de coordenadas da câmera, os planos near e far
        // estão no sentido negativo! Veja slides 176-204 do documento Aula_09_Projecoes.pdf.
        float nearplane = -0.1f;  // Posição do "near plane"
        float farplane  = -10.0f; // Posição do "far plane"

        if (g_UsePerspectiveProjection)
        {
            // Projeção Perspectiva.
            // Para definição do field of view (FOV), veja slides 205-215 do documento Aula_09_Projecoes.pdf.
            float field_of_view = 3.141592 / 3.0f;
            projection = Matrix_Perspective(field_of_view, g_ScreenRatio, nearplane, farplane);
        }
        else
        {
            // Projeção Ortográfica.
            // Para definição dos valores l, r, b, t ("left", "right", "bottom", "top"),
            // PARA PROJEÇÃO ORTOGRÁFICA veja slides 219-224 do documento Aula_09_Projecoes.pdf.
            // Para simular um "zoom" ortográfico, computamos o valor de "t"
            // utilizando a variável g_CameraDistance.
            float t = 1.5f*g_CameraDistance/2.5f;
            float b = -t;
            float r = t*g_ScreenRatio;
            float l = -r;
            projection = Matrix_Orthographic(l, r, b, t, nearplane, farplane);
        }

        // As matrizes acima produzem coordenadas no cubo NDC do OpenGL, que é o
        // que vimos em aula. A matriz abaixo converte esse resultado para o cubo
        // NDC do Vulkan (eixo Y invertido e profundidade em [0,1]). Veja a
        // explicação completa em Matrix_Vulkan_Clip(), no arquivo
        // "include/matrices.h".
        projection = Matrix_Vulkan_Clip() * projection;

        glm::mat4 model = Matrix_Identity(); // Transformação identidade de modelagem

        // Enviamos as matrizes "view" e "projection" para a placa de vídeo
        // (GPU). Veja o arquivo "shader_vertex.vert", onde estas são
        // efetivamente aplicadas em todos os pontos.
        //
        // No OpenGL isso era feito com duas chamadas glUniformMatrix4fv(). Em
        // Vulkan, escrevemos os dados diretamente no Uniform Buffer Object
        // deste quadro, que está permanentemente mapeado na memória da CPU.
        SceneUniforms scene_uniforms;
        scene_uniforms.view       = view;
        scene_uniforms.projection = projection;

        memcpy(g_UniformBuffersMapped[g_Vk.current_frame], &scene_uniforms, sizeof(scene_uniforms));

        // Ligamos o descriptor set deste quadro, que dá aos shaders acesso ao
        // Uniform Buffer Object acima e às imagens de textura.
        vkCmdBindDescriptorSets(
            command_buffer,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            g_PipelineLayout,
            0, 1, &g_DescriptorSets[g_Vk.current_frame],
            0, NULL
        );

        #define SPHERE 0
        #define BUNNY  1
        #define PLANE  2

        // Desenhamos o modelo da esfera
        model = Matrix_Translate(-1.0f,0.0f,0.0f)
              * Matrix_Rotate_Z(0.6f)
              * Matrix_Rotate_X(0.2f)
              * Matrix_Rotate_Y(g_AngleY + (float)glfwGetTime() * 0.1f);
        g_ObjectConstants.model     = model;
        g_ObjectConstants.object_id = SPHERE;
        DrawVirtualObject(command_buffer, "the_sphere");

        // Desenhamos o modelo do coelho
        model = Matrix_Translate(1.0f,0.0f,0.0f)
              * Matrix_Rotate_X(g_AngleX + (float)glfwGetTime() * 0.1f);
        g_ObjectConstants.model     = model;
        g_ObjectConstants.object_id = BUNNY;
        DrawVirtualObject(command_buffer, "the_bunny");

        // Desenhamos o plano do chão
        model = Matrix_Translate(0.0f,-1.1f,0.0f);
        g_ObjectConstants.model     = model;
        g_ObjectConstants.object_id = PLANE;
        DrawVirtualObject(command_buffer, "the_plane");

        // Imprimimos na tela os ângulos de Euler que controlam a rotação do
        // terceiro cubo.
        TextRendering_ShowEulerAngles(window);

        // Imprimimos na informação sobre a matriz de projeção sendo utilizada.
        TextRendering_ShowProjection(window);

        // Imprimimos na tela informação sobre o número de quadros renderizados
        // por segundo (frames per second).
        TextRendering_ShowFramesPerSecond(window);

        // O framebuffer onde a GPU executa as operações de renderização não
        // é o mesmo que está sendo mostrado para o usuário, caso contrário
        // seria possível ver artefatos conhecidos como "screen tearing". A
        // chamada abaixo submete os comandos gravados acima para a GPU e pede
        // para que a imagem resultante seja apresentada na tela. É o
        // equivalente ao glfwSwapBuffers() do código base.
        // Veja o link: https://en.wikipedia.org/w/index.php?title=Multiple_buffering&oldid=793452829#Double_buffering_in_computer_graphics
        VulkanContext_EndFrame(window);

        // Verificamos com o sistema operacional se houve alguma interação do
        // usuário (teclado, mouse, ...). Caso positivo, as funções de callback
        // definidas anteriormente usando glfwSet*Callback() serão chamadas
        // pela biblioteca GLFW.
        glfwPollEvents();
    }

    // Finalizamos o uso dos recursos do sistema operacional e da GPU.
    //
    // Diferente do OpenGL, onde o driver liberava tudo automaticamente ao
    // destruir o contexto, em Vulkan cada objeto criado precisa ser destruído
    // explicitamente -- e na ordem inversa da criação.
    DestroyApplicationResources();
    VulkanContext_Destroy();

    glfwDestroyWindow(window);
    glfwTerminate();

    // Fim do programa
    return 0;
}

// ===========================================================================
// Texturas
// ===========================================================================

// Cria uma textura 1x1 totalmente branca. Ela é usada para preencher os
// "bindings" de textura declarados pelo Fragment Shader que não receberam uma
// imagem de verdade. O Vulkan, ao contrário do OpenGL, considera um erro deixar
// um descritor declarado pelo shader sem nenhum recurso associado.
void CreateFallbackTexture()
{
    const unsigned char white[4] = { 255, 255, 255, 255 };

    VkBuffer       staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;

    Vulkan_CreateBuffer(
        sizeof(white),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging_buffer,
        &staging_memory
    );

    void* mapped = NULL;
    VK_CHECK(vkMapMemory(g_Vk.device, staging_memory, 0, sizeof(white), 0, &mapped));
    memcpy(mapped, white, sizeof(white));
    vkUnmapMemory(g_Vk.device, staging_memory);

    Vulkan_CreateImage(
        1, 1, 1,
        VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_FallbackTexture.image,
        &g_FallbackTexture.memory
    );

    Vulkan_TransitionImageLayout(g_FallbackTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    Vulkan_CopyBufferToImage(staging_buffer, g_FallbackTexture.image, 1, 1);
    Vulkan_TransitionImageLayout(g_FallbackTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

    vkDestroyBuffer(g_Vk.device, staging_buffer, NULL);
    vkFreeMemory(g_Vk.device, staging_memory, NULL);

    g_FallbackTexture.mip_levels = 1;
    g_FallbackTexture.view = Vulkan_CreateImageView(g_FallbackTexture.image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 1);

    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter    = VK_FILTER_LINEAR;
    sampler_info.minFilter    = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.borderColor  = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.compareOp    = VK_COMPARE_OP_ALWAYS;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.maxLod        = 0.0f;

    VK_CHECK(vkCreateSampler(g_Vk.device, &sampler_info, NULL, &g_FallbackTexture.sampler));
}

// Função que carrega uma imagem para ser utilizada como textura
void LoadTextureImage(const char* filename)
{
    if (g_NumLoadedTextures >= MAX_TEXTURES)
    {
        fprintf(stderr,
            "ERROR: Número máximo de texturas (%d) atingido ao carregar \"%s\".\n"
            "       Adicione novos \"binding\"s em \"shader_fragment.frag\",\n"
            "       em CreateDescriptorSetLayout() e em CreateDescriptorPoolAndSets().\n",
            MAX_TEXTURES, filename);
        std::exit(EXIT_FAILURE);
    }

    printf("Carregando imagem \"%s\"... ", filename);

    // Primeiro fazemos a leitura da imagem do disco
    stbi_set_flip_vertically_on_load(true);
    int width;
    int height;
    int channels;
    // NOTE: pedimos 4 canais (RGBA), e não 3 como no código base em OpenGL.
    // O Vulkan não obriga os drivers a suportarem formatos de 3 canais como
    // textura, então usamos sempre RGBA.
    unsigned char *data = stbi_load(filename, &width, &height, &channels, 4);

    if ( data == NULL )
    {
        fprintf(stderr, "ERROR: Cannot open image file \"%s\".\n", filename);
        std::exit(EXIT_FAILURE);
    }

    printf("OK (%dx%d).\n", width, height);

    Texture& texture = g_Textures[g_NumLoadedTextures];

    // O formato _SRGB faz com que a GPU converta automaticamente de sRGB para
    // espaço linear quando o shader lê a textura. É o equivalente ao formato
    // interno GL_SRGB8 usado pelo código base.
    const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;

    // Número de níveis de mipmap. Equivalente ao glGenerateMipmap() do código
    // base, que gerava todos os níveis até 1x1.
    uint32_t mip_levels = 1;
    if (Vulkan_FormatSupportsLinearBlit(format))
    {
        int max_dimension = (width > height) ? width : height;
        mip_levels = (uint32_t)(std::floor(std::log2((double)max_dimension))) + 1;
    }
    else
    {
        fprintf(stderr,
            "WARNING: A GPU não suporta geração de mipmaps para este formato.\n"
            "         A textura \"%s\" será usada sem mipmaps.\n", filename);
    }

    texture.mip_levels = mip_levels;

    VkDeviceSize image_size = (VkDeviceSize)width * (VkDeviceSize)height * 4;

    // 1) Copiamos os pixels lidos do disco para um buffer visível pela CPU.
    VkBuffer       staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;

    Vulkan_CreateBuffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging_buffer,
        &staging_memory
    );

    void* mapped = NULL;
    VK_CHECK(vkMapMemory(g_Vk.device, staging_memory, 0, image_size, 0, &mapped));
    memcpy(mapped, data, (size_t)image_size);
    vkUnmapMemory(g_Vk.device, staging_memory);

    stbi_image_free(data);

    // 2) Criamos a imagem na memória da GPU. Ela precisa de TRANSFER_SRC porque
    //    a geração de mipmaps lê de um nível para escrever no seguinte.
    Vulkan_CreateImage(
        (uint32_t)width,
        (uint32_t)height,
        mip_levels,
        format,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &texture.image,
        &texture.memory
    );

    // 3) Agora enviamos a imagem lida do disco para a GPU.
    Vulkan_TransitionImageLayout(texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mip_levels);
    Vulkan_CopyBufferToImage(staging_buffer, texture.image, (uint32_t)width, (uint32_t)height);

    if (mip_levels > 1)
    {
        // Vulkan_GenerateMipmaps() deixa TODOS os níveis no layout
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ao terminar.
        Vulkan_GenerateMipmaps(texture.image, format, width, height, mip_levels);
    }
    else
    {
        Vulkan_TransitionImageLayout(texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    }

    vkDestroyBuffer(g_Vk.device, staging_buffer, NULL);
    vkFreeMemory(g_Vk.device, staging_memory, NULL);

    texture.view = Vulkan_CreateImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT, mip_levels);

    // 4) Criamos o amostrador (VkSampler), com os mesmos parâmetros que o
    //    código base configurava com glSamplerParameteri():
    //      GL_TEXTURE_WRAP_S / GL_TEXTURE_WRAP_T = GL_CLAMP_TO_EDGE
    //      GL_TEXTURE_MIN_FILTER = GL_LINEAR_MIPMAP_LINEAR
    //      GL_TEXTURE_MAG_FILTER = GL_LINEAR
    //    Veja slides 95-96 do documento Aula_20_Mapeamento_de_Texturas.pdf
    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter     = VK_FILTER_LINEAR;
    sampler_info.minFilter     = VK_FILTER_LINEAR;
    sampler_info.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.borderColor   = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp     = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipLodBias    = 0.0f;
    sampler_info.minLod        = 0.0f;
    sampler_info.maxLod        = (float)mip_levels;

    // Filtragem anisotrópica: melhora bastante a qualidade de texturas vistas
    // em ângulos rasantes, como a do chão.
    if (g_Vk.physical_device_features.samplerAnisotropy)
    {
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy    = g_Vk.physical_device_properties.limits.maxSamplerAnisotropy;
    }
    else
    {
        sampler_info.anisotropyEnable = VK_FALSE;
        sampler_info.maxAnisotropy    = 1.0f;
    }

    VK_CHECK(vkCreateSampler(g_Vk.device, &sampler_info, NULL, &texture.sampler));

    g_NumLoadedTextures += 1;
}

// ===========================================================================
// Descriptor sets e Uniform Buffer Objects
// ===========================================================================

// Descreve quais recursos os shaders esperam receber, e em quais "bindings".
// Esta descrição precisa bater EXATAMENTE com os "layout(set = 0, binding = N)"
// declarados em "shader_vertex.vert" e "shader_fragment.frag".
void CreateDescriptorSetLayout()
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;

    // binding 0: Uniform Buffer Object com as matrizes "view" e "projection".
    // É lido tanto pelo Vertex Shader (para projetar os vértices) quanto pelo
    // Fragment Shader (que usa inverse(view) para achar a posição da câmera).
    VkDescriptorSetLayoutBinding uniform_binding = {};
    uniform_binding.binding         = 0;
    uniform_binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniform_binding.descriptorCount = 1;
    uniform_binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(uniform_binding);

    // bindings 1, 2, 3: TextureImage0, TextureImage1 e TextureImage2.
    for (uint32_t i = 0; i < MAX_TEXTURES; ++i)
    {
        VkDescriptorSetLayoutBinding texture_binding = {};
        texture_binding.binding         = 1 + i;
        texture_binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture_binding.descriptorCount = 1;
        texture_binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(texture_binding);
    }

    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = (uint32_t)bindings.size();
    layout_info.pBindings    = bindings.data();

    VK_CHECK(vkCreateDescriptorSetLayout(g_Vk.device, &layout_info, NULL, &g_DescriptorSetLayout));
}

void CreateUniformBuffers()
{
    VkDeviceSize buffer_size = sizeof(SceneUniforms);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        Vulkan_CreateBuffer(
            buffer_size,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &g_UniformBuffers[i],
            &g_UniformBufferMemories[i]
        );

        // Mantemos o buffer permanentemente mapeado: atualizar as matrizes a
        // cada quadro passa a ser apenas um memcpy().
        VK_CHECK(vkMapMemory(g_Vk.device, g_UniformBufferMemories[i], 0, buffer_size, 0, &g_UniformBuffersMapped[i]));
    }
}

void CreateDescriptorPoolAndSets()
{
    VkDescriptorPoolSize pool_sizes[2] = {};
    pool_sizes[0].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = MAX_FRAMES_IN_FLIGHT;
    pool_sizes[1].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT * MAX_TEXTURES;

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes    = pool_sizes;
    pool_info.maxSets       = MAX_FRAMES_IN_FLIGHT;

    VK_CHECK(vkCreateDescriptorPool(g_Vk.device, &pool_info, NULL, &g_DescriptorPool));

    VkDescriptorSetLayout layouts[MAX_FRAMES_IN_FLIGHT];
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        layouts[i] = g_DescriptorSetLayout;

    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = g_DescriptorPool;
    alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    alloc_info.pSetLayouts        = layouts;

    VK_CHECK(vkAllocateDescriptorSets(g_Vk.device, &alloc_info, g_DescriptorSets));

    // Agora preenchemos cada descriptor set com os recursos de verdade.
    for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; ++frame)
    {
        VkDescriptorBufferInfo buffer_info = {};
        buffer_info.buffer = g_UniformBuffers[frame];
        buffer_info.offset = 0;
        buffer_info.range  = sizeof(SceneUniforms);

        VkDescriptorImageInfo image_infos[MAX_TEXTURES] = {};
        for (uint32_t i = 0; i < MAX_TEXTURES; ++i)
        {
            // Bindings de textura não utilizados recebem a textura branca 1x1.
            // Isso corresponde, no código base, ao fato de "TextureImage2" ser
            // declarado no shader mesmo sem nenhuma imagem ter sido carregada
            // na unidade de textura 2.
            const Texture& source = (i < g_NumLoadedTextures) ? g_Textures[i] : g_FallbackTexture;

            image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_infos[i].imageView   = source.view;
            image_infos[i].sampler     = source.sampler;
        }

        VkWriteDescriptorSet writes[1 + MAX_TEXTURES] = {};

        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = g_DescriptorSets[frame];
        writes[0].dstBinding      = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &buffer_info;

        for (uint32_t i = 0; i < MAX_TEXTURES; ++i)
        {
            writes[1 + i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1 + i].dstSet          = g_DescriptorSets[frame];
            writes[1 + i].dstBinding      = 1 + i;
            writes[1 + i].dstArrayElement = 0;
            writes[1 + i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1 + i].descriptorCount = 1;
            writes[1 + i].pImageInfo      = &image_infos[i];
        }

        vkUpdateDescriptorSets(g_Vk.device, 1 + MAX_TEXTURES, writes, 0, NULL);
    }
}

// ===========================================================================
// Desenho
// ===========================================================================

// Função que desenha um objeto armazenado em g_VirtualScene. Veja definição
// dos objetos na função BuildTrianglesAndAddToVirtualScene().
void DrawVirtualObject(VkCommandBuffer command_buffer, const char* object_name)
{
    std::map<std::string, SceneObject>::iterator it = g_VirtualScene.find(object_name);

    if (it == g_VirtualScene.end())
    {
        fprintf(stderr, "ERROR: O objeto \"%s\" não existe na cena virtual.\n", object_name);
        std::exit(EXIT_FAILURE);
    }

    const SceneObject& object = it->second;

    // "Ligamos" os buffers de atributos deste modelo. No OpenGL, esse
    // agrupamento era guardado em um Vertex Array Object (VAO) e bastava um
    // glBindVertexArray(); em Vulkan, ligamos os três buffers de uma vez.
    //
    // A ordem abaixo corresponde aos "layout (location = N)" declarados em
    // "shader_vertex.vert":
    //   binding 0 -> location 0 -> model_coefficients   (vec4)
    //   binding 1 -> location 1 -> normal_coefficients  (vec4)
    //   binding 2 -> location 2 -> texture_coefficients (vec2)
    VkBuffer     vertex_buffers[3] = {
        object.gpu_model->position_buffer,
        object.gpu_model->normal_buffer,
        object.gpu_model->texcoord_buffer
    };
    VkDeviceSize offsets[3] = { 0, 0, 0 };

    vkCmdBindVertexBuffers(command_buffer, 0, 3, vertex_buffers, offsets);
    vkCmdBindIndexBuffer(command_buffer, object.gpu_model->index_buffer, 0, VK_INDEX_TYPE_UINT32);

    // Setamos as variáveis "bbox_min" e "bbox_max" do fragment shader
    // com os parâmetros da axis-aligned bounding box (AABB) do modelo.
    g_ObjectConstants.bbox_min = glm::vec4(object.bbox_min.x, object.bbox_min.y, object.bbox_min.z, 1.0f);
    g_ObjectConstants.bbox_max = glm::vec4(object.bbox_max.x, object.bbox_max.y, object.bbox_max.z, 1.0f);

    // Enviamos, de uma só vez, a Model matrix, o identificador do objeto e a
    // bounding box. No código base isso eram quatro chamadas glUniform*().
    vkCmdPushConstants(
        command_buffer,
        g_PipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(ObjectConstants),
        &g_ObjectConstants
    );

    // Pedimos para a GPU rasterizar os vértices apontados pelos buffers acima.
    // É o equivalente ao glDrawElements() do código base: o parâmetro
    // "firstIndex" faz o mesmo papel do offset dentro do buffer de índices.
    vkCmdDrawIndexed(
        command_buffer,
        (uint32_t)object.num_indices,
        1,                               // instanceCount
        (uint32_t)object.first_index,    // firstIndex
        0,                               // vertexOffset
        0                                // firstInstance
    );
}

// ===========================================================================
// Pipeline gráfico (o "programa de GPU" da versão OpenGL)
// ===========================================================================

// Tenta compilar um shader GLSL para SPIR-V usando o compilador "glslc", que
// acompanha o Vulkan SDK. Retorna "true" em caso de sucesso.
//
// Esta função existe apenas para manter funcionando o atalho de teclado "R" do
// código base, que recarrega os shaders sem reiniciar o programa. Durante a
// compilação normal do projeto, quem chama o glslc é o CMake.
static bool CompileShaderToSpirv(const char* glsl_filename, const char* spirv_filename)
{
    std::string glslc = "glslc";

    // Se o Vulkan SDK estiver instalado, preferimos o glslc que veio com ele.
    const char* vulkan_sdk = std::getenv("VULKAN_SDK");
    if (vulkan_sdk != NULL)
    {
        std::string candidate = std::string(vulkan_sdk) + "/bin/glslc";
        glslc = candidate;
    }

    std::string command = "\"" + glslc + "\" \"" + glsl_filename + "\" -o \"" + spirv_filename + "\"";

#ifdef _WIN32
    // O cmd.exe do Windows exige um par extra de aspas ao redor de um comando
    // que já contém caminhos entre aspas.
    command = "\"" + command + "\"";
#endif

    int result = std::system(command.c_str());

    return result == 0;
}

// Função que carrega os shaders de vértices e de fragmentos que serão
// utilizados para renderização, criando o pipeline gráfico.
//
// Veja slides 180-200 do documento Aula_03_Rendering_Pipeline_Grafico.pdf.
void LoadShadersFromFiles(bool recompile_glsl)
{
    // Note que o caminho para os arquivos de shader está fixado, sendo que
    // assumimos a existência da seguinte estrutura no sistema de arquivos:
    //
    //    + cgvis-base-vulkan/
    //    |
    //    +--+ bin/
    //    |  |
    //    |  +--+ Release/  (ou Debug/ ou Linux/)
    //    |  |  |
    //    |  |  o-- main.exe
    //    |  |
    //    |  +--+ shaders/
    //    |     |
    //    |     o-- shader_vertex.vert.spv
    //    |     |
    //    |     o-- shader_fragment.frag.spv
    //    |
    //    +--+ src/
    //       |
    //       o-- shader_vertex.vert
    //       |
    //       o-- shader_fragment.frag
    //
    // Os arquivos ".spv" são gerados automaticamente pelo CMake, que chama o
    // compilador glslc do Vulkan SDK.
    //
    // Na inicialização (recompile_glsl == false) apenas lemos esses arquivos.
    // Já quando o usuário aperta a tecla "R", recompilamos o GLSL antes, para
    // que o atalho continue tendo o mesmo efeito que tinha no código base em
    // OpenGL: ver na hora o resultado de uma edição nos shaders.
    if (recompile_glsl)
    {
        bool ok_vertex   = CompileShaderToSpirv("../../src/shader_vertex.vert",   "../shaders/shader_vertex.vert.spv");
        bool ok_fragment = CompileShaderToSpirv("../../src/shader_fragment.frag", "../shaders/shader_fragment.frag.spv");

        if (!ok_vertex || !ok_fragment)
        {
            fprintf(stderr,
                "WARNING: O compilador \"glslc\" falhou ou não foi encontrado.\n"
                "         Se houve um erro de compilação, ele foi impresso acima.\n"
                "         Os arquivos SPIR-V anteriores continuarão sendo utilizados.\n");
        }
    }

    // Deletamos o pipeline anterior, caso ele exista.
    DestroyGraphicsPipeline();

    CreateGraphicsPipeline();
}

void CreateGraphicsPipeline()
{
    VkShaderModule vertex_module   = Vulkan_CreateShaderModuleFromFile("../shaders/shader_vertex.vert.spv");
    VkShaderModule fragment_module = Vulkan_CreateShaderModuleFromFile("../shaders/shader_fragment.frag.spv");

    if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE)
    {
        fprintf(stderr, "ERROR: Não foi possível carregar os shaders.\n");
        std::exit(EXIT_FAILURE);
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName  = "main";

    // Formato dos atributos de vértice. No código base em OpenGL, isso era
    // descrito por chamadas glVertexAttribPointer() dentro do VAO; em Vulkan,
    // faz parte do pipeline.
    //
    // Usamos um buffer separado por atributo (assim como o código base usava um
    // VBO por atributo), o que em Vulkan corresponde a três "bindings".
    VkVertexInputBindingDescription vertex_bindings[3] = {};
    vertex_bindings[0].binding   = 0;
    vertex_bindings[0].stride    = 4 * sizeof(float); // vec4 model_coefficients
    vertex_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_bindings[1].binding   = 1;
    vertex_bindings[1].stride    = 4 * sizeof(float); // vec4 normal_coefficients
    vertex_bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vertex_bindings[2].binding   = 2;
    vertex_bindings[2].stride    = 2 * sizeof(float); // vec2 texture_coefficients
    vertex_bindings[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertex_attributes[3] = {};
    vertex_attributes[0].location = 0; // "(location = 0)" em "shader_vertex.vert"
    vertex_attributes[0].binding  = 0;
    vertex_attributes[0].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_attributes[0].offset   = 0;
    vertex_attributes[1].location = 1; // "(location = 1)" em "shader_vertex.vert"
    vertex_attributes[1].binding  = 1;
    vertex_attributes[1].format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_attributes[1].offset   = 0;
    vertex_attributes[2].location = 2; // "(location = 2)" em "shader_vertex.vert"
    vertex_attributes[2].binding  = 2;
    vertex_attributes[2].format   = VK_FORMAT_R32G32_SFLOAT;
    vertex_attributes[2].offset   = 0;

    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 3;
    vertex_input.pVertexBindingDescriptions      = vertex_bindings;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions    = vertex_attributes;

    // Equivalente ao GL_TRIANGLES do código base.
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport e scissor são definidos dinamicamente, a cada quadro, dentro de
    // VulkanContext_BeginFrame(). Assim não precisamos recriar o pipeline
    // quando a janela é redimensionada.
    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;

    // Backface Culling. Equivalente a:
    //     glEnable(GL_CULL_FACE);
    //     glCullFace(GL_BACK);
    //     glFrontFace(GL_CCW);
    //
    // ATENÇÃO ao frontFace: como a matriz Matrix_Vulkan_Clip() inverte o eixo Y
    // (veja "include/matrices.h"), a orientação dos triângulos em coordenadas
    // de tela também inverte. Por isso usamos CLOCKWISE aqui para obter
    // exatamente o mesmo resultado visual que o glFrontFace(GL_CCW) do código
    // base em OpenGL.
    //
    // Veja slides 8-13 do documento Aula_02_Fundamentos_Matematicos.pdf, slides
    // 23-34 do documento Aula_13_Clipping_and_Culling.pdf e slides 112-123 do
    // documento Aula_14_Laboratorio_3_Revisao.pdf.
    rasterizer.cullMode  = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Z-buffer. Equivalente ao glEnable(GL_DEPTH_TEST) do código base.
    // Veja slides 104-116 do documento Aula_09_Projecoes.pdf.
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType                 = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable       = VK_TRUE;
    depth_stencil.depthWriteEnable      = VK_TRUE;
    depth_stencil.depthCompareOp        = VK_COMPARE_OP_LESS;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable     = VK_FALSE;

    // Objetos opacos: blending desabilitado.
    VkPipelineColorBlendAttachmentState color_blend_attachment = {};
    color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                          | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable    = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo color_blending = {};
    color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments    = &color_blend_attachment;

    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    // O "pipeline layout" descreve quais recursos os shaders acessam: os
    // descriptor sets (uniformes e texturas) e as push constants.
    VkPushConstantRange push_constant_range = {};
    push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push_constant_range.offset     = 0;
    push_constant_range.size       = sizeof(ObjectConstants);

    // A especificação do Vulkan garante no mínimo 128 bytes de push constants.
    if (sizeof(ObjectConstants) > 128)
    {
        fprintf(stderr,
            "ERROR: A struct ObjectConstants tem %d bytes, mas o Vulkan só\n"
            "       garante 128 bytes de push constants. Mova algum campo para\n"
            "       um Uniform Buffer Object.\n", (int)sizeof(ObjectConstants));
        std::exit(EXIT_FAILURE);
    }

    if (sizeof(ObjectConstants) > g_Vk.physical_device_properties.limits.maxPushConstantsSize)
    {
        fprintf(stderr,
            "ERROR: Esta GPU suporta no máximo %u bytes de push constants, mas a\n"
            "       struct ObjectConstants tem %d bytes.\n",
            g_Vk.physical_device_properties.limits.maxPushConstantsSize,
            (int)sizeof(ObjectConstants));
        std::exit(EXIT_FAILURE);
    }

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount         = 1;
    layout_info.pSetLayouts            = &g_DescriptorSetLayout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges    = &push_constant_range;

    VK_CHECK(vkCreatePipelineLayout(g_Vk.device, &layout_info, NULL, &g_PipelineLayout));

    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount          = 2;
    pipeline_info.pStages             = stages;
    pipeline_info.pVertexInputState   = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState      = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState   = &multisampling;
    pipeline_info.pDepthStencilState  = &depth_stencil;
    pipeline_info.pColorBlendState    = &color_blending;
    pipeline_info.pDynamicState       = &dynamic_state;
    pipeline_info.layout              = g_PipelineLayout;
    pipeline_info.renderPass          = g_Vk.render_pass;
    pipeline_info.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(g_Vk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &g_GpuProgramID));

    // Os "Shader Modules" podem ser destruídos assim que o pipeline é criado.
    vkDestroyShaderModule(g_Vk.device, vertex_module, NULL);
    vkDestroyShaderModule(g_Vk.device, fragment_module, NULL);
}

void DestroyGraphicsPipeline()
{
    if (g_GpuProgramID == VK_NULL_HANDLE && g_PipelineLayout == VK_NULL_HANDLE)
        return;

    // Não podemos destruir um pipeline que a GPU ainda esteja usando.
    vkDeviceWaitIdle(g_Vk.device);

    if (g_GpuProgramID != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_Vk.device, g_GpuProgramID, NULL);
        g_GpuProgramID = VK_NULL_HANDLE;
    }

    if (g_PipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_Vk.device, g_PipelineLayout, NULL);
        g_PipelineLayout = VK_NULL_HANDLE;
    }
}

// ===========================================================================
// Liberação de recursos
// ===========================================================================

static void DestroyTexture(Texture& texture)
{
    if (texture.sampler != VK_NULL_HANDLE) vkDestroySampler(g_Vk.device, texture.sampler, NULL);
    if (texture.view    != VK_NULL_HANDLE) vkDestroyImageView(g_Vk.device, texture.view, NULL);
    if (texture.image   != VK_NULL_HANDLE) vkDestroyImage(g_Vk.device, texture.image, NULL);
    if (texture.memory  != VK_NULL_HANDLE) vkFreeMemory(g_Vk.device, texture.memory, NULL);

    texture = Texture();
}

void DestroyApplicationResources()
{
    vkDeviceWaitIdle(g_Vk.device);

    TextRendering_Destroy();

    for (size_t i = 0; i < g_GpuModels.size(); ++i)
    {
        GpuModel* gpu_model = g_GpuModels[i];

        vkDestroyBuffer(g_Vk.device, gpu_model->position_buffer, NULL);
        vkFreeMemory(g_Vk.device, gpu_model->position_memory, NULL);

        vkDestroyBuffer(g_Vk.device, gpu_model->normal_buffer, NULL);
        vkFreeMemory(g_Vk.device, gpu_model->normal_memory, NULL);

        vkDestroyBuffer(g_Vk.device, gpu_model->texcoord_buffer, NULL);
        vkFreeMemory(g_Vk.device, gpu_model->texcoord_memory, NULL);

        vkDestroyBuffer(g_Vk.device, gpu_model->index_buffer, NULL);
        vkFreeMemory(g_Vk.device, gpu_model->index_memory, NULL);

        delete gpu_model;
    }
    g_GpuModels.clear();
    g_VirtualScene.clear();

    for (uint32_t i = 0; i < g_NumLoadedTextures; ++i)
        DestroyTexture(g_Textures[i]);
    g_NumLoadedTextures = 0;

    DestroyTexture(g_FallbackTexture);

    DestroyGraphicsPipeline();

    if (g_DescriptorPool != VK_NULL_HANDLE)
        vkDestroyDescriptorPool(g_Vk.device, g_DescriptorPool, NULL);

    if (g_DescriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(g_Vk.device, g_DescriptorSetLayout, NULL);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        if (g_UniformBufferMemories[i] != VK_NULL_HANDLE)
        {
            vkUnmapMemory(g_Vk.device, g_UniformBufferMemories[i]);
            vkDestroyBuffer(g_Vk.device, g_UniformBuffers[i], NULL);
            vkFreeMemory(g_Vk.device, g_UniformBufferMemories[i], NULL);
        }
    }
}

// Função que pega a matriz M e guarda a mesma no topo da pilha
void PushMatrix(glm::mat4 M)
{
    g_MatrixStack.push(M);
}

// Função que remove a matriz atualmente no topo da pilha e armazena a mesma na variável M
void PopMatrix(glm::mat4& M)
{
    if ( g_MatrixStack.empty() )
    {
        M = Matrix_Identity();
    }
    else
    {
        M = g_MatrixStack.top();
        g_MatrixStack.pop();
    }
}

// Função que computa as normais de um ObjModel, caso elas não tenham sido
// especificadas dentro do arquivo ".obj"
void ComputeNormals(ObjModel* model)
{
    if ( !model->attrib.normals.empty() )
        return;

    // Primeiro computamos as normais para todos os TRIÂNGULOS.
    // Segundo, computamos as normais dos VÉRTICES através do método proposto
    // por Gouraud, onde a normal de cada vértice vai ser a média das normais de
    // todas as faces que compartilham este vértice e que pertencem ao mesmo "smoothing group".

    // Obtemos a lista dos smoothing groups que existem no objeto
    std::set<unsigned int> sgroup_ids;
    for (size_t shape = 0; shape < model->shapes.size(); ++shape)
    {
        size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

        assert(model->shapes[shape].mesh.smoothing_group_ids.size() == num_triangles);

        for (size_t triangle = 0; triangle < num_triangles; ++triangle)
        {
            assert(model->shapes[shape].mesh.num_face_vertices[triangle] == 3);
            unsigned int sgroup = model->shapes[shape].mesh.smoothing_group_ids[triangle];
            assert(sgroup >= 0);
            sgroup_ids.insert(sgroup);
        }
    }

    size_t num_vertices = model->attrib.vertices.size() / 3;
    model->attrib.normals.reserve( 3*num_vertices );

    // Processamos um smoothing group por vez
    for (const unsigned int & sgroup : sgroup_ids)
    {
        std::vector<int> num_triangles_per_vertex(num_vertices, 0);
        std::vector<glm::vec4> vertex_normals(num_vertices, glm::vec4(0.0f,0.0f,0.0f,0.0f));

        // Acumulamos as normais dos vértices de todos triângulos deste smoothing group
        for (size_t shape = 0; shape < model->shapes.size(); ++shape)
        {
            size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

            for (size_t triangle = 0; triangle < num_triangles; ++triangle)
            {
                unsigned int sgroup_tri = model->shapes[shape].mesh.smoothing_group_ids[triangle];

                if (sgroup_tri != sgroup)
                    continue;

                glm::vec4  vertices[3];
                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    tinyobj::index_t idx = model->shapes[shape].mesh.indices[3*triangle + vertex];
                    const float vx = model->attrib.vertices[3*idx.vertex_index + 0];
                    const float vy = model->attrib.vertices[3*idx.vertex_index + 1];
                    const float vz = model->attrib.vertices[3*idx.vertex_index + 2];
                    vertices[vertex] = glm::vec4(vx,vy,vz,1.0);
                }

                const glm::vec4  a = vertices[0];
                const glm::vec4  b = vertices[1];
                const glm::vec4  c = vertices[2];

                const glm::vec4  n = crossproduct(b-a,c-a);

                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    tinyobj::index_t idx = model->shapes[shape].mesh.indices[3*triangle + vertex];
                    num_triangles_per_vertex[idx.vertex_index] += 1;
                    vertex_normals[idx.vertex_index] += n;
                }
            }
        }

        // Computamos a média das normais acumuladas
        std::vector<size_t> normal_indices(num_vertices, 0);

        for (size_t vertex_index = 0; vertex_index < vertex_normals.size(); ++vertex_index)
        {
            if (num_triangles_per_vertex[vertex_index] == 0)
                continue;

            glm::vec4 n = vertex_normals[vertex_index] / (float)num_triangles_per_vertex[vertex_index];
            n /= norm(n);

            model->attrib.normals.push_back( n.x );
            model->attrib.normals.push_back( n.y );
            model->attrib.normals.push_back( n.z );

            size_t normal_index = (model->attrib.normals.size() / 3) - 1;
            normal_indices[vertex_index] = normal_index;
        }

        // Escrevemos os índices das normais para os vértices dos triângulos deste smoothing group
        for (size_t shape = 0; shape < model->shapes.size(); ++shape)
        {
            size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

            for (size_t triangle = 0; triangle < num_triangles; ++triangle)
            {
                unsigned int sgroup_tri = model->shapes[shape].mesh.smoothing_group_ids[triangle];

                if (sgroup_tri != sgroup)
                    continue;

                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    tinyobj::index_t idx = model->shapes[shape].mesh.indices[3*triangle + vertex];
                    model->shapes[shape].mesh.indices[3*triangle + vertex].normal_index =
                        normal_indices[ idx.vertex_index ];
                }
            }
        }

    }
}

// Constrói triângulos para futura renderização a partir de um ObjModel.
void BuildTrianglesAndAddToVirtualScene(ObjModel* model)
{
    // Este conjunto de buffers na GPU faz o papel do Vertex Array Object (VAO)
    // que o código base criava aqui com glGenVertexArrays().
    GpuModel* gpu_model = new GpuModel();

    std::vector<uint32_t> indices;
    std::vector<float>    model_coefficients;
    std::vector<float>    normal_coefficients;
    std::vector<float>    texture_coefficients;

    for (size_t shape = 0; shape < model->shapes.size(); ++shape)
    {
        size_t first_index = indices.size();
        size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

        // Em 2026-05-18, corrigido bug encontrado pelo aluno Arthur Prediger:
        // std::numeric_limits<float>::min() retorna o menor valor positivo
        // normalizado representável, não o menor valor possível (negativo). Para
        // inicializar o limite máximo da bounding box com um valor "muito
        // pequeno", deve ser usado std::numeric_limits<float>::lowest()
        const float minval = std::numeric_limits<float>::lowest();
        const float maxval = std::numeric_limits<float>::max();

        glm::vec3 bbox_min = glm::vec3(maxval,maxval,maxval);
        glm::vec3 bbox_max = glm::vec3(minval,minval,minval);

        for (size_t triangle = 0; triangle < num_triangles; ++triangle)
        {
            assert(model->shapes[shape].mesh.num_face_vertices[triangle] == 3);

            for (size_t vertex = 0; vertex < 3; ++vertex)
            {
                tinyobj::index_t idx = model->shapes[shape].mesh.indices[3*triangle + vertex];

                indices.push_back(first_index + 3*triangle + vertex);

                const float vx = model->attrib.vertices[3*idx.vertex_index + 0];
                const float vy = model->attrib.vertices[3*idx.vertex_index + 1];
                const float vz = model->attrib.vertices[3*idx.vertex_index + 2];
                //printf("tri %d vert %d = (%.2f, %.2f, %.2f)\n", (int)triangle, (int)vertex, vx, vy, vz);
                model_coefficients.push_back( vx ); // X
                model_coefficients.push_back( vy ); // Y
                model_coefficients.push_back( vz ); // Z
                model_coefficients.push_back( 1.0f ); // W

                bbox_min.x = std::min(bbox_min.x, vx);
                bbox_min.y = std::min(bbox_min.y, vy);
                bbox_min.z = std::min(bbox_min.z, vz);
                bbox_max.x = std::max(bbox_max.x, vx);
                bbox_max.y = std::max(bbox_max.y, vy);
                bbox_max.z = std::max(bbox_max.z, vz);

                // Inspecionando o código da tinyobjloader, o aluno Bernardo
                // Sulzbach (2017/1) apontou que a maneira correta de testar se
                // existem normais e coordenadas de textura no ObjModel é
                // comparando se o índice retornado é -1. Fazemos isso abaixo.

                if ( idx.normal_index != -1 )
                {
                    const float nx = model->attrib.normals[3*idx.normal_index + 0];
                    const float ny = model->attrib.normals[3*idx.normal_index + 1];
                    const float nz = model->attrib.normals[3*idx.normal_index + 2];
                    normal_coefficients.push_back( nx ); // X
                    normal_coefficients.push_back( ny ); // Y
                    normal_coefficients.push_back( nz ); // Z
                    normal_coefficients.push_back( 0.0f ); // W
                }

                if ( idx.texcoord_index != -1 )
                {
                    const float u = model->attrib.texcoords[2*idx.texcoord_index + 0];
                    const float v = model->attrib.texcoords[2*idx.texcoord_index + 1];
                    texture_coefficients.push_back( u );
                    texture_coefficients.push_back( v );
                }
            }
        }

        size_t last_index = indices.size() - 1;

        SceneObject theobject;
        theobject.name           = model->shapes[shape].name;
        theobject.first_index    = first_index; // Primeiro índice
        theobject.num_indices    = last_index - first_index + 1; // Número de indices
        theobject.rendering_mode = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // Índices correspondem ao tipo de rasterização GL_TRIANGLES.
        theobject.gpu_model      = gpu_model;

        theobject.bbox_min = bbox_min;
        theobject.bbox_max = bbox_max;

        g_VirtualScene[model->shapes[shape].name] = theobject;
    }

    // DIFERENÇA IMPORTANTE em relação ao código base em OpenGL:
    //
    // No OpenGL, um atributo de vértice que não fosse preenchido simplesmente
    // recebia um valor padrão. Em Vulkan, o pipeline declara TRÊS bindings de
    // vértice, e todos precisam estar ligados a um buffer válido e com o mesmo
    // número de elementos -- caso contrário o comportamento é indefinido.
    //
    // Por isso, quando o arquivo ".obj" não possui normais ou coordenadas de
    // textura para todos os vértices, completamos os arrays com zeros.
    size_t num_vertices = model_coefficients.size() / 4;

    if ( normal_coefficients.size() != num_vertices * 4 )
    {
        if ( !normal_coefficients.empty() )
            fprintf(stderr, "WARNING: O modelo possui normais para apenas parte dos vértices.\n");

        normal_coefficients.resize(num_vertices * 4, 0.0f);
    }

    if ( texture_coefficients.size() != num_vertices * 2 )
    {
        if ( !texture_coefficients.empty() )
            fprintf(stderr, "WARNING: O modelo possui coordenadas de textura para apenas parte dos vértices.\n");

        texture_coefficients.resize(num_vertices * 2, 0.0f);
    }

    // Enviamos os dados para a memória da GPU. Cada chamada abaixo é o
    // equivalente do trio glGenBuffers() + glBufferData() + glBufferSubData()
    // do código base.
    Vulkan_CreateBufferWithData(
        model_coefficients.data(),
        model_coefficients.size() * sizeof(float),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        &gpu_model->position_buffer,
        &gpu_model->position_memory
    );

    Vulkan_CreateBufferWithData(
        normal_coefficients.data(),
        normal_coefficients.size() * sizeof(float),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        &gpu_model->normal_buffer,
        &gpu_model->normal_memory
    );

    Vulkan_CreateBufferWithData(
        texture_coefficients.data(),
        texture_coefficients.size() * sizeof(float),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        &gpu_model->texcoord_buffer,
        &gpu_model->texcoord_memory
    );

    Vulkan_CreateBufferWithData(
        indices.data(),
        indices.size() * sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        &gpu_model->index_buffer,
        &gpu_model->index_memory
    );

    g_GpuModels.push_back(gpu_model);
}

// Definição da função que será chamada sempre que a janela do sistema
// operacional for redimensionada, por consequência alterando o tamanho do
// "framebuffer" (região de memória onde são armazenados os pixels da imagem).
void FramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    // No código base em OpenGL, era aqui que aparecia a chamada
    //
    //     glViewport(0, 0, width, height);
    //
    // que define o mapeamento das "normalized device coordinates" (NDC) para
    // "pixel coordinates". Essa é a operação de "Screen Mapping" ou "Viewport
    // Mapping" vista em aula ({+ViewportMapping2+}).
    //
    // Em Vulkan, o viewport é gravado no command buffer a cada quadro (veja
    // vkCmdSetViewport() em VulkanContext_BeginFrame()). O que precisamos fazer
    // aqui é apenas avisar que o swapchain ficou com o tamanho errado e precisa
    // ser recriado.
    g_Vk.framebuffer_resized = true;

    // Atualizamos também a razão que define a proporção da janela (largura /
    // altura), a qual será utilizada na definição das matrizes de projeção,
    // tal que não ocorra distorções durante o processo de "Screen Mapping"
    // acima, quando NDC é mapeado para coordenadas de pixels. Veja slides 205-215 do documento Aula_09_Projecoes.pdf.
    //
    // O cast para float é necessário pois números inteiros são arredondados ao
    // serem divididos!
    if (height > 0)
        g_ScreenRatio = (float)width / height;
}

// Variáveis globais que armazenam a última posição do cursor do mouse, para
// que possamos calcular quanto que o mouse se movimentou entre dois instantes
// de tempo. Utilizadas no callback CursorPosCallback() abaixo.
double g_LastCursorPosX, g_LastCursorPosY;

// Função callback chamada sempre que o usuário aperta algum dos botões do mouse
void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
    {
        // Se o usuário pressionou o botão esquerdo do mouse, guardamos a
        // posição atual do cursor nas variáveis g_LastCursorPosX e
        // g_LastCursorPosY.  Também, setamos a variável
        // g_LeftMouseButtonPressed como true, para saber que o usuário está
        // com o botão esquerdo pressionado.
        glfwGetCursorPos(window, &g_LastCursorPosX, &g_LastCursorPosY);
        g_LeftMouseButtonPressed = true;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE)
    {
        // Quando o usuário soltar o botão esquerdo do mouse, atualizamos a
        // variável abaixo para false.
        g_LeftMouseButtonPressed = false;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS)
    {
        // Se o usuário pressionou o botão esquerdo do mouse, guardamos a
        // posição atual do cursor nas variáveis g_LastCursorPosX e
        // g_LastCursorPosY.  Também, setamos a variável
        // g_RightMouseButtonPressed como true, para saber que o usuário está
        // com o botão esquerdo pressionado.
        glfwGetCursorPos(window, &g_LastCursorPosX, &g_LastCursorPosY);
        g_RightMouseButtonPressed = true;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_RELEASE)
    {
        // Quando o usuário soltar o botão esquerdo do mouse, atualizamos a
        // variável abaixo para false.
        g_RightMouseButtonPressed = false;
    }
    if (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_PRESS)
    {
        // Se o usuário pressionou o botão esquerdo do mouse, guardamos a
        // posição atual do cursor nas variáveis g_LastCursorPosX e
        // g_LastCursorPosY.  Também, setamos a variável
        // g_MiddleMouseButtonPressed como true, para saber que o usuário está
        // com o botão esquerdo pressionado.
        glfwGetCursorPos(window, &g_LastCursorPosX, &g_LastCursorPosY);
        g_MiddleMouseButtonPressed = true;
    }
    if (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_RELEASE)
    {
        // Quando o usuário soltar o botão esquerdo do mouse, atualizamos a
        // variável abaixo para false.
        g_MiddleMouseButtonPressed = false;
    }
}

// Função callback chamada sempre que o usuário movimentar o cursor do mouse em
// cima da janela.
void CursorPosCallback(GLFWwindow* window, double xpos, double ypos)
{
    // Abaixo executamos o seguinte: caso o botão esquerdo do mouse esteja
    // pressionado, computamos quanto que o mouse se movimento desde o último
    // instante de tempo, e usamos esta movimentação para atualizar os
    // parâmetros que definem a posição da câmera dentro da cena virtual.
    // Assim, temos que o usuário consegue controlar a câmera.

    if (g_LeftMouseButtonPressed)
    {
        // Deslocamento do cursor do mouse em x e y de coordenadas de tela!
        float dx = xpos - g_LastCursorPosX;
        float dy = ypos - g_LastCursorPosY;

        // Atualizamos parâmetros da câmera com os deslocamentos
        g_CameraTheta -= 0.01f*dx;
        g_CameraPhi   += 0.01f*dy;

        // Em coordenadas esféricas, o ângulo phi deve ficar entre -pi/2 e +pi/2.
        float phimax = 3.141592f/2;
        float phimin = -phimax;

        if (g_CameraPhi > phimax)
            g_CameraPhi = phimax;

        if (g_CameraPhi < phimin)
            g_CameraPhi = phimin;

        // Atualizamos as variáveis globais para armazenar a posição atual do
        // cursor como sendo a última posição conhecida do cursor.
        g_LastCursorPosX = xpos;
        g_LastCursorPosY = ypos;
    }

    if (g_RightMouseButtonPressed)
    {
        // Deslocamento do cursor do mouse em x e y de coordenadas de tela!
        float dx = xpos - g_LastCursorPosX;
        float dy = ypos - g_LastCursorPosY;

        // Atualizamos parâmetros da antebraço com os deslocamentos
        g_ForearmAngleZ -= 0.01f*dx;
        g_ForearmAngleX += 0.01f*dy;

        // Atualizamos as variáveis globais para armazenar a posição atual do
        // cursor como sendo a última posição conhecida do cursor.
        g_LastCursorPosX = xpos;
        g_LastCursorPosY = ypos;
    }

    if (g_MiddleMouseButtonPressed)
    {
        // Deslocamento do cursor do mouse em x e y de coordenadas de tela!
        float dx = xpos - g_LastCursorPosX;
        float dy = ypos - g_LastCursorPosY;

        // Atualizamos parâmetros da antebraço com os deslocamentos
        g_TorsoPositionX += 0.01f*dx;
        g_TorsoPositionY -= 0.01f*dy;

        // Atualizamos as variáveis globais para armazenar a posição atual do
        // cursor como sendo a última posição conhecida do cursor.
        g_LastCursorPosX = xpos;
        g_LastCursorPosY = ypos;
    }
}

// Função callback chamada sempre que o usuário movimenta a "rodinha" do mouse.
void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
    // Atualizamos a distância da câmera para a origem utilizando a
    // movimentação da "rodinha", simulando um ZOOM.
    g_CameraDistance -= 0.1f*yoffset;

    // Uma câmera look-at nunca pode estar exatamente "em cima" do ponto para
    // onde ela está olhando, pois isto gera problemas de divisão por zero na
    // definição do sistema de coordenadas da câmera. Isto é, a variável abaixo
    // nunca pode ser zero. Versões anteriores deste código possuíam este bug,
    // o qual foi detectado pelo aluno Vinicius Fraga (2017/2).
    const float verysmallnumber = std::numeric_limits<float>::epsilon();
    if (g_CameraDistance < verysmallnumber)
        g_CameraDistance = verysmallnumber;
}

void Correcao_KeyCallback(int key, int action, int mod);

// Definição da função que será chamada sempre que o usuário pressionar alguma
// tecla do teclado. Veja http://www.glfw.org/docs/latest/input_guide.html#input_key
void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mod)
{
    // =======================
    // Não modifique esta chamada! Ela é utilizada para correção automatizada dos
    // laboratórios. Deve ser sempre o primeiro comando desta função KeyCallback().
    Correcao_KeyCallback(key, action, mod);
    // =======================

    // Se o usuário pressionar a tecla ESC, fechamos a janela.
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);

    // O código abaixo implementa a seguinte lógica:
    //   Se apertar tecla X       então g_AngleX += delta;
    //   Se apertar tecla shift+X então g_AngleX -= delta;
    //   Se apertar tecla Y       então g_AngleY += delta;
    //   Se apertar tecla shift+Y então g_AngleY -= delta;
    //   Se apertar tecla Z       então g_AngleZ += delta;
    //   Se apertar tecla shift+Z então g_AngleZ -= delta;

    float delta = 3.141592 / 16; // 22.5 graus, em radianos.

    if (key == GLFW_KEY_X && action == GLFW_PRESS)
    {
        g_AngleX += (mod & GLFW_MOD_SHIFT) ? -delta : delta;
    }

    if (key == GLFW_KEY_Y && action == GLFW_PRESS)
    {
        g_AngleY += (mod & GLFW_MOD_SHIFT) ? -delta : delta;
    }
    if (key == GLFW_KEY_Z && action == GLFW_PRESS)
    {
        g_AngleZ += (mod & GLFW_MOD_SHIFT) ? -delta : delta;
    }

    // Se o usuário apertar a tecla espaço, resetamos os ângulos de Euler para zero.
    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
    {
        g_AngleX = 0.0f;
        g_AngleY = 0.0f;
        g_AngleZ = 0.0f;
        g_ForearmAngleX = 0.0f;
        g_ForearmAngleZ = 0.0f;
        g_TorsoPositionX = 0.0f;
        g_TorsoPositionY = 0.0f;
    }

    // Se o usuário apertar a tecla P, utilizamos projeção perspectiva.
    if (key == GLFW_KEY_P && action == GLFW_PRESS)
    {
        g_UsePerspectiveProjection = true;
    }

    // Se o usuário apertar a tecla O, utilizamos projeção ortográfica.
    if (key == GLFW_KEY_O && action == GLFW_PRESS)
    {
        g_UsePerspectiveProjection = false;
    }

    // Se o usuário apertar a tecla H, fazemos um "toggle" do texto informativo mostrado na tela.
    if (key == GLFW_KEY_H && action == GLFW_PRESS)
    {
        g_ShowInfoText = !g_ShowInfoText;
    }

    // Se o usuário apertar a tecla R, recarregamos os shaders dos arquivos
    // "shader_fragment.frag" e "shader_vertex.vert".
    //
    // NOTE: em Vulkan isso exige recompilar o GLSL para SPIR-V (com o glslc) e
    // recriar o pipeline gráfico inteiro. Veja LoadShadersFromFiles().
    if (key == GLFW_KEY_R && action == GLFW_PRESS)
    {
        LoadShadersFromFiles(true);
        fprintf(stdout,"Shaders recarregados!\n");
        fflush(stdout);
    }
}

// Definimos o callback para impressão de erros da GLFW no terminal
void ErrorCallback(int error, const char* description)
{
    fprintf(stderr, "ERROR: GLFW: %s\n", description);
}

// Esta função recebe um vértice com coordenadas de modelo p_model e passa o
// mesmo por todos os sistemas de coordenadas armazenados nas matrizes model,
// view, e projection; e escreve na tela as matrizes e pontos resultantes
// dessas transformações.
void TextRendering_ShowModelViewProjection(
    GLFWwindow* window,
    glm::mat4 projection,
    glm::mat4 view,
    glm::mat4 model,
    glm::vec4 p_model
)
{
    if ( !g_ShowInfoText )
        return;

    glm::vec4 p_world = model*p_model;
    glm::vec4 p_camera = view*p_world;
    glm::vec4 p_clip = projection*p_camera;
    glm::vec4 p_ndc = p_clip / p_clip.w;

    float pad = TextRendering_LineHeight(window);

    TextRendering_PrintString(window, " Model matrix             Model     In World Coords.", -1.0f, 1.0f-pad, 1.0f);
    TextRendering_PrintMatrixVectorProduct(window, model, p_model, -1.0f, 1.0f-2*pad, 1.0f);

    TextRendering_PrintString(window, "                                        |  ", -1.0f, 1.0f-6*pad, 1.0f);
    TextRendering_PrintString(window, "                            .-----------'  ", -1.0f, 1.0f-7*pad, 1.0f);
    TextRendering_PrintString(window, "                            V              ", -1.0f, 1.0f-8*pad, 1.0f);

    TextRendering_PrintString(window, " View matrix              World     In Camera Coords.", -1.0f, 1.0f-9*pad, 1.0f);
    TextRendering_PrintMatrixVectorProduct(window, view, p_world, -1.0f, 1.0f-10*pad, 1.0f);

    TextRendering_PrintString(window, "                                        |  ", -1.0f, 1.0f-14*pad, 1.0f);
    TextRendering_PrintString(window, "                            .-----------'  ", -1.0f, 1.0f-15*pad, 1.0f);
    TextRendering_PrintString(window, "                            V              ", -1.0f, 1.0f-16*pad, 1.0f);

    TextRendering_PrintString(window, " Projection matrix        Camera                    In NDC", -1.0f, 1.0f-17*pad, 1.0f);
    TextRendering_PrintMatrixVectorProductDivW(window, projection, p_camera, -1.0f, 1.0f-18*pad, 1.0f);

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);

    glm::vec2 a = glm::vec2(-1, -1);
    glm::vec2 b = glm::vec2(+1, +1);
    glm::vec2 p = glm::vec2( 0,  0);
    glm::vec2 q = glm::vec2(width, height);

    glm::mat4 viewport_mapping = Matrix(
        (q.x - p.x)/(b.x-a.x), 0.0f, 0.0f, (b.x*p.x - a.x*q.x)/(b.x-a.x),
        0.0f, (q.y - p.y)/(b.y-a.y), 0.0f, (b.y*p.y - a.y*q.y)/(b.y-a.y),
        0.0f , 0.0f , 1.0f , 0.0f ,
        0.0f , 0.0f , 0.0f , 1.0f
    );

    TextRendering_PrintString(window, "                                                       |  ", -1.0f, 1.0f-22*pad, 1.0f);
    TextRendering_PrintString(window, "                            .--------------------------'  ", -1.0f, 1.0f-23*pad, 1.0f);
    TextRendering_PrintString(window, "                            V                           ", -1.0f, 1.0f-24*pad, 1.0f);

    TextRendering_PrintString(window, " Viewport matrix           NDC      In Pixel Coords.", -1.0f, 1.0f-25*pad, 1.0f);
    TextRendering_PrintMatrixVectorProductMoreDigits(window, viewport_mapping, p_ndc, -1.0f, 1.0f-26*pad, 1.0f);
}

// Escrevemos na tela os ângulos de Euler definidos nas variáveis globais
// g_AngleX, g_AngleY, e g_AngleZ.
void TextRendering_ShowEulerAngles(GLFWwindow* window)
{
    if ( !g_ShowInfoText )
        return;

    float pad = TextRendering_LineHeight(window);

    char buffer[80];
    snprintf(buffer, 80, "Euler Angles rotation matrix = Z(%.2f)*Y(%.2f)*X(%.2f)\n", g_AngleZ, g_AngleY, g_AngleX);

    TextRendering_PrintString(window, buffer, -1.0f+pad/10, -1.0f+2*pad/10, 1.0f);
}

// Escrevemos na tela qual matriz de projeção está sendo utilizada.
void TextRendering_ShowProjection(GLFWwindow* window)
{
    if ( !g_ShowInfoText )
        return;

    float lineheight = TextRendering_LineHeight(window);
    float charwidth = TextRendering_CharWidth(window);

    if ( g_UsePerspectiveProjection )
        TextRendering_PrintString(window, "Perspective", 1.0f-13*charwidth, -1.0f+2*lineheight/10, 1.0f);
    else
        TextRendering_PrintString(window, "Orthographic", 1.0f-13*charwidth, -1.0f+2*lineheight/10, 1.0f);
}

// Escrevemos na tela o número de quadros renderizados por segundo (frames per
// second).
void TextRendering_ShowFramesPerSecond(GLFWwindow* window)
{
    if ( !g_ShowInfoText )
        return;

    // Variáveis estáticas (static) mantém seus valores entre chamadas
    // subsequentes da função!
    static float old_seconds = (float)glfwGetTime();
    static int   ellapsed_frames = 0;
    static char  buffer[20] = "?? fps";
    static int   numchars = 7;

    ellapsed_frames += 1;

    // Recuperamos o número de segundos que passou desde a execução do programa
    float seconds = (float)glfwGetTime();

    // Número de segundos desde o último cálculo do fps
    float ellapsed_seconds = seconds - old_seconds;

    if ( ellapsed_seconds > 1.0f )
    {
        numchars = snprintf(buffer, 20, "%.2f fps", ellapsed_frames / ellapsed_seconds);

        old_seconds = seconds;
        ellapsed_frames = 0;
    }

    float lineheight = TextRendering_LineHeight(window);
    float charwidth = TextRendering_CharWidth(window);

    TextRendering_PrintString(window, buffer, 1.0f-(numchars + 1)*charwidth, 1.0f-lineheight, 1.0f);
}

// Função para debugging: imprime no terminal todas informações de um modelo
// geométrico carregado de um arquivo ".obj".
// Veja: https://github.com/syoyo/tinyobjloader/blob/22883def8db9ef1f3ffb9b404318e7dd25fdbb51/loader_example.cc#L98
void PrintObjModelInfo(ObjModel* model)
{
  const tinyobj::attrib_t                & attrib    = model->attrib;
  const std::vector<tinyobj::shape_t>    & shapes    = model->shapes;
  const std::vector<tinyobj::material_t> & materials = model->materials;

  printf("# of vertices  : %d\n", (int)(attrib.vertices.size() / 3));
  printf("# of normals   : %d\n", (int)(attrib.normals.size() / 3));
  printf("# of texcoords : %d\n", (int)(attrib.texcoords.size() / 2));
  printf("# of shapes    : %d\n", (int)shapes.size());
  printf("# of materials : %d\n", (int)materials.size());

  for (size_t v = 0; v < attrib.vertices.size() / 3; v++) {
    printf("  v[%ld] = (%f, %f, %f)\n", static_cast<long>(v),
           static_cast<const double>(attrib.vertices[3 * v + 0]),
           static_cast<const double>(attrib.vertices[3 * v + 1]),
           static_cast<const double>(attrib.vertices[3 * v + 2]));
  }

  for (size_t v = 0; v < attrib.normals.size() / 3; v++) {
    printf("  n[%ld] = (%f, %f, %f)\n", static_cast<long>(v),
           static_cast<const double>(attrib.normals[3 * v + 0]),
           static_cast<const double>(attrib.normals[3 * v + 1]),
           static_cast<const double>(attrib.normals[3 * v + 2]));
  }

  for (size_t v = 0; v < attrib.texcoords.size() / 2; v++) {
    printf("  uv[%ld] = (%f, %f)\n", static_cast<long>(v),
           static_cast<const double>(attrib.texcoords[2 * v + 0]),
           static_cast<const double>(attrib.texcoords[2 * v + 1]));
  }

  // For each shape
  for (size_t i = 0; i < shapes.size(); i++) {
    printf("shape[%ld].name = %s\n", static_cast<long>(i),
           shapes[i].name.c_str());
    printf("Size of shape[%ld].indices: %lu\n", static_cast<long>(i),
           static_cast<unsigned long>(shapes[i].mesh.indices.size()));

    size_t index_offset = 0;

    assert(shapes[i].mesh.num_face_vertices.size() ==
           shapes[i].mesh.material_ids.size());

    printf("shape[%ld].num_faces: %lu\n", static_cast<long>(i),
           static_cast<unsigned long>(shapes[i].mesh.num_face_vertices.size()));

    // For each face
    for (size_t f = 0; f < shapes[i].mesh.num_face_vertices.size(); f++) {
      size_t fnum = shapes[i].mesh.num_face_vertices[f];

      printf("  face[%ld].fnum = %ld\n", static_cast<long>(f),
             static_cast<unsigned long>(fnum));

      // For each vertex in the face
      for (size_t v = 0; v < fnum; v++) {
        tinyobj::index_t idx = shapes[i].mesh.indices[index_offset + v];
        printf("    face[%ld].v[%ld].idx = %d/%d/%d\n", static_cast<long>(f),
               static_cast<long>(v), idx.vertex_index, idx.normal_index,
               idx.texcoord_index);
      }

      printf("  face[%ld].material_id = %d\n", static_cast<long>(f),
             shapes[i].mesh.material_ids[f]);

      index_offset += fnum;
    }

    printf("shape[%ld].num_tags: %lu\n", static_cast<long>(i),
           static_cast<unsigned long>(shapes[i].mesh.tags.size()));
    for (size_t t = 0; t < shapes[i].mesh.tags.size(); t++) {
      printf("  tag[%ld] = %s ", static_cast<long>(t),
             shapes[i].mesh.tags[t].name.c_str());
      printf(" ints: [");
      for (size_t j = 0; j < shapes[i].mesh.tags[t].intValues.size(); ++j) {
        printf("%ld", static_cast<long>(shapes[i].mesh.tags[t].intValues[j]));
        if (j < (shapes[i].mesh.tags[t].intValues.size() - 1)) {
          printf(", ");
        }
      }
      printf("]");

      printf(" floats: [");
      for (size_t j = 0; j < shapes[i].mesh.tags[t].floatValues.size(); ++j) {
        printf("%f", static_cast<const double>(
                         shapes[i].mesh.tags[t].floatValues[j]));
        if (j < (shapes[i].mesh.tags[t].floatValues.size() - 1)) {
          printf(", ");
        }
      }
      printf("]");

      printf(" strings: [");
      for (size_t j = 0; j < shapes[i].mesh.tags[t].stringValues.size(); ++j) {
        printf("%s", shapes[i].mesh.tags[t].stringValues[j].c_str());
        if (j < (shapes[i].mesh.tags[t].stringValues.size() - 1)) {
          printf(", ");
        }
      }
      printf("]");
      printf("\n");
    }
  }

  for (size_t i = 0; i < materials.size(); i++) {
    printf("material[%ld].name = %s\n", static_cast<long>(i),
           materials[i].name.c_str());
    printf("  material.Ka = (%f, %f ,%f)\n",
           static_cast<const double>(materials[i].ambient[0]),
           static_cast<const double>(materials[i].ambient[1]),
           static_cast<const double>(materials[i].ambient[2]));
    printf("  material.Kd = (%f, %f ,%f)\n",
           static_cast<const double>(materials[i].diffuse[0]),
           static_cast<const double>(materials[i].diffuse[1]),
           static_cast<const double>(materials[i].diffuse[2]));
    printf("  material.Ks = (%f, %f ,%f)\n",
           static_cast<const double>(materials[i].specular[0]),
           static_cast<const double>(materials[i].specular[1]),
           static_cast<const double>(materials[i].specular[2]));
    printf("  material.Tr = (%f, %f ,%f)\n",
           static_cast<const double>(materials[i].transmittance[0]),
           static_cast<const double>(materials[i].transmittance[1]),
           static_cast<const double>(materials[i].transmittance[2]));
    printf("  material.Ke = (%f, %f ,%f)\n",
           static_cast<const double>(materials[i].emission[0]),
           static_cast<const double>(materials[i].emission[1]),
           static_cast<const double>(materials[i].emission[2]));
    printf("  material.Ns = %f\n",
           static_cast<const double>(materials[i].shininess));
    printf("  material.Ni = %f\n", static_cast<const double>(materials[i].ior));
    printf("  material.dissolve = %f\n",
           static_cast<const double>(materials[i].dissolve));
    printf("  material.illum = %d\n", materials[i].illum);
    printf("  material.map_Ka = %s\n", materials[i].ambient_texname.c_str());
    printf("  material.map_Kd = %s\n", materials[i].diffuse_texname.c_str());
    printf("  material.map_Ks = %s\n", materials[i].specular_texname.c_str());
    printf("  material.map_Ns = %s\n",
           materials[i].specular_highlight_texname.c_str());
    printf("  material.map_bump = %s\n", materials[i].bump_texname.c_str());
    printf("  material.map_d = %s\n", materials[i].alpha_texname.c_str());
    printf("  material.disp = %s\n", materials[i].displacement_texname.c_str());
    printf("  <<PBR>>\n");
    printf("  material.Pr     = %f\n", materials[i].roughness);
    printf("  material.Pm     = %f\n", materials[i].metallic);
    printf("  material.Ps     = %f\n", materials[i].sheen);
    printf("  material.Pc     = %f\n", materials[i].clearcoat_thickness);
    printf("  material.Pcr    = %f\n", materials[i].clearcoat_thickness);
    printf("  material.aniso  = %f\n", materials[i].anisotropy);
    printf("  material.anisor = %f\n", materials[i].anisotropy_rotation);
    printf("  material.map_Ke = %s\n", materials[i].emissive_texname.c_str());
    printf("  material.map_Pr = %s\n", materials[i].roughness_texname.c_str());
    printf("  material.map_Pm = %s\n", materials[i].metallic_texname.c_str());
    printf("  material.map_Ps = %s\n", materials[i].sheen_texname.c_str());
    printf("  material.norm   = %s\n", materials[i].normal_texname.c_str());
    std::map<std::string, std::string>::const_iterator it(
        materials[i].unknown_parameter.begin());
    std::map<std::string, std::string>::const_iterator itEnd(
        materials[i].unknown_parameter.end());

    for (; it != itEnd; it++) {
      printf("  material.%s = %s\n", it->first.c_str(), it->second.c_str());
    }
    printf("\n");
  }
}

// set makeprg=cd\ ..\ &&\ make\ run\ >/dev/null
// vim: set spell spelllang=pt_br :
