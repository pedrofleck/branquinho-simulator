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
    float speed;
    glm::vec4 position;
    glm::vec3 scale;

    // Keys
    bool keyW;
    bool keyA;
    bool keyS;
    bool keyD;

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
