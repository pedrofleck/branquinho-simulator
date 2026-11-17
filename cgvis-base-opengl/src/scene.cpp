// ============================================================================
// scene.cpp - Cena virtual, terreno multi-material e infraestrutura de GPU
//
// Responsável por:
//   - Definir as variáveis globais da cena virtual (g_VirtualScene), da pilha
//     de matrizes (g_MatrixStack) e dos identificadores de programa/uniforms
//     da GPU;
//   - Carregar modelos .obj (ObjModel, via tinyobjloader), calcular normais
//     quando ausentes e enviar a geometria para a GPU (VAO/VBOs), registrando
//     cada shape como um SceneObject desenhável por nome (DrawVirtualObject);
//   - Construir a malha do terreno 16-bit descartando as áreas em preto da
//     máscara (`valid_mask.png`) e instanciar a floresta de árvores 3D usando
//     exclusivamente as matrizes de `matrices.h`;
//   - Carregar texturas RGB (sRGB) e Splatmaps RGBA (Lineares);
//   - Ler, compilar e linkar os shaders GLSL (vertex/fragment) e obter as
//     localizações das variáveis uniform utilizadas na renderização.
// ============================================================================

#include "scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cassert>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <set>
#include <limits>
#include <algorithm>
#include <unordered_map>
#include <cstdint>

#include <stb_image.h>
#include <glm/gtc/type_ptr.hpp>

#include "matrices.h"
#include "utils.h"

// Definições de variáveis globais de cena e GPU
std::map<std::string, SceneObject> g_VirtualScene;
std::stack<glm::mat4>              g_MatrixStack;

// Dados de altimetria do terreno carregado via PNG 16-bit para consulta de altura O(1) na CPU
static std::vector<float> g_TerrainHeights;
static int                g_TerrainGridW      = 0;
static int                g_TerrainGridH      = 0;
static float              g_TerrainWorldWidth = 100.0f;
static float              g_TerrainWorldDepth = 100.0f;

// Controle das árvores instanciadas no terreno
struct TreeInstanceData
{
    glm::mat4 model;
};
static bool                          g_TreeUseBatchedMesh = false;
static bool                          g_TreeLoaded         = false;
static std::vector<TreeInstanceData> g_TreeInstances;
static std::vector<std::string>      g_TreeTrunkShapes;
static std::vector<std::string>      g_TreeCanopyShapes;

GLuint g_GpuProgramID = 0;
GLint  g_model_uniform;
GLint  g_view_uniform;
GLint  g_projection_uniform;
GLint  g_object_id_uniform;
GLint  g_bbox_min_uniform;
GLint  g_bbox_max_uniform;
GLuint g_NumLoadedTextures   = 0;
bool   g_HasSatelliteTexture = false;
bool   g_SatelliteTerrainMode = false;
bool   g_HasTerrainSplatmaps = false;

// Construtor do ObjModel
ObjModel::ObjModel(const char* filename, const char* basepath, bool triangulate)
{
    printf("Carregando objetos do arquivo \"%s\"...\n", filename);

    std::string fullpath(filename);
    std::string dirname;
    if (basepath == NULL)
    {
        auto i = fullpath.find_last_of("/");
        if (i != std::string::npos)
        {
            dirname = fullpath.substr(0, i + 1);
            basepath = dirname.c_str();
        }
    }

    std::string warn;
    std::string err;
    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename, basepath, triangulate);

    if (!err.empty())
        fprintf(stderr, "\n%s\n", err.c_str());

    if (!ret)
        throw std::runtime_error("Erro ao carregar modelo.");

    for (size_t shape = 0; shape < shapes.size(); ++shape)
    {
        if (shapes[shape].name.empty())
        {
            fprintf(stderr,
                    "*********************************************\n"
                    "Erro: Objeto sem nome dentro do arquivo '%s'.\n"
                    "*********************************************\n",
                    filename);
            throw std::runtime_error("Objeto sem nome.");
        }
        printf("- Objeto '%s'\n", shapes[shape].name.c_str());
    }

    printf("OK.\n");
}

void PushMatrix(glm::mat4 M)
{
    g_MatrixStack.push(M);
}

void PopMatrix(glm::mat4& M)
{
    if (g_MatrixStack.empty())
    {
        M = Matrix_Identity();
    }
    else
    {
        M = g_MatrixStack.top();
        g_MatrixStack.pop();
    }
}

void ComputeNormals(ObjModel* model)
{
    if (!model->attrib.normals.empty())
        return;

    std::set<unsigned int> sgroup_ids;
    for (size_t shape = 0; shape < model->shapes.size(); ++shape)
    {
        size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();
        assert(model->shapes[shape].mesh.smoothing_group_ids.size() == num_triangles);

        for (size_t triangle = 0; triangle < num_triangles; ++triangle)
        {
            assert(model->shapes[shape].mesh.num_face_vertices[triangle] == 3);
            unsigned int sgroup = model->shapes[shape].mesh.smoothing_group_ids[triangle];
            assert(sgroup >= 0);
            sgroup_ids.insert(sgroup);
        }
    }

    size_t num_vertices = model->attrib.vertices.size() / 3;
    model->attrib.normals.reserve(3 * num_vertices);

    for (const unsigned int& sgroup : sgroup_ids)
    {
        std::vector<int> num_triangles_per_vertex(num_vertices, 0);
        std::vector<glm::vec4> vertex_normals(num_vertices, glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));

        for (size_t shape = 0; shape < model->shapes.size(); ++shape)
        {
            size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

            for (size_t triangle = 0; triangle < num_triangles; ++triangle)
            {
                unsigned int sgroup_tri = model->shapes[shape].mesh.smoothing_group_ids[triangle];
                if (sgroup_tri != sgroup)
                    continue;

                glm::vec4 vertices[3];
                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    tinyobj::index_t idx = model->shapes[shape].mesh.indices[3 * triangle + vertex];
                    const float vx = model->attrib.vertices[3 * idx.vertex_index + 0];
                    const float vy = model->attrib.vertices[3 * idx.vertex_index + 1];
                    const float vz = model->attrib.vertices[3 * idx.vertex_index + 2];
                    vertices[vertex] = glm::vec4(vx, vy, vz, 1.0f);
                }

                const glm::vec4 a = vertices[0];
                const glm::vec4 b = vertices[1];
                const glm::vec4 c = vertices[2];

                const glm::vec4 n = crossproduct(b - a, c - a);

                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    tinyobj::index_t idx = model->shapes[shape].mesh.indices[3 * triangle + vertex];
                    num_triangles_per_vertex[idx.vertex_index] += 1;
                    vertex_normals[idx.vertex_index] += n;
                }
            }
        }

        std::vector<size_t> normal_indices(num_vertices, 0);

        for (size_t vertex_index = 0; vertex_index < vertex_normals.size(); ++vertex_index)
        {
            if (num_triangles_per_vertex[vertex_index] == 0)
                continue;

            glm::vec4 n = vertex_normals[vertex_index] / (float)num_triangles_per_vertex[vertex_index];
            n /= norm(n);

            model->attrib.normals.push_back(n.x);
            model->attrib.normals.push_back(n.y);
            model->attrib.normals.push_back(n.z);

            size_t normal_index = (model->attrib.normals.size() / 3) - 1;
            normal_indices[vertex_index] = normal_index;
        }

        for (size_t shape = 0; shape < model->shapes.size(); ++shape)
        {
            size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

            for (size_t triangle = 0; triangle < num_triangles; ++triangle)
            {
                unsigned int sgroup_tri = model->shapes[shape].mesh.smoothing_group_ids[triangle];
                if (sgroup_tri != sgroup)
                    continue;

                for (size_t vertex = 0; vertex < 3; ++vertex)
                {
                    tinyobj::index_t idx = model->shapes[shape].mesh.indices[3 * triangle + vertex];
                    model->shapes[shape].mesh.indices[3 * triangle + vertex].normal_index =
                        normal_indices[idx.vertex_index];
                }
            }
        }
    }
}

void BuildTrianglesAndAddToVirtualScene(ObjModel* model)
{
    GLuint vertex_array_object_id;
    glGenVertexArrays(1, &vertex_array_object_id);
    glBindVertexArray(vertex_array_object_id);

    std::vector<GLuint> indices;
    std::vector<float>  model_coefficients;
    std::vector<float>  normal_coefficients;
    std::vector<float>  texture_coefficients;

    for (size_t shape = 0; shape < model->shapes.size(); ++shape)
    {
        size_t first_index = indices.size();
        size_t num_triangles = model->shapes[shape].mesh.num_face_vertices.size();

        const float minval = std::numeric_limits<float>::lowest();
        const float maxval = std::numeric_limits<float>::max();

        glm::vec3 bbox_min = glm::vec3(maxval, maxval, maxval);
        glm::vec3 bbox_max = glm::vec3(minval, minval, minval);

        for (size_t triangle = 0; triangle < num_triangles; ++triangle)
        {
            assert(model->shapes[shape].mesh.num_face_vertices[triangle] == 3);

            for (size_t vertex = 0; vertex < 3; ++vertex)
            {
                tinyobj::index_t idx = model->shapes[shape].mesh.indices[3 * triangle + vertex];

                indices.push_back(first_index + 3 * triangle + vertex);

                const float vx = model->attrib.vertices[3 * idx.vertex_index + 0];
                const float vy = model->attrib.vertices[3 * idx.vertex_index + 1];
                const float vz = model->attrib.vertices[3 * idx.vertex_index + 2];

                model_coefficients.push_back(vx);
                model_coefficients.push_back(vy);
                model_coefficients.push_back(vz);
                model_coefficients.push_back(1.0f);

                bbox_min.x = std::min(bbox_min.x, vx);
                bbox_min.y = std::min(bbox_min.y, vy);
                bbox_min.z = std::min(bbox_min.z, vz);
                bbox_max.x = std::max(bbox_max.x, vx);
                bbox_max.y = std::max(bbox_max.y, vy);
                bbox_max.z = std::max(bbox_max.z, vz);

                if (idx.normal_index != -1)
                {
                    const float nx = model->attrib.normals[3 * idx.normal_index + 0];
                    const float ny = model->attrib.normals[3 * idx.normal_index + 1];
                    const float nz = model->attrib.normals[3 * idx.normal_index + 2];
                    normal_coefficients.push_back(nx);
                    normal_coefficients.push_back(ny);
                    normal_coefficients.push_back(nz);
                    normal_coefficients.push_back(0.0f);
                }

                if (idx.texcoord_index != -1)
                {
                    const float u = model->attrib.texcoords[2 * idx.texcoord_index + 0];
                    const float v = model->attrib.texcoords[2 * idx.texcoord_index + 1];
                    texture_coefficients.push_back(u);
                    texture_coefficients.push_back(v);
                }
            }
        }

        size_t last_index = indices.size() - 1;

        SceneObject theobject;
        theobject.name           = model->shapes[shape].name;
        theobject.first_index    = first_index;
        theobject.num_indices    = last_index - first_index + 1;
        theobject.rendering_mode = GL_TRIANGLES;
        theobject.vertex_array_object_id = vertex_array_object_id;

        theobject.bbox_min = bbox_min;
        theobject.bbox_max = bbox_max;

        g_VirtualScene[model->shapes[shape].name] = theobject;
    }

    GLuint VBO_model_coefficients_id;
    glGenBuffers(1, &VBO_model_coefficients_id);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_model_coefficients_id);
    glBufferData(GL_ARRAY_BUFFER, model_coefficients.size() * sizeof(float), NULL, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, model_coefficients.size() * sizeof(float), model_coefficients.data());
    GLuint location = 0;
    GLint  number_of_dimensions = 4;
    glVertexAttribPointer(location, number_of_dimensions, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(location);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!normal_coefficients.empty())
    {
        GLuint VBO_normal_coefficients_id;
        glGenBuffers(1, &VBO_normal_coefficients_id);
        glBindBuffer(GL_ARRAY_BUFFER, VBO_normal_coefficients_id);
        glBufferData(GL_ARRAY_BUFFER, normal_coefficients.size() * sizeof(float), NULL, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, normal_coefficients.size() * sizeof(float), normal_coefficients.data());
        location = 1;
        number_of_dimensions = 4;
        glVertexAttribPointer(location, number_of_dimensions, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(location);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    if (!texture_coefficients.empty())
    {
        GLuint VBO_texture_coefficients_id;
        glGenBuffers(1, &VBO_texture_coefficients_id);
        glBindBuffer(GL_ARRAY_BUFFER, VBO_texture_coefficients_id);
        glBufferData(GL_ARRAY_BUFFER, texture_coefficients.size() * sizeof(float), NULL, GL_STATIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, texture_coefficients.size() * sizeof(float), texture_coefficients.data());
        location = 2;
        number_of_dimensions = 2;
        glVertexAttribPointer(location, number_of_dimensions, GL_FLOAT, GL_FALSE, 0, 0);
        glEnableVertexAttribArray(location);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    GLuint indices_id;
    glGenBuffers(1, &indices_id);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_id);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), NULL, GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices.size() * sizeof(GLuint), indices.data());

    glBindVertexArray(0);
}

void DrawVirtualObject(const char* object_name)
{
    auto it = g_VirtualScene.find(object_name);
    if (it == g_VirtualScene.end())
        return;

    const SceneObject& obj = it->second;
    glBindVertexArray(obj.vertex_array_object_id);

    glm::vec3 bbox_min = obj.bbox_min;
    glm::vec3 bbox_max = obj.bbox_max;
    glUniform4f(g_bbox_min_uniform, bbox_min.x, bbox_min.y, bbox_min.z, 1.0f);
    glUniform4f(g_bbox_max_uniform, bbox_max.x, bbox_max.y, bbox_max.z, 1.0f);

    glDrawElements(
        obj.rendering_mode,
        (GLsizei)obj.num_indices,
        GL_UNSIGNED_INT,
        (void*)(obj.first_index * sizeof(GLuint))
    );

    glBindVertexArray(0);
}

// ----------------------------------------------------------------------------
// GetTerrainHeight()
// Retorna a altura Y (em metros) do terreno nas coordenadas (world_x, world_z)
// usando interpolação bilinear sobre a grade 1:1 gerada pelo heightmap 16-bit.
// ----------------------------------------------------------------------------
float GetTerrainHeight(float world_x, float world_z)
{
    if (g_TerrainHeights.empty() || g_TerrainGridW < 2 || g_TerrainGridH < 2)
        return 0.0f;

    // Converte de coordenadas do mundo [-W/2, +W/2] x [-H/2, +H/2] para índices contínuos de pixel
    float fx = world_x + 0.5f * g_TerrainWorldWidth;
    float fz = world_z + 0.5f * g_TerrainWorldDepth;

    fx = std::max(0.0f, std::min((float)(g_TerrainGridW - 1), fx));
    fz = std::max(0.0f, std::min((float)(g_TerrainGridH - 1), fz));

    int x0 = (int)std::floor(fx);
    int z0 = (int)std::floor(fz);
    int x1 = std::min(x0 + 1, g_TerrainGridW - 1);
    int z1 = std::min(z0 + 1, g_TerrainGridH - 1);

    float tx = fx - (float)x0;
    float tz = fz - (float)z0;

    float h00 = g_TerrainHeights[(size_t)z0 * (size_t)g_TerrainGridW + (size_t)x0];
    float h10 = g_TerrainHeights[(size_t)z0 * (size_t)g_TerrainGridW + (size_t)x1];
    float h01 = g_TerrainHeights[(size_t)z1 * (size_t)g_TerrainGridW + (size_t)x0];
    float h11 = g_TerrainHeights[(size_t)z1 * (size_t)g_TerrainGridW + (size_t)x1];

    float h0 = h00 * (1.0f - tx) + h10 * tx;
    float h1 = h01 * (1.0f - tx) + h11 * tx;

    return h0 * (1.0f - tz) + h1 * tz;
}

// Estrutura de vértice intercalado (Posição vec3, Normal vec3, UV vec2) para o VBO único do terreno
struct TerrainVertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// Suaviza o terreno em float32 estritamente DENTRO do asfalto (`splatmap_0.r + splatmap_0.g`)
// por difusão geodésica 3x3 e aplica o rebaixamento físico de -10 cm somente no asfalto.
static void SmoothTerrainAlongRoads(
    std::vector<float>& heights,
    int                 W,
    int                 H,
    const char*         splat0_filename,
    bool                unapply_recess_first,
    std::vector<float>* out_unrecessed_heights
)
{
    int sw = 0, sh = 0, sc = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* splat0 = stbi_load(splat0_filename, &sw, &sh, &sc, 4);
    if (splat0 == NULL)
    {
        if (out_unrecessed_heights)
            *out_unrecessed_heights = heights;
        return;
    }

    const size_t N = static_cast<size_t>(W) * static_cast<size_t>(H);
    std::vector<unsigned char> in_asph(N, 0);
    std::vector<float> w_asph(N, 0.0f);

    for (int y = 0; y < H; ++y)
    {
        float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(H) - 0.5f;
        sy = std::max(0.0f, std::min(static_cast<float>(sh - 1), sy));
        int y0 = static_cast<int>(sy);
        int y1 = std::min(sh - 1, y0 + 1);
        float ty = sy - static_cast<float>(y0);

        for (int x = 0; x < W; ++x)
        {
            float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(W) - 0.5f;
            sx = std::max(0.0f, std::min(static_cast<float>(sw - 1), sx));
            int x0 = static_cast<int>(sx);
            int x1 = std::min(sw - 1, x0 + 1);
            float tx = sx - static_cast<float>(x0);

            const unsigned char* p00 = &splat0[(static_cast<size_t>(y0) * sw + x0) * 4];
            const unsigned char* p10 = &splat0[(static_cast<size_t>(y0) * sw + x1) * 4];
            const unsigned char* p01 = &splat0[(static_cast<size_t>(y1) * sw + x0) * 4];
            const unsigned char* p11 = &splat0[(static_cast<size_t>(y1) * sw + x1) * 4];

            auto sample_ch = [&](int c) -> float {
                float a = p00[c] * (1.0f - tx) + p10[c] * tx;
                float b = p01[c] * (1.0f - tx) + p11[c] * tx;
                return (a * (1.0f - ty) + b * ty) / 255.0f;
            };

            float r = sample_ch(0);
            float g = sample_ch(1);
            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            w_asph[idx] = std::max(0.0f, std::min(1.0f, r + g));
        }
    }
    stbi_image_free(splat0);

    // Desfaz temporariamente -10 cm se vale_processed.png já veio rebaixado
    std::vector<float> h_unrec(heights);
    if (unapply_recess_first)
    {
        for (size_t i = 0; i < N; ++i)
        {
            if (w_asph[i] > 0.45f && heights[i] > 33.02f)
                h_unrec[i] += 0.10f;
        }
    }

    std::vector<size_t> active_indices;
    active_indices.reserve(N / 8);

    for (int y = 0; y < H; ++y)
    {
        int yU = std::max(0, y - 1);
        int yD = std::min(H - 1, y + 1);
        for (int x = 0; x < W; ++x)
        {
            size_t idx = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
            if (w_asph[idx] <= 0.45f || h_unrec[idx] <= 33.02f)
                continue;

            int xL = std::max(0, x - 1);
            int xR = std::min(W - 1, x + 1);
            float dx = (xR > xL) ? (h_unrec[static_cast<size_t>(y) * W + xR] - h_unrec[static_cast<size_t>(y) * W + xL]) / static_cast<float>(xR - xL) : 0.0f;
            float dy = (yD > yU) ? (h_unrec[static_cast<size_t>(yD) * W + x] - h_unrec[static_cast<size_t>(yU) * W + x]) / static_cast<float>(yD - yU) : 0.0f;
            float grad = std::hypot(dx, dy);
            if (grad <= 0.85f)
            {
                in_asph[idx] = 1;
                active_indices.push_back(idx);
            }
        }
    }

    std::vector<float> w_node(N, 0.0f);
    for (size_t idx : active_indices)
    {
        int x = static_cast<int>(idx % static_cast<size_t>(W));
        int y = static_cast<int>(idx / static_cast<size_t>(W));
        bool all_inner = true;
        for (int dy = -1; dy <= 1 && all_inner; ++dy)
        {
            int ny = std::max(0, std::min(H - 1, y + dy));
            for (int dx = -1; dx <= 1; ++dx)
            {
                int nx = std::max(0, std::min(W - 1, x + dx));
                if (!in_asph[static_cast<size_t>(ny) * static_cast<size_t>(W) + nx])
                {
                    all_inner = false;
                    break;
                }
            }
        }
        w_node[idx] = all_inner ? 1.0f : 0.15f;
    }

    std::vector<float> h_cur(h_unrec);
    std::vector<float> h_next(h_unrec);
    static const float kKernel[3][3] = {
        { 0.7f, 1.0f, 0.7f },
        { 1.0f, 1.2f, 1.0f },
        { 0.7f, 1.0f, 0.7f }
    };

    for (int it = 0; it < 35; ++it)
    {
        for (size_t idx : active_indices)
        {
            int x = static_cast<int>(idx % static_cast<size_t>(W));
            int y = static_cast<int>(idx / static_cast<size_t>(W));
            double num = 0.0;
            double den = 0.0;
            for (int dy = -1; dy <= 1; ++dy)
            {
                int ny = std::max(0, std::min(H - 1, y + dy));
                for (int dx = -1; dx <= 1; ++dx)
                {
                    int nx = std::max(0, std::min(W - 1, x + dx));
                    size_t nidx = static_cast<size_t>(ny) * static_cast<size_t>(W) + nx;
                    double kw = static_cast<double>(kKernel[dy + 1][dx + 1]) * static_cast<double>(w_node[nidx]);
                    num += kw * static_cast<double>(h_cur[nidx]);
                    den += kw;
                }
            }
            if (den > 1e-5)
                h_next[idx] = static_cast<float>(num / den);
        }
        h_cur.swap(h_next);
    }

    if (out_unrecessed_heights)
        *out_unrecessed_heights = h_cur;

    for (size_t idx : active_indices)
        heights[idx] = h_cur[idx] - 0.10f;

    printf("Suavizacao geodesica estrita e rebaixamento (-10 cm) no asfalto aplicados (%zu vertices).\n", active_indices.size());
}

// ============================================================================
// Meio-Fio 3D Geométrico (Marching Squares + Taubin + Ramer-Douglas-Peucker)
// Extrai bordas retas e suaves da camada vermelha (`s0.r`), recorta encontros
// com o asfalto secundário (`s0.g`) e gera blocos prismáticos com quinas de 90°.
// ============================================================================
static float SampleHeightGridBilinear(
    const std::vector<float>& grid,
    int                       W,
    int                       H,
    float                     world_x,
    float                     world_z
)
{
    float gx = (world_x + 0.5f * g_TerrainWorldWidth) / g_TerrainWorldWidth * static_cast<float>(W - 1);
    float gz = (world_z + 0.5f * g_TerrainWorldDepth) / g_TerrainWorldDepth * static_cast<float>(H - 1);
    gx = std::max(0.0f, std::min(static_cast<float>(W - 1), gx));
    gz = std::max(0.0f, std::min(static_cast<float>(H - 1), gz));
    int x0 = static_cast<int>(gx), y0 = static_cast<int>(gz);
    int x1 = std::min(W - 1, x0 + 1), y1 = std::min(H - 1, y0 + 1);
    float tx = gx - static_cast<float>(x0);
    float ty = gz - static_cast<float>(y0);
    float h00 = grid[static_cast<size_t>(y0) * static_cast<size_t>(W) + x0];
    float h10 = grid[static_cast<size_t>(y0) * static_cast<size_t>(W) + x1];
    float h01 = grid[static_cast<size_t>(y1) * static_cast<size_t>(W) + x0];
    float h11 = grid[static_cast<size_t>(y1) * static_cast<size_t>(W) + x1];
    float a = h00 * (1.0f - tx) + h10 * tx;
    float b = h01 * (1.0f - tx) + h11 * tx;
    return a * (1.0f - ty) + b * ty;
}

static void RDPSimplifyRange(
    const std::vector<glm::vec2>& pts,
    size_t                        start_idx,
    size_t                        end_idx,
    float                         eps,
    std::vector<glm::vec2>&       out
)
{
    if (end_idx <= start_idx + 1)
    {
        out.push_back(pts[start_idx]);
        return;
    }

    const glm::vec2& A = pts[start_idx];
    const glm::vec2& B = pts[end_idx];
    float abx = B.x - A.x;
    float abz = B.y - A.y;
    float len = std::hypot(abx, abz);

    float max_d = -1.0f;
    size_t best_i = start_idx;
    for (size_t i = start_idx + 1; i < end_idx; ++i)
    {
        float d = (len < 1e-6f)
                ? std::hypot(pts[i].x - A.x, pts[i].y - A.y)
                : std::fabs(abx * (pts[i].y - A.y) - abz * (pts[i].x - A.x)) / len;
        if (d > max_d)
        {
            max_d = d;
            best_i = i;
        }
    }

    if (max_d > eps)
    {
        RDPSimplifyRange(pts, start_idx, best_i, eps, out);
        RDPSimplifyRange(pts, best_i, end_idx, eps, out);
    }
    else
    {
        out.push_back(pts[start_idx]);
    }
}

static void AddCurbQuad(
    std::vector<TerrainVertex>& verts,
    std::vector<GLuint>&        indices,
    glm::vec3&                  bbox_min,
    glm::vec3&                  bbox_max,
    const glm::vec3&            p0, const glm::vec2& uv0,
    const glm::vec3&            p1, const glm::vec2& uv1,
    const glm::vec3&            p2, const glm::vec2& uv2,
    const glm::vec3&            p3, const glm::vec2& uv3,
    const glm::vec3&            face_normal
)
{
    auto expand_bb = [&](const glm::vec3& p) {
        bbox_min.x = std::min(bbox_min.x, p.x); bbox_min.y = std::min(bbox_min.y, p.y); bbox_min.z = std::min(bbox_min.z, p.z);
        bbox_max.x = std::max(bbox_max.x, p.x); bbox_max.y = std::max(bbox_max.y, p.y); bbox_max.z = std::max(bbox_max.z, p.z);
    };
    GLuint base = static_cast<GLuint>(verts.size());
    verts.push_back({ p0, face_normal, uv0 });
    verts.push_back({ p1, face_normal, uv1 });
    verts.push_back({ p2, face_normal, uv2 });
    verts.push_back({ p3, face_normal, uv3 });
    expand_bb(p0); expand_bb(p1); expand_bb(p2); expand_bb(p3);

    glm::vec3 e1 = p1 - p0;
    glm::vec3 e2 = p2 - p0;
    glm::vec3 cross_n = glm::vec3(
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x
    );
    float d = cross_n.x * face_normal.x + cross_n.y * face_normal.y + cross_n.z * face_normal.z;
    if (d >= 0.0f)
    {
        indices.push_back(base + 0); indices.push_back(base + 1); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 3);
    }
    else
    {
        indices.push_back(base + 0); indices.push_back(base + 3); indices.push_back(base + 2);
        indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 1);
    }
}

static void BuildRoadCurbsAndAddToVirtualScene(
    const char*                       splat0_filename,
    const std::vector<unsigned char>& valid_mask,
    int                               hmW,
    int                               hmH,
    const std::vector<float>&         unrecessed_heights,
    const std::vector<float>&         recessed_heights
)
{
    int sw = 0, sh = 0, sc = 0;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* splat0 = stbi_load(splat0_filename, &sw, &sh, &sc, 4);
    if (splat0 == NULL || sw < 4 || sh < 4)
        return;

    const size_t SN = static_cast<size_t>(sw) * static_cast<size_t>(sh);
    std::vector<float> r_f(SN, 0.0f), r_tmp(SN, 0.0f);
    std::vector<unsigned char> g_dil(SN, 0), g_tmp(SN, 0);
    std::vector<unsigned char> valid_s(SN, 255), valid_tmp(SN, 255);

    for (int y = 0; y < sh; ++y)
    {
        int my = std::min(hmH - 1, static_cast<int>((static_cast<size_t>(y) * hmH) / sh));
        for (int x = 0; x < sw; ++x)
        {
            int mx = std::min(hmW - 1, static_cast<int>((static_cast<size_t>(x) * hmW) / sw));
            size_t idx = static_cast<size_t>(y) * sw + x;
            r_f[idx]     = static_cast<float>(splat0[idx * 4 + 0]) / 255.0f;
            g_dil[idx]   = splat0[idx * 4 + 1];
            valid_s[idx] = valid_mask[static_cast<size_t>(my) * hmW + mx];
        }
    }
    stbi_image_free(splat0);

    // 2 passagens 3x3 Gaussiana leve sobre r_f para eliminar serrilhado de pixel antes do Marching Squares
    for (int pass = 0; pass < 2; ++pass)
    {
        for (int y = 0; y < sh; ++y)
        {
            int y0 = std::max(0, y - 1), y2 = std::min(sh - 1, y + 1);
            for (int x = 0; x < sw; ++x)
            {
                int x0 = std::max(0, x - 1), x2 = std::min(sw - 1, x + 1);
                float sum =
                    1.0f * r_f[static_cast<size_t>(y0) * sw + x0] + 2.0f * r_f[static_cast<size_t>(y0) * sw + x] + 1.0f * r_f[static_cast<size_t>(y0) * sw + x2] +
                    2.0f * r_f[static_cast<size_t>(y)  * sw + x0] + 4.0f * r_f[static_cast<size_t>(y)  * sw + x] + 2.0f * r_f[static_cast<size_t>(y)  * sw + x2] +
                    1.0f * r_f[static_cast<size_t>(y2) * sw + x0] + 2.0f * r_f[static_cast<size_t>(y2) * sw + x] + 1.0f * r_f[static_cast<size_t>(y2) * sw + x2];
                r_tmp[static_cast<size_t>(y) * sw + x] = sum / 16.0f;
            }
        }
        r_f.swap(r_tmp);
    }

    // Dilatação 5x5 sobre g (asfalto secundário) para recortar encontros, e erosão 5x5 sobre valid_s
    for (int y = 0; y < sh; ++y)
    {
        for (int x = 0; x < sw; ++x)
        {
            unsigned char gm = 0, vm = 255;
            for (int dx = -2; dx <= 2; ++dx)
            {
                int nx = std::max(0, std::min(sw - 1, x + dx));
                size_t ni = static_cast<size_t>(y) * sw + nx;
                gm = std::max(gm, g_dil[ni]);
                vm = std::min(vm, valid_s[ni]);
            }
            g_tmp[static_cast<size_t>(y) * sw + x]     = gm;
            valid_tmp[static_cast<size_t>(y) * sw + x] = vm;
        }
    }
    for (int y = 0; y < sh; ++y)
    {
        for (int x = 0; x < sw; ++x)
        {
            unsigned char gm = 0, vm = 255;
            for (int dy = -2; dy <= 2; ++dy)
            {
                int ny = std::max(0, std::min(sh - 1, y + dy));
                size_t ni = static_cast<size_t>(ny) * sw + x;
                gm = std::max(gm, g_tmp[ni]);
                vm = std::min(vm, valid_tmp[ni]);
            }
            g_dil[static_cast<size_t>(y) * sw + x]   = gm;
            valid_s[static_cast<size_t>(y) * sw + x] = vm;
        }
    }

    struct MSEdge { glm::vec2 p0, p1; uint64_t k0, k1; };
    auto qkey = [](const glm::vec2& p) -> uint64_t {
        int32_t qx = static_cast<int32_t>(std::llround(p.x * 100.0f));
        int32_t qz = static_cast<int32_t>(std::llround(p.y * 100.0f));
        return (static_cast<uint64_t>(static_cast<uint32_t>(qx)) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(qz));
    };

    const float dx_w = g_TerrainWorldWidth  / static_cast<float>(sw);
    const float dz_w = g_TerrainWorldDepth  / static_cast<float>(sh);
    const float iso  = 0.48f;

    std::vector<MSEdge> raw_edges;
    std::unordered_map<uint64_t, std::vector<int>> adj;

    static const int kMSTable[16][5] = {
        {-1,-1,-1,-1,-1}, {3,0,-1,-1,-1}, {0,1,-1,-1,-1}, {3,1,-1,-1,-1},
        {1,2,-1,-1,-1}, {3,0,1,2,-1}, {0,2,-1,-1,-1}, {3,2,-1,-1,-1},
        {2,3,-1,-1,-1}, {2,0,-1,-1,-1}, {0,1,2,3,-1}, {2,1,-1,-1,-1},
        {1,3,-1,-1,-1}, {1,0,-1,-1,-1}, {0,3,-1,-1,-1}, {-1,-1,-1,-1,-1}
    };

    for (int y = 0; y < sh - 1; ++y)
    {
        for (int x = 0; x < sw - 1; ++x)
        {
            size_t i00 = static_cast<size_t>(y) * sw + x;
            if (g_dil[i00] > 45 || valid_s[i00] < 128)
                continue;

            float v00 = r_f[i00];
            float v10 = r_f[i00 + 1];
            float v11 = r_f[i00 + sw + 1];
            float v01 = r_f[i00 + sw];

            int c = ((v00 >= iso) ? 1 : 0) |
                    ((v10 >= iso) ? 2 : 0) |
                    ((v11 >= iso) ? 4 : 0) |
                    ((v01 >= iso) ? 8 : 0);
            if (c == 0 || c == 15)
                continue;

            auto interp = [&](float ax, float ay, float bx, float by, float va, float vb) -> glm::vec2 {
                float den = vb - va;
                float t = (std::fabs(den) < 1e-6f) ? 0.5f : std::max(0.0f, std::min(1.0f, (iso - va) / den));
                return glm::vec2(
                    (ax + t * (bx - ax) + 0.5f) * dx_w - 0.5f * g_TerrainWorldWidth,
                    (ay + t * (by - ay) + 0.5f) * dz_w - 0.5f * g_TerrainWorldDepth
                );
            };

            glm::vec2 epts[4];
            if ((c & 1) != ((c >> 1) & 1))        epts[0] = interp(float(x), float(y), float(x+1), float(y), v00, v10);
            if (((c >> 1) & 1) != ((c >> 2) & 1)) epts[1] = interp(float(x+1), float(y), float(x+1), float(y+1), v10, v11);
            if (((c >> 2) & 1) != ((c >> 3) & 1)) epts[2] = interp(float(x), float(y+1), float(x+1), float(y+1), v01, v11);
            if ((c & 1) != ((c >> 3) & 1))        epts[3] = interp(float(x), float(y), float(x), float(y+1), v00, v01);

            for (int k = 0; k < 4 && kMSTable[c][k] != -1; k += 2)
            {
                glm::vec2 p0 = epts[kMSTable[c][k]];
                glm::vec2 p1 = epts[kMSTable[c][k + 1]];
                uint64_t k0 = qkey(p0), k1 = qkey(p1);
                if (k0 == k1) continue;
                int ei = static_cast<int>(raw_edges.size());
                raw_edges.push_back({ p0, p1, k0, k1 });
                adj[k0].push_back(ei);
                adj[k1].push_back(ei);
            }
        }
    }

    // Encadeia segmentos em polilinhas ordenadas (extremidades abertas primeiro, depois anéis fechados)
    struct CurbPolyline { std::vector<glm::vec2> pts; bool closed; };
    std::vector<CurbPolyline> chains;
    std::vector<unsigned char> used(raw_edges.size(), 0);

    for (int pass = 0; pass < 2; ++pass)
    {
        for (const auto& kv : adj)
        {
            if (pass == 0 && kv.second.size() != 1)
                continue;
            uint64_t start_k = kv.first;
            for (int ei : kv.second)
            {
                if (used[ei]) continue;
                used[ei] = 1;
                const MSEdge& E = raw_edges[ei];
                std::vector<glm::vec2> ch;
                uint64_t cur_k = 0;
                if (E.k0 == start_k) { ch.push_back(E.p0); ch.push_back(E.p1); cur_k = E.k1; }
                else                 { ch.push_back(E.p1); ch.push_back(E.p0); cur_k = E.k0; }

                bool closed = false;
                while (true)
                {
                    if (cur_k == start_k && ch.size() >= 3)
                    {
                        closed = true;
                        break;
                    }
                    int next_ei = -1;
                    auto it = adj.find(cur_k);
                    if (it != adj.end())
                    {
                        for (int nei : it->second)
                        {
                            if (!used[nei]) { next_ei = nei; break; }
                        }
                    }
                    if (next_ei == -1) break;
                    used[next_ei] = 1;
                    const MSEdge& NE = raw_edges[next_ei];
                    if (NE.k0 == cur_k) { ch.push_back(NE.p1); cur_k = NE.k1; }
                    else                { ch.push_back(NE.p0); cur_k = NE.k0; }
                }
                chains.push_back({ std::move(ch), closed });
            }
        }
    }

    // Suavização de Taubin + simplificação Ramer-Douglas-Peucker para deixar o meio-fio reto e limpo
    std::vector<CurbPolyline> clean_polys;
    for (CurbPolyline& cp : chains)
    {
        float perim = 0.0f;
        for (size_t i = 1; i < cp.pts.size(); ++i)
            perim += std::hypot(cp.pts[i].x - cp.pts[i-1].x, cp.pts[i].y - cp.pts[i-1].y);
        if (perim < 6.0f) continue;

        std::vector<glm::vec2> sm = (cp.closed && cp.pts.size() >= 4)
            ? std::vector<glm::vec2>(cp.pts.begin(), cp.pts.end() - 1)
            : cp.pts;
        size_t M = sm.size();
        if (M < 2) continue;

        if (M >= 4)
        {
            std::vector<glm::vec2> tmp_pts(M);
            for (int it = 0; it < 12; ++it)
            {
                for (float w : { 0.50f, -0.52f })
                {
                    for (size_t i = 0; i < M; ++i)
                    {
                        if (!cp.closed && (i == 0 || i == M - 1)) { tmp_pts[i] = sm[i]; continue; }
                        glm::vec2 prev = sm[(i + M - 1) % M];
                        glm::vec2 next = sm[(i + 1) % M];
                        glm::vec2 mid  = 0.5f * (prev + next);
                        tmp_pts[i] = sm[i] + w * (mid - sm[i]);
                    }
                    sm.swap(tmp_pts);
                }
            }
        }

        std::vector<glm::vec2> simp;
        if (cp.closed && M >= 4)
        {
            // Divide no vértice mais distante de sm[0] para simplificar ambas metades sem criar quina falsa
            size_t k_far = 0;
            float max_d2 = -1.0f;
            for (size_t i = 1; i < M; ++i)
            {
                float d2 = (sm[i].x - sm[0].x) * (sm[i].x - sm[0].x) + (sm[i].y - sm[0].y) * (sm[i].y - sm[0].y);
                if (d2 > max_d2) { max_d2 = d2; k_far = i; }
            }
            RDPSimplifyRange(sm, 0, k_far, 0.16f, simp);
            sm.push_back(sm[0]);
            RDPSimplifyRange(sm, k_far, sm.size() - 1, 0.16f, simp);
            if (simp.size() < 3) continue;
            simp.push_back(simp[0]);
        }
        else
        {
            RDPSimplifyRange(sm, 0, M - 1, 0.16f, simp);
            simp.push_back(sm.back());
            if (simp.size() < 2) continue;
        }
        clean_polys.push_back({ std::move(simp), cp.closed });
    }

    auto sample_r_world = [&](float wx, float wz) -> float {
        float sx = (wx + 0.5f * g_TerrainWorldWidth) / g_TerrainWorldWidth * static_cast<float>(sw - 1);
        float sz = (wz + 0.5f * g_TerrainWorldDepth) / g_TerrainWorldDepth * static_cast<float>(sh - 1);
        int ix = std::max(0, std::min(sw - 1, static_cast<int>(std::lround(sx))));
        int iz = std::max(0, std::min(sh - 1, static_cast<int>(std::lround(sz))));
        return r_f[static_cast<size_t>(iz) * sw + ix];
    };

    std::vector<TerrainVertex> curb_verts;
    std::vector<GLuint>        curb_indices;
    glm::vec3 bbox_min(std::numeric_limits<float>::max());
    glm::vec3 bbox_max(std::numeric_limits<float>::lowest());

    for (const CurbPolyline& cp : clean_polys)
    {
        const std::vector<glm::vec2>& pts = cp.pts;
        size_t M = pts.size(); // para fechado, pts.back() == pts.front()
        size_t num_segs = M - 1;
        if (num_segs < 1) continue;

        // Normais externas (apontam para fora do asfalto) por segmento
        std::vector<glm::vec2> seg_nout(num_segs);
        for (size_t i = 0; i < num_segs; ++i)
        {
            glm::vec2 d = pts[i + 1] - pts[i];
            float len = std::hypot(d.x, d.y);
            glm::vec2 t = (len > 1e-6f) ? (d / len) : glm::vec2(1.0f, 0.0f);
            glm::vec2 nc(-t.y, t.x);
            glm::vec2 mid = 0.5f * (pts[i] + pts[i + 1]);
            float rp = sample_r_world(mid.x + nc.x * 0.85f, mid.y + nc.y * 0.85f);
            float rn = sample_r_world(mid.x - nc.x * 0.85f, mid.y - nc.y * 0.85f);
            if (std::fabs(rp - rn) > 0.02f)
                seg_nout[i] = (rp > rn) ? -nc : nc;
            else if (i > 0)
                seg_nout[i] = (nc.x * seg_nout[i-1].x + nc.y * seg_nout[i-1].y >= 0.0f) ? nc : -nc;
            else
                seg_nout[i] = nc;
        }

        // Normais com miter nos vértices (largura física de 20 cm: -0.03m interno .. +0.17m externo)
        size_t num_u = cp.closed ? num_segs : M;
        std::vector<glm::vec2> p_in(M), p_out(M);
        for (size_t i = 0; i < M; ++i)
        {
            glm::vec2 n_avg;
            glm::vec2 n_ref;
            if (cp.closed)
            {
                size_t si = i % num_u;
                glm::vec2 n0 = seg_nout[(si + num_segs - 1) % num_segs];
                glm::vec2 n1 = seg_nout[si];
                n_avg = n0 + n1;
                n_ref = n1;
            }
            else if (i == 0)         { n_avg = seg_nout[0]; n_ref = n_avg; }
            else if (i == M - 1)     { n_avg = seg_nout[num_segs - 1]; n_ref = n_avg; }
            else                     { n_avg = seg_nout[i - 1] + seg_nout[i]; n_ref = seg_nout[i]; }

            float alen = std::hypot(n_avg.x, n_avg.y);
            glm::vec2 mdir = (alen > 1e-4f) ? (n_avg / alen) : n_ref;
            float cos_m = mdir.x * n_ref.x + mdir.y * n_ref.y;
            float mscale = 1.0f / std::max(0.55f, cos_m);
            mscale = std::min(1.6f, mscale);

            p_in[i]  = pts[i] - mdir * (0.03f * mscale);
            p_out[i] = pts[i] + mdir * (0.17f * mscale);
        }

        // Altura do topo acompanha exatamente o terreno não-rebaixado (+1.5 cm flush anti-Z-fighting)
        std::vector<float> y_top(M), y_bot_in(M), y_bot_out(M), arc_s(M, 0.0f);
        for (size_t i = 0; i < M; ++i)
        {
            float h_edge  = SampleHeightGridBilinear(unrecessed_heights, hmW, hmH, pts[i].x, pts[i].y);
            float h_outer = SampleHeightGridBilinear(unrecessed_heights, hmW, hmH, p_out[i].x, p_out[i].y);
            y_top[i] = std::max(h_edge, h_outer) + 0.015f;
            if (i > 0)
                arc_s[i] = arc_s[i - 1] + std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        }

        // Suaviza levemente Y_top ao longo da polilinha para o topo ficar reto longitudinalmente (sem degraus de 1m)
        if (num_u >= 4)
        {
            std::vector<float> y_tmp(M);
            for (int p = 0; p < 5; ++p)
            {
                for (size_t i = 0; i < num_u; ++i)
                {
                    if (!cp.closed && (i == 0 || i == num_u - 1)) { y_tmp[i] = y_top[i]; continue; }
                    float yp = y_top[(i + num_u - 1) % num_u];
                    float yn = y_top[(i + 1) % num_u];
                    y_tmp[i] = 0.25f * yp + 0.50f * y_top[i] + 0.25f * yn;
                }
                for (size_t i = 0; i < num_u; ++i) y_top[i] = y_tmp[i];
                if (cp.closed) y_top[M - 1] = y_top[0];
            }
        }

        for (size_t i = 0; i < M; ++i)
        {
            float h_asph_in = SampleHeightGridBilinear(recessed_heights, hmW, hmH, p_in[i].x, p_in[i].y);
            float h_terr_out = SampleHeightGridBilinear(recessed_heights, hmW, hmH, p_out[i].x, p_out[i].y);
            y_bot_in[i]  = std::min(y_top[i] - 0.13f, h_asph_in - 0.06f);
            y_bot_out[i] = std::min(y_top[i] - 0.06f, h_terr_out - 0.06f);
        }

        // Emite prismas retangulares de quina viva (90°) sem compartilhamento de normais entre topo e parede
        for (size_t i = 0; i < num_segs; ++i)
        {
            glm::vec3 i_top0(p_in[i].x,   y_top[i],     p_in[i].y);
            glm::vec3 i_top1(p_in[i+1].x, y_top[i+1],   p_in[i+1].y);
            glm::vec3 o_top0(p_out[i].x,  y_top[i],     p_out[i].y);
            glm::vec3 o_top1(p_out[i+1].x,y_top[i+1],   p_out[i+1].y);

            glm::vec3 i_bot0(p_in[i].x,   y_bot_in[i],  p_in[i].y);
            glm::vec3 i_bot1(p_in[i+1].x, y_bot_in[i+1],p_in[i+1].y);
            glm::vec3 o_bot0(p_out[i].x,  y_bot_out[i], p_out[i].y);
            glm::vec3 o_bot1(p_out[i+1].x,y_bot_out[i+1],p_out[i+1].y);

            glm::vec3 n_out(seg_nout[i].x, 0.0f, seg_nout[i].y);
            glm::vec3 n_in  = -n_out;

            // Normal do topo exatamente ortogonal à inclinação longitudinal da rua e à parede lateral
            glm::vec3 along = i_top1 - i_top0;
            glm::vec3 n_top(
                along.y * n_out.z - along.z * n_out.y,
                along.z * n_out.x - along.x * n_out.z,
                along.x * n_out.y - along.y * n_out.x
            );
            float tlen = std::sqrt(n_top.x*n_top.x + n_top.y*n_top.y + n_top.z*n_top.z);
            n_top = (tlen > 1e-6f) ? (n_top / tlen) : glm::vec3(0.0f, 1.0f, 0.0f);
            if (n_top.y < 0.0f) n_top = -n_top;

            float s0 = arc_s[i], s1 = arc_s[i + 1];

            // 1. Face Superior (Topo) - 90 graus com as paredes
            AddCurbQuad(curb_verts, curb_indices, bbox_min, bbox_max,
                        i_top0, {s0, 0.45f}, i_top1, {s1, 0.45f}, o_top1, {s1, 0.00f}, o_top0, {s0, 0.00f}, n_top);
            // 2. Parede Lateral Interna (voltada para o asfalto e faróis do ônibus)
            AddCurbQuad(curb_verts, curb_indices, bbox_min, bbox_max,
                        i_top0, {s0, 0.50f}, i_bot0, {s0, 1.00f}, i_bot1, {s1, 1.00f}, i_top1, {s1, 0.50f}, n_in);
            // 3. Parede Lateral Externa (voltada para calçada/grama)
            AddCurbQuad(curb_verts, curb_indices, bbox_min, bbox_max,
                        o_top0, {s0, 0.50f}, o_top1, {s1, 0.50f}, o_bot1, {s1, 1.00f}, o_bot0, {s0, 1.00f}, n_out);
        }

        // Tampas nas extremidades abertas (onde o meio-fio termina em um encontro de asfalto secundário)
        if (!cp.closed)
        {
            glm::vec2 t0 = pts[1] - pts[0];
            float l0 = std::hypot(t0.x, t0.y);
            if (l0 > 1e-5f)
            {
                glm::vec3 n_cap0(-t0.x / l0, 0.0f, -t0.y / l0);
                AddCurbQuad(curb_verts, curb_indices, bbox_min, bbox_max,
                            {p_in[0].x, y_top[0], p_in[0].y},   {0.0f, 0.50f},
                            {p_out[0].x, y_top[0], p_out[0].y}, {0.2f, 0.50f},
                            {p_out[0].x, y_bot_out[0], p_out[0].y}, {0.2f, 1.00f},
                            {p_in[0].x, y_bot_in[0], p_in[0].y},    {0.0f, 1.00f}, n_cap0);
            }
            size_t e = M - 1;
            glm::vec2 te = pts[e] - pts[e - 1];
            float le = std::hypot(te.x, te.y);
            if (le > 1e-5f)
            {
                glm::vec3 n_cape(te.x / le, 0.0f, te.y / le);
                AddCurbQuad(curb_verts, curb_indices, bbox_min, bbox_max,
                            {p_in[e].x, y_top[e], p_in[e].y},   {0.0f, 0.50f},
                            {p_in[e].x, y_bot_in[e], p_in[e].y},    {0.0f, 1.00f},
                            {p_out[e].x, y_bot_out[e], p_out[e].y}, {0.2f, 1.00f},
                            {p_out[e].x, y_top[e], p_out[e].y}, {0.2f, 0.50f}, n_cape);
            }
        }
    }

    if (curb_indices.empty())
        return;

    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, curb_verts.size() * sizeof(TerrainVertex), curb_verts.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, position));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, normal));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, uv));
    glEnableVertexAttribArray(2);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, curb_indices.size() * sizeof(GLuint), curb_indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    SceneObject curb_obj;
    curb_obj.name                   = "road_curb";
    curb_obj.first_index            = 0;
    curb_obj.num_indices            = curb_indices.size();
    curb_obj.rendering_mode         = GL_TRIANGLES;
    curb_obj.vertex_array_object_id = vao;
    curb_obj.bbox_min               = bbox_min;
    curb_obj.bbox_max               = bbox_max;
    g_VirtualScene["road_curb"] = curb_obj;

    printf("Malha 3D do meio-fio construida: %zu linhas, %zu triangulos (topo na altura do entorno, asfalto -10 cm).\n",
           clean_polys.size(), curb_indices.size() / 3);
}

void DrawRoadCurbs()
{
    if (g_VirtualScene.find("road_curb") == g_VirtualScene.end())
        return;

    glm::mat4 model = Matrix_Identity();
    glUniformMatrix4fv(g_model_uniform, 1, GL_FALSE, glm::value_ptr(model));
    glUniform1i(g_object_id_uniform, CURB);
    DrawVirtualObject("road_curb");
}

// ----------------------------------------------------------------------------
// BuildHeightmapAndAddToVirtualScene()
// Carrega o mapa topográfico do Campus do Vale (SIRGAS 2000 / UTM 22S - EPSG:31982)
// em PNG 16-bit via stbi_load_16():
//   - Escala horizontal 1:1 (1 pixel = grid_spacing = 1.0 metro), centralizado na origem;
//   - Calibração altimétrica: y = min_altitude + (pixel_uint16 / 65535.0f) * delta_h;
//   - Normais por diferenças finitas centrais: normalize(vec3(hL - hR, 2*gridSpacing, hU - hD));
//   - Descarta da malha 3D todas as células que estão em preto na máscara (`valid_mask.png`).
// ----------------------------------------------------------------------------
void BuildHeightmapAndAddToVirtualScene(
    const char* filename,
    const char* object_name,
    float       grid_spacing,
    float       min_altitude,
    float       delta_h,
    float       uv_tile,
    const char* mask_filename
)
{
    printf("Carregando heightmap 16-bit \"%s\"... ", filename);

    // Garante que não haja inversão vertical ao carregar o heightmap (X cresce p/ Leste, Z cresce p/ Sul)
    stbi_set_flip_vertically_on_load(false);
    int width = 0, height = 0, channels = 0;
    unsigned short* raw_pixels = stbi_load_16(filename, &width, &height, &channels, 1);
    bool used_fallback = false;

    if (raw_pixels == NULL)
    {
        used_fallback = true;
        // Fallback automático para data/vale.png caso vale_processed.png ainda não exista
        const char* fallback = "../../../data/vale.png";
        printf("\n  -> Aviso: \"%s\" nao encontrado, usando \"%s\"... ", filename, fallback);
        raw_pixels = stbi_load_16(fallback, &width, &height, &channels, 1);
        if (raw_pixels == NULL)
        {
            fprintf(stderr, "ERROR: Nao foi possivel abrir o heightmap 16-bit \"%s\".\n", filename);
            std::exit(EXIT_FAILURE);
        }
    }

    g_TerrainGridW      = width;
    g_TerrainGridH      = height;
    g_TerrainWorldWidth = (float)width  * grid_spacing;
    g_TerrainWorldDepth = (float)height * grid_spacing;

    size_t num_vertices = (size_t)width * (size_t)height;
    g_TerrainHeights.resize(num_vertices);

    // 1. Converte os valores UInt16 (0..65535) para altitude real em metros:
    //    y = 33.0f + (float(pixel_uint16) / 65535.0f) * 104.043f
    for (size_t i = 0; i < num_vertices; ++i)
    {
        float t = (float)raw_pixels[i] / 65535.0f;
        g_TerrainHeights[i] = min_altitude + t * delta_h;
    }

    stbi_image_free(raw_pixels);

    printf("OK (%dx%dm, altitude [%.1fm .. %.3fm]).\n",
           width, height, min_altitude, min_altitude + delta_h);

    // Suaviza o asfalto em float32 guiado por splatmap_0.png mantendo uma cópia antes do rebaixamento (-10 cm)
    std::vector<float> unrecessed_heights;
    SmoothTerrainAlongRoads(g_TerrainHeights, width, height, "../../../data/terrain/splatmap_0.png", !used_fallback, &unrecessed_heights);

    // Carrega a máscara de área válida (gerada a partir de bruto_2012_mascara.tiff)
    // Tudo o que está em preto (0) na máscara é removido da malha 3D do cenário!
    std::vector<unsigned char> valid_mask(num_vertices, 255);
    if (mask_filename != NULL)
    {
        int mw = 0, mh = 0, mc = 0;
        stbi_set_flip_vertically_on_load(false);
        unsigned char* mdata = stbi_load(mask_filename, &mw, &mh, &mc, 1);
        if (mdata != NULL)
        {
            if (mw == width && mh == height)
            {
                std::copy(mdata, mdata + num_vertices, valid_mask.begin());
            }
            else
            {
                for (int z = 0; z < height; ++z)
                {
                    int sz = std::min(mh - 1, (int)((size_t)z * (size_t)mh / (size_t)height));
                    for (int x = 0; x < width; ++x)
                    {
                        int sx = std::min(mw - 1, (int)((size_t)x * (size_t)mw / (size_t)width));
                        valid_mask[(size_t)z * (size_t)width + (size_t)x] = mdata[(size_t)sz * (size_t)mw + (size_t)sx];
                    }
                }
            }
            stbi_image_free(mdata);
            printf("Mascara de recorte do terreno \"%s\" aplicada (areas pretas removidas da malha).\n", mask_filename);
        }
    }

    // 2. Preenche o VBO único intercalado (Posição vec3, Normal vec3, UV vec2)
    std::vector<TerrainVertex> vertices(num_vertices);

    const float minval = std::numeric_limits<float>::lowest();
    const float maxval = std::numeric_limits<float>::max();
    glm::vec3 bbox_min = glm::vec3(maxval, maxval, maxval);
    glm::vec3 bbox_max = glm::vec3(minval, minval, minval);

    float half_w = 0.5f * g_TerrainWorldWidth;
    float half_h = 0.5f * g_TerrainWorldDepth;

    for (int z = 0; z < height; ++z)
    {
        float vz = (float)z * grid_spacing - half_h;
        float v  = ((float)z / (float)(height - 1)) * uv_tile;

        int zU = std::max(0, z - 1);
        int zD = std::min(height - 1, z + 1);

        for (int x = 0; x < width; ++x)
        {
            size_t idx = (size_t)z * (size_t)width + (size_t)x;

            float vx = (float)x * grid_spacing - half_w;
            float vy = g_TerrainHeights[idx];
            float u  = ((float)x / (float)(width - 1)) * uv_tile;

            // Diferenças finitas centrais usando as alturas reais já escaladas dos vizinhos
            int xL = std::max(0, x - 1);
            int xR = std::min(width - 1, x + 1);

            float hL = g_TerrainHeights[(size_t)z  * (size_t)width + (size_t)xL];
            float hR = g_TerrainHeights[(size_t)z  * (size_t)width + (size_t)xR];
            float hU = g_TerrainHeights[(size_t)zU * (size_t)width + (size_t)x];
            float hD = g_TerrainHeights[(size_t)zD * (size_t)width + (size_t)x];

            glm::vec4 n = glm::vec4(hL - hR, 2.0f * grid_spacing, hU - hD, 0.0f);
            n = n / norm(n);

            vertices[idx].position = glm::vec3(vx, vy, vz);
            vertices[idx].normal   = glm::vec3(n.x, n.y, n.z);
            vertices[idx].uv       = glm::vec2(u, v);

            if (valid_mask[idx] > 0)
            {
                bbox_min.x = std::min(bbox_min.x, vx);
                bbox_min.y = std::min(bbox_min.y, vy);
                bbox_min.z = std::min(bbox_min.z, vz);
                bbox_max.x = std::max(bbox_max.x, vx);
                bbox_max.y = std::max(bbox_max.y, vy);
                bbox_max.z = std::max(bbox_max.z, vz);
            }
        }
    }

    // 3. Gera o EBO (GL_TRIANGLES emitindo apenas os quads dentro da máscara válida)
    std::vector<GLuint> indices;
    indices.reserve((size_t)(width - 1) * (size_t)(height - 1) * 2);

    for (int z = 0; z < height - 1; ++z)
    {
        for (int x = 0; x < width - 1; ++x)
        {
            size_t i00 = (size_t)z       * (size_t)width + (size_t)x;
            size_t i10 = (size_t)z       * (size_t)width + (size_t)(x + 1);
            size_t i01 = (size_t)(z + 1) * (size_t)width + (size_t)x;
            size_t i11 = (size_t)(z + 1) * (size_t)width + (size_t)(x + 1);

            // Se todos os 4 cantos estão na área preta descartada, não emite triângulos
            if (valid_mask[i00] == 0 && valid_mask[i10] == 0 &&
                valid_mask[i01] == 0 && valid_mask[i11] == 0)
            {
                continue;
            }

            // Dois triângulos com orientação GL_CCW apontando para +Y
            indices.push_back((GLuint)i00);
            indices.push_back((GLuint)i01);
            indices.push_back((GLuint)i10);

            indices.push_back((GLuint)i10);
            indices.push_back((GLuint)i01);
            indices.push_back((GLuint)i11);
        }
    }

    printf("Malha do terreno construida: %zu triangulos ativos.\n", indices.size() / 3);

    // 4. Configura VAO, VBO único intercalado e EBO na GPU
    GLuint vertex_array_object_id;
    glGenVertexArrays(1, &vertex_array_object_id);
    glBindVertexArray(vertex_array_object_id);

    GLuint vbo_id;
    glGenBuffers(1, &vbo_id);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(TerrainVertex), vertices.data(), GL_STATIC_DRAW);

    // Atributo 0: Posição (vec3 -> expandido pela GPU para vec4 com w = 1.0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, position));
    glEnableVertexAttribArray(0);

    // Atributo 1: Normal (vec3)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, normal));
    glEnableVertexAttribArray(1);

    // Atributo 2: Coordenadas UV (vec2)
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, uv));
    glEnableVertexAttribArray(2);

    GLuint ebo_id;
    glGenBuffers(1, &ebo_id);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_id);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    SceneObject theobject;
    theobject.name                   = object_name;
    theobject.first_index            = 0;
    theobject.num_indices            = indices.size();
    theobject.rendering_mode         = GL_TRIANGLES;
    theobject.vertex_array_object_id = vertex_array_object_id;
    theobject.bbox_min               = bbox_min;
    theobject.bbox_max               = bbox_max;

    g_VirtualScene[object_name] = theobject;

    // 5. Constrói a geometria 3D real do meio-fio na borda da camada vermelha (`s0.r`), recortada por `s0.g`
    BuildRoadCurbsAndAddToVirtualScene("../../../data/terrain/splatmap_0.png", valid_mask, width, height, unrecessed_heights, g_TerrainHeights);
}

// ----------------------------------------------------------------------------
// BuildTreeForestAndAddToVirtualScene()
// Carrega o modelo 3D de árvore (`data/tree.obj`) e a lista de instâncias
// (`data/terrain/trees.txt`), pré-calculando a matriz Model de cada árvore
// exclusivamente com `Matrix_Translate`, `Matrix_Rotate_Y` e `Matrix_Scale`
// de `matrices.h`.
// Para modelos low-poly (<= 1500 vértices), consolida a floresta em dois
// batches estáticos (`trees_trunk_batch` e `trees_canopy_batch`) para
// renderizar milhares de árvores em apenas 2 draw calls!
// ----------------------------------------------------------------------------
void BuildTreeForestAndAddToVirtualScene(
    const char* obj_filename,
    const char* instances_filename
)
{
    std::ifstream inst_file(instances_filename);
    if (!inst_file.is_open())
    {
        printf("Aviso: Lista de arvores \"%s\" nao encontrada.\n", instances_filename);
        return;
    }

    ObjModel treemodel(obj_filename);
    ComputeNormals(&treemodel);
    BuildTrianglesAndAddToVirtualScene(&treemodel);

    g_TreeTrunkShapes.clear();
    g_TreeCanopyShapes.clear();
    size_t total_triangles_per_tree = 0;

    for (size_t s = 0; s < treemodel.shapes.size(); ++s)
    {
        const std::string& sname = treemodel.shapes[s].name;
        total_triangles_per_tree += treemodel.shapes[s].mesh.num_face_vertices.size();
        if (sname.find("trunk") != std::string::npos || sname.find("bark") != std::string::npos || sname.find("cau") != std::string::npos)
            g_TreeTrunkShapes.push_back(sname);
        else
            g_TreeCanopyShapes.push_back(sname);
    }

    g_TreeInstances.clear();
    std::string line;
    while (std::getline(inst_file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream iss(line);
        float wx = 0.0f, wy = 0.0f, wz = 0.0f, scale = 1.0f, yaw = 0.0f;
        if (!(iss >> wx >> wy >> wz >> scale >> yaw))
            continue;

        // Ajusta a altura Y exatamente sobre a malha nivelada do terreno
        wy = GetTerrainHeight(wx, wz);

        // Regra mandatória da disciplina: usar exclusivamente funções de matrices.h
        glm::mat4 M = Matrix_Translate(wx, wy, wz)
                    * Matrix_Rotate_Y(yaw)
                    * Matrix_Scale(scale, scale, scale);

        TreeInstanceData inst;
        inst.model = M;
        g_TreeInstances.push_back(inst);
    }

    if (g_TreeInstances.empty())
        return;

    // Se o modelo de árvore for low-poly, consolida todas as instâncias em 2 VBOs estáticos
    if (total_triangles_per_tree <= 600)
    {
        g_TreeUseBatchedMesh = true;

        std::vector<TerrainVertex> batch_vertices;
        std::vector<GLuint>        batch_indices;
        batch_vertices.reserve(g_TreeInstances.size() * total_triangles_per_tree * 3);
        batch_indices.reserve(g_TreeInstances.size() * total_triangles_per_tree * 3);

        auto append_part_batch = [&](const std::vector<std::string>& shape_names, const char* batch_obj_name)
        {
            size_t first_idx = batch_indices.size();
            glm::vec3 bbox_min(std::numeric_limits<float>::max());
            glm::vec3 bbox_max(std::numeric_limits<float>::lowest());

            for (size_t inst_i = 0; inst_i < g_TreeInstances.size(); ++inst_i)
            {
                const glm::mat4& M = g_TreeInstances[inst_i].model;

                for (size_t s = 0; s < treemodel.shapes.size(); ++s)
                {
                    const std::string& sname = treemodel.shapes[s].name;
                    bool match = false;
                    for (const std::string& target : shape_names)
                    {
                        if (sname == target) { match = true; break; }
                    }
                    if (!match)
                        continue;

                    size_t num_tri = treemodel.shapes[s].mesh.num_face_vertices.size();
                    for (size_t t = 0; t < num_tri; ++t)
                    {
                        for (size_t v = 0; v < 3; ++v)
                        {
                            tinyobj::index_t idx = treemodel.shapes[s].mesh.indices[3 * t + v];
                            glm::vec4 pos_local(
                                treemodel.attrib.vertices[3 * idx.vertex_index + 0],
                                treemodel.attrib.vertices[3 * idx.vertex_index + 1],
                                treemodel.attrib.vertices[3 * idx.vertex_index + 2],
                                1.0f
                            );
                            glm::vec4 pos_world = M * pos_local;

                            glm::vec4 norm_local(0.0f, 1.0f, 0.0f, 0.0f);
                            if (idx.normal_index != -1)
                            {
                                norm_local = glm::vec4(
                                    treemodel.attrib.normals[3 * idx.normal_index + 0],
                                    treemodel.attrib.normals[3 * idx.normal_index + 1],
                                    treemodel.attrib.normals[3 * idx.normal_index + 2],
                                    0.0f
                                );
                            }
                            glm::vec4 norm_world = M * norm_local;
                            float nl = norm(norm_world);
                            if (nl > 1e-5f)
                                norm_world /= nl;

                            glm::vec2 uv(0.0f, 0.0f);
                            if (idx.texcoord_index != -1)
                            {
                                uv.x = treemodel.attrib.texcoords[2 * idx.texcoord_index + 0];
                                uv.y = treemodel.attrib.texcoords[2 * idx.texcoord_index + 1];
                            }

                            TerrainVertex tv;
                            tv.position = glm::vec3(pos_world.x, pos_world.y, pos_world.z);
                            tv.normal   = glm::vec3(norm_world.x, norm_world.y, norm_world.z);
                            tv.uv       = uv;

                            GLuint new_v_idx = (GLuint)batch_vertices.size();
                            batch_vertices.push_back(tv);
                            batch_indices.push_back(new_v_idx);

                            bbox_min.x = std::min(bbox_min.x, tv.position.x);
                            bbox_min.y = std::min(bbox_min.y, tv.position.y);
                            bbox_min.z = std::min(bbox_min.z, tv.position.z);
                            bbox_max.x = std::max(bbox_max.x, tv.position.x);
                            bbox_max.y = std::max(bbox_max.y, tv.position.y);
                            bbox_max.z = std::max(bbox_max.z, tv.position.z);
                        }
                    }
                }
            }

            SceneObject batch_obj;
            batch_obj.name           = batch_obj_name;
            batch_obj.first_index    = first_idx;
            batch_obj.num_indices    = batch_indices.size() - first_idx;
            batch_obj.rendering_mode = GL_TRIANGLES;
            batch_obj.bbox_min       = bbox_min;
            batch_obj.bbox_max       = bbox_max;
            g_VirtualScene[batch_obj_name] = batch_obj;
        };

        append_part_batch(g_TreeTrunkShapes,  "trees_trunk_batch");
        append_part_batch(g_TreeCanopyShapes, "trees_canopy_batch");

        GLuint vao_id, vbo_id, ebo_id;
        glGenVertexArrays(1, &vao_id);
        glBindVertexArray(vao_id);

        glGenBuffers(1, &vbo_id);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_id);
        glBufferData(GL_ARRAY_BUFFER, batch_vertices.size() * sizeof(TerrainVertex), batch_vertices.data(), GL_STATIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, position));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, normal));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(TerrainVertex), (void*)offsetof(TerrainVertex, uv));
        glEnableVertexAttribArray(2);

        glGenBuffers(1, &ebo_id);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_id);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, batch_indices.size() * sizeof(GLuint), batch_indices.data(), GL_STATIC_DRAW);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        g_VirtualScene["trees_trunk_batch"].vertex_array_object_id  = vao_id;
        g_VirtualScene["trees_canopy_batch"].vertex_array_object_id = vao_id;
    }
    else
    {
        g_TreeUseBatchedMesh = false;
    }

    g_TreeLoaded = true;
    printf("Floresta 3D carregada: %zu arvores posicionadas no terreno.\n", g_TreeInstances.size());
}

void DrawTreeForest()
{
    if (!g_TreeLoaded)
        return;

    if (g_TreeUseBatchedMesh)
    {
        glm::mat4 identity = Matrix_Identity();
        glUniformMatrix4fv(g_model_uniform, 1, GL_FALSE, glm::value_ptr(identity));

        glUniform1i(g_object_id_uniform, TREE_TRUNK);
        DrawVirtualObject("trees_trunk_batch");

        glUniform1i(g_object_id_uniform, TREE_CANOPY);
        DrawVirtualObject("trees_canopy_batch");
    }
    else
    {
        for (size_t i = 0; i < g_TreeInstances.size(); ++i)
        {
            glUniformMatrix4fv(g_model_uniform, 1, GL_FALSE, glm::value_ptr(g_TreeInstances[i].model));

            glUniform1i(g_object_id_uniform, TREE_TRUNK);
            for (const std::string& sname : g_TreeTrunkShapes)
                DrawVirtualObject(sname.c_str());

            glUniform1i(g_object_id_uniform, TREE_CANOPY);
            for (const std::string& sname : g_TreeCanopyShapes)
                DrawVirtualObject(sname.c_str());
        }
    }
}

static std::vector<unsigned char> ResampleRGBNearest(const unsigned char* src, int src_w, int src_h, int dst_w, int dst_h)
{
    std::vector<unsigned char> dst((size_t)dst_w * (size_t)dst_h * 3);
    for (int y = 0; y < dst_h; ++y)
    {
        int src_y = std::min(src_h - 1, (int)((size_t)y * (size_t)src_h / (size_t)dst_h));
        for (int x = 0; x < dst_w; ++x)
        {
            int src_x = std::min(src_w - 1, (int)((size_t)x * (size_t)src_w / (size_t)dst_w));
            size_t dst_idx = ((size_t)y * (size_t)dst_w + (size_t)x) * 3;
            size_t src_idx = ((size_t)src_y * (size_t)src_w + (size_t)src_x) * 3;
            dst[dst_idx + 0] = src[src_idx + 0];
            dst[dst_idx + 1] = src[src_idx + 1];
            dst[dst_idx + 2] = src[src_idx + 2];
        }
    }
    return dst;
}

bool LoadTextureImage(const char* filename, bool flip_vertically, GLint wrap_mode, bool optional)
{
    printf("Carregando imagem \"%s\"... ", filename);

    stbi_set_flip_vertically_on_load(flip_vertically);
    int width;
    int height;
    int channels;
    unsigned char *data = stbi_load(filename, &width, &height, &channels, 3);

    if (data == NULL)
    {
        if (optional)
        {
            printf("NAO ENCONTRADA (opcional).\n");
            return false;
        }
        fprintf(stderr, "ERROR: Cannot open image file \"%s\".\n", filename);
        std::exit(EXIT_FAILURE);
    }

    printf("OK (%dx%d).\n", width, height);

    GLint max_tex_size = 8192;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);

    std::vector<unsigned char> resized_buffer;
    const unsigned char* upload_data = data;

    if (width > max_tex_size || height > max_tex_size)
    {
        float scale = (float)max_tex_size / (float)std::max(width, height);
        int new_w   = std::max(1, (int)std::floor((float)width  * scale));
        int new_h   = std::max(1, (int)std::floor((float)height * scale));

        printf("  -> Redimensionando textura gigante (%dx%d) para caber no limite da GPU (%dx%d)...\n",
               width, height, new_w, new_h);

        resized_buffer = ResampleRGBNearest(data, width, height, new_w, new_h);
        width       = new_w;
        height      = new_h;
        upload_data = resized_buffer.data();
    }

    GLuint texture_id;
    GLuint sampler_id;
    glGenTextures(1, &texture_id);
    glGenSamplers(1, &sampler_id);

    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_S, wrap_mode);
    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_T, wrap_mode);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    GLuint textureunit = g_NumLoadedTextures;
    glActiveTexture(GL_TEXTURE0 + textureunit);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, upload_data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindSampler(textureunit, sampler_id);

    stbi_image_free(data);

    g_NumLoadedTextures += 1;
    return true;
}

// ----------------------------------------------------------------------------
// LoadTextureImageRGBA()
// Carrega mapas de pesos RGBA lineares (como splatmap_0.png e splatmap_1.png)
// preservando os 4 canais (R, G, B, A) em formato GL_RGBA8 sem distorção sRGB.
// ----------------------------------------------------------------------------
bool LoadTextureImageRGBA(const char* filename, bool flip_vertically, GLint wrap_mode, bool optional)
{
    printf("Carregando mapa RGBA \"%s\"... ", filename);

    stbi_set_flip_vertically_on_load(flip_vertically);
    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, 4);

    if (data == NULL)
    {
        if (optional)
        {
            printf("NAO ENCONTRADO (opcional).\n");
            return false;
        }
        fprintf(stderr, "ERROR: Cannot open RGBA file \"%s\".\n", filename);
        std::exit(EXIT_FAILURE);
    }

    printf("OK (%dx%d, 4 canais RGBA).\n", width, height);

    GLuint texture_id;
    GLuint sampler_id;
    glGenTextures(1, &texture_id);
    glGenSamplers(1, &sampler_id);

    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_S, wrap_mode);
    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_T, wrap_mode);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    GLuint textureunit = g_NumLoadedTextures;
    glActiveTexture(GL_TEXTURE0 + textureunit);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindSampler(textureunit, sampler_id);

    stbi_image_free(data);

    g_NumLoadedTextures += 1;
    return true;
}

// ----------------------------------------------------------------------------
// LoadTextureArray()
// Carrega N imagens RGB (sRGB) como camadas de um único GL_TEXTURE_2D_ARRAY,
// garantindo o mesmo tamanho de camada via reamostragem quando necessário.
// ----------------------------------------------------------------------------
static bool LoadTextureArray(const std::vector<std::string>& filenames, bool flip_vertically, GLint wrap_mode, bool optional)
{
    if (filenames.empty())
        return false;

    GLint max_tex_size = 8192;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex_size);

    stbi_set_flip_vertically_on_load(flip_vertically);

    int layer_w = 0;
    int layer_h = 0;
    struct LayerPixels
    {
        int w;
        int h;
        unsigned char* raw;
        LayerPixels(int w_, int h_, unsigned char* raw_) : w(w_), h(h_), raw(raw_) {}
    };
    std::vector<LayerPixels> loaded;
    loaded.reserve(filenames.size());

    auto free_loaded = [&]() {
        for (auto& lp : loaded)
        {
            if (lp.raw)
                stbi_image_free(lp.raw);
        }
    };

    for (size_t i = 0; i < filenames.size(); ++i)
    {
        printf("Carregando camada[%zu] de Texture2DArray \"%s\"... ", i, filenames[i].c_str());
        int w = 0, h = 0, channels = 0;
        unsigned char* data = stbi_load(filenames[i].c_str(), &w, &h, &channels, 3);
        if (data == NULL)
        {
            if (optional)
            {
                printf("NAO ENCONTRADA (opcional).\n");
                free_loaded();
                return false;
            }
            fprintf(stderr, "ERROR: Cannot open texture array layer \"%s\".\n", filenames[i].c_str());
            free_loaded();
            std::exit(EXIT_FAILURE);
        }
        printf("OK (%dx%d).\n", w, h);

        if (i == 0)
        {
            layer_w = std::min(w, (int)max_tex_size);
            layer_h = std::min(h, (int)max_tex_size);
        }
        loaded.push_back({w, h, data});
    }

    GLuint texture_id = 0;
    GLuint sampler_id = 0;
    glGenTextures(1, &texture_id);
    glGenSamplers(1, &sampler_id);

    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_S, wrap_mode);
    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_T, wrap_mode);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    GLuint textureunit = g_NumLoadedTextures;
    glActiveTexture(GL_TEXTURE0 + textureunit);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture_id);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8, layer_w, layer_h, (GLsizei)loaded.size(), 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);

    std::vector<unsigned char> resized;
    for (size_t i = 0; i < loaded.size(); ++i)
    {
        const unsigned char* src_ptr = loaded[i].raw;
        if (loaded[i].w != layer_w || loaded[i].h != layer_h)
        {
            resized = ResampleRGBNearest(loaded[i].raw, loaded[i].w, loaded[i].h, layer_w, layer_h);
            src_ptr = resized.data();
        }
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, (GLint)i, layer_w, layer_h, 1, GL_RGB, GL_UNSIGNED_BYTE, src_ptr);
    }

    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindSampler(textureunit, sampler_id);

    free_loaded();
    g_NumLoadedTextures += 1;
    return true;
}

// ----------------------------------------------------------------------------
// LoadTerrainTextures()
// Carrega os Splatmaps RGBA (3-4), Macro Albedo (5), o Texture2DArray dos
// 8 materiais placeholder do terreno (6) e texturas de árvore (7-8).
// ----------------------------------------------------------------------------
void LoadTerrainTextures()
{
    bool ok0 = LoadTextureImageRGBA("../../../data/terrain/splatmap_0.png", false, GL_CLAMP_TO_EDGE, true); // Unit 3
    bool ok1 = LoadTextureImageRGBA("../../../data/terrain/splatmap_1.png", false, GL_CLAMP_TO_EDGE, true); // Unit 4
    bool ok2 = LoadTextureImage("../../../data/terrain/macro_albedo.png",   false, GL_CLAMP_TO_EDGE, true); // Unit 5

    std::vector<std::string> material_layers = {
        "../../../data/terrain/mat_0_asphalt_main.png", // Layer 0: Asfalto Principal
        "../../../data/terrain/mat_1_asphalt_sec.png",  // Layer 1: Asfalto Secundário / Concreto
        "../../../data/terrain/mat_2_sidewalk.png",     // Layer 2: Calçadas / Meio-fio
        "../../../data/terrain/mat_3_grass.png",        // Layer 3: Grama Aberta
        "../../../data/terrain/mat_4_forest.png",       // Layer 4: Solo de Mata
        "../../../data/terrain/mat_5_dirt.png",         // Layer 5: Terra Exposta / Taludes
        "../../../data/terrain/mat_6_water.png",        // Layer 6: Água (textura estática placeholder)
        "../../../data/terrain/mat_7_building.png"      // Layer 7: Fundação / Base de Prédios
    };
    bool ok3 = LoadTextureArray(material_layers, true, GL_REPEAT, true); // Unit 6

    bool ok4 = LoadTextureImage("../../../data/terrain/tree_bark.png",    true, GL_REPEAT, true); // Unit 7
    bool ok5 = LoadTextureImage("../../../data/terrain/tree_foliage.png", true, GL_REPEAT, true); // Unit 8

    g_HasTerrainSplatmaps = (ok0 && ok1 && ok2 && ok3 && ok4 && ok5);

    glUseProgram(g_GpuProgramID);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "has_terrain_splatmaps"), g_HasTerrainSplatmaps ? 1 : 0);
    glUseProgram(0);
}

void LoadShader(const char* filename, GLuint shader_id)
{
    std::ifstream file;
    try {
        file.exceptions(std::ifstream::failbit);
        file.open(filename);
    } catch (std::exception& e) {
        fprintf(stderr, "ERROR: Cannot open file \"%s\".\n", filename);
        std::exit(EXIT_FAILURE);
    }
    std::stringstream shader;
    shader << file.rdbuf();
    std::string str = shader.str();
    const GLchar* shader_string = str.c_str();
    const GLint   shader_string_length = static_cast<GLint>(str.length());

    glShaderSource(shader_id, 1, &shader_string, &shader_string_length);
    glCompileShader(shader_id);

    GLint compiled_ok;
    glGetShaderiv(shader_id, GL_COMPILE_STATUS, &compiled_ok);

    GLint log_length = 0;
    glGetShaderiv(shader_id, GL_INFO_LOG_LENGTH, &log_length);

    GLchar* log = new GLchar[log_length];
    glGetShaderInfoLog(shader_id, log_length, &log_length, log);

    if (log_length != 0)
    {
        std::string output;
        if (!compiled_ok)
        {
            output += "ERROR: OpenGL compilation of \"";
            output += filename;
            output += "\" failed.\n== Start of compilation log\n";
            output += log;
            output += "== End of compilation log\n";
        }
        else
        {
            output += "WARNING: OpenGL compilation of \"";
            output += filename;
            output += "\".\n== Start of compilation log\n";
            output += log;
            output += "== End of compilation log\n";
        }
        fprintf(stderr, "%s", output.c_str());
    }

    delete[] log;
}

GLuint LoadShader_Vertex(const char* filename)
{
    GLuint vertex_shader_id = glCreateShader(GL_VERTEX_SHADER);
    LoadShader(filename, vertex_shader_id);
    return vertex_shader_id;
}

GLuint LoadShader_Fragment(const char* filename)
{
    GLuint fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
    LoadShader(filename, fragment_shader_id);
    return fragment_shader_id;
}

GLuint CreateGpuProgram(GLuint vertex_shader_id, GLuint fragment_shader_id)
{
    GLuint program_id = glCreateProgram();

    glAttachShader(program_id, vertex_shader_id);
    glAttachShader(program_id, fragment_shader_id);

    glLinkProgram(program_id);

    GLint linked_ok = GL_FALSE;
    glGetProgramiv(program_id, GL_LINK_STATUS, &linked_ok);

    if (linked_ok == GL_FALSE)
    {
        GLint log_length = 0;
        glGetProgramiv(program_id, GL_INFO_LOG_LENGTH, &log_length);

        GLchar* log = new GLchar[log_length];
        glGetProgramInfoLog(program_id, log_length, &log_length, log);

        std::string output;
        output += "ERROR: OpenGL linking of program failed.\n== Start of link log\n";
        output += log;
        output += "\n== End of link log\n";

        delete[] log;
        fprintf(stderr, "%s", output.c_str());
    }

    glDeleteShader(vertex_shader_id);
    glDeleteShader(fragment_shader_id);

    return program_id;
}

void LoadShadersFromFiles()
{
    GLuint vertex_shader_id = LoadShader_Vertex("../../src/shader_vertex.glsl");
    GLuint fragment_shader_id = LoadShader_Fragment("../../src/shader_fragment.glsl");

    if (g_GpuProgramID != 0)
        glDeleteProgram(g_GpuProgramID);

    g_GpuProgramID = CreateGpuProgram(vertex_shader_id, fragment_shader_id);

    g_model_uniform      = glGetUniformLocation(g_GpuProgramID, "model");
    g_view_uniform       = glGetUniformLocation(g_GpuProgramID, "view");
    g_projection_uniform = glGetUniformLocation(g_GpuProgramID, "projection");
    g_object_id_uniform  = glGetUniformLocation(g_GpuProgramID, "object_id");
    g_bbox_min_uniform   = glGetUniformLocation(g_GpuProgramID, "bbox_min");
    g_bbox_max_uniform   = glGetUniformLocation(g_GpuProgramID, "bbox_max");

    glUseProgram(g_GpuProgramID);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TextureImage0"),      0);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TextureImage1"),      1);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TextureImage2"),      2);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TerrainSplat0"),      3);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TerrainSplat1"),      4);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TerrainMacroAlbedo"), 5);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TerrainMaterials"),   6);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TreeBarkTex"),        7);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TreeFoliageTex"),     8);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "has_satellite_texture"), (g_HasSatelliteTexture && g_SatelliteTerrainMode) ? 1 : 0);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "has_terrain_splatmaps"), g_HasTerrainSplatmaps ? 1 : 0);
    glUseProgram(0);
}
