// ============================================================================
// bus.cpp - Entidade do ônibus "Branquinho" (Mascarello Gran Midi)
//
// Responsável pelo estado do veículo na cena (posição, escala e, futuramente,
// orientação/velocidade) e pela composição da sua matriz Model:
//
//     M_bus = Translate(pos) * Rotate_Y(yaw) * Rotate_X(-pi/2) * Scale(s)
//
// onde Rotate_X(-pi/2) converte o modelo exportado do Blender (Z-up) para a
// convenção Y-up da cena. Também desenha todas as shapes do ObjModel do ônibus
// enviando a matriz Model e o object_id BUS para os shaders.
//
// Todas as transformações usam exclusivamente as funções de "matrices.h".
// ============================================================================

#include "bus.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include "matrices.h"

Bus g_Bus;

Bus::Bus()
    : position(glm::vec4(0.0f, -0.4f, 0.0f, 1.0f))
    , scale(glm::vec3(0.25f, 0.25f, 0.25f))
{
}

glm::mat4 Bus::GetModelMatrix() const
{
    return Matrix_Translate(position.x, position.y, position.z)
         * Matrix_Rotate_Y((float)glfwGetTime())
         * Matrix_Rotate_X(-3.14159265f / 2.0f)
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
