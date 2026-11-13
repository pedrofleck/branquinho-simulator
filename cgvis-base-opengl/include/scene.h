#ifndef _SCENE_H
#define _SCENE_H

#include <string>
#include <vector>
#include <map>
#include <stack>

#include <glad/glad.h>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <tiny_obj_loader.h>

// Identificadores de objetos passados ao Fragment Shader
#define SPHERE         0
#define BUNNY          1
#define PLANE          2
#define BUS            3
#define LIGHTPOLE      4
#define LIGHTBULB      5
#define HEADLIGHT_LENS 6
#define TREE_TRUNK     7
#define TREE_CANOPY    8

// Estrutura que representa um modelo geométrico carregado a partir de um arquivo .obj
struct ObjModel
{
    tinyobj::attrib_t                 attrib;
    std::vector<tinyobj::shape_t>     shapes;
    std::vector<tinyobj::material_t>  materials;

    ObjModel(const char* filename, const char* basepath = NULL, bool triangulate = true);
};

// Estrutura com dados necessários para renderizar cada parte/objeto da cena virtual
struct SceneObject
{
    std::string  name;
    size_t       first_index;
    size_t       num_indices;
    GLenum       rendering_mode;
    GLuint       vertex_array_object_id;
    glm::vec3    bbox_min;
    glm::vec3    bbox_max;
};

// Dicionário com os objetos da cena virtual
extern std::map<std::string, SceneObject> g_VirtualScene;

// Pilha de matrizes de modelagem
extern std::stack<glm::mat4> g_MatrixStack;

// Funções da pilha de modelagem
void PushMatrix(glm::mat4 M);
void PopMatrix(glm::mat4& M);

// Funções de manipulação de malha, terreno (heightmap 16-bit em escala real 1px = 1m) e cena
void BuildTrianglesAndAddToVirtualScene(ObjModel* model);
void ComputeNormals(ObjModel* model);
void BuildHeightmapAndAddToVirtualScene(
    const char* filename,
    const char* object_name  = "terrain",
    float       grid_spacing = 1.0f,
    float       min_altitude = 33.0f,
    float       delta_h      = 104.043f,
    float       uv_tile      = 1.0f,
    const char* mask_filename = "../../../data/terrain/valid_mask.png"
);
float GetTerrainHeight(float world_x, float world_z);
void DrawVirtualObject(const char* object_name);

// Carregamento e renderização das árvores 3D instanciadas no terreno
void BuildTreeForestAndAddToVirtualScene(
    const char* obj_filename       = "../../../data/tree.obj",
    const char* instances_filename = "../../../data/terrain/trees.txt"
);
void DrawTreeForest();

// Funções de shaders e GPU
void LoadShadersFromFiles();
GLuint LoadShader_Vertex(const char* filename);
GLuint LoadShader_Fragment(const char* filename);
void LoadShader(const char* filename, GLuint shader_id);
GLuint CreateGpuProgram(GLuint vertex_shader_id, GLuint fragment_shader_id);

// Funções de texturas (RGB sRGB e RGBA Linear para Splatmaps)
bool LoadTextureImage(const char* filename, bool flip_vertically = true, GLint wrap_mode = GL_REPEAT, bool optional = false);
bool LoadTextureImageRGBA(const char* filename, bool flip_vertically = false, GLint wrap_mode = GL_CLAMP_TO_EDGE, bool optional = false);
void LoadTerrainTextures();

// Variáveis globais dos uniforms do shader
extern GLuint g_GpuProgramID;
extern GLint  g_model_uniform;
extern GLint  g_view_uniform;
extern GLint  g_projection_uniform;
extern GLint  g_object_id_uniform;
extern GLint  g_bbox_min_uniform;
extern GLint  g_bbox_max_uniform;
extern GLuint g_NumLoadedTextures;
extern bool   g_HasSatelliteTexture;
extern bool   g_SatelliteTerrainMode;
extern bool   g_HasTerrainSplatmaps;

#endif // _SCENE_H
