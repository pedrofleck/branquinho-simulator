// ============================================================================
// lighting.cpp - Implementação da dinâmica solar, iluminação e névoa
//
// Modela a trajetória do Sol no céu como uma trajetória orbital no plano
// vertical, determinando a cada instante:
//   1. Vetor unitário de incidência da luz solar direta (l);
//   2. Temperatura e intensidade da luz solar (dispersão atmosférica aproximada);
//   3. Luz ambiente indireta espalhada pela abóbada celeste;
//   4. Cor do horizonte para fundos (skybox/clear) e névoa volumétrica.
//
// O modelo segue rigorosamente as diretrizes da disciplina, utilizando as
// funções de álgebra linear de "matrices.h" para cálculos com vetores homogêneos.
// ============================================================================

#include "lighting.h"

#include <cmath>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include "matrices.h"

// Instância global declarada em lighting.h
Lighting g_Lighting;

// ----------------------------------------------------------------------------
// Função Auxiliar: Interpolação Linear de Cores (Lerp)
// Computa C(t) = (1 - t)*A + t*B com fator t limitado ao intervalo [0.0, 1.0].
// Utilizada para transição suave entre paletas de cores do dia, pôr do sol e noite.
// ----------------------------------------------------------------------------
static glm::vec3 LerpColor(const glm::vec3& a, const glm::vec3& b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    return a * (1.0f - t) + b * t;
}

Lighting::Lighting()
    : sun_angle(1.0f)      // Posição inicial: Sol elevado a ~57 graus do horizonte
    , fog_density(0.025f)   // Densidade moderada de névoa atmosférica
{
}

// ----------------------------------------------------------------------------
// GetSunDirection()
// Computa o vetor unitário que aponta para o Sol a partir do ângulo de elevação.
// O movimento ocorre primordialmente no plano XY com uma leve inclinação em Z (0.3)
// para criar sombras e relevo tridimensional mais interessante.
// ----------------------------------------------------------------------------
glm::vec4 Lighting::GetSunDirection() const
{
    glm::vec4 dir = glm::vec4(std::cos(sun_angle), std::sin(sun_angle), 0.3f, 0.0f);

    // Normalização feita manualmente usando a função norm() do matrices.h
    return dir / norm(dir);
}

// ----------------------------------------------------------------------------
// GetSunColor()
// Modela a variação da cor da luz solar em função da sua elevação (height = sin(angle)):
//   - Altura máxima (> 0.4): Luz branca-amarelada diurna (1.00, 0.98, 0.92)
//   - Horizonte (0.0 a 0.4): Transição para tons alaranjados de pôr do sol (1.00, 0.45, 0.15)
//   - Abaixo do horizonte (< 0.0): Luar azul escuro tênue (0.05, 0.08, 0.18)
// ----------------------------------------------------------------------------
glm::vec3 Lighting::GetSunColor() const
{
    float height = std::sin(sun_angle); // Varia de -1.0 (meia-noite) a +1.0 (meio-dia)

    glm::vec3 night_sun  = glm::vec3(0.05f, 0.08f, 0.18f); // Luz noturna azulada
    glm::vec3 sunset_sun = glm::vec3(1.00f, 0.45f, 0.15f); // Sol poente alaranjado
    glm::vec3 noon_sun   = glm::vec3(1.00f, 0.98f, 0.92f); // Luz branca solar do meio-dia

    if (height < 0.0f)
    {
        // Transição entre o crepúsculo e a escuridão da noite
        float t = (height + 0.3f) / 0.3f;
        return LerpColor(night_sun, sunset_sun, t);
    }
    else
    {
        // Transição entre o pôr do sol e o Sol a pino
        float t = height / 0.4f;
        return LerpColor(sunset_sun, noon_sun, t);
    }
}

// ----------------------------------------------------------------------------
// GetSkyColor()
// Define a tonalidade do céu no horizonte. Essa cor é fundamental para evitar
// contraste visual incorreto: ela é usada simultaneamente no glClearColor() do
// fundo e como a cor de saturação da névoa (fog_color) no fragment shader.
// ----------------------------------------------------------------------------
glm::vec3 Lighting::GetSkyColor() const
{
    float height = std::sin(sun_angle);

    glm::vec3 night_sky  = glm::vec3(0.03f, 0.04f, 0.09f); // Céu noturno escuro
    glm::vec3 sunset_sky = glm::vec3(0.85f, 0.50f, 0.35f); // Céu quente de entardecer
    glm::vec3 day_sky    = glm::vec3(0.75f, 0.85f, 0.98f); // Céu azul límpido de dia

    if (height < 0.0f)
    {
        float t = (height + 0.3f) / 0.3f;
        return LerpColor(night_sky, sunset_sky, t);
    }
    else
    {
        float t = height / 0.4f;
        return LerpColor(sunset_sky, day_sky, t);
    }
}

// ----------------------------------------------------------------------------
// GetAmbientColor()
// Computa a iluminação ambiente indireta que atinge todas as faces igualmente.
// É proporcional à cor do céu (simulando a luz difusa da abóbada celeste)
// com uma constante mínima de segurança para evitar penumbra 100% preta à noite.
// ----------------------------------------------------------------------------
glm::vec3 Lighting::GetAmbientColor() const
{
    return GetSkyColor() * 0.25f + glm::vec3(0.02f, 0.02f, 0.03f);
}

// ----------------------------------------------------------------------------
// OnKey()
// Interação em tempo real para controle da hora do dia:
// Permite que o usuário e avaliadores observem a transição de iluminação
// e névoa pressionando K (retroceder) ou L (avançar).
// ----------------------------------------------------------------------------
void Lighting::OnKey(int key, int action)
{
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        if (key == GLFW_KEY_K)
            sun_angle -= 0.08f;
        if (key == GLFW_KEY_L)
            sun_angle += 0.08f;
    }
}

// ----------------------------------------------------------------------------
// ApplyUniforms()
// Transfere o estado de iluminação da CPU para a memória da GPU (Fragment Shader)
// através de variáveis uniformes declaradas em shader_fragment.glsl.
// ----------------------------------------------------------------------------
void Lighting::ApplyUniforms(GLuint gpu_program_id) const
{
    glm::vec4 sun_dir = GetSunDirection();
    glm::vec3 sun_col = GetSunColor();
    glm::vec3 amb_col = GetAmbientColor();
    glm::vec3 fog_col = GetSkyColor();

    glUniform4fv(glGetUniformLocation(gpu_program_id, "sun_direction"), 1, glm::value_ptr(sun_dir));
    glUniform3fv(glGetUniformLocation(gpu_program_id, "sun_color"),     1, glm::value_ptr(sun_col));
    glUniform3fv(glGetUniformLocation(gpu_program_id, "ambient_color"), 1, glm::value_ptr(amb_col));
    glUniform3fv(glGetUniformLocation(gpu_program_id, "fog_color"),     1, glm::value_ptr(fog_col));
    glUniform1f(glGetUniformLocation(gpu_program_id,  "fog_density"),   fog_density);
}
