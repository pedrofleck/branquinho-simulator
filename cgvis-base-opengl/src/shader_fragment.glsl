#version 330 core

// ============================================================================
// shader_fragment.glsl - Fragment Shader principal da cena
//
// Executado uma vez por fragmento na GPU. Responsável por:
//   - Determinar as coordenadas de textura (U,V) de acordo com o objeto sendo
//     desenhado (object_id): coordenadas do .obj para o ônibus e o plano,
//     ou projeções esférica/planar para objetos de teste;
//   - Amostrar a refletância difusa (Kd) das texturas;
//   - Aplicar o modelo de iluminação (Difuso + Especular Blinn-Phong/BRDF),
//     névoa exponencial por distância e correção gamma (sRGB).
// ============================================================================

// Atributos de fragmentos recebidos como entrada ("in") pelo Fragment Shader.
in vec4 position_world;
in vec4 normal;
in vec4 position_model;
in vec2 texcoords;

// Matrizes computadas no código C++ e enviadas para a GPU
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

// Identificador que define qual objeto está sendo desenhado no momento
#define SPHERE 0
#define BUNNY  1
#define PLANE  2
#define BUS    3
uniform int object_id;

// Parâmetros da axis-aligned bounding box (AABB) do modelo
uniform vec4 bbox_min;
uniform vec4 bbox_max;

// Variáveis para acesso das imagens de textura
uniform sampler2D TextureImage0;
uniform sampler2D TextureImage1;
uniform sampler2D TextureImage2;

// Parâmetros do sistema de iluminação (módulo lighting.h / lighting.cpp)
uniform vec4  sun_direction;
uniform vec3  sun_color;
uniform vec3  ambient_color;
uniform vec3  fog_color;
uniform float fog_density;

// O valor de saída ("out") de um Fragment Shader é a cor final do fragmento.
out vec4 color;

// Constantes
#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923

void main()
{
    // Obtemos a posição da câmera utilizando a inversa da matriz View
    vec4 origin = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 camera_position = inverse(view) * origin;

    // Ponto atual da superfície em coordenadas do mundo (World coordinates)
    vec4 p = position_world;

    // Normal da superfície e vetores de luz (l), visão (v) e half-vector (h)
    vec4 n = normalize(normal);
    vec4 l = normalize(sun_direction);
    vec4 v = normalize(camera_position - p);
    vec4 h = normalize(l + v);

    // Coordenadas de textura U e V
    float U = 0.0;
    float V = 0.0;

    // Propriedades ópticas da superfície (difusa Kd, especular Ks e expoente q)
    vec3  Kd0 = vec3(0.8, 0.8, 0.8);
    vec3  Ks  = vec3(0.2, 0.2, 0.2);
    float q   = 32.0;

    if ( object_id == SPHERE )
    {
        vec4 bbox_center = (bbox_min + bbox_max) / 2.0;
        vec4 d = position_model - bbox_center;

        float rho   = length(d);
        float theta = atan(d.x, d.z);
        float phi   = asin(d.y / rho);

        U = (theta + M_PI) / 2.0 / M_PI;
        V = (phi + M_PI_2) / M_PI;

        Kd0 = texture(TextureImage0, vec2(U, V)).rgb;
    }
    else if ( object_id == BUNNY )
    {
        float minx = bbox_min.x;
        float maxx = bbox_max.x;
        float miny = bbox_min.y;
        float maxy = bbox_max.y;

        U = (position_model.x - minx) / (maxx - minx);
        V = (position_model.y - miny) / (maxy - miny);

        Kd0 = texture(TextureImage0, vec2(U, V)).rgb;
    }
    else if ( object_id == BUS )
    {
        U = texcoords.x;
        V = texcoords.y;

        Kd0 = texture(TextureImage0, vec2(U, V)).rgb;
        Ks  = vec3(0.35, 0.35, 0.35); // Brilho especular da lataria
        q   = 64.0;
    }
    else if ( object_id == PLANE )
    {
        U = texcoords.x;
        V = texcoords.y;

        Kd0 = texture(TextureImage1, vec2(U, V)).rgb;
        Ks  = vec3(0.05, 0.05, 0.05); // Chão mais fosco (menos reflexo especular)
        q   = 16.0;
    }

    // 1. Termo Difuso (Lambert) + Termo Especular (Blinn-Phong com half-vector h)
    float NdotL = max(0.0, dot(n, l));
    float NdotH = max(0.0, dot(n, h));
    float LdotH = max(0.0, dot(l, h));

    // Aproximação de Schlick para o efeito Fresnel (base do Disney BRDF):
    // ângulos rasantes refletem mais luz especular
    vec3 fresnel = Ks + (vec3(1.0) - Ks) * pow(1.0 - LdotH, 5.0);
    vec3 specular_term = fresnel * pow(NdotH, q) * NdotL;

    // Combinação da luz ambiente do céu + luz direta do Sol (difusa + especular)
    vec3 lit_color = Kd0 * ambient_color + (Kd0 * NdotL + specular_term) * sun_color;

    // 2. Correção gamma na cor iluminada (espaço linear -> sRGB)
    lit_color = pow(lit_color, vec3(1.0, 1.0, 1.0) / 2.2);

    // 3. Névoa Exponencial com a distância até a câmera (funde suavemente com o céu)
    float dist = length(camera_position - p);
    float fog_factor = exp(-fog_density * dist);
    fog_factor = clamp(fog_factor, 0.0, 1.0);

    color.rgb = mix(fog_color, lit_color, fog_factor);
    color.a = 1.0;
}
