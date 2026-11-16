#ifndef _BUS_H
#define _BUS_H

#include <glm/vec4.hpp>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

struct ObjModel;    // declaração antecipada no lugar de #include "scene.h"
struct SceneObject; // declaração antecipada no lugar de #include "scene.h"

// Classe que encapsula o estado e a renderização do ônibus
class Bus
{
public:
    Bus();

    // Data
    glm::vec4 position;
    glm::vec3 scale;
    glm::vec4 forward_local;
    float direction;
    float wheel_angle;
    int turn;
    float air_resistance;
    float rolling_resistance;
    float engine_power;
    
    // Physics
    float speed;
    float steering_target;
    float steering_angle;
    float steering_rate;
    float pitch, yaw, roll;
    float pitch_target, yaw_target, roll_target;

    // Suspensão
    //   A carroceria é um sistema massa-mola-amortecedor em 3 graus de liberdade
    //   (altura "heave", pitch e roll), puxado em direção a alvos calculados a
    //   partir do terreno sob as rodas + efeitos inerciais (aceleração/curva).
    //   As rodas seguem o chão independentemente, deslocando-se no eixo Y local
    //   da carroceria (limitado pelo curso da suspensão).
    float heave, heave_target;               // altura do pivô da carroceria (nível do chão)
    float pivot_fraction;                    // posição do pivô (centro de massa) entre eixos: 0 = traseiro, 1 = dianteiro
    float body_pivot_z;                      // z local do pivô em unidades de mundo (calculado em UpdateSuspension)
    float heave_vel, pitch_vel, roll_vel;    // velocidades dos graus de liberdade da carroceria
    float heave_omega, heave_zeta;           // frequência natural (rad/s) e razão de amortecimento da altura
    float body_omega,  body_zeta;            // idem, para pitch e roll
    float squat_gain;                        // rad por m/s² de aceleração longitudinal (empina/afunda)
    float lean_gain;                         // rad por m/s² de aceleração lateral (inclina na curva)
    float max_body_angle;                    // limite dos alvos de pitch/roll (rad)
    float ride_height;                       // elevação da carroceria em repouso (mundo); rodas estendem para baixo
    float wheel_travel;                      // curso máximo da roda em torno do repouso (unid. locais)
    float wheel_offset_fl, wheel_offset_fr;  // deslocamento Y local de cada roda (aplicado em Draw)
    float wheel_offset_rl, wheel_offset_rr;
    float prev_speed;
    bool  suspension_initialized;

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

    // Retorna altura do terreno na roda
    float GetTerrainHeightAtWheel(const SceneObject& wheel);

    // Atualiza a posição do ônibus
    void UpdatePos(const double deltaT);

    // Atualiza carroceria (molas) e rodas (contato com o chão)
    void UpdateSuspension(const double deltaT);

    // Atualiza teclas
    void OnKey(int key, int action);

    // Retorna a matriz Model computada via matrices.h
    glm::mat4 GetModelMatrix() const;

    // Renderiza todas as malhas do ônibus
    void Draw(ObjModel& model);
};

// Instância global do ônibus
extern Bus g_Bus;

#endif // _BUS_H
