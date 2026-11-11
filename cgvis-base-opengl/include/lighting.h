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

#include <glad/glad.h>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

class Lighting
{
public:
    Lighting();

    // ------------------------------------------------------------------------
    // Parâmetros de Estado
    // ------------------------------------------------------------------------

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
    //   - Tecla K: retrocede a posição do Sol (movimento horário / amanhecer)
    //   - Tecla L: avança a posição do Sol (movimento anti-horário / anoitecer)
    void OnKey(int key, int action);

    // Localiza e envia todos os uniforms de luz e névoa para o programa de shaders
    // ativo na GPU (sun_direction, sun_color, ambient_color, fog_color, fog_density).
    void ApplyUniforms(GLuint gpu_program_id) const;
};

// Instância global do sistema de iluminação
extern Lighting g_Lighting;

#endif // _LIGHTING_H
