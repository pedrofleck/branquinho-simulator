#ifndef _COLLISIONS_H
#define _COLLISIONS_H

// ===========================================================================
// collisions.h -- Testes de intersecção entre objetos virtuais
// ===========================================================================
//
// O enunciado do trabalho final exige que:
//
//   "Os testes de colisão devem ser implementados em um arquivo separado,
//    nomeado collisions.cpp para projetos em C++."
//
// Portanto, TODAS as funções de teste de intersecção da aplicação devem ser
// declaradas aqui e implementadas em "src/collisions.cpp".
//
// As funções abaixo cobrem os três tipos clássicos de teste pedidos na
// disciplina. Elas fazem parte do código base: cabe a você usá-las (e/ou
// acrescentar novas) de modo que os testes tenham um PROPÓSITO dentro da
// lógica da sua aplicação.
//
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// --- Esfera x Esfera ------------------------------------------------------
// Retorna "true" se as duas esferas se intersectam.
bool CollisionSphereSphere(
    glm::vec4 center_a, float radius_a,
    glm::vec4 center_b, float radius_b
);

// --- AABB x AABB ----------------------------------------------------------
// Retorna "true" se as duas "axis-aligned bounding boxes" se intersectam.
bool CollisionAABBAABB(
    glm::vec3 min_a, glm::vec3 max_a,
    glm::vec3 min_b, glm::vec3 max_b
);

// --- Esfera x AABB --------------------------------------------------------
// Retorna "true" se a esfera intersecta a caixa.
bool CollisionSphereAABB(
    glm::vec4 center, float radius,
    glm::vec3 box_min, glm::vec3 box_max
);

// --- Esfera x Plano -------------------------------------------------------
// O plano é definido pela equação  dot(normal, p) + d = 0, com "normal"
// normalizado. Retorna "true" se a esfera toca ou atravessa o plano.
bool CollisionSpherePlane(
    glm::vec4 center, float radius,
    glm::vec4 plane_normal, float plane_d
);

// --- Ponto x AABB ---------------------------------------------------------
// Retorna "true" se o ponto está dentro da caixa.
bool CollisionPointAABB(
    glm::vec4 point,
    glm::vec3 box_min, glm::vec3 box_max
);

// --- Raio x AABB ----------------------------------------------------------
// Teste de intersecção entre um raio e uma caixa, pelo método dos "slabs".
// Útil, por exemplo, para implementar seleção de objetos com o mouse
// (picking), que é uma das sugestões de funcionalidade extra do enunciado.
//
// Se houver intersecção e "out_t" não for NULL, "*out_t" recebe a distância
// (em múltiplos do comprimento de "ray_direction") até o ponto de entrada.
bool CollisionRayAABB(
    glm::vec4 ray_origin, glm::vec4 ray_direction,
    glm::vec3 box_min, glm::vec3 box_max,
    float* out_t
);

// --- Utilitário -----------------------------------------------------------
// Transforma a bounding box de um objeto (que está em coordenadas do MODELO,
// como armazenada em g_VirtualScene) para coordenadas do MUNDO, aplicando a
// Model matrix do objeto.
//
// NOTE: o resultado é a AABB que envolve os 8 vértices transformados. Para
// objetos rotacionados, essa caixa é maior que a caixa original.
void ComputeWorldAABB(
    const glm::mat4& model,
    glm::vec3 bbox_min, glm::vec3 bbox_max,
    glm::vec3* out_min, glm::vec3* out_max
);

#endif // _COLLISIONS_H
