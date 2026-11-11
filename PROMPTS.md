# Adaptar código base OpenGL para Vulkan

PROMPT: Adapte as funcionalidades do código base OpenGL para Vulkan, considerando as restrições impostas na especificação do trabalho.

---

# Modularização do código OpenGL

PROMPT: Separe o código de main.cpp em módulos distintos (scene, camera, bus) preservando todas as funcionalidades existentes e verifique a compilação no backend OpenGL.

---

# Limpeza e simplificação do main.cpp

PROMPT: Remova todo o código legado e não utilizado de main.cpp (modelos antigos, variáveis de torso/antebraço, ângulos de Euler e textos de debug), mantendo apenas a renderização do ônibus com textura e o plano, e verifique se a compilação continua funcionando.

---

# Sistema de iluminação solar, hora do dia e névoa

PROMPT: Crie um módulo de iluminação (lighting.h e lighting.cpp) com suporte à posição do Sol (hora do dia), cor ambiente e névoa exponencial, e atualize o fragment shader com iluminação difusa e especular (Blinn-Phong com Fresnel).

---

# Correção e integração de main.cpp

PROMPT: Resolva os marcadores de conflito de merge em main.cpp, integrando o carregamento do terreno de teste, cinemática com Delta T e os controles de iluminação e movimentação.

---

# Implementação de V-Sync e indicador no HUD

PROMPT: Implemente o controle de sincronização vertical (V-Sync) via GLFW com tecla de atalho V para alternar e atualize o HUD para exibir o status do V-Sync junto ao FPS.
