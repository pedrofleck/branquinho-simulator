#version 450

// ===========================================================================
// Fragment Shader utilizado para rasterização de texto.
// ===========================================================================

// Atlas de glifos da fonte DejaVu (veja "include/dejavufont.h"). A imagem
// possui um único canal (VK_FORMAT_R8_UNORM), que guarda a "cobertura" de cada
// pixel da letra.
layout (set = 0, binding = 0) uniform sampler2D tex;

layout (location = 0) in vec2 texCoords;

layout (location = 0) out vec4 fragColor;

void main()
{
    // Texto preto, com transparência dada pelo atlas de glifos. O pipeline que
    // desenha o texto habilita blending (veja "src/textrendering.cpp").
    fragColor = vec4(0.0, 0.0, 0.0, texture(tex, texCoords).r);
}
