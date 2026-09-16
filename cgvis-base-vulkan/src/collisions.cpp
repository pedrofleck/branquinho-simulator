// ===========================================================================
// collisions.cpp -- Testes de intersecção entre objetos virtuais
// ===========================================================================
//
// Veja "include/collisions.h" para a documentação de cada função e para a
// justificativa da existência deste arquivo separado (exigência do enunciado
// do trabalho final).
//
// NOTE: este arquivo NÃO inclui "matrices.h". Aquele header define funções
// (e não apenas as declara), então incluí-lo em mais de um arquivo .cpp
// causaria erro de "multiple definition" na hora da linkagem. Aqui usamos a
// GLM apenas como container de vetores e matrizes.
//
#include <cmath>
#include <algorithm>

#include "collisions.h"

// ---------------------------------------------------------------------------
// Esfera x Esfera
// ---------------------------------------------------------------------------
//
// Duas esferas se intersectam quando a distância entre seus centros é menor ou
// igual à soma de seus raios. Comparamos as distâncias AO QUADRADO para evitar
// o cálculo (mais caro) de uma raiz quadrada.
//
bool CollisionSphereSphere(
    glm::vec4 center_a, float radius_a,
    glm::vec4 center_b, float radius_b
)
{
    float dx = center_a.x - center_b.x;
    float dy = center_a.y - center_b.y;
    float dz = center_a.z - center_b.z;

    float distance_squared = dx*dx + dy*dy + dz*dz;
    float radius_sum       = radius_a + radius_b;

    return distance_squared <= radius_sum * radius_sum;
}

// ---------------------------------------------------------------------------
// AABB x AABB
// ---------------------------------------------------------------------------
//
// Duas caixas alinhadas aos eixos se intersectam se, e somente se, seus
// intervalos se sobrepõem SIMULTANEAMENTE nos três eixos X, Y e Z. Basta um
// eixo em que os intervalos estejam separados para garantir que não há
// colisão (este é o "Separating Axis Theorem" no seu caso mais simples).
//
bool CollisionAABBAABB(
    glm::vec3 min_a, glm::vec3 max_a,
    glm::vec3 min_b, glm::vec3 max_b
)
{
    if (max_a.x < min_b.x || min_a.x > max_b.x) return false;
    if (max_a.y < min_b.y || min_a.y > max_b.y) return false;
    if (max_a.z < min_b.z || min_a.z > max_b.z) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Esfera x AABB
// ---------------------------------------------------------------------------
//
// Encontramos o ponto da caixa que é o MAIS PRÓXIMO do centro da esfera. Isso
// é feito "grampeando" (clamp) cada coordenada do centro ao intervalo da caixa
// naquele eixo. Havendo colisão, esse ponto está a uma distância menor ou igual
// ao raio.
//
bool CollisionSphereAABB(
    glm::vec4 center, float radius,
    glm::vec3 box_min, glm::vec3 box_max
)
{
    float closest_x = std::max(box_min.x, std::min(center.x, box_max.x));
    float closest_y = std::max(box_min.y, std::min(center.y, box_max.y));
    float closest_z = std::max(box_min.z, std::min(center.z, box_max.z));

    float dx = center.x - closest_x;
    float dy = center.y - closest_y;
    float dz = center.z - closest_z;

    float distance_squared = dx*dx + dy*dy + dz*dz;

    return distance_squared <= radius * radius;
}

// ---------------------------------------------------------------------------
// Esfera x Plano
// ---------------------------------------------------------------------------
//
// A distância COM SINAL de um ponto p a um plano de equação
// dot(normal, p) + d = 0 (com "normal" unitário) é simplesmente
// dot(normal, p) + d. A esfera toca o plano quando o valor absoluto dessa
// distância é menor ou igual ao raio.
//
bool CollisionSpherePlane(
    glm::vec4 center, float radius,
    glm::vec4 plane_normal, float plane_d
)
{
    float signed_distance = plane_normal.x * center.x
                          + plane_normal.y * center.y
                          + plane_normal.z * center.z
                          + plane_d;

    return std::fabs(signed_distance) <= radius;
}

// ---------------------------------------------------------------------------
// Ponto x AABB
// ---------------------------------------------------------------------------
bool CollisionPointAABB(
    glm::vec4 point,
    glm::vec3 box_min, glm::vec3 box_max
)
{
    return point.x >= box_min.x && point.x <= box_max.x
        && point.y >= box_min.y && point.y <= box_max.y
        && point.z >= box_min.z && point.z <= box_max.z;
}

// ---------------------------------------------------------------------------
// Raio x AABB (método dos "slabs")
// ---------------------------------------------------------------------------
//
// A caixa pode ser vista como a intersecção de três "fatias" (slabs) infinitas,
// uma por eixo. Para cada eixo calculamos em quais valores do parâmetro t o
// raio entra e sai da fatia daquele eixo. O raio atravessa a caixa se o maior
// dos valores de entrada for menor ou igual ao menor dos valores de saída.
//
// Divisões por zero (raio paralelo a um eixo) resultam em +/- infinito no
// padrão IEEE 754, e as comparações abaixo continuam corretas nesse caso.
//
bool CollisionRayAABB(
    glm::vec4 ray_origin, glm::vec4 ray_direction,
    glm::vec3 box_min, glm::vec3 box_max,
    float* out_t
)
{
    float t_enter = -INFINITY;
    float t_exit  =  INFINITY;

    const float origin[3]    = { ray_origin.x,    ray_origin.y,    ray_origin.z    };
    const float direction[3] = { ray_direction.x, ray_direction.y, ray_direction.z };
    const float minimum[3]   = { box_min.x,       box_min.y,       box_min.z       };
    const float maximum[3]   = { box_max.x,       box_max.y,       box_max.z       };

    for (int axis = 0; axis < 3; ++axis)
    {
        if (direction[axis] == 0.0f)
        {
            // O raio é paralelo a esta fatia: só pode haver intersecção se a
            // origem já estiver dentro dela.
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
                return false;

            continue;
        }

        float t1 = (minimum[axis] - origin[axis]) / direction[axis];
        float t2 = (maximum[axis] - origin[axis]) / direction[axis];

        if (t1 > t2)
        {
            float swap = t1;
            t1 = t2;
            t2 = swap;
        }

        if (t1 > t_enter) t_enter = t1;
        if (t2 < t_exit)  t_exit  = t2;

        if (t_enter > t_exit)
            return false;
    }

    // A caixa está inteiramente "atrás" da origem do raio.
    if (t_exit < 0.0f)
        return false;

    if (out_t != 0)
        *out_t = (t_enter < 0.0f) ? 0.0f : t_enter;

    return true;
}

// ---------------------------------------------------------------------------
// Utilitário: bounding box em coordenadas do mundo
// ---------------------------------------------------------------------------
void ComputeWorldAABB(
    const glm::mat4& model,
    glm::vec3 bbox_min, glm::vec3 bbox_max,
    glm::vec3* out_min, glm::vec3* out_max
)
{
    // Os 8 vértices da caixa, em coordenadas do modelo.
    const glm::vec4 corners[8] = {
        glm::vec4(bbox_min.x, bbox_min.y, bbox_min.z, 1.0f),
        glm::vec4(bbox_max.x, bbox_min.y, bbox_min.z, 1.0f),
        glm::vec4(bbox_min.x, bbox_max.y, bbox_min.z, 1.0f),
        glm::vec4(bbox_max.x, bbox_max.y, bbox_min.z, 1.0f),
        glm::vec4(bbox_min.x, bbox_min.y, bbox_max.z, 1.0f),
        glm::vec4(bbox_max.x, bbox_min.y, bbox_max.z, 1.0f),
        glm::vec4(bbox_min.x, bbox_max.y, bbox_max.z, 1.0f),
        glm::vec4(bbox_max.x, bbox_max.y, bbox_max.z, 1.0f)
    };

    glm::vec4 first = model * corners[0];

    glm::vec3 result_min = glm::vec3(first.x, first.y, first.z);
    glm::vec3 result_max = result_min;

    for (int i = 1; i < 8; ++i)
    {
        glm::vec4 p = model * corners[i];

        result_min.x = std::min(result_min.x, p.x);
        result_min.y = std::min(result_min.y, p.y);
        result_min.z = std::min(result_min.z, p.z);

        result_max.x = std::max(result_max.x, p.x);
        result_max.y = std::max(result_max.y, p.y);
        result_max.z = std::max(result_max.z, p.z);
    }

    *out_min = result_min;
    *out_max = result_max;
}

// vim: set spell spelllang=pt_br :
