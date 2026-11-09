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
#define SPHERE 0
#define BUNNY  1
#define PLANE  2
#define BUS    3

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

// Funções de manipulação de malha e cena
void BuildTrianglesAndAddToVirtualScene(ObjModel* model);
void ComputeNormals(ObjModel* model);
void DrawVirtualObject(const char* object_name);

// Funções de shaders e GPU
void LoadShadersFromFiles();
GLuint LoadShader_Vertex(const char* filename);
GLuint LoadShader_Fragment(const char* filename);
void LoadShader(const char* filename, GLuint shader_id);
GLuint CreateGpuProgram(GLuint vertex_shader_id, GLuint fragment_shader_id);

// Função de texturas
void LoadTextureImage(const char* filename);

// Variáveis globais dos uniforms do shader
extern GLuint g_GpuProgramID;
extern GLint g_model_uniform;
extern GLint g_view_uniform;
extern GLint g_projection_uniform;
extern GLint g_object_id_uniform;
extern GLint g_bbox_min_uniform;
extern GLint g_bbox_max_uniform;
extern GLuint g_NumLoadedTextures;

#endif // _SCENE_H
