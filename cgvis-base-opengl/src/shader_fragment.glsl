#version 330 core

// ============================================================================
// shader_fragment.glsl - Fragment Shader Principal (Pipeline de Renderização)
//
// Executado na GPU uma vez para cada fragmento (pixel potencial) gerado pelo
// processo de rasterização dos triângulos. Responsável por:
//
//   1. Coordenadas de Textura (U, V) e Splatting Multi-Material do Terreno:
//      - Combina 8 materiais (splatmap_0 e splatmap_1) com modulação cromática
//        real de alta resolução (macro_albedo 4K) e micro-detalhe seamless.
//      - Preserva o modo de debug via F10 (satelite_2012_low.png via TextureImage2).
//      - Renderiza as árvores 3D instanciadas (TREE_TRUNK e TREE_CANOPY).
//
//   2. Modelo de Iluminação Fotométrico (Disney BRDF / Blinn-Phong):
//      - Difusa de Lambert: espalhamento isotrópico proporcional a dot(n, l).
//      - Especular de Blinn-Phong: reflexo direcionado usando o half-vector h.
//      - Efeito Fresnel de Schlick: reflexão intensificada em ângulos rasantes.
//      - Parâmetros Ks e q dinâmicos por material (água, asfalto, grama/terra).
//
//   3. Névoa Atmosférica Exponencial (Distance Fog) e Correção Gamma (sRGB).
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

// --- 4. Textura do Terreno (8 Materiais Placeholder via Texture2DArray) ---
const vec2  TERRAIN_WORLD_METERS        = vec2(1930.0, 1703.0); // 1 pixel = 1.0 metro em vale_processed.png
const float TERRAIN_MACRO_TINT_STRENGTH = 0.30;                 // Modulação cromática suave apenas em áreas naturais
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
#define SPHERE         0
#define BUNNY          1
#define PLANE          2
#define BUS            3
#define LIGHTPOLE      4
#define LIGHTBULB      5
#define HEADLIGHT_LENS 6
#define TREE_TRUNK     7
#define TREE_CANOPY    8
uniform int object_id;

uniform vec4 bbox_min;  // Menor coordenada XYZ da caixa delimitadora do objeto
uniform vec4 bbox_max;  // Maior coordenada XYZ da caixa delimitadora do objeto

// ----------------------------------------------------------------------------
// Amostradores de Textura 2D (Unidades de textura ativas da GPU)
// ----------------------------------------------------------------------------
uniform sampler2D      TextureImage0;      // Unit 0: Textura difusa principal (tijolos)
uniform sampler2D      TextureImage1;      // Unit 1: Textura secundária (detalhe rochoso)
uniform sampler2D      TextureImage2;      // Unit 2: Ortofoto satelite_2012_low.png (alternada via F10)
uniform sampler2D      TerrainSplat0;      // Unit 3: Splatmap 0 RGBA (R=Asfalto Prin, G=Asfalto Sec, B=Calçada, A=Grama)
uniform sampler2D      TerrainSplat1;      // Unit 4: Splatmap 1 RGBA (R=Mata, G=Terra, B=Água, A=Prédio)
uniform sampler2D      TerrainMacroAlbedo; // Unit 5: Macro Albedo (sutil variação cromática regional)
uniform sampler2DArray TerrainMaterials;   // Unit 6: 8 texturas seamless placeholder (mat_0..mat_7)
uniform sampler2D      TreeBarkTex;        // Unit 7: Textura placeholder de casca de tronco
uniform sampler2D      TreeFoliageTex;     // Unit 8: Textura placeholder de folhagem de árvore

uniform int has_satellite_texture;    // 1 quando o modo de satélite (F10) está ativo
uniform int has_terrain_splatmaps;    // 1 quando os ativos de terreno multi-material foram carregados

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
    vec4 origin = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 camera_position = inverse(view) * origin;

    vec4 p = position_world;
    vec4 n = normalize(normal);
    vec4 l = normalize(sun_direction);
    vec4 v = normalize(camera_position - p);
    vec4 h = normalize(l + v);

    // ========================================================================
    // 2. Mapeamento de Textura e Propriedades Ópticas do Material
    // ========================================================================
    float U = 0.0;
    float V = 0.0;

    vec3  Kd0      = vec3(0.8, 0.8, 0.8);
    vec3  Ks       = vec3(0.2, 0.2, 0.2);
    float q        = 32.0;
    vec3  emissive = vec3(0.0, 0.0, 0.0);

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

        Kd0 = vec3(0.95, 0.95, 0.95);
        Ks  = vec3(0.50, 0.50, 0.50);
        q   = 64.0;
    }
    else if ( object_id == PLANE )
    {
        U = texcoords.x;
        V = texcoords.y;

        // No modo satélite (F10), exibe satelite_2012_low.png (TextureImage2)
        if (has_satellite_texture == 1)
        {
            vec3 detail_color = texture(TextureImage1, vec2(U, V) * 80.0).rgb;
            vec3 sat_color    = texture(TextureImage2, vec2(U, V)).rgb;
            float detail_gain = mix(1.0, (detail_color.r + detail_color.g + detail_color.b) / 1.5, 0.15);
            Kd0 = sat_color * detail_gain;
            Ks  = vec3(0.05, 0.05, 0.05);
            q   = 16.0;
        }
        else if (has_terrain_splatmaps == 1)
        {
            // Lê os pesos dos 8 materiais nos Splatmaps RGBA lineares
            vec4 s0 = texture(TerrainSplat0, vec2(U, V));
            vec4 s1 = texture(TerrainSplat1, vec2(U, V));
            float w_sum = (s0.r + s0.g + s0.b + s0.a) + (s1.r + s1.g + s1.b + s1.a);

            // Descarta fragmentos fora da máscara válida (áreas em preto de bruto_2012_mascara.tiff)
            if (w_sum < 0.02)
                discard;

            s0 /= w_sum;
            s1 /= w_sum;

            float w_asph_main = s0.r;
            float w_asph_sec  = s0.g;
            float w_sidewalk  = s0.b;
            float w_grass     = s0.a;
            float w_forest    = s1.r;
            float w_dirt      = s1.g;
            float w_water     = s1.b;
            float w_building  = s1.a;

            // Amostragem métrica das 8 texturas placeholder no Texture2DArray (sem usar o satélite borrado como piso)
            vec2 world_m = vec2(U, V) * TERRAIN_WORLD_METERS;

            vec2 uv_road     = world_m / 4.8;
            vec2 uv_sidewalk = world_m / 3.6;
            vec2 uv_organic  = world_m / 5.5;
            vec2 uv_org_far  = world_m / 19.0 + vec2(0.37, 0.71);
            vec2 uv_water    = world_m / 11.0;
            vec2 uv_building = world_m / 6.0;

            vec3 c_asph_main = texture(TerrainMaterials, vec3(uv_road,     0.0)).rgb;
            vec3 c_asph_sec  = texture(TerrainMaterials, vec3(uv_road,     1.0)).rgb;
            vec3 c_sidewalk  = texture(TerrainMaterials, vec3(uv_sidewalk, 2.0)).rgb;
            vec3 c_grass     = mix(texture(TerrainMaterials, vec3(uv_organic, 3.0)).rgb,
                                   texture(TerrainMaterials, vec3(uv_org_far, 3.0)).rgb, 0.28);
            vec3 c_forest    = mix(texture(TerrainMaterials, vec3(uv_organic, 4.0)).rgb,
                                   texture(TerrainMaterials, vec3(uv_org_far, 4.0)).rgb, 0.28);
            vec3 c_dirt      = mix(texture(TerrainMaterials, vec3(uv_organic, 5.0)).rgb,
                                   texture(TerrainMaterials, vec3(uv_org_far, 5.0)).rgb, 0.28);
            vec3 c_water     = texture(TerrainMaterials, vec3(uv_water,    6.0)).rgb;
            vec3 c_building  = texture(TerrainMaterials, vec3(uv_building, 7.0)).rgb;

            vec3 mat_color = w_asph_main * c_asph_main
                           + w_asph_sec  * c_asph_sec
                           + w_sidewalk  * c_sidewalk
                           + w_grass     * c_grass
                           + w_forest    * c_forest
                           + w_dirt      * c_dirt
                           + w_water     * c_water
                           + w_building  * c_building;

            // Modulação cromática sutil (chroma-only) apenas sobre grama/mata/terra
            vec3 macro_col   = texture(TerrainMacroAlbedo, vec2(U, V)).rgb;
            float macro_lum  = max(dot(macro_col, vec3(0.299, 0.587, 0.114)), 0.015);
            vec3 chroma_tint = clamp(macro_col / macro_lum, vec3(0.75), vec3(1.25));
            float w_organic  = clamp(w_grass + w_forest + w_dirt, 0.0, 1.0);
            Kd0 = mat_color * mix(vec3(1.0), chroma_tint, TERRAIN_MACRO_TINT_STRENGTH * w_organic);

            // Propriedades fotométricas padrão de terreno (água com textura estática simples por enquanto)
            float w_asph  = w_asph_main + w_asph_sec;
            float w_paved = w_sidewalk + w_building;
            Ks = w_asph * vec3(0.10, 0.10, 0.10) + (1.0 - w_asph) * vec3(0.04, 0.04, 0.04);
            q  = w_asph * 24.0 + w_paved * 14.0 + (1.0 - w_asph - w_paved) * 8.0;
        }
        else
        {
            Kd0 = texture(TextureImage1, vec2(U, V) * 80.0).rgb;
            Ks  = vec3(0.05, 0.05, 0.05);
            q   = 16.0;
        }
    }
    else if ( object_id == TREE_TRUNK )
    {
        U = texcoords.x * 2.0;
        V = texcoords.y * 3.0;
        Kd0 = texture(TreeBarkTex, vec2(U, V)).rgb;
        Ks  = vec3(0.03, 0.03, 0.03);
        q   = 8.0;
    }
    else if ( object_id == TREE_CANOPY )
    {
        U = texcoords.x * 3.0;
        V = texcoords.y * 3.0;
        vec3 fol_tex = texture(TreeFoliageTex, vec2(U, V)).rgb;

        // Harmoniza sutilmente o tom da copa com o macro_albedo da região onde a árvore está plantada
        vec2 terrain_uv = vec2(p.x / 1930.0 + 0.5, p.z / 1703.0 + 0.5);
        vec3 local_sat  = texture(TerrainMacroAlbedo, clamp(terrain_uv, 0.0, 1.0)).rgb;
        float shade_grad = mix(0.72, 1.14, clamp(n.y * 0.5 + 0.5, 0.0, 1.0));

        Kd0 = mix(fol_tex, local_sat * 1.15, 0.32) * shade_grad;
        Ks  = vec3(0.04, 0.05, 0.03);
        q   = 10.0;
    }
    else if ( object_id == LIGHTPOLE )
    {
        Kd0 = vec3(0.22, 0.23, 0.25);
        Ks  = vec3(0.60, 0.60, 0.65);
        q   = 64.0;
    }
    else if ( object_id == LIGHTBULB )
    {
        Kd0      = BULB_GLOW_COLOR;
        Ks       = vec3(0.8, 0.8, 0.8);
        q        = 64.0;
        emissive = BULB_GLOW_COLOR * BULB_GLOW_INTENSITY;
    }
    else if ( object_id == HEADLIGHT_LENS )
    {
        if (position_model.z < -7.75 && position_model.y < 1.50 && abs(position_model.x) < 1.20)
        {
            Kd0      = HEADLIGHT_COLOR;
            Ks       = vec3(0.9, 0.9, 0.9);
            q        = 96.0;
            emissive = HEADLIGHT_COLOR * (BULB_GLOW_INTENSITY * 1.2);
        }
        else
        {
            Kd0 = vec3(0.95, 0.95, 0.95);
            Ks  = vec3(0.50, 0.50, 0.50);
            q   = 64.0;
        }
    }

    // ========================================================================
    // 3. Equação de Iluminação (Difusa + Especular com Aproximação de Fresnel)
    // ========================================================================
    float NdotL = max(0.0, dot(n, l));
    float NdotH = max(0.0, dot(n, h));
    float LdotH = max(0.0, dot(l, h));

    vec3 fresnel = Ks + (vec3(1.0) - Ks) * pow(1.0 - LdotH, 5.0);
    vec3 specular_term = fresnel * pow(NdotH, q) * NdotL;

    vec3 lit_color = Kd0 * ambient_color + (Kd0 * NdotL + specular_term) * sun_color;

    for (int i = 0; i < num_point_lights; ++i)
    {
        lit_color += CalcPointLight(point_lights[i], n, p, v, Kd0, Ks, q);
    }

    lit_color += emissive;

    // ========================================================================
    // 4. Correção Gamma (Linear -> sRGB)
    // ========================================================================
    lit_color = pow(lit_color, vec3(1.0, 1.0, 1.0) / 2.2);

    // ========================================================================
    // 5. Névoa Atmosférica Exponencial (Distance Fog)
    // ========================================================================
    float dist = length(camera_position - p);
    float fog_factor = exp(-fog_density * dist);
    fog_factor = clamp(fog_factor, 0.0, 1.0);

    color.rgb = mix(fog_color, lit_color, fog_factor);
    color.a = 1.0;
}
