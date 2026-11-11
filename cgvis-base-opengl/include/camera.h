#ifndef _CAMERA_H
#define _CAMERA_H

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

class Bus;  // declaração antecipada

// Classe que encapsula a câmera Look-At em coordenadas esféricas e projeções
class Camera
{
public:
    Camera();

    // Parâmetros em coordenadas esféricas
    float theta;       // Ângulo horizontal no plano XZ em relação a Z
    float phi;         // Ângulo vertical em relação ao plano XZ
    float distance;    // Distância da câmera ao ponto focal (look-at)

    // Parâmetros de projeção
    bool  use_perspective;
    float screen_ratio;
    float nearplane;
    float farplane;
    float fov;
    enum CameraMode { Orbit, Bumper, Free };
    CameraMode mode;
    CameraMode previous_mode;
    glm::vec4 bumper_offset_local;
    glm::vec4 free_position;

    // Teclas
    bool keyW, keyA, keyS, keyD;

    // Estado do mouse para controle da órbita
    bool   left_button_pressed;
    double last_cursor_x;
    double last_cursor_y;

    // Eventos de entrada
    void OnCursorPos(double xpos, double ypos);
    void OnMouseButton(int button, int action, int mods, double xpos, double ypos);
    void OnScroll(double xoffset, double yoffset);
    void SetScreenRatio(int width, int height);
    void ToggleCameraMode();
    void ToggleFreeCamera(const glm::vec4& focus_position);
    void OnKey(int key, int action);
    void UpdateFreeCamera(double deltaT);

    // Métodos de cálculo de matrizes usando matrices.h
    glm::vec4 GetFreeViewDirection() const;
    glm::mat4 ComputeViewMatrix(const Bus& bus) const;
    glm::mat4 ComputeProjectionMatrix() const;
};

// Instância global da câmera
extern Camera g_Camera;

#endif // _CAMERA_H
