#ifndef _BUS_H
#define _BUS_H

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include "scene.h"

// Classe que encapsula o estado e a renderização do ônibus
class Bus
{
public:
    Bus();

    // Data
    float yaw;
    float direction;
    glm::vec4 position;
    glm::vec3 scale;
    glm::vec4 forward_local;

    // Physics
    float speed;

    // Keys
    bool keyW;
    bool keyA;
    bool keyS;
    bool keyD;

    // Estado dos faróis frontais:
    //   headlight_state (tecla L):
    //     0 = Desligado
    //     1 = Luz menor do meio (_tex_electric__emissive_)
    //     2 = Farol principal maior (_tex_low_beam__emissive_)
    //   Luz alta separada (_tex_high_beam__emissive_, tecla P):
    //     Pisca ao dar um toque rápido em P; se segurar P, trava acesa até apertar P novamente.
    int    headlight_state;
    bool   high_beam_held;
    bool   high_beam_latched;
    bool   high_beam_ignore_hold;
    double high_beam_press_time;

    // Retorna true se o farol separado de luz alta estiver ativo (piscando ou travado aceso)
    bool IsHighBeamActive() const { return high_beam_held || high_beam_latched; }

    // Atualiza a posição do ônibus
    void UpdatePos(const double deltaT);
    void OnKey(int key, int action);

    // Retorna a matriz Model computada via matrices.h
    glm::mat4 GetModelMatrix() const;

    // Renderiza todas as malhas do ônibus
    void Draw(ObjModel& model);
};

// Instância global do ônibus
extern Bus g_Bus;

#endif // _BUS_H
