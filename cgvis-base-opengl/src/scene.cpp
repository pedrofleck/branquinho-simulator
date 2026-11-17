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

    if (raw_pixels == NULL)
    {
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
