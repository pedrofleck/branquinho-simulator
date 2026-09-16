#version 450

// ===========================================================================
// Vertex Shader -- versão Vulkan
// ===========================================================================
//
// Diferenças em relação ao mesmo shader em OpenGL ("shader_vertex.glsl"):
//
//  1) "#version 450" ao invés de "#version 330 core". O Vulkan consome
//     SPIR-V, e o compilador glslc traduz este GLSL para SPIR-V.
//
//  2) Em Vulkan não existem "uniforms soltos" (como "uniform mat4 model;").
//     Todo uniform precisa estar dentro de um bloco, e todo bloco precisa de
//     um "descriptor set" e um "binding" explícitos. Abaixo, as matrizes
//     "view" e "projection" (que mudam uma vez por quadro) vivem em um
//     Uniform Buffer Object, e a matriz "model" (que muda a cada objeto
//     desenhado) vive em "push constants", que são muito mais baratas.
//
//  3) As variáveis de saída ("out") também precisam de um "location"
//     explícito, que deve ser o mesmo usado no Fragment Shader.
//

// Atributos de vértice recebidos como entrada ("in") pelo Vertex Shader.
// Veja a função BuildTrianglesAndAddToVirtualScene() em "main.cpp".
layout (location = 0) in vec4 model_coefficients;
layout (location = 1) in vec4 normal_coefficients;
layout (location = 2) in vec2 texture_coefficients;

// Matrizes de câmera e projeção, enviadas uma vez por quadro pelo código C++
// através de um Uniform Buffer Object.
layout (set = 0, binding = 0) uniform SceneUniforms
{
    mat4 view;
    mat4 projection;
} scene;

// Dados que mudam a cada objeto desenhado. Push constants são gravadas
// diretamente no command buffer com vkCmdPushConstants(), e por isso são o
// substituto natural das chamadas glUniform*() feitas antes de cada desenho.
//
// ATENÇÃO: este bloco precisa ser idêntico ao declarado em
// "shader_fragment.frag" e à struct ObjectConstants em "main.cpp".
layout (push_constant) uniform ObjectConstants
{
    mat4 model;
    vec4 bbox_min;
    vec4 bbox_max;
    int  object_id;
} object;

// Atributos de vértice que serão gerados como saída ("out") pelo Vertex Shader.
// ** Estes serão interpolados pelo rasterizador! ** gerando, assim, valores
// para cada fragmento, os quais serão recebidos como entrada pelo Fragment
// Shader. Veja o arquivo "shader_fragment.frag".
layout (location = 0) out vec4 position_world;
layout (location = 1) out vec4 position_model;
layout (location = 2) out vec4 normal;
layout (location = 3) out vec2 texcoords;

void main()
{
    // A variável gl_Position define a posição final de cada vértice
    // OBRIGATORIAMENTE em "normalized device coordinates" (NDC).
    //
    // O código em "main.cpp" define os vértices dos modelos em coordenadas
    // locais de cada modelo (array model_coefficients). Abaixo, utilizamos
    // operações de modelagem, definição da câmera, e projeção, para computar
    // as coordenadas finais em NDC (variável gl_Position). Após a execução
    // deste Vertex Shader, a placa de vídeo (GPU) fará a divisão por W.
    //
    // NOTE: a matriz "scene.projection" enviada pelo C++ já inclui a matriz de
    // correção Matrix_Vulkan_Clip() (veja "include/matrices.h"), que converte
    // do cubo NDC do OpenGL (visto em aula) para o do Vulkan.
    gl_Position = scene.projection * scene.view * object.model * model_coefficients;

    // Agora definimos outros atributos dos vértices que serão interpolados pelo
    // rasterizador para gerar atributos únicos para cada fragmento gerado.

    // Posição do vértice atual no sistema de coordenadas global (World).
    position_world = object.model * model_coefficients;

    // Posição do vértice atual no sistema de coordenadas local do modelo.
    position_model = model_coefficients;

    // Normal do vértice atual no sistema de coordenadas global (World).
    // Veja slides 123-151 do documento Aula_07_Transformacoes_Geometricas_3D.pdf.
    normal = inverse(transpose(object.model)) * normal_coefficients;
    normal.w = 0.0;

    // Coordenadas de textura obtidas do arquivo OBJ (se existirem!)
    texcoords = texture_coefficients;
}
