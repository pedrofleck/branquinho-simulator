// ============================================================================
// scene.cpp - Cena virtual e infraestrutura de GPU
//
// Responsável por:
//   - Definir as variáveis globais da cena virtual (g_VirtualScene), da pilha
//     de matrizes (g_MatrixStack) e dos identificadores de programa/uniforms
//     da GPU;
//   - Carregar modelos .obj (ObjModel, via tinyobjloader), calcular normais
//     quando ausentes e enviar a geometria para a GPU (VAO/VBOs), registrando
//     cada shape como um SceneObject desenhável por nome (DrawVirtualObject);
//   - Carregar texturas a partir de imagens (via stb_image);
//   - Ler, compilar e linkar os shaders GLSL (vertex/fragment) e obter as
//     localizações das variáveis uniform utilizadas na renderização.
// ============================================================================

#include "scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <set>
#include <limits>
#include <algorithm>

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

GLuint g_GpuProgramID = 0;
GLint  g_model_uniform;
GLint  g_view_uniform;
GLint  g_projection_uniform;
GLint  g_object_id_uniform;
GLint  g_bbox_min_uniform;
GLint  g_bbox_max_uniform;
GLuint g_NumLoadedTextures = 0;

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
    glBindVertexArray(g_VirtualScene[object_name].vertex_array_object_id);

    glm::vec3 bbox_min = g_VirtualScene[object_name].bbox_min;
    glm::vec3 bbox_max = g_VirtualScene[object_name].bbox_max;
    glUniform4f(g_bbox_min_uniform, bbox_min.x, bbox_min.y, bbox_min.z, 1.0f);
    glUniform4f(g_bbox_max_uniform, bbox_max.x, bbox_max.y, bbox_max.z, 1.0f);

    glDrawElements(
        g_VirtualScene[object_name].rendering_mode,
        g_VirtualScene[object_name].num_indices,
        GL_UNSIGNED_INT,
        (void*)(g_VirtualScene[object_name].first_index * sizeof(GLuint))
    );

    glBindVertexArray(0);
}

// ----------------------------------------------------------------------------
// GetTerrainHeight()
// Retorna a altura Y do terreno nas coordenadas (world_x, world_z) do mundo
// usando interpolação bilinear sobre a grade gerada pelo heightmap 16-bit.
// ----------------------------------------------------------------------------
float GetTerrainHeight(float world_x, float world_z)
{
    if (g_TerrainHeights.empty() || g_TerrainGridW < 2 || g_TerrainGridH < 2)
        return 0.0f;

    float u = (world_x + 0.5f * g_TerrainWorldWidth) / g_TerrainWorldWidth;
    float v = (world_z + 0.5f * g_TerrainWorldDepth) / g_TerrainWorldDepth;

    u = std::max(0.0f, std::min(1.0f, u));
    v = std::max(0.0f, std::min(1.0f, v));

    float fx = u * (float)(g_TerrainGridW - 1);
    float fz = v * (float)(g_TerrainGridH - 1);

    int x0 = (int)std::floor(fx);
    int z0 = (int)std::floor(fz);
    int x1 = std::min(x0 + 1, g_TerrainGridW - 1);
    int z1 = std::min(z0 + 1, g_TerrainGridH - 1);

    float tx = fx - (float)x0;
    float tz = fz - (float)z0;

    float h00 = g_TerrainHeights[z0 * g_TerrainGridW + x0];
    float h10 = g_TerrainHeights[z0 * g_TerrainGridW + x1];
    float h01 = g_TerrainHeights[z1 * g_TerrainGridW + x0];
    float h11 = g_TerrainHeights[z1 * g_TerrainGridW + x1];

    float h0 = h00 * (1.0f - tx) + h10 * tx;
    float h1 = h01 * (1.0f - tx) + h11 * tx;

    return h0 * (1.0f - tz) + h1 * tz;
}

// ----------------------------------------------------------------------------
// BuildHeightmapAndAddToVirtualScene()
// Carrega um mapa topográfico em PNG de 16 bits via stbi_load_16(), gera uma
// malha indexada com normais analíticas suaves e registra o objeto em g_VirtualScene.
// ----------------------------------------------------------------------------
void BuildHeightmapAndAddToVirtualScene(
    const char* filename,
    const char* object_name,
    float world_width,
    float max_height,
    int   subsample,
    float uv_tile
)
{
    printf("Carregando mapa topografico 16-bit \"%s\"... ", filename);

    stbi_set_flip_vertically_on_load(false);
    int img_w = 0, img_h = 0, channels = 0;
    unsigned short* raw_pixels = stbi_load_16(filename, &img_w, &img_h, &channels, 1);

    if (raw_pixels == NULL)
    {
        fprintf(stderr, "ERROR: Nao foi possivel abrir o heightmap \"%s\".\n", filename);
        std::exit(EXIT_FAILURE);
    }

    subsample = std::max(1, subsample);
    g_TerrainGridW      = (img_w - 1) / subsample + 1;
    g_TerrainGridH      = (img_h - 1) / subsample + 1;
    g_TerrainWorldWidth = world_width;
    g_TerrainWorldDepth = world_width * ((float)img_h / (float)img_w);

    // Encontra o intervalo real [min_raw, max_raw] de 16 bits na imagem para aproveitar toda a faixa dinâmica
    unsigned short min_raw = 65535;
    unsigned short max_raw = 0;
    size_t total_pixels = (size_t)img_w * (size_t)img_h;
    for (size_t i = 0; i < total_pixels; ++i)
    {
        unsigned short v = raw_pixels[i];
        if (v < min_raw) min_raw = v;
        if (v > max_raw) max_raw = v;
    }
    float raw_range = (max_raw > min_raw) ? (float)(max_raw - min_raw) : 1.0f;

    printf("OK (%dx%d pixels -> malha %dx%d, 16-bit [%u..%u]).\n",
           img_w, img_h, g_TerrainGridW, g_TerrainGridH, (unsigned)min_raw, (unsigned)max_raw);

    // Amostra a imagem 16-bit com filtro de caixa (box filter) de raio subsample para suavizar o relevo
    g_TerrainHeights.resize((size_t)g_TerrainGridW * (size_t)g_TerrainGridH);
    int filter_radius = std::max(1, subsample / 2);

    for (int gz = 0; gz < g_TerrainGridH; ++gz)
    {
        int center_pz = std::min(gz * subsample, img_h - 1);
        for (int gx = 0; gx < g_TerrainGridW; ++gx)
        {
            int center_px = std::min(gx * subsample, img_w - 1);

            double sum   = 0.0;
            int    count = 0;
            for (int dz = -filter_radius; dz <= filter_radius; ++dz)
            {
                int pz = std::max(0, std::min(img_h - 1, center_pz + dz));
                for (int dx = -filter_radius; dx <= filter_radius; ++dx)
                {
                    int px = std::max(0, std::min(img_w - 1, center_px + dx));
                    sum += (double)raw_pixels[(size_t)pz * (size_t)img_w + (size_t)px];
                    count++;
                }
            }

            float avg_raw    = (float)(sum / (double)count);
            float normalized = (avg_raw - (float)min_raw) / raw_range;
            g_TerrainHeights[(size_t)gz * (size_t)g_TerrainGridW + (size_t)gx] = normalized * max_height;
        }
    }

    stbi_image_free(raw_pixels);

    // Centraliza verticalmente a origem (0, 0) para que o ponto inicial do ônibus fique em Y = 0.0
    float origin_height = GetTerrainHeight(0.0f, 0.0f);
    for (size_t i = 0; i < g_TerrainHeights.size(); ++i)
    {
        g_TerrainHeights[i] -= origin_height;
    }

    // Constrói os buffers de vértices, normais analíticas, coordenadas UV e índices da malha
    size_t num_vertices = (size_t)g_TerrainGridW * (size_t)g_TerrainGridH;
    std::vector<float>  model_coefficients;
    std::vector<float>  normal_coefficients;
    std::vector<float>  texture_coefficients;
    std::vector<GLuint> indices;

    model_coefficients.reserve(num_vertices * 4);
    normal_coefficients.reserve(num_vertices * 4);
    texture_coefficients.reserve(num_vertices * 2);
    indices.reserve((size_t)(g_TerrainGridW - 1) * (size_t)(g_TerrainGridH - 1) * 6);

    const float minval = std::numeric_limits<float>::lowest();
    const float maxval = std::numeric_limits<float>::max();
    glm::vec3 bbox_min = glm::vec3(maxval, maxval, maxval);
    glm::vec3 bbox_max = glm::vec3(minval, minval, minval);

    float dx = g_TerrainWorldWidth / (float)(g_TerrainGridW - 1);
    float dz = g_TerrainWorldDepth / (float)(g_TerrainGridH - 1);

    for (int gz = 0; gz < g_TerrainGridH; ++gz)
    {
        float u_z = (float)gz / (float)(g_TerrainGridH - 1);
        float vz  = (u_z - 0.5f) * g_TerrainWorldDepth;

        for (int gx = 0; gx < g_TerrainGridW; ++gx)
        {
            float u_x = (float)gx / (float)(g_TerrainGridW - 1);
            float vx  = (u_x - 0.5f) * g_TerrainWorldWidth;
            float vy  = g_TerrainHeights[(size_t)gz * (size_t)g_TerrainGridW + (size_t)gx];

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

            // Cálculo da normal suave por diferenças centrais e produto vetorial de matrices.h
            int gx_prev = std::max(0, gx - 1);
            int gx_next = std::min(g_TerrainGridW - 1, gx + 1);
            int gz_prev = std::max(0, gz - 1);
            int gz_next = std::min(g_TerrainGridH - 1, gz + 1);

            float hL = g_TerrainHeights[(size_t)gz * (size_t)g_TerrainGridW + (size_t)gx_prev];
            float hR = g_TerrainHeights[(size_t)gz * (size_t)g_TerrainGridW + (size_t)gx_next];
            float hU = g_TerrainHeights[(size_t)gz_prev * (size_t)g_TerrainGridW + (size_t)gx];
            float hD = g_TerrainHeights[(size_t)gz_next * (size_t)g_TerrainGridW + (size_t)gx];

            glm::vec4 tangent_x = glm::vec4((float)(gx_next - gx_prev) * dx, hR - hL, 0.0f, 0.0f);
            glm::vec4 tangent_z = glm::vec4(0.0f, hD - hU, (float)(gz_next - gz_prev) * dz, 0.0f);
            glm::vec4 n         = crossproduct(tangent_z, tangent_x);
            n = n / norm(n);

            normal_coefficients.push_back(n.x);
            normal_coefficients.push_back(n.y);
            normal_coefficients.push_back(n.z);
            normal_coefficients.push_back(0.0f);

            texture_coefficients.push_back(u_x * uv_tile);
            texture_coefficients.push_back(u_z * uv_tile);
        }
    }

    // Gera os 2 triângulos (sentido anti-horário GL_CCW visto de cima +Y) para cada célula da grade
    for (int gz = 0; gz < g_TerrainGridH - 1; ++gz)
    {
        for (int gx = 0; gx < g_TerrainGridW - 1; ++gx)
        {
            GLuint v00 = (GLuint)(gz * g_TerrainGridW + gx);
            GLuint v10 = (GLuint)(gz * g_TerrainGridW + (gx + 1));
            GLuint v01 = (GLuint)((gz + 1) * g_TerrainGridW + gx);
            GLuint v11 = (GLuint)((gz + 1) * g_TerrainGridW + (gx + 1));

            indices.push_back(v00);
            indices.push_back(v01);
            indices.push_back(v10);

            indices.push_back(v10);
            indices.push_back(v01);
            indices.push_back(v11);
        }
    }

    GLuint vertex_array_object_id;
    glGenVertexArrays(1, &vertex_array_object_id);
    glBindVertexArray(vertex_array_object_id);

    GLuint VBO_model_coefficients_id;
    glGenBuffers(1, &VBO_model_coefficients_id);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_model_coefficients_id);
    glBufferData(GL_ARRAY_BUFFER, model_coefficients.size() * sizeof(float), model_coefficients.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(0);

    GLuint VBO_normal_coefficients_id;
    glGenBuffers(1, &VBO_normal_coefficients_id);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_normal_coefficients_id);
    glBufferData(GL_ARRAY_BUFFER, normal_coefficients.size() * sizeof(float), normal_coefficients.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(1);

    GLuint VBO_texture_coefficients_id;
    glGenBuffers(1, &VBO_texture_coefficients_id);
    glBindBuffer(GL_ARRAY_BUFFER, VBO_texture_coefficients_id);
    glBufferData(GL_ARRAY_BUFFER, texture_coefficients.size() * sizeof(float), texture_coefficients.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glEnableVertexAttribArray(2);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLuint indices_id;
    glGenBuffers(1, &indices_id);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_id);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLuint), indices.data(), GL_STATIC_DRAW);

    glBindVertexArray(0);

    SceneObject theobject;
    theobject.name                   = object_name;
    theobject.first_index            = 0;
    theobject.num_indices            = indices.size();
    theobject.rendering_mode         = GL_TRIANGLES;
    theobject.vertex_array_object_id = vertex_array_object_id;
    theobject.bbox_min               = bbox_min;
    theobject.bbox_max               = bbox_max;

    g_VirtualScene[object_name] = theobject;
}

void LoadTextureImage(const char* filename)
{
    printf("Carregando imagem \"%s\"... ", filename);

    stbi_set_flip_vertically_on_load(true);
    int width;
    int height;
    int channels;
    unsigned char *data = stbi_load(filename, &width, &height, &channels, 3);

    if (data == NULL)
    {
        fprintf(stderr, "ERROR: Cannot open image file \"%s\".\n", filename);
        std::exit(EXIT_FAILURE);
    }

    printf("OK (%dx%d).\n", width, height);

    GLuint texture_id;
    GLuint sampler_id;
    glGenTextures(1, &texture_id);
    glGenSamplers(1, &sampler_id);

    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(sampler_id, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glSamplerParameteri(sampler_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);

    GLuint textureunit = g_NumLoadedTextures;
    glActiveTexture(GL_TEXTURE0 + textureunit);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindSampler(textureunit, sampler_id);

    stbi_image_free(data);

    g_NumLoadedTextures += 1;
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
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TextureImage0"), 0);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TextureImage1"), 1);
    glUniform1i(glGetUniformLocation(g_GpuProgramID, "TextureImage2"), 2);
    glUseProgram(0);
}
