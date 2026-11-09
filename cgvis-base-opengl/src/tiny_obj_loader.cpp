// ============================================================================
// tiny_obj_loader.cpp - Unidade de compilação da biblioteca tinyobjloader
//
// A tinyobjloader é uma biblioteca "single-header" de terceiros para leitura
// de modelos Wavefront (.obj/.mtl). Definir TINYOBJLOADER_IMPLEMENTATION antes
// do include faz com que a implementação seja compilada uma única vez neste
// arquivo. Utilizada por ObjModel (scene.cpp) para carregar, por exemplo,
// data/branquinho.obj.
// ============================================================================

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
