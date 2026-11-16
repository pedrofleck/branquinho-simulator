// ============================================================================
// bus.cpp - Entidade do ônibus "Branquinho" (Mascarello Gran Midi)
//
// Responsável pelo estado do veículo na cena (posição, escala e, futuramente,
// orientação/velocidade) e pela composição da sua matriz Model:
//
//     M_bus = Translate(pos) * Rotate_Y(yaw) * Translate(pivot) * Rotate_X(pitch)
//           * Rotate_Z(roll) * Translate(-pivot) * Scale(s)
//
// Modelo exportado do Blender em .obj usando Y up e -Z forward.
// A origem do modelo fica no eixo traseiro, ao nível do chão (base das rodas).
// Também desenha todas as shapes do ObjModel do ônibus
// enviando a matriz Model e o object_id BUS para os shaders.
//
// Todas as transformações usam exclusivamente as funções de "matrices.h".
// ============================================================================

#include "bus.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <glm/gtc/type_ptr.hpp>

#include "scene.h"
#include "matrices.h"

Bus g_Bus;

Bus::Bus(){
    position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    scale = glm::vec3(1.0f, 1.0f, 1.0f);
    forward_local = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    up_local = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    direction = 0.0f;
    wheel_angle = 0.0f;
    turn = 0;
    air_resistance = 0.002f;
    rolling_resistance = 0.5f;
    engine_power = 2.0f;

    // Physics
    speed = 0.0f;
    steering_target = 0.0f;
    steering_angle = 0.0f;
    steering_rate = 5.0f;
    pitch = 0.0f;
    yaw = 0.0f;
    roll = 0.0f;
    pitch_target = 0.0f;
    yaw_target = 0.0f;
    roll_target = 0.0f;

    // Suspensão
    heave        = 0.0f;
    heave_target = 0.0f;
    heave_vel    = 0.0f;
    pitch_vel    = 0.0f;
    roll_vel     = 0.0f;
    pivot_fraction = 0.5f;  // meio do entre-eixos
    body_pivot_z = 0.0f;
    heave_omega  = 16.0f;
    heave_zeta   = 0.65f;
    body_omega   = 14.0f;
    body_zeta    = 0.6f;
    squat_gain   = 0.005f;
    lean_gain    = 0.008f;
    max_body_angle = 0.5f;
    ride_height  = 0.06f;
    wheel_travel = 0.2f;
    wheel_offset_fl = wheel_offset_fr = 0.0f;
    wheel_offset_rl = wheel_offset_rr = 0.0f;
    prev_speed   = 0.0f;
    suspension_initialized = false;

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
}

// Centro da roda e raio, em coordenadas locais do modelo
static glm::vec3 WheelCenter(const SceneObject& w) { return (w.bbox_max + w.bbox_min) * 0.5f; }
static float     WheelRadius(const SceneObject& w) { return (w.bbox_max.y - w.bbox_min.y) * 0.5f; }

// Mola amortecida de 2ª ordem (Euler semi-implícito):
//   x'' = omega² (alvo - x) - 2 zeta omega x'
static void SpringDamper(float& x, float& v, float target, float omega, float zeta, float dt)
{
    const float k = omega * omega;
    const float c = 2.0f * zeta * omega;
    v += (k * (target - x) - c * v) * dt;
    x += v * dt;
}

float Bus::GetTerrainHeightAtWheel(const SceneObject& wheel){
    // Projeção horizontal do centro da roda (só yaw + escala; pitch/roll pouco
    // alteram o XZ e incluí-los criaria realimentação com a própria suspensão)
    glm::vec3 c = WheelCenter(wheel);
    glm::vec4 c_world = Matrix_Translate(position.x, 0.0f, position.z)
                      * Matrix_Rotate_Y(yaw)
                      * Matrix_Scale(scale.x, scale.y, scale.z)
                      * glm::vec4(c.x, c.y, c.z, 1.0f);

    return GetTerrainHeight(c_world.x, c_world.z);
}

void Bus::UpdateSuspension(double deltaT){
    const float dt = (float)deltaT;
    if (dt <= 0.0f) return;

    const SceneObject& wheel_fl = g_VirtualScene["wheel_fl"];
    const SceneObject& wheel_fr = g_VirtualScene["wheel_fr"];
    const SceneObject& wheel_rl = g_VirtualScene["wheel_rl"];
    const SceneObject& wheel_rr = g_VirtualScene["wheel_rr"];

    // --- 1. Geometria do chassi (a partir das bounding boxes das rodas) ---
    const glm::vec3 c_fl = WheelCenter(wheel_fl), c_fr = WheelCenter(wheel_fr);
    const glm::vec3 c_rl = WheelCenter(wheel_rl), c_rr = WheelCenter(wheel_rr);
    const float z_front = 0.5f * (c_fl.z + c_fr.z);
    const float z_rear  = 0.5f * (c_rl.z + c_rr.z);
    const float wheel_base  = std::abs(z_front - z_rear) * scale.z;
    const float track_front = std::abs(c_fr.x - c_fl.x) * scale.x;
    const float track_rear  = std::abs(c_rr.x - c_rl.x) * scale.x;
    const float track_width = 0.5f * (track_front + track_rear);

    // --- 2. Altura do terreno sob cada roda ---
    const float h_fl = GetTerrainHeightAtWheel(wheel_fl);
    const float h_fr = GetTerrainHeightAtWheel(wheel_fr);
    const float h_rl = GetTerrainHeightAtWheel(wheel_rl);
    const float h_rr = GetTerrainHeightAtWheel(wheel_rr);
    const float h_front = 0.5f * (h_fl + h_fr);
    const float h_rear  = 0.5f * (h_rl + h_rr);
    const float h_left  = 0.5f * (h_fl + h_rl);
    const float h_right = 0.5f * (h_fr + h_rr);

    // --- 3. Alvos da carroceria: plano do terreno + efeitos inerciais ---
    // Aceleração longitudinal: acelerar empina o nariz (pitch +), frear afunda (pitch -)
    const float accel = (speed - prev_speed) / dt;
    prev_speed = speed;
    // Aceleração lateral (centrípeta): v * taxa de giro. Positiva ao virar à esquerda;
    // a carroceria inclina para fora da curva (lado direito desce => roll -)
    const float yaw_rate = speed * std::tan(steering_angle) / wheel_base;
    const float lat_acc  = speed * yaw_rate;

    pitch_target = std::atan2(h_front - h_rear, wheel_base) + squat_gain * accel;
    roll_target  = std::atan2(h_right - h_left, track_width) - lean_gain * lat_acc;
    pitch_target = glm::clamp(pitch_target, -max_body_angle, max_body_angle);
    roll_target  = glm::clamp(roll_target,  -max_body_angle, max_body_angle);

    // Pivô da carroceria (aprox. centro de massa) entre os eixos. Pitch e roll
    // giram em torno dele, então sob aceleração a frente sobe e a traseira desce.
    // A altura alvo é a do plano do terreno nesse ponto, mais a altura de rodagem.
    body_pivot_z = (z_rear + (z_front - z_rear) * pivot_fraction) * scale.z;
    heave_target = h_rear + (h_front - h_rear) * pivot_fraction + ride_height;

    // No primeiro quadro, posiciona a carroceria direto no repouso (sem "queda")
    if (!suspension_initialized)
    {
        heave = heave_target;
        pitch = pitch_target;
        roll  = roll_target;
        heave_vel = pitch_vel = roll_vel = 0.0f;
        suspension_initialized = true;
    }

    // --- 4. Integração das molas da carroceria, com sub-passos fixos de ~5 ms
    //        para manter a estabilidade mesmo com FPS baixo ---
    const int   steps = std::min(200, std::max(1, (int)std::ceil(dt / 0.005f)));
    const float h     = dt / steps;
    for (int i = 0; i < steps; ++i)
    {
        SpringDamper(heave, heave_vel, heave_target, heave_omega, heave_zeta, h);
        SpringDamper(pitch, pitch_vel, pitch_target, body_omega,  body_zeta,  h);
        SpringDamper(roll,  roll_vel,  roll_target,  body_omega,  body_zeta,  h);
    }
    position.y = heave;
    position.w = 1.0f;

    // --- 5. Rodas: com a pose final da carroceria, desloca cada roda no Y local
    //        até tocar o chão, limitado pelo curso da suspensão ---
    const glm::mat4 M = GetModelMatrix();
    auto WheelOffset = [&](const SceneObject& w, float ground) {
        const glm::vec3 c = WheelCenter(w);
        const float     r = WheelRadius(w);
        const glm::vec4 c_world = M * glm::vec4(c.x, c.y, c.z, 1.0f);
        // Quanto o centro precisa subir (+) ou descer (-) para a roda tocar o chão.
        // Divide pela escala para voltar a unidades locais (inclinação é pequena, cos ~ 1)
        // Em repouso a roda fica estendida em -ride_height; o curso é centrado nesse ponto.
        const float dy_world = (ground + r * scale.y) - c_world.y;
        const float rest     = -ride_height / scale.y;
        return glm::clamp(dy_world / scale.y, rest - wheel_travel, rest + wheel_travel);
    };
    wheel_offset_fl = WheelOffset(wheel_fl, h_fl);
    wheel_offset_fr = WheelOffset(wheel_fr, h_fr);
    wheel_offset_rl = WheelOffset(wheel_rl, h_rl);
    wheel_offset_rr = WheelOffset(wheel_rr, h_rr);
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
    speed += direction * engine_power * deltaT;
    float drag = (speed * speed * air_resistance + rolling_resistance) * deltaT;

    // apply drag against speed (positive if going backwards)
    if (speed > 0) speed -= drag;
    else speed += drag;

    // stop movement once speed is very low
    if (std::abs(speed) <= drag) speed = 0;

    // Calculate turning rate/direction
    turn = keyA - keyD;

    // Wheel base (distance between rear and front axles), from the wheel bboxes
    const SceneObject& wheel_fl = g_VirtualScene["wheel_fl"];
    const SceneObject& wheel_rl = g_VirtualScene["wheel_rl"];
    const SceneObject& wheel_rr = g_VirtualScene["wheel_rr"];
    const float wheel_base = std::abs(WheelCenter(wheel_fl).z - WheelCenter(wheel_rl).z) * scale.z;

    // Update steering angle
    steering_target = glm::clamp(turn * 3.0f / (std::abs(speed) + 1.0f), -0.6f, 0.6f);
    float alpha = 1.0f - std::exp(-steering_rate * deltaT);
    steering_angle += (steering_target - steering_angle) * alpha;

    // Update angle (only while moving, invert if reversing)
    yaw += speed * std::tan(steering_angle) / wheel_base * deltaT;

    // Update horizontal position (height is handled by the suspension)
    glm::vec4 forward = Matrix_Rotate_Y(yaw) * forward_local;
    position += forward * speed * (float)deltaT;
    position.w = 1.0f;

    // Update wheel rotation
    wheel_angle += (speed * deltaT) / (WheelRadius(wheel_rr) * scale.y);

    // Body height, pitch, roll and wheel contact
    UpdateSuspension(deltaT);
}

glm::mat4 Bus::GetModelMatrix() const
{
    // position.xz = eixo traseiro; position.y = altura do pivô (heave).
    // Pitch/roll são aplicados em torno do pivô (0, 0, body_pivot_z) no referencial do ônibus.
    return Matrix_Translate(position.x, position.y, position.z)
         * Matrix_Rotate_Y(yaw)
         * Matrix_Translate(0.0f, 0.0f, body_pivot_z)
         * Matrix_Rotate_X(pitch)
         * Matrix_Rotate_Z(roll)
         * Matrix_Translate(0.0f, 0.0f, -body_pivot_z)
         * Matrix_Scale(scale.x, scale.y, scale.z);
}

glm::vec4 Bus::GetCenterGlobal() const
{
    // Meio do entre-eixos (body_pivot_z, já em unidades de mundo), a ~1.5 m de altura,
    // no referencial do ônibus. Só yaw: pitch/roll fariam a câmera balançar com a suspensão.
    // Como é reconstruído a partir do eixo traseiro + yaw, este ponto descreve um arco
    // em torno do eixo traseiro nas curvas, como o meio de um ônibus real.
    glm::vec4 center_local = glm::vec4(0.0f, 1.5f * scale.y, body_pivot_z + 1.0f, 0.0f);
    glm::vec4 center = position + Matrix_Rotate_Y(yaw) * center_local;
    center.w = 1.0f;
    return center;
}

void Bus::Draw(ObjModel& model)
{
    glm::mat4 model_mat = GetModelMatrix();
    
    // Desenha todas as partes que compõem o modelo .obj
    for (size_t s = 0; s < model.shapes.size(); ++s)
    {
        const std::string& shape_name = model.shapes[s].name;
        glm::mat4 new_model_mat;

        // Rodas: sobe/desce pela suspensão, esterça (dianteiras) e gira, em torno do seu centro
        const bool is_front = (shape_name == "wheel_fr" || shape_name == "wheel_fl");
        const bool is_rear  = (shape_name == "wheel_rr" || shape_name == "wheel_rl");
        if (is_front || is_rear){
            float susp_offset = 0.0f;
            if      (shape_name == "wheel_fl") susp_offset = wheel_offset_fl;
            else if (shape_name == "wheel_fr") susp_offset = wheel_offset_fr;
            else if (shape_name == "wheel_rl") susp_offset = wheel_offset_rl;
            else                               susp_offset = wheel_offset_rr;

            const glm::vec3 wheel_c = WheelCenter(g_VirtualScene[shape_name]);
            const float steer = is_front ? steering_angle : 0.0f;

            new_model_mat = model_mat
                          * Matrix_Translate(wheel_c.x, wheel_c.y + susp_offset, wheel_c.z)
                          * Matrix_Rotate_Y(steer)
                          * Matrix_Rotate_X(-wheel_angle)
                          * Matrix_Translate(-wheel_c.x, -wheel_c.y, -wheel_c.z);
        }
        else new_model_mat = model_mat;
        
        glUniformMatrix4fv(g_model_uniform, 1, GL_FALSE, glm::value_ptr(new_model_mat));

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
