#include "camera.h"

#include <cmath>
#include <limits>
#include <algorithm>
#include <GLFW/glfw3.h>

#include "matrices.h"

// Instância global
Camera g_Camera;

Camera::Camera()
    : theta(0.0f)
    , phi(0.35f)
    , distance(6.0f)
    , use_perspective(true)
    , screen_ratio(1.0f)
    , nearplane(-0.1f)
    , farplane(-200.0f)
    , fov(3.141592f / 3.0f)
    , left_button_pressed(false)
    , last_cursor_x(0.0)
    , last_cursor_y(0.0)
{
}

void Camera::OnCursorPos(double xpos, double ypos)
{
    if (left_button_pressed)
    {
        float dx = (float)(xpos - last_cursor_x);
        float dy = (float)(ypos - last_cursor_y);

        theta -= 0.01f * dx;
        phi   += 0.01f * dy;

        // Limita a elevação para não virar a câmera de cabeça para baixo
        // e phimin > 0 para não atravessar o plano do chão
        float phimax = 3.141592f / 2.0f - 0.05f;
        float phimin = 0.02f;

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
    distance -= 0.1f * (float)yoffset;

    const float verysmallnumber = std::numeric_limits<float>::epsilon();
    if (distance < verysmallnumber)
        distance = verysmallnumber;
}

void Camera::SetScreenRatio(int width, int height)
{
    screen_ratio = (float)width / (float)height;
}

glm::mat4 Camera::ComputeViewMatrix(glm::vec4 lookat_point) const
{
    float r = distance;
    float y = r * std::sin(phi);
    float z = r * std::cos(phi) * std::cos(theta);
    float x = r * std::cos(phi) * std::sin(theta);

    glm::vec4 camera_position_c  = lookat_point + glm::vec4(x, y, z, 0.0f);
    camera_position_c.w = 1.0f;
    glm::vec4 camera_lookat_l    = lookat_point;
    glm::vec4 camera_view_vector = camera_lookat_l - camera_position_c;
    glm::vec4 camera_up_vector   = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);

    return Matrix_Camera_View(camera_position_c, camera_view_vector, camera_up_vector);
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
