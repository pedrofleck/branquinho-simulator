#version 450

// ===========================================================================
// Fragment Shader -- versão Vulkan
// ===========================================================================
//
// Veja os comentários no topo de "shader_vertex.vert" para a lista de
// diferenças em relação à versão OpenGL deste shader.
//

// Atributos de fragmentos recebidos como entrada ("in") pelo Fragment Shader.
// Estes atributos foram gerados pelo rasterizador como a interpolação dos
// atributos de saída do Vertex Shader. Os "location" abaixo precisam ser
// exatamente os mesmos usados em "shader_vertex.vert".
layout (location = 0) in vec4 position_world;
layout (location = 1) in vec4 position_model;
layout (location = 2) in vec4 normal;
layout (location = 3) in vec2 texcoords;

// Matrizes computadas no código C++ e enviadas para a GPU.
layout (set = 0, binding = 0) uniform SceneUniforms
{
    mat4 view;
    mat4 projection;
} scene;

// Identificador que define qual objeto está sendo desenhado no momento, além
// dos parâmetros da axis-aligned bounding box (AABB) do modelo.
//
// ATENÇÃO: este bloco precisa ser idêntico ao declarado em
// "shader_vertex.vert" e à struct ObjectConstants em "main.cpp".
layout (push_constant) uniform ObjectConstants
{
    mat4 model;
    vec4 bbox_min;
    vec4 bbox_max;
    int  object_id;
} object;

#define SPHERE 0
#define BUNNY  1
#define PLANE  2

// Variáveis para acesso das imagens de textura. Em Vulkan, um "sampler2D" é a
// combinação de uma imagem (VkImage/VkImageView) com um amostrador
// (VkSampler), e é associado ao shader através de um descriptor set.
layout (set = 0, binding = 1) uniform sampler2D TextureImage0;
layout (set = 0, binding = 2) uniform sampler2D TextureImage1;
layout (set = 0, binding = 3) uniform sampler2D TextureImage2;

// O valor de saída ("out") de um Fragment Shader é a cor final do fragmento.
layout (location = 0) out vec4 color;

// Constantes
#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923

void main()
{
    // Obtemos a posição da câmera utilizando a inversa da matriz que define o
    // sistema de coordenadas da câmera.
    vec4 origin = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 camera_position = inverse(scene.view) * origin;

    // O fragmento atual é coberto por um ponto que pertence à superfície de um
    // dos objetos virtuais da cena. Este ponto, p, possui uma posição no
    // sistema de coordenadas global (World coordinates). Esta posição é obtida
    // através da interpolação, feita pelo rasterizador, da posição de cada
    // vértice.
    vec4 p = position_world;

    // Normal do fragmento atual, interpolada pelo rasterizador a partir das
    // normais de cada vértice.
    vec4 n = normalize(normal);

    // Vetor que define o sentido da fonte de luz em relação ao ponto atual.
    vec4 l = normalize(vec4(1.0,1.0,0.0,0.0));

    // Vetor que define o sentido da câmera em relação ao ponto atual.
    vec4 v = normalize(camera_position - p);

    // Coordenadas de textura U e V
    float U = 0.0;
    float V = 0.0;

    // Coeficiente de refletância difusa
    vec3 Kd0 = vec3(0.0, 0.0, 0.0);

    if ( object.object_id == SPHERE )
    {
        // PREENCHA AQUI as coordenadas de textura da esfera, computadas com
        // projeção esférica EM COORDENADAS DO MODELO. Utilize como referência
        // o slides 134-150 do documento Aula_20_Mapeamento_de_Texturas.pdf.
        // A esfera que define a projeção deve estar centrada na posição
        // "bbox_center" definida abaixo.

        // Você deve utilizar:
        //   função 'length( )' : comprimento Euclidiano de um vetor
        //   função 'atan( , )' : arcotangente. Veja https://en.wikipedia.org/wiki/Atan2.
        //   função 'asin( )'   : seno inverso.
        //   constante M_PI
        //   variável position_model

        vec4 bbox_center = (object.bbox_min + object.bbox_max) / 2.0;
        vec4 d = position_model - bbox_center;

        float rho   = length(d);
        float theta = atan(d.x,d.z);
        float phi   = asin(d.y / rho);

        U = (theta + M_PI) / 2.0 / M_PI;
        V = (phi + M_PI_2) / M_PI;

        // Obtemos a refletância difusa a partir da leitura da imagem TextureImage0
        Kd0 = texture(TextureImage0, vec2(U,V)).rgb;
    }
    else if ( object.object_id == BUNNY )
    {
        // PREENCHA AQUI as coordenadas de textura do coelho, computadas com
        // projeção planar XY em COORDENADAS DO MODELO. Utilize como referência
        // o slides 99-104 do documento Aula_20_Mapeamento_de_Texturas.pdf,
        // e também use as variáveis min*/max* definidas abaixo para normalizar
        // as coordenadas de textura U e V dentro do intervalo [0,1]. Para
        // tanto, veja por exemplo o mapeamento da variável 'p_v' utilizando
        // 'h' no slides 158-160 do documento Aula_20_Mapeamento_de_Texturas.pdf.
        // Veja também a Questão 4 do Questionário 4 no Moodle.

        float minx = object.bbox_min.x;
        float maxx = object.bbox_max.x;

        float miny = object.bbox_min.y;
        float maxy = object.bbox_max.y;

        float minz = object.bbox_min.z;
        float maxz = object.bbox_max.z;

        U = (position_model.x - minx) / (maxx - minx);
        V = (position_model.y - miny) / (maxy - miny);

        // Obtemos a refletância difusa a partir da leitura da imagem TextureImage0
        Kd0 = texture(TextureImage0, vec2(U,V)).rgb;
    }
    else if ( object.object_id == PLANE )
    {
        // Coordenadas de textura do plano, obtidas do arquivo OBJ.
        U = texcoords.x;
        V = texcoords.y;

        // Obtemos a refletância difusa a partir da leitura da imagem TextureImage1
        Kd0 = texture(TextureImage1, vec2(U,V)).rgb;
    }

    // Equação de Iluminação
    float lambert = max(0.0,dot(n,l));

    color.rgb = Kd0 * (lambert + 0.01);

    // NOTE: Se você quiser fazer o rendering de objetos transparentes, é
    // necessário:
    // 1) Habilitar a operação de "blending" no pipeline gráfico que desenha os
    //    objetos transparentes. Em Vulkan isso é feito preenchendo a struct
    //    VkPipelineColorBlendAttachmentState com blendEnable = VK_TRUE e
    //    srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
    //    dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA.
    //    Veja como isso é feito para o texto em "src/textrendering.cpp".
    // 2) Realizar o desenho de todos objetos transparentes *após* ter desenhado
    //    todos os objetos opacos; e
    // 3) Realizar o desenho de objetos transparentes ordenados de acordo com
    //    suas distâncias para a câmera (desenhando primeiro objetos
    //    transparentes que estão mais longe da câmera).
    // Alpha default = 1 = 100% opaco = 0% transparente
    color.a = 1.0;

    // Cor final com correção gama, considerando monitor sRGB.
    //
    // NOTE: esta correção é feita "na mão" aqui porque o swapchain é criado com
    // um formato UNORM (e não sRGB). Veja ChooseSwapSurfaceFormat() em
    // "src/vkcontext.cpp" para a explicação completa.
    //
    // Veja https://en.wikipedia.org/w/index.php?title=Gamma_correction&oldid=751281772#Windows.2C_Mac.2C_sRGB_and_TV.2Fvideo_standard_gammas
    color.rgb = pow(color.rgb, vec3(1.0,1.0,1.0)/2.2);
}
