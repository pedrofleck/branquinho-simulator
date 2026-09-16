#version 450

// ===========================================================================
// Vertex Shader utilizado para rasterização de texto.
// ===========================================================================
//
// No código base em OpenGL, este shader ficava embutido como uma string em
// "textrendering.cpp". Em Vulkan isso não é possível, pois a API só aceita
// SPIR-V: o shader precisa estar em um arquivo que será compilado pelo glslc.
//

// Cada vértice carrega a posição da letra em NDC (xy) e a coordenada de
// textura dentro do atlas de glifos (zw).
layout (location = 0) in vec4 position;

layout (location = 0) out vec2 texCoords;

void main()
{
    // A função TextRendering_PrintString(), em "textrendering.cpp", recebe as
    // coordenadas x e y em NDC *no padrão do OpenGL*, isto é, com o eixo Y
    // apontando para CIMA e a origem no centro da tela. Como o NDC do Vulkan
    // tem o eixo Y apontando para BAIXO, invertemos o sinal de Y aqui.
    //
    // Fazemos a inversão dentro do shader (e não no C++) justamente para que a
    // interface de TextRendering_PrintString() continue idêntica à do código
    // base em OpenGL.
    gl_Position = vec4(position.x, -position.y, 0.0, 1.0);

    texCoords = position.zw;
}
