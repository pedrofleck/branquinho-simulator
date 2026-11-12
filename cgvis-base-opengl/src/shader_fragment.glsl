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

// ============================================================================
// PAINEL DE AJUSTE EM TEMPO REAL (HOT-RELOAD: Salve este arquivo e aperte 'R' no jogo!)
// ============================================================================
// --- 1. Postes de Iluminação Pública ---
const vec3  LAMP_COLOR          = vec3(1.00, 0.82, 0.50); // Cor amarela quente (0.0 a 1.0)
const float LAMP_INTENSITY      = 3.0;                    // Potência da luz do poste
const float LAMP_ATT_CONSTANT   = 1.0;
const float LAMP_ATT_LINEAR     = 0.07;                   // Menor = luz alcança mais longe
const float LAMP_ATT_QUADRATIC  = 0.017;                  // Menor = queda mais suave nas bordas
const float LAMP_INNER_COS      = 0.45;                   // Cone central (~63 graus)
const float LAMP_OUTER_COS      = 0.05;                   // Corte horizontal (~87 graus, 0 luz para cima)

// --- 2. Faróis Frontais do Ônibus (Tecla 'L' liga/desliga) ---
const vec3  HEADLIGHT_COLOR           = vec3(1.00, 0.96, 0.88); // Branco halógeno/LED automotivo
const float HEADLIGHT_INTENSITY       = 4.5;                    // Potência do facho dos faróis
const float HEADLIGHT_SPILL_INTENSITY = 0.18;                   // Dispersão periférica larga perto do parachoque
const float HEADLIGHT_SPILL_COS       = 0.45;                   // Abertura do halo periférico (~63 graus)

// --- 3. Emissão Visual das Lâmpadas/Lentes Acesas ---
const vec3  BULB_GLOW_COLOR     = vec3(1.00, 0.92, 0.72);
const float BULB_GLOW_INTENSITY = 1.8;

// --- 4. Textura do Terreno (Foto de Satélite satelite_2012.png + Micro-Detalhe) ---
const float TERRAIN_DETAIL_TILE     = 80.0; // Repetições da textura de detalhe pelo mapa de 1930x1703m
const float TERRAIN_DETAIL_STRENGTH = 0.15; // 0.0 = 100% foto de satélite pura; 0.15 = nitidez de perto
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
#define SPHERE    0
#define BUNNY     1
#define PLANE     2
#define BUS       3
#define LIGHTPOLE      4
#define LIGHTBULB      5
#define HEADLIGHT_LENS 6
uniform int object_id;

uniform vec4 bbox_min;  // Menor coordenada XYZ da caixa delimitadora do objeto
uniform vec4 bbox_max;  // Maior coordenada XYZ da caixa delimitadora do objeto

// ----------------------------------------------------------------------------
// Amostradores de Textura 2D (Unidades de textura ativas da GPU)
// ----------------------------------------------------------------------------
uniform sampler2D TextureImage0; // Textura difusa principal (ex.: lataria/tijolos)
uniform sampler2D TextureImage1; // Textura secundária (ex.: detalhe de solo/asfalto)
uniform sampler2D TextureImage2; // Textura de satélite do Campus do Vale (satelite_2012.png)
uniform int       has_satellite_texture; // 1 se satelite_2012.png foi carregada, 0 caso contrário

// ----------------------------------------------------------------------------
// Uniforms do Sistema de Iluminação Solar, Luzes Pontuais e Névoa (lighting.cpp)
// ----------------------------------------------------------------------------
uniform vec4  sun_direction;  // Vetor unitário l que aponta para o Sol (World coordinates)
uniform vec3  sun_color;      // Cor e intensidade da luz solar direta
uniform vec3  ambient_color;  // Intensidade da luz ambiente atmosférica
uniform vec3  fog_color;      // Cor da névoa no horizonte (sincronizada com o céu)
uniform float fog_density;    // Coeficiente de densidade da névoa exponencial

// Estrutura para fontes de luz locais direcionadas (Postes de Luz e Faróis)
struct PointLight
{
    vec3  position;         // Posição no mundo (XYZ)
    vec3  direction;        // Direção principal do feixe (ex.: (0, -1, 0) apontando para baixo)
    vec3  color;            // Cor multiplicadora vinda do C++ (0.0 a 1.0)
    float intensity;        // Intensidade multiplicadora vinda do C++
    float constant;         // Atenuação constante (kc)
    float linear;           // Atenuação linear (kl)
    float quadratic;        // Atenuação quadrática (kq)
    float inner_cutoff_cos; // Cosseno do ângulo interno do cone (iluminação máxima)
    float outer_cutoff_cos; // Cosseno do ângulo externo de corte (0 luz fora dele)
};

#define MAX_POINT_LIGHTS 12
uniform int        num_point_lights;
uniform PointLight point_lights[MAX_POINT_LIGHTS];

// ----------------------------------------------------------------------------
// Saída do Fragment Shader
// ----------------------------------------------------------------------------
out vec4 color;  // Cor RGBA final do fragmento enviada para o Framebuffer

// Constantes matemáticas
#define M_PI   3.14159265358979323846
#define M_PI_2 1.57079632679489661923

// ----------------------------------------------------------------------------
// Calcula a contribuição de uma fonte de luz pontual direcionada (poste ou farol)
// utilizando os parâmetros do painel de ajuste em tempo real + estado do C++.
// ----------------------------------------------------------------------------
vec3 CalcPointLight(PointLight light, vec4 n, vec4 p, vec4 v, vec3 Kd0, vec3 Ks, float q)
{
    vec3  light_vec = light.position - p.xyz;
    float dist      = length(light_vec);
    vec4  l         = vec4(light_vec / dist, 0.0);

    // Vetor que vai da fonte de luz até o fragmento
    vec3  light_to_frag = -l.xyz;
    vec3  spot_dir      = normalize(light.direction);
    float cos_theta     = dot(light_to_frag, spot_dir);

    bool  is_street_lamp = (spot_dir.y < -0.9);
    float spot_intensity = 0.0;
    float attenuation    = 0.0;
    vec3  final_light_color;

    if (is_street_lamp)
    {
        // Poste de iluminação pública (aponta verticalmente para baixo, 0 luz para cima)
        if (cos_theta <= LAMP_OUTER_COS)
            return vec3(0.0);

        float epsilon  = max(0.0001, LAMP_INNER_COS - LAMP_OUTER_COS);
        spot_intensity = clamp((cos_theta - LAMP_OUTER_COS) / epsilon, 0.0, 1.0);
        attenuation    = 1.0 / (LAMP_ATT_CONSTANT + LAMP_ATT_LINEAR * dist + LAMP_ATT_QUADRATIC * (dist * dist));
        final_light_color = (LAMP_COLOR * light.color) * (LAMP_INTENSITY * light.intensity);
    }
    else
    {
        // Farol automotivo do ônibus: combina o facho assimétrico concentrado (core)
        // com uma dispersão periférica suave mais aberta (spill/halo do refletor)
        if (cos_theta <= HEADLIGHT_SPILL_COS)
            return vec3(0.0);

        float core_eps   = max(0.0001, light.inner_cutoff_cos - light.outer_cutoff_cos);
        float core_beam  = clamp((cos_theta - light.outer_cutoff_cos) / core_eps, 0.0, 1.0);
        core_beam        = core_beam * core_beam; // Transição óptica suave no foco principal

        float spill_eps  = max(0.0001, light.outer_cutoff_cos - HEADLIGHT_SPILL_COS);
        float spill_beam = clamp((cos_theta - HEADLIGHT_SPILL_COS) / spill_eps, 0.0, 1.0);

        spot_intensity    = max(core_beam, spill_beam * HEADLIGHT_SPILL_INTENSITY);
        attenuation       = 1.0 / (light.constant + light.linear * dist + light.quadratic * (dist * dist));
        final_light_color = (HEADLIGHT_COLOR * light.color) * (HEADLIGHT_INTENSITY * light.intensity);
    }

    vec4  h     = normalize(l + v);
    float NdotL = max(0.0, dot(n, l));
    float NdotH = max(0.0, dot(n, h));
    float LdotH = max(0.0, dot(l, h));

    vec3  fresnel  = Ks + (vec3(1.0) - Ks) * pow(1.0 - LdotH, 5.0);
    vec3  specular = fresnel * pow(NdotH, q) * NdotL;

    return (Kd0 * NdotL + specular) * final_light_color * attenuation * spot_intensity;
}

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

    // emissive: Luz própria emitida pelo objeto (ex.: bulbo da lâmpada do poste)
    vec3  emissive = vec3(0.0, 0.0, 0.0);

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
        Ks  = vec3(0.50, 0.50, 0.50); // Lataria do ônibus possui reflexo especular nítido
        q   = 64.0;                    // Destaque especular concentrado
    }
    else if ( object_id == PLANE )
    {
        // Coordenadas UV normalizadas [0.0 .. 1.0] cobrindo o mapa inteiro de vale.png
        U = texcoords.x;
        V = texcoords.y;

        vec3 detail_color = texture(TextureImage1, vec2(U, V) * TERRAIN_DETAIL_TILE).rgb;

        if (has_satellite_texture == 1)
        {
            vec3 sat_color    = texture(TextureImage2, vec2(U, V)).rgb;
            float detail_gain = mix(1.0, (detail_color.r + detail_color.g + detail_color.b) / 1.5, TERRAIN_DETAIL_STRENGTH);
            Kd0 = sat_color * detail_gain;
        }
        else
        {
            Kd0 = detail_color;
        }

        Ks  = vec3(0.05, 0.05, 0.05); // Chão fosco / asfalto (pouco reflexo especular)
        q   = 16.0;                    // Destaque especular suave e disperso
    }
    else if ( object_id == LIGHTPOLE )
    {
        // Estrutura metálica cinza escura do poste de iluminação
        Kd0 = vec3(0.22, 0.23, 0.25);
        Ks  = vec3(0.60, 0.60, 0.65);
        q   = 64.0;
    }
    else if ( object_id == LIGHTBULB )
    {
        // Bulbo luminoso no topo do poste (usa as constantes do painel de hot-reload)
        Kd0      = BULB_GLOW_COLOR;
        Ks       = vec3(0.8, 0.8, 0.8);
        q        = 64.0;
        emissive = BULB_GLOW_COLOR * BULB_GLOW_INTENSITY;
    }
    else if ( object_id == HEADLIGHT_LENS )
    {
        // Na malha branquinho.obj, _tex_low_beam__emissive_ agrupa os faróis frontais
        // (z ~ -7.88, |x| ~ 1.08) junto com luzes marcadoras na lateral do ônibus (|x| ~ 1.27, z > -7.65).
        // Filtramos por position_model para acender SOMENTE as lentes frontais do parachoque:
        if (position_model.z < -7.75 && position_model.y < 1.50 && abs(position_model.x) < 1.20)
        {
            Kd0      = HEADLIGHT_COLOR;
            Ks       = vec3(0.9, 0.9, 0.9);
            q        = 96.0;
            emissive = HEADLIGHT_COLOR * (BULB_GLOW_INTENSITY * 1.2);
        }
        else
        {
            // Luzes laterais permanecem apagadas com o material padrão do ônibus
            Kd0 = vec3(0.95, 0.95, 0.95);
            Ks  = vec3(0.50, 0.50, 0.50);
            q   = 64.0;
        }
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

    // Equação da cor iluminada pelo Sol e ambiente:
    // Cor = (Luz Ambiente * Kd) + (Luz Direta do Sol * (Kd * Difuso + Especular))
    vec3 lit_color = Kd0 * ambient_color + (Kd0 * NdotL + specular_term) * sun_color;

    // Acumula a contribuição de todas as luzes pontuais/direcionadas (ex.: postes de luz)
    for (int i = 0; i < num_point_lights; ++i)
    {
        lit_color += CalcPointLight(point_lights[i], n, p, v, Kd0, Ks, q);
    }

    // Soma a emissão própria do material (ex.: lâmpada acesa)
    lit_color += emissive;

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
