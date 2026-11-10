// ============================================================================
// bus.cpp - Entidade do ônibus "Branquinho" (Mascarello Gran Midi)
//
// Responsável pelo estado do veículo na cena (posição, escala e, futuramente,
// orientação/velocidade) e pela composição da sua matriz Model:
//
//     M_bus = Translate(pos) * Rotate_Y(yaw) * Scale(s)
//
// Modelo exportado do Blender em .obj usando Z forward e Y up
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
    scale = glm::vec3(0.25f, 0.25f, 0.25f);
    yaw = 0;
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
    // Calculate speed
    if (keyW)       speed = 1.0f;
    else if (keyS)  speed = -1.0f;
    else            speed = 0.0f;

    // Calculate yaw angle
    int turn = keyA - keyD;

    // Update position
    position.x += speed * sin(yaw) * deltaT;
    position.z += speed * cos(yaw) * deltaT;

    // Update angle (only while moving, invert if reversing)
    if (speed != 0) yaw += turn * deltaT * speed;

    PrintVector(position);
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
