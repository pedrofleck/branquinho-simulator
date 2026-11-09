#ifndef _BUS_H
#define _BUS_H

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>
#include "scene.h"

// Classe que encapsula o estado, a cinemática (WASD) e a renderização do ônibus
class Bus
{
public:
    Bus();

    glm::vec4 position;
    glm::vec3 scale;
    float     yaw;               // Rotação horizontal em radianos
    float     speed;             // Velocidade escalar (m/s)
    bool      spin_continuously; // Modo debug de rotação automática

    // Parâmetros de física/cinemática
    float max_forward_speed;
    float max_reverse_speed;
    float acceleration;
    float braking;
    float drag;
    float turn_rate;

    // Estado das teclas de controle (WASD)
    bool key_w;
    bool key_a;
    bool key_s;
    bool key_d;

    // Eventos de entrada e atualização
    void OnKey(int key, int action);
    void Update(float delta_t);

    // Retorna o vetor unitário apontando para a frente do ônibus
    glm::vec4 GetForwardVector() const;

    // Retorna a matriz Model computada via matrices.h
    glm::mat4 GetModelMatrix() const;

    // Renderiza todas as malhas do ônibus
    void Draw(ObjModel& model);
};

// Instância global do ônibus
extern Bus g_Bus;

#endif // _BUS_H
