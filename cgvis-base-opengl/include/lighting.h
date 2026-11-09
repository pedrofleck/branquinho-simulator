#ifndef _LIGHTING_H
#define _LIGHTING_H

#include <glad/glad.h>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Classe que gerencia a luz direcional (Sol), ciclo de hora do dia e neblina (Fog)
class Lighting
{
public:
    Lighting();

    // Ângulo de elevação do Sol em radianos (0 = nascer do sol, PI/2 = meio-dia, PI = pôr do sol)
    float sun_angle;

    // Densidade da névoa exponencial (0.0 = sem névoa)
    float fog_density;

    // Direção apontando para o Sol (vetor unitário l, com w = 0.0)
    // No futuro, também será usado para posicionar a câmera do Shadow Mapping
    glm::vec4 GetSunDirection() const;

    // Cores dinâmicas calculadas conforme a altura do Sol (hora do dia)
    glm::vec3 GetSunColor() const;
    glm::vec3 GetAmbientColor() const;
    glm::vec3 GetSkyColor() const;

    // Atualiza o ângulo do Sol pelo teclado (teclas K e L para testar hora do dia)
    void OnKey(int key, int action);

    // Envia todos os parâmetros de iluminação e névoa para o Fragment Shader
    void ApplyUniforms(GLuint gpu_program_id) const;
};

extern Lighting g_Lighting;

#endif // _LIGHTING_H
