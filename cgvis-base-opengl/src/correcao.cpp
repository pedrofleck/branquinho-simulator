// ============================================================================
// correcao.cpp - Rotina de correção automatizada da disciplina (INF01047)
//
// Fornecido pela disciplina. Define Correcao_KeyCallback(), que encerra o
// programa com um código de saída específico (100 + dígito) quando o usuário
// pressiona SHIFT + [0-9]. É utilizada pelos scripts de correção automática.
//
// NÃO MODIFICAR. A chamada Correcao_KeyCallback(key, action, mod) deve ser
// sempre a primeira instrução de KeyCallback() em main.cpp.
// ============================================================================

#include <cstdlib>
#include <GLFW/glfw3.h>

void Correcao_KeyCallback(int key, int action, int mod)
{
    // Não modifique este loop! Ele é utilizando para correção automatizada dos
    // laboratórios. Deve ser sempre o primeiro comando da função KeyCallback().
    for (int i = 0; i < 10; ++i)
        if (key == GLFW_KEY_0 + i && action == GLFW_PRESS && mod == GLFW_MOD_SHIFT)
            std::exit(100 + i);
}
