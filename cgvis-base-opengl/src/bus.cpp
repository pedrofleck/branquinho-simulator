#include "bus.h"

#include <cmath>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>
#include "matrices.h"

Bus g_Bus;

Bus::Bus()
    : position(glm::vec4(0.0f, -0.4f, 0.0f, 1.0f))
    , scale(glm::vec3(0.25f, 0.25f, 0.25f))
    , yaw(0.0f)
    , speed(0.0f)
    , spin_continuously(true)
    , max_forward_speed(12.0f)
    , max_reverse_speed(-5.0f)
    , acceleration(6.0f)
    , braking(10.0f)
    , drag(3.0f)
    , turn_rate(1.1f)
    , key_w(false)
    , key_a(false)
    , key_s(false)
    , key_d(false)
{
}

void Bus::OnKey(int key, int action)
{
    bool is_down = (action != GLFW_RELEASE);

    if (key == GLFW_KEY_W)
        key_w = is_down;
    if (key == GLFW_KEY_A)
        key_a = is_down;
    if (key == GLFW_KEY_S)
        key_s = is_down;
    if (key == GLFW_KEY_D)
        key_d = is_down;
}

glm::vec4 Bus::GetForwardVector() const
{
    // O modelo do Blender com rotação em X de -90 graus tem a frente apontando para -Z
    // Ao rotacionar em torno de Y por 'yaw', a direção frontal no plano XZ é:
    return glm::vec4(-std::sin(yaw), 0.0f, -std::cos(yaw), 0.0f);
}

void Bus::Update(float delta_t)
{
    if (spin_continuously)
        return;

    // Aceleração e ré / frenagem
    if (key_w)
    {
        speed += acceleration * delta_t;
    }
    else if (key_s)
    {
        speed -= braking * delta_t;
    }
    else
    {
        // Desaceleração suave por atrito / resistência ao rolamento
        if (speed > 0.0f)
        {
            speed -= drag * delta_t;
            if (speed < 0.0f)
                speed = 0.0f;
        }
        else if (speed < 0.0f)
        {
            speed += drag * delta_t;
            if (speed > 0.0f)
                speed = 0.0f;
        }
    }

    // Limites de velocidade para frente e ré
    if (speed > max_forward_speed)
        speed = max_forward_speed;
    if (speed < max_reverse_speed)
        speed = max_reverse_speed;

    // Esterçamento: o veículo só vira quando está em movimento
    if (std::abs(speed) > 0.05f)
    {
        float direction = (speed >= 0.0f) ? 1.0f : -1.0f;
        if (key_a)
            yaw += turn_rate * delta_t * direction;
        if (key_d)
            yaw -= turn_rate * delta_t * direction;
    }

    // Atualização de posição baseada no vetor frontal e delta_t
    glm::vec4 forward = GetForwardVector();
    position += forward * (speed * delta_t);
}

glm::mat4 Bus::GetModelMatrix() const
{
    float current_yaw = spin_continuously ? (float)glfwGetTime() : yaw;

    return Matrix_Translate(position.x, position.y, position.z)
         * Matrix_Rotate_Y(current_yaw)
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
