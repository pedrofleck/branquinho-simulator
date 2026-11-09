#include "lighting.h"

#include <cmath>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include "matrices.h"

Lighting g_Lighting;

// Interpolação linear auxiliar entre duas cores RGB
static glm::vec3 LerpColor(const glm::vec3& a, const glm::vec3& b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    return a * (1.0f - t) + b * t;
}

Lighting::Lighting()
    : sun_angle(1.0f)      // Começa com o Sol elevado (~57 graus)
    , fog_density(0.04f)   // Densidade inicial da névoa
{
}

glm::vec4 Lighting::GetSunDirection() const
{
    // Calcula o vetor direção do Sol no arco Leste-Oeste (plano XY com leve inclinação em Z)
    glm::vec4 dir = glm::vec4(std::cos(sun_angle), std::sin(sun_angle), 0.3f, 0.0f);
    return dir / norm(dir);
}

glm::vec3 Lighting::GetSunColor() const
{
    float height = std::sin(sun_angle); // -1 (meia-noite) a +1 (meio-dia)

    glm::vec3 night_sun  = glm::vec3(0.05f, 0.08f, 0.18f); // Luar azulado fraco
    glm::vec3 sunset_sun = glm::vec3(1.00f, 0.45f, 0.15f); // Laranja pôr do sol
    glm::vec3 noon_sun   = glm::vec3(1.00f, 0.98f, 0.92f); // Luz branca solar

    if (height < 0.0f)
    {
        float t = (height + 0.3f) / 0.3f;
        return LerpColor(night_sun, sunset_sun, t);
    }
    else
    {
        float t = height / 0.4f;
        return LerpColor(sunset_sun, noon_sun, t);
    }
}

glm::vec3 Lighting::GetSkyColor() const
{
    float height = std::sin(sun_angle);

    glm::vec3 night_sky  = glm::vec3(0.03f, 0.04f, 0.09f);
    glm::vec3 sunset_sky = glm::vec3(0.85f, 0.50f, 0.35f);
    glm::vec3 day_sky    = glm::vec3(0.75f, 0.85f, 0.98f);

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

glm::vec3 Lighting::GetAmbientColor() const
{
    // A iluminação ambiente vem do próprio espalhamento do céu
    return GetSkyColor() * 0.25f + glm::vec3(0.02f, 0.02f, 0.03f);
}

void Lighting::OnKey(int key, int action)
{
    if (action == GLFW_PRESS || action == GLFW_REPEAT)
    {
        // Teclas K e L avançam ou retrocedem a posição do Sol (hora do dia)
        if (key == GLFW_KEY_K)
            sun_angle -= 0.08f;
        if (key == GLFW_KEY_L)
            sun_angle += 0.08f;
    }
}

void Lighting::ApplyUniforms(GLuint gpu_program_id) const
{
    glm::vec4 sun_dir   = GetSunDirection();
    glm::vec3 sun_col   = GetSunColor();
    glm::vec3 amb_col   = GetAmbientColor();
    glm::vec3 fog_col   = GetSkyColor();

    glUniform4fv(glGetUniformLocation(gpu_program_id, "sun_direction"), 1, glm::value_ptr(sun_dir));
    glUniform3fv(glGetUniformLocation(gpu_program_id, "sun_color"),     1, glm::value_ptr(sun_col));
    glUniform3fv(glGetUniformLocation(gpu_program_id, "ambient_color"), 1, glm::value_ptr(amb_col));
    glUniform3fv(glGetUniformLocation(gpu_program_id, "fog_color"),     1, glm::value_ptr(fog_col));
    glUniform1f(glGetUniformLocation(gpu_program_id,  "fog_density"),   fog_density);
}
