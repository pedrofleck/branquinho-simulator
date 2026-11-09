// ============================================================================
// stb_image.cpp - Unidade de compilação da biblioteca stb_image
//
// A stb_image é uma biblioteca "single-header" de terceiros para leitura de
// imagens (PNG, JPG, ...). Definir STB_IMAGE_IMPLEMENTATION antes do include
// faz com que a implementação seja compilada uma única vez neste arquivo;
// os demais arquivos incluem apenas as declarações de "stb_image.h".
// Utilizada em scene.cpp para carregar texturas.
// ============================================================================

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
