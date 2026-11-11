// ============================================================================
// lighting.h - Interface do módulo de iluminação solar, hora do dia e névoa
//
// Define a classe Lighting, responsável por:
//   - Controlar a posição orbital do Sol no céu em função da hora do dia;
//   - Calcular as cores dinâmicas da luz solar, luz ambiente e abóbada celeste;
//   - Configurar os parâmetros da névoa atmosférica exponencial (fog);
//   - Transmitir todos os uniforms de iluminação para o Fragment Shader.
//
// O sistema foi projetado de forma modular para servir de base direta para o
// futuro cálculo de sombras projetadas (Shadow Mapping).
// ============================================================================

#ifndef _LIGHTING_H
#define _LIGHTING_H

#include <vector>
#include <glad/glad.h>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

// ----------------------------------------------------------------------------
// Estrutura para fontes de luz locais direcionadas (Postes de Luz e Faróis)
// ----------------------------------------------------------------------------
struct PointLight
{
    glm::vec3 position;         // Posição da fonte de luz no espaço do mundo (XYZ)
    glm::vec3 direction;        // Direção principal do feixe (ex.: (0, -1, 0) apontando para baixo)
    glm::vec3 color;            // Cor pura RGB da lâmpada (valores normalizados de 0.0 a 1.0)
    float     intensity;        // Potência/intensidade multiplicadora do brilho da luz
    float     constant;         // Atenuação constante (kc = 1.0)
    float     linear;           // Atenuação linear (kl)
    float     quadratic;        // Atenuação quadrática (kq)
    float     inner_cutoff_cos; // Cosseno do ângulo interno do cone (iluminação plena)
    float     outer_cutoff_cos; // Cosseno do ângulo externo de corte (0 luz acima/fora dele)
};

class Lighting
{
public:
    Lighting();

    // ------------------------------------------------------------------------
    // Parâmetros de Estado
    // ------------------------------------------------------------------------

    // Lista de luzes pontuais/direcionadas presentes na cena (ex.: postes de luz)
    std::vector<PointLight> point_lights;

    // Lista de matrizes Model dos postes de iluminação posicionados no mapa
    std::vector<glm::mat4> light_pole_matrices;

    // Faróis frontais do ônibus controlados pela tecla L (0 = Esquerdo, 1 = Direito)
    PointLight bus_headlights[2];
    bool       bus_headlights_active;

    // Farol separado de Luz Alta controlado pela tecla P (0 = Esquerdo, 1 = Direito)
    PointLight bus_high_beams[2];
    bool       bus_high_beams_active;

    // Ângulo de elevação do Sol no plano vertical em radianos:
    //   - sun_angle = 0.0          -> Alvorada / Nascer do Sol (horizonte leste)
    //   - sun_angle = PI/2 (~1.57) -> Meio-dia (Sol a pino no zênite)
    //   - sun_angle = PI   (~3.14) -> Ocaso / Pôr do Sol (horizonte oeste)
    //   - sun_angle > PI           -> Período noturno (Sol abaixo do horizonte)
    float sun_angle;

    // Coeficiente de densidade da névoa exponencial (Beer-Lambert):
    //   fator = exp(-fog_density * distancia_camera)
    //   - 0.00: Sem névoa (visibilidade infinita)
    //   - 0.04: Névoa suave de profundidade (valor padrão)
    //   - 0.10+: Neblina densa
    float fog_density;

    // ------------------------------------------------------------------------
    // Cálculos de Iluminação e Geometria da Luz
    // ------------------------------------------------------------------------

    // Calcula o vetor diretor normalizado (w = 0.0) que aponta na direção do Sol.
    // Utilizado no cálculo difuso/especular do shader e, futuramente, como
    // vetor de visão da câmera ortográfica do Shadow Mapping.
    glm::vec4 GetSunDirection() const;

    // Retorna a cor da luz solar direta (RGB em intensidade linear), variando de
    // branca/amarelada ao meio-dia, alaranjada no crepúsculo e azul escuro tênue à noite.
    glm::vec3 GetSunColor() const;

    // Retorna a luz ambiente refletida pela atmosfera (Sky/Ambient Light),
    // impedindo que faces não iluminadas diretamente fiquem totalmente pretas.
    glm::vec3 GetAmbientColor() const;

    // Retorna a cor da abóbada celeste no horizonte, utilizada para limpar a tela
    // (glClearColor) e como cor de saturação da névoa distante.
    glm::vec3 GetSkyColor() const;

    // ------------------------------------------------------------------------
    // Controle e Envio para a GPU
    // ------------------------------------------------------------------------

    // Processa teclas de atalho para controle dinâmico da hora do dia:
    //   - Tecla J: retrocede a posição do Sol (movimento horário / amanhecer)
    //   - Tecla K: avança a posição do Sol (movimento anti-horário / anoitecer)
    void OnKey(int key, int action);

    // Localiza e envia todos os uniforms de luz e névoa para o programa de shaders
    // ativo na GPU (sun_direction, sun_color, ambient_color, fog_color, fog_density).
    void ApplyUniforms(GLuint gpu_program_id, bool fog_enabled = true) const;

    // Adiciona um poste de luz (lightpole.obj) na posição de chão especificada (X, Y, Z),
    // calculando sua matriz Model e registrando automaticamente o ponto de luz no topo.
    void AddStreetLamp(glm::vec3 ground_position, float rotation_y = 0.0f, float scale = 0.5f);

    // Atualiza a posição e orientação dos faróis frontais do ônibus a cada frame
    // conforme o estágio da tecla L (0=off, 1=luz menor do meio, 2=farol principal maior)
    // e o estado do farol separado de Luz Alta (tecla P).
    void UpdateBusHeadlights(const glm::mat4& bus_model_matrix, float bus_yaw, int headlight_state, bool high_beam_active);

    // Renderiza todos os postes de luz e seus bulbos luminosos registrados na cena.
    void DrawLightPoles() const;
};

// Instância global do sistema de iluminação
extern Lighting g_Lighting;

#endif // _LIGHTING_H
