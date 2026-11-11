// ============================================================================
// main.cpp - Ponto de Entrada do Simulador "Branquinho: Rota do Vale"
//
// Universidade Federal do Rio Grande do Sul - Instituto de Informática
// INF01047 Computação Gráfica e Visualização I - Prof. Eduardo Gastal
//
// Responsável por orquestrar a execução da aplicação gráfica:
//   1. Inicialização da janela com GLFW e carregamento de funções com GLAD;
//   2. Registro de callbacks do sistema operacional (teclado, mouse, redimensionamento);
//   3. Carregamento dos shaders de GPU, texturas e malhas poligonais dos objetos;
//   4. Loop principal de renderização:
//      - Limpeza do framebuffer com a cor do céu atual;
//      - Atualização e transmissão das matrizes de projeção e câmera (View);
//      - Aplicação de iluminação solar dinâmica (hora do dia) e névoa;
//      - Atualização da cinemática baseada em Delta T e desenho do ônibus;
//      - Renderização do terreno da cena virtual;
//      - Renderização da taxa de quadros por segundo (FPS).
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

// Headers locais do projeto
#include "matrices.h"
#include "scene.h"
#include "camera.h"
#include "bus.h"
#include "lighting.h"

// ----------------------------------------------------------------------------
// Declarações de Funções Auxiliares de Renderização de Texto (textrendering.cpp)
// ----------------------------------------------------------------------------
void TextRendering_Init();
float TextRendering_LineHeight(GLFWwindow* window);
float TextRendering_CharWidth(GLFWwindow* window);
void TextRendering_PrintString(GLFWwindow* window, const std::string &str, float x, float y, float scale = 1.0f);
void TextRendering_ShowFramesPerSecond(GLFWwindow* window);

// ----------------------------------------------------------------------------
// Declarações de Callbacks GLFW
// ----------------------------------------------------------------------------
void FramebufferSizeCallback(GLFWwindow* window, int width, int height);
void ErrorCallback(int error, const char* description);
void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mode);
void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
void CursorPosCallback(GLFWwindow* window, double xpos, double ypos);
void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
void Correcao_KeyCallback(int key, int action, int mod);

int main(int argc, char* argv[])
{
    // ------------------------------------------------------------------------
    // 1. Inicialização da GLFW e Criação da Janela
    // ------------------------------------------------------------------------
    int success = glfwInit();
    if (!success)
    {
        fprintf(stderr, "ERROR: glfwInit() failed.\n");
        std::exit(EXIT_FAILURE);
    }

    glfwSetErrorCallback(ErrorCallback);

    // Solicitamos um contexto OpenGL versão 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);

    #ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif

    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(800, 600, "INF01047 - Branquinho Simulator", NULL, NULL);
    if (!window)
    {
        glfwTerminate();
        fprintf(stderr, "ERROR: glfwCreateWindow() failed.\n");
        std::exit(EXIT_FAILURE);
    }

    // Registramos os tratadores de eventos de entrada
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);

    // Definimos o contexto OpenGL da janela criada como o ativo na thread atual
    glfwMakeContextCurrent(window);

    // Carregamos todos os ponteiros de funções OpenGL 3.3 via GLAD
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    // Registramos e forçamos a chamada do callback de redimensionamento para definir a proporção inicial da tela
    glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
    FramebufferSizeCallback(window, 800, 600);

    // Exibimos informações da GPU no terminal para diagnóstico
    const GLubyte *vendor      = glGetString(GL_VENDOR);
    const GLubyte *renderer    = glGetString(GL_RENDERER);
    const GLubyte *glversion   = glGetString(GL_VERSION);
    const GLubyte *glslversion = glGetString(GL_SHADING_LANGUAGE_VERSION);
    printf("GPU: %s, %s, OpenGL %s, GLSL %s\n", vendor, renderer, glversion, glslversion);

    // ------------------------------------------------------------------------
    // 2. Carregamento de Recursos (Shaders, Texturas e Malhas 3D)
    // ------------------------------------------------------------------------

    // Compila e linka vertex e fragment shaders criando o programa de GPU
    LoadShadersFromFiles();

    // Carregamos as imagens de textura utilizadas na cena
    LoadTextureImage("../../data/red_brick_diff_1k.jpg");      // TextureImage0 (lataria/tijolos)
    LoadTextureImage("../../data/rocky_terrain_02_diff_1k.jpg"); // TextureImage1 (chão)

    // Carregamento das malhas do chão (terreno) e do ônibus Mascarello Gran Midi
    ObjModel planemodel("../../../data/terrain.obj");
    ComputeNormals(&planemodel);
    BuildTrianglesAndAddToVirtualScene(&planemodel);

    // Carregamos a malha tridimensional complexa do ônibus Mascarello Gran Midi
    ObjModel branquinhomodel("../../../data/branquinho.obj");
    ComputeNormals(&branquinhomodel);
    BuildTrianglesAndAddToVirtualScene(&branquinhomodel);

    // Inicializamos o subsistema de rasterização de fontes para o HUD
    TextRendering_Init();

    // ------------------------------------------------------------------------
    // 3. Configuração de Estado Global do OpenGL
    // ------------------------------------------------------------------------

    // Habilitamos o teste de profundidade (Z-buffer) para visibilidade correta
    glEnable(GL_DEPTH_TEST);

    // Habilitamos o descarte de faces traseiras (Backface Culling) para ganho de desempenho
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // ------------------------------------------------------------------------
    // 4. Loop Principal de Renderização (Game Loop)
    // ------------------------------------------------------------------------
    double prevTime = 0.0;

    while (!glfwWindowShouldClose(window))
    {
        // Limpamos o framebuffer com a cor dinâmica do céu (variando com a hora do dia)
        glm::vec3 sky_color = g_Lighting.GetSkyColor();
        glClearColor(sky_color.r, sky_color.g, sky_color.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Ativamos o programa de GPU que contém nossos shaders
        glUseProgram(g_GpuProgramID);

        // Atualizamos os parâmetros dinâmicos de luz solar, céu e névoa no shader
        g_Lighting.ApplyUniforms(g_GpuProgramID);

        // Computamos as matrizes View e Projection através do módulo Camera
        glm::mat4 view = g_Camera.ComputeViewMatrix();
        glm::mat4 projection = g_Camera.ComputeProjectionMatrix();

        glUniformMatrix4fv(g_view_uniform, 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(g_projection_uniform, 1, GL_FALSE, glm::value_ptr(projection));

        // Atualização da cinemática baseada no tempo decorrido (Delta T)
        double curTime = glfwGetTime();
        double deltaT = curTime - prevTime;
        prevTime = curTime;

        // Atualiza a física/cinemática do ônibus e o renderiza na cena
        g_Bus.UpdatePos(deltaT);
        g_Bus.Draw(branquinhomodel);

        // Renderiza o terreno de teste
        glm::mat4 model = Matrix_Translate(0.0f, 0.0f, 0.0f);
        glUniformMatrix4fv(g_model_uniform, 1, GL_FALSE, glm::value_ptr(model));
        glUniform1i(g_object_id_uniform, PLANE);
        DrawVirtualObject("terrain");

        // Exibimos a taxa de quadros por segundo no canto superior da tela
        TextRendering_ShowFramesPerSecond(window);

        // Troca os buffers de tela (Double Buffering) para exibir o quadro renderizado
        glfwSwapBuffers(window);

        // Processa eventos da fila do sistema operacional (teclado, mouse, fechar janela)
        glfwPollEvents();
    }

    // Finalizamos e liberamos recursos da GLFW
    glfwTerminate();
    return 0;
}

// ----------------------------------------------------------------------------
// Implementações dos Callbacks GLFW
// ----------------------------------------------------------------------------

void FramebufferSizeCallback(GLFWwindow* window, int width, int height)
{
    // Mapeamento das coordenadas normalizadas de dispositivo (NDC) para pixels da tela
    glViewport(0, 0, width, height);

    // Atualiza a razão de aspecto da câmera para evitar distorções nas matrizes de projeção
    g_Camera.SetScreenRatio(width, height);
}

void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    // Encaminha cliques de mouse para controle da órbita da câmera
    g_Camera.OnMouseButton(button, action, mods, xpos, ypos);
}

void CursorPosCallback(GLFWwindow* window, double xpos, double ypos)
{
    // Encaminha a movimentação do cursor para atualizar ângulos theta/phi da câmera Look-At
    g_Camera.OnCursorPos(xpos, ypos);
}

void ScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
    // Encaminha rolagem da roda do mouse para efetuar zoom in / zoom out
    g_Camera.OnScroll(xoffset, yoffset);
}

void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mod)
{
    // ========================================================================
    // Não modifique esta chamada! Utilizada para correção automatizada do laboratório.
    Correcao_KeyCallback(key, action, mod);
    // ========================================================================

    // Encaminha teclas K e L para o controle da posição do Sol (hora do dia)
    g_Lighting.OnKey(key, action);

    // Encaminha teclas de movimentação (W, A, S, D) para o controle do ônibus
    g_Bus.OnKey(key, action);

    // ESC fecha a janela da aplicação
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GL_TRUE);

    // Tecla P seleciona Projeção Perspectiva
    if (key == GLFW_KEY_P && action == GLFW_PRESS)
        g_Camera.use_perspective = true;

    // Tecla O seleciona Projeção Ortográfica
    if (key == GLFW_KEY_O && action == GLFW_PRESS)
        g_Camera.use_perspective = false;

    // Tecla R recarrega os arquivos de shader em tempo de execução (Hot-Reload)
    if (key == GLFW_KEY_R && action == GLFW_PRESS)
    {
        LoadShadersFromFiles();
        fprintf(stdout, "Shaders recarregados com sucesso!\n");
        fflush(stdout);
    }
}

void ErrorCallback(int error, const char* description)
{
    fprintf(stderr, "ERROR: GLFW: %s\n", description);
}

// ----------------------------------------------------------------------------
// Medição e Exibição da Taxa de Quadros por Segundo (Frames Per Second - FPS)
// ----------------------------------------------------------------------------
void TextRendering_ShowFramesPerSecond(GLFWwindow* window)
{
    static float old_seconds = (float)glfwGetTime();
    static int   ellapsed_frames = 0;
    static char  buffer[20] = "?? fps";
    static int   numchars = 7;

    ellapsed_frames += 1;

    float seconds = (float)glfwGetTime();
    float ellapsed_seconds = seconds - old_seconds;

    // Atualiza a medição de FPS uma vez a cada segundo
    if (ellapsed_seconds > 1.0f)
    {
        numchars = snprintf(buffer, 20, "%.2f fps", ellapsed_frames / ellapsed_seconds);
        old_seconds = seconds;
        ellapsed_frames = 0;
    }

    float lineheight = TextRendering_LineHeight(window);
    float charwidth = TextRendering_CharWidth(window);

    TextRendering_PrintString(window, buffer, 1.0f - (numchars + 1) * charwidth, 1.0f - lineheight, 1.0f);
}
