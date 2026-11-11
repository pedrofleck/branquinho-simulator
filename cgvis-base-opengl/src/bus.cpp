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

#include <cstdio>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include "scene.h"
#include "matrices.h"

Bus g_Bus;

Bus::Bus(){
    position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    scale = glm::vec3(1.0f, 1.0f, 1.0f);
    forward_local = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    yaw = 0.0f;
    direction = 0.0f;

    // Physics
    speed = 0.0f;

    // Keys & Headlights
    keyW = false;
    keyA = false;
    keyS = false;
    keyD = false;
    headlight_state       = 0;
    high_beam_held        = false;
    high_beam_latched     = false;
    high_beam_ignore_hold = false;
    high_beam_press_time  = 0.0;
}

// TODO: fix holding key bug
void Bus::OnKey(int key, int action){
    bool isPressed = false;
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

    // Tecla L alterna em ciclo os estágios do farol frontal:
    //   0 -> 1 (Luz menor do meio) -> 2 (Farol principal maior) -> 0 (Desligado)
    if (key == GLFW_KEY_L && action == GLFW_PRESS)
    {
        headlight_state = (headlight_state + 1) % 3;
        const char* state_names[] = { "DESLIGADOS", "LUZ MENOR DO MEIO", "FAROL PRINCIPAL (MAIOR)" };
        printf("Farois do onibus (L): %s\n", state_names[headlight_state]);
    }

    // Tecla P controla o farol separado de Luz Alta (_tex_high_beam__emissive_):
    //   - Toque curto: pisca a luz alta enquanto P está pressionada e apaga ao soltar.
    //   - Segurar P: trava a luz alta acesa continuamente até que P seja pressionada novamente.
    if (key == GLFW_KEY_P)
    {
        if (action == GLFW_PRESS)
        {
            if (high_beam_latched)
            {
                // Já estava travada acesa: apertar P novamente desliga a luz alta
                high_beam_latched     = false;
                high_beam_held        = false;
                high_beam_ignore_hold = true;
                printf("Luz Alta (P): DESLIGADA\n");
            }
            else
            {
                // Estava desligada: acende imediatamente (para piscar ou iniciar segurar)
                high_beam_held        = true;
                high_beam_ignore_hold = false;
                high_beam_press_time  = glfwGetTime();
            }
        }
        else if (action == GLFW_REPEAT)
        {
            if (!high_beam_ignore_hold && !high_beam_latched)
            {
                high_beam_latched = true;
                printf("Luz Alta (P): TRAVADA ACESA\n");
            }
        }
        else if (action == GLFW_RELEASE)
        {
            high_beam_held        = false;
            high_beam_ignore_hold = false;
        }
    }

    // printf("Bus::OnKey %d %d\n", key, action);
}

void Bus::UpdatePos(double deltaT){
    // Se o jogador estiver segurando a tecla P por mais de 0.25s, trava a Luz Alta ligada
    if (high_beam_held && !high_beam_latched && !high_beam_ignore_hold)
    {
        if (glfwGetTime() - high_beam_press_time >= 0.25)
        {
            high_beam_latched = true;
            printf("Luz Alta (P): TRAVADA ACESA\n");
        }
    }

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

    // Desenha todas as partes que compõem o modelo .obj
    for (size_t s = 0; s < model.shapes.size(); ++s)
    {
        const std::string& shape_name = model.shapes[s].name;

        // 1. Luz menor do meio (_tex_electric__emissive_ em ±0.96, 1.12):
        //    acende ao apertar L 1 vez (estágio 1) e continua acesa no estágio 2
        bool middle_small_lit = (headlight_state >= 1) && (shape_name == "_tex_electric__emissive_");

        // 2. Farol principal maior (_tex_low_beam__emissive_ em ±1.08, 1.07):
        //    acende ao apertar L 2 vezes (estágio 2)
        bool main_large_lit = (headlight_state == 2) && (shape_name == "_tex_low_beam__emissive_");

        // 3. Farol separado de Luz Alta (_tex_high_beam__emissive_ em ±0.90, 1.01):
        //    pisca ao apertar P ou fica aceso continuamente ao segurar P
        bool high_beam_lit = IsHighBeamActive() && (shape_name == "_tex_high_beam__emissive_");

        if (middle_small_lit || main_large_lit || high_beam_lit)
            glUniform1i(g_object_id_uniform, HEADLIGHT_LENS);
        else
            glUniform1i(g_object_id_uniform, BUS);

        DrawVirtualObject(shape_name.c_str());
    }
}
