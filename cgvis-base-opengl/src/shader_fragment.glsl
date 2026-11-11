#version 330 core

// ============================================================================
// shader_fragment.glsl - Fragment Shader Principal (Pipeline de Renderização)
//
// Executado na GPU uma vez para cada fragmento (pixel potencial) gerado pelo
// processo de rasterização dos triângulos. Responsável por:
//
//   1. Coordenadas de Textura (U, V):
//      - Mapeia atributos 'vt' do OBJ para o ônibus (BUS) e chão (PLANE).
//      - Suporta projeção esférica e planar para objetos de teste (SPHERE/BUNNY).
//
//   2. Modelo de Iluminação Fotométrico (Disney BRDF / Blinn-Phong):
//      - Difusa de Lambert: espalhamento isotrópico proporcional a dot(n, l).
//      - Especular de Blinn-Phong: reflexo direcionado usando o half-vector h.
//      - Efeito Fresnel de Schlick: reflexão intensificada em ângulos de visão rasantes.
//      - Luz ambiente: espalhamento indireto da luz da abóbada celeste.
//
//   3. Névoa Atmosférica Exponencial (Distance Fog):
//      - Simula a dispersão de luz pela atmosfera conforme a distância da câmera,
//        fundindo suavemente objetos distantes com a cor do horizonte/céu.
//
//   4. Correção Gamma:
//      - Converte a iluminação calculada em espaço linear para o espaço de cor
//        sRGB do monitor (potência de 1/2.2).
// ============================================================================

// ----------------------------------------------------------------------------
// Atributos de Entrada (Interpolados linearmente pelo rasterizador a partir dos vértices)
// ----------------------------------------------------------------------------
in vec4 position_world;  // Posição do ponto no sistema de coordenadas do mundo (World)
in vec4 normal;          // Vetor normal à superfície no ponto atual (World)
in vec4 position_model;  // Posição local do vértice dentro do modelo OBJ original
in vec2 texcoords;       // Coordenadas de textura UV obtidas do arquivo OBJ

// ----------------------------------------------------------------------------
// Uniforms de Transformação (Comuns da cena)
// ----------------------------------------------------------------------------
uniform mat4 model;       // Matriz Model: coordenadas locais -> coordenadas do mundo
uniform mat4 view;        // Matriz View: coordenadas do mundo -> coordenadas da câmera
uniform mat4 projection;  // Matriz Projection: coordenadas da câmera -> NDC

// ----------------------------------------------------------------------------
// Identificador do Objeto e Bounding Box
// ----------------------------------------------------------------------------
#define SPHERE 0
#define BUNNY  1
#define PLANE  2
#define BUS    3
uniform int object_id;

uniform vec4 bbox_min;  // Menor coordenada XYZ da caixa delimitadora do objeto
uniform vec4 bbox_max;  // Maior coordenada XYZ da caixa delimitadora do objeto

// ----------------------------------------------------------------------------
// Amostradores de Textura 2D (Unidades de textura ativas da GPU)
// ----------------------------------------------------------------------------
uniform sampler2D TextureImage0; // Textura difusa principal (ex.: lataria/tijolos)
uniform sampler2D TextureImage1; // Textura secundária (ex.: terreno/asfalto)
uniform sampler2D TextureImage2; // Textura de detalhes adicionais

// ----------------------------------------------------------------------------
// Uniforms do Sistema de Iluminação Solar e Névoa (vindos de lighting.cpp)
// ----------------------------------------------------------------------------
uniform vec4  sun_direction;  // Vetor unitário l que aponta para o Sol (World coordinates)
uniform vec3  sun_color;      // Cor e intensidade da luz solar direta
uniform vec3  ambient_color;  // Intensidade da luz ambiente atmosférica
uniform vec3  fog_color;      // Cor da névoa no horizonte (sincronizada com o céu)
uniform float fog_density;    // Coeficiente de densidade da névoa exponencial

// ----------------------------------------------------------------------------
// Saída do Fragment Shader
// ----------------------------------------------------------------------------
out vec4 color;  // Cor RGBA final do fragmento enviada para o Framebuffer

// Constantes matemáticas
#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923

void main()
{
    // ========================================================================
    // 1. Posição da Câmera e Vetores Geométricos de Iluminação
    // ========================================================================

    // A posição da câmera no mundo é obtida aplicando a inversa da matriz View
    // à origem do espaço da câmera (0, 0, 0, 1).
    vec4 origin = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 camera_position = inverse(view) * origin;

    // Ponto da superfície sendo renderizado no espaço do mundo
    vec4 p = position_world;

    // n: Vetor normal à superfície, renormalizado após interpolação do rasterizador
    vec4 n = normalize(normal);

    // l: Vetor unitário que aponta da superfície em direção à fonte de luz (Sol)
    vec4 l = normalize(sun_direction);

    // v: Vetor unitário que aponta da superfície em direção ao observador (Câmera)
    vec4 v = normalize(camera_position - p);

    // h: Half-vector de Blinn-Phong (bissetriz entre a luz e o observador).
    // Representa a orientação ideal de uma microrrugosidade para refletir luz especular diretamente na câmera.
    vec4 h = normalize(l + v);

    // ========================================================================
    // 2. Mapeamento de Textura e Propriedades Ópticas do Material
    // ========================================================================
    float U = 0.0;
    float V = 0.0;

    // Kd0: Coeficiente de refletância difusa (cor base da superfície)
    vec3  Kd0 = vec3(0.8, 0.8, 0.8);

    // Ks: Coeficiente de refletância especular (capacidade de reflexão espelhada)
    vec3  Ks  = vec3(0.2, 0.2, 0.2);

    // q: Expoente de rugosidade/brilho de Phong (valores maiores = brilho menor e mais concentrado)
    float q   = 32.0;

    if ( object_id == SPHERE )
    {
        // Projeção esférica baseada nas coordenadas do modelo
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
        // Projeção planar XY normalizada pela bounding box
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
        // Coordenadas UV vindas diretamente dos atributos 'vt' do arquivo branquinho.obj
        U = texcoords.x;
        V = texcoords.y;

        Kd0 = vec3(0.95, 0.95, 0.95); // placeholder de "textura" branca
        // Kd0 = texture(TextureImage0, vec2(U, V)).rgb; // antiga textura de tijolos
        Ks  = vec3(0.35, 0.35, 0.35); // Lataria do ônibus possui reflexo especular nítido
        q   = 64.0;                    // Destaque especular concentrado
    }
    else if ( object_id == PLANE )
    {
        // Coordenadas UV do plano do chão vindas do plane.obj
        U = texcoords.x;
        V = texcoords.y;

        Kd0 = texture(TextureImage1, vec2(U, V)).rgb;
        Ks  = vec3(0.05, 0.05, 0.05); // Chão fosco / asfalto (pouco reflexo especular)
        q   = 16.0;                    // Destaque especular suave e disperso
    }

    // ========================================================================
    // 3. Equação de Iluminação (Difusa + Especular com Aproximação de Fresnel)
    // ========================================================================

    // Produtos escalares fundamentais do modelo de iluminação:
    // NdotL: Cosseno do ângulo de incidência da luz (Lei do Cosseno de Lambert)
    float NdotL = max(0.0, dot(n, l));

    // NdotH: Proximidade da normal ao half-vector (intensidade do brilho especular)
    float NdotH = max(0.0, dot(n, h));

    // LdotH: Cosseno do ângulo entre a luz e o half-vector (usado no efeito Fresnel)
    float LdotH = max(0.0, dot(l, h));

    // Aproximação de Schlick para o Efeito Fresnel:
    // Superfícies dielétricas refletem uma fração Ks de luz quando vistas de frente,
    // mas a refletância sobe para 1.0 (100%) em ângulos tangenciais/rasantes.
    vec3 fresnel = Ks + (vec3(1.0) - Ks) * pow(1.0 - LdotH, 5.0);

    // Termo especular Blinn-Phong ponderado pelo Fresnel e atenuado por NdotL
    vec3 specular_term = fresnel * pow(NdotH, q) * NdotL;

    // Equação completa da cor iluminada:
    // Cor = (Luz Ambiente * Kd) + (Luz Direta do Sol * (Kd * Difuso + Especular))
    vec3 lit_color = Kd0 * ambient_color + (Kd0 * NdotL + specular_term) * sun_color;

    // ========================================================================
    // 4. Correção Gamma (Linear -> sRGB)
    // ========================================================================
    // Todos os cálculos de física da luz (somas, multiplicações de radiação) ocorrem
    // em espaço de cor linear. Os monitores aplicam uma curva gamma padrão (~2.2),
    // portanto devemos aplicar o inverso (potência de 1/2.2) antes da exibição.
    lit_color = pow(lit_color, vec3(1.0, 1.0, 1.0) / 2.2);

    // ========================================================================
    // 5. Névoa Atmosférica Exponencial (Distance Fog)
    // ========================================================================
    // Modela a extinção da luz refletida do objeto e a sobreposição da luz
    // dispersada pela atmosfera ao longo da distância de visualização:
    //   fog_factor = exp(-densidade * distancia)
    // Quando a distância é zero, fog_factor = 1.0 (cor pura do objeto).
    // Em distâncias grandes, fog_factor -> 0.0 (objeto se funde à cor do céu).
    float dist = length(camera_position - p);
    float fog_factor = exp(-fog_density * dist);
    fog_factor = clamp(fog_factor, 0.0, 1.0);

    // Mistura linear entre a cor do objeto iluminado e a cor do horizonte
    color.rgb = mix(fog_color, lit_color, fog_factor);
    color.a = 1.0;
}
