// ============================================================================
// bus.cpp - Entidade do ônibus "Branquinho" (Mascarello Gran Midi)
//
// Responsável pelo estado do veículo na cena (posição, escala e, futuramente,
// orientação/velocidade) e pela composição da sua matriz Model:
//
//     M_bus = Translate(pos) * Rotate_Y(yaw) * Scale(s)
//
// Modelo exportado do Blender em .obj usando Y up e -Z forward
// Também desenha todas as shapes do ObjModel do ônibus
// enviando a matriz Model e o object_id BUS para os shaders.
//
// Todas as transformações usam exclusivamente as funções de "matrices.h".
// ============================================================================

#include "bus.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include "matrices.h"

Bus g_Bus;

Bus::Bus(){
    position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    scale = glm::vec3(1.0f, 1.0f, 1.0f);
    forward_local = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    yaw = 0;

    // Physics
    speed = 0.0f;
}

// TODO: fix holding key bug
void Bus::OnKey(int key, int action){
    bool isPressed;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) isPressed = true;
    else if (action == GLFW_RELEASE) isPressed = false;

    if (key == GLFW_KEY_W){
        keyW = isPressed;
    } else if (key == GLFW_KEY_S){
        keyS = isPressed;
    } else if (key == GLFW_KEY_A){
        keyA = isPressed;
    } else if (key == GLFW_KEY_D){
        keyD = isPressed;
    }

    // printf("Bus::OnKey %d %d\n", key, action);
}

void Bus::UpdatePos(double deltaT){
    // Drive direction
    if (keyW)       direction = 1.0f;
    else if (keyS)  direction = -1.0f;
    else            direction = 0.0f;

    // Calculate speed
    // TODO: remove hardcoded magic numbers
    speed += direction * 2.0f * deltaT; // 2.0f = engine power
    float drag = (speed * speed * 0.1f + 0.5f)  * deltaT; // 0.1f = air resistance, 0.5f = rolling resistance

    // apply drag against speed (positive if going backwards)
    if (speed > 0) speed -= drag;
    else speed += drag;

    // stop movement once speed is very low
    if (std::abs(speed) <= drag) speed = 0;

    // Calculate turning rate/direction
    int turn = keyA - keyD;

    // Forward vector
    glm::vec4 forward = Matrix_Rotate_Y(yaw) * forward_local;

    // Update position
    position += forward * speed * (float)deltaT;

    // Update angle (only while moving, invert if reversing)
    yaw += turn * speed * 0.2f * deltaT;
}

glm::mat4 Bus::GetModelMatrix() const
{
    return Matrix_Translate(position.x, position.y, position.z)
         * Matrix_Rotate_Y(yaw)
         * Matrix_Scale(scale.x, scale.y, scale.z);
}

void Bus::Draw(ObjModel& model)
{
    glm::mat4 model_mat = GetModelMatrix();
    glUniformMatrix4fv(g_model_uniform, 1, GL_FALSE, glm::value_ptr(model_mat));
    glUniform1i(g_object_id_uniform, BUS);

    // Desenha todas as partes que compõem o modelo .obj
    for (size_t s = 0; s < model.shapes.size(); ++s)
    {
        DrawVirtualObject(model.shapes[s].name.c_str());
    }
}
