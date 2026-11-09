# Adaptar código base OpenGL para Vulkan

PROMPT: Adapte as funcionalidades do código base OpenGL para Vulkan, considerando as restrições impostas na especificação do trabalho.

---

# Modularização do código OpenGL

PROMPT: Separe o código de main.cpp em módulos distintos (scene, camera, bus) preservando todas as funcionalidades existentes e verifique a compilação no backend OpenGL.

---

# Limpeza e simplificação do main.cpp

PROMPT: Remova todo o código legado e não utilizado de main.cpp (modelos antigos, variáveis de torso/antebraço, ângulos de Euler e textos de debug), mantendo apenas a renderização do ônibus com textura e o plano, e verifique se a compilação continua funcionando.

---

# Comentários de cabeçalho nos arquivos de código fonte

PROMPT: Escreva um comentário cabeçalho para cada arquivo de código fonte na pasta cgvis-base-opengl/src/ explicando sobre o que aquele arquivo é responsável. Alguns arquivos já possuem um cabeçalho, cuidado para não sobrescrever.
