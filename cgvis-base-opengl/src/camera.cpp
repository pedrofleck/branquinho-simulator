// ============================================================================
// camera.cpp - Câmera orbital (look-at) e projeção
//
// Responsável por:
//   - Manter o estado da câmera em coordenadas esféricas (theta, phi,
//     distance) em torno de um ponto de interesse (look-at), tipicamente o
//     ônibus;
//   - Tratar a entrada do mouse que controla a câmera (arrastar com o botão
//     esquerdo altera theta/phi; scroll altera a distância);
//   - Calcular a matriz View via Matrix_Camera_View() e a matriz Projection
//     (perspectiva ou ortográfica) via Matrix_Perspective()/Matrix_Orthographic(),
//     considerando a razão de aspecto da janela e os planos near/far.
//
// Todas as matrizes são construídas exclusivamente com as funções de
// "matrices.h" (proibido usar glm::lookAt, glm::perspective, etc.).
// ============================================================================

#include "camera.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <algorithm>
#include <GLFW/glfw3.h>

#include "bus.h"
#include "matrices.h"

// Instância global
Camera g_Camera;

Camera::Camera(){
    theta = 0.0f;
    phi = 0.35f;
    distance = 6.0f;
    use_perspective = true;
    screen_ratio = 1.0f;
    nearplane = -0.1f;
    farplane = -3500.0f;
    fov = 3.141592f / 3.0f;
    left_button_pressed = false;
    last_cursor_x = 0.0;
    last_cursor_y = 0.0;
    mode = Orbit;
    previous_mode = Orbit;
    bumper_offset_local = glm::vec4(0.0f, 0.7f, -8.3f, 1.0f);
    free_position = glm::vec4(0.0f, 5.0f, 8.0f, 1.0f);
    keyW = false;
    keyA = false;
    keyS = false;
    keyD = false;
}

void Camera::OnCursorPos(double xpos, double ypos)
{
    // Só executa se câmera estiver em Orbit ou Free
    if (mode == Bumper) return;

    if (left_button_pressed)
    {
        float dx = (float)(xpos - last_cursor_x);
        float dy = (float)(ypos - last_cursor_y);

        theta -= 0.01f * dx;
        phi   += 0.01f * dy;

        // No modo Orbit limita phimin > 0 para não atravessar o chão;
        // no modo Free permite olhar livremente para cima e para baixo
        float phimax = 3.141592f / 2.0f - 0.05f;
        float phimin = (mode == Free) ? -phimax : 0.02f;

        if (phi > phimax)
            phi = phimax;
        if (phi < phimin)
            phi = phimin;

        last_cursor_x = xpos;
        last_cursor_y = ypos;
    }
}

void Camera::OnMouseButton(int button, int action, int mods, double xpos, double ypos)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT)
    {
        if (action == GLFW_PRESS)
        {
            last_cursor_x = xpos;
            last_cursor_y = ypos;
            left_button_pressed = true;
        }
        else if (action == GLFW_RELEASE)
        {
            left_button_pressed = false;
        }
    }
}

void Camera::OnScroll(double xoffset, double yoffset)
{
    // Só executa se câmera estiver em Orbit
    if (mode != Orbit) return;

    distance -= 0.1f * (float)yoffset;

    const float verysmallnumber = std::numeric_limits<float>::epsilon();
    if (distance < verysmallnumber)
        distance = verysmallnumber;
}

// Trecho da câmera livre
void Camera::OnKey(int key, int action)
{
    bool isPressed = (action == GLFW_PRESS || action == GLFW_REPEAT);

    if (key == GLFW_KEY_W)      keyW = isPressed;
    else if (key == GLFW_KEY_S) keyS = isPressed;
    else if (key == GLFW_KEY_A) keyA = isPressed;
    else if (key == GLFW_KEY_D) keyD = isPressed;
}

glm::vec4 Camera::GetFreeViewDirection() const
{
    return glm::vec4(
        -std::cos(phi) * std::sin(theta),
        -std::sin(phi),
        -std::cos(phi) * std::cos(theta),
         0.0f
    );
}

void Camera::UpdateFreeCamera(double deltaT)
{
    if (mode != Free) return;

    float speed = 120.0f * (float)deltaT;

    glm::vec4 view_dir  = GetFreeViewDirection();
    glm::vec4 up_vec    = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    glm::vec4 right_dir = crossproduct(view_dir, up_vec);
    right_dir = right_dir / norm(right_dir);

    if (keyW) free_position += view_dir  * speed;
    if (keyS) free_position -= view_dir  * speed;
    if (keyD) free_position += right_dir * speed;
    if (keyA) free_position -= right_dir * speed;
    free_position.w = 1.0f;
}

void Camera::SetScreenRatio(int width, int height)
{
    screen_ratio = (float)width / (float)height;
}

glm::mat4 Camera::ComputeViewMatrix(const Bus& bus) const
{
    float r, x, y, z;
    glm::vec4 camera_position_c;
    glm::vec4 camera_view_vector;
    glm::vec4 camera_up_vector;

    // Câmera orbital (look-at)
    if (mode == Orbit){
        r = distance;
        y = r * std::sin(phi);
        z = r * std::cos(phi) * std::cos(theta);
        x = r * std::cos(phi) * std::sin(theta);
    
        camera_up_vector = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        camera_position_c  = bus.position + glm::vec4(x, y, z, 0.0f);
        camera_position_c.w = 1.0f;
        glm::vec4 camera_lookat_l = bus.position;
        camera_view_vector = camera_lookat_l - camera_position_c;
    // Câmera bumper
    } else if (mode == Bumper){
        camera_up_vector = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        camera_position_c = bus.GetModelMatrix() * bumper_offset_local;
        camera_view_vector = Matrix_Rotate_Y(bus.yaw) * bus.forward_local;
    // Câmera livre
    } else if (mode == Free){
        camera_up_vector = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        camera_position_c = free_position;
        camera_view_vector = GetFreeViewDirection();
    }

    return Matrix_Camera_View(camera_position_c, camera_view_vector, camera_up_vector);
}

void Camera::ToggleCameraMode(){
    keyW = keyA = keyS = keyD = false;

    if (mode == Orbit) mode = Bumper;
    else // nova condicional para a orbital
    {
        mode = Orbit;
        if (phi < 0.02f) phi = 0.02f;
    }
}

// Trecho da câmera livre
void Camera::ToggleFreeCamera(const glm::vec4& focus_position){
    keyW = keyA = keyS = keyD = false;

    if (mode != Free)
    {
        previous_mode = mode;
        mode = Free;
        free_position = focus_position + glm::vec4(0.0f, 5.0f, 8.0f, 0.0f);
        free_position.w = 1.0f;
        printf("Camera: Livre (F11 para sair, WASD para mover, mouse para olhar)\n");
    }
    else
    {
        mode = previous_mode;
        if (mode == Orbit && phi < 0.02f) phi = 0.02f;
        printf("Camera: %s\n", (mode == Orbit) ? "Orbit" : "Bumper");
    }
}

glm::mat4 Camera::ComputeProjectionMatrix() const
{
    if (use_perspective)
    {
        return Matrix_Perspective(fov, screen_ratio, nearplane, farplane);
    }
    else
    {
        float t = 1.5f * distance / 2.5f;
        float b = -t;
        float r = t * screen_ratio;
        float l = -r;
        return Matrix_Orthographic(l, r, b, t, nearplane, farplane);
    }
}
