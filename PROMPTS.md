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

---

# Adição do modelo de poste de luz com ponto de luz direcionado

PROMPT: Carregue e renderize o modelo lightpole.obj na cena com um ponto de luz posicionado no topo apontando para baixo (sem emissão de luz para cima) no sistema de iluminação e no fragment shader.

---

# Centralização dos postes no módulo de iluminação e painel de ajuste em tempo real no shader

PROMPT: Separe cor e intensidade na estrutura PointLight, mova a criação e renderização dos postes de luz para a classe Lighting e adicione constantes de ajuste em tempo real no topo de shader_fragment.glsl para recarregamento via tecla R.

---

# Implementação dos faróis frontais assimétricos do ônibus com controle pela tecla L

PROMPT: Implemente os faróis frontais do ônibus vinculados à sua matriz de modelagem e orientação, com ângulos assimétricos realistas entre os lados esquerdo e direito, dispersão periférica no shader e alternância ligada/desligada pela tecla L.

---

# Ciclo de estágios dos faróis frontais (tecla L), lampejo/luz alta (tecla P) e remoção das luzes laterais

PROMPT: Configure o ciclo da tecla L para ligar na primeira vez a luz menor do meio (_tex_electric__emissive_), na segunda vez o farol principal maior (_tex_low_beam__emissive_) e na terceira desligar; configure o farol separado de luz alta (_tex_high_beam__emissive_) para piscar com um toque na tecla P ou travar aceso ao segurar P até que P seja pressionada novamente; e remova a emissão das luzes laterais do ônibus.

---

# Geração de terreno a partir de mapa topográfico PNG 16 bits (vale.png)

PROMPT: Implemente o carregamento direto do mapa topográfico data/vale.png em PNG de 16 bits via stbi_load_16 para gerar a malha do terreno com normais analíticas suaves e consulta de altura bilinear GetTerrainHeight(x, z) para posicionar o ônibus e os postes sobre o relevo.

---

# Implementação de suspensão no ônibus

PROMPT: Temos uma implementação primitiva de uma suspensão no ônibus, mas as rodas não acompanham o terreno. Implemente um modelo de suspensão com parâmetros ajustáveis.

---

# Modificar centro da câmera look-at

PROMPT: Atualmente a câmera orbita o eixo traseiro do ônibus, que é a sua origem. Mova o centro da câmera Orbit para o meio do ônibus.

---

# Comportamento da suspensão em subida ou descida

PROMPT: A suspensão está apresentando um comportamento meio estranho quando o ônibus está subindo ou descendo um terreno inclinado.
Quando está subindo, as molas são comprimidas e o ônibus fica mais baixo que o normal. Já quando está descendo, o ônibus fica bem alto, com a suspensão esticada. Investigue e conserte este bug

---

# Geração de terreno multi-material por satélite, recorte de área útil, floresta 3D e edição via PSD

PROMPT: Implemente um pipeline de geração de terreno usando exclusivamente a ortofoto mascarada (`tools/bruto_2012_mascara.tiff`) reprojetada sobre o heightmap 16-bit (`data/vale.png`), descartando regiões pretas externas via `valid_mask.png`, classificando 8 materiais em dois splatmaps (`splatmap_0.png` e `splatmap_1.png`) com modulador cromático regional (`macro_albedo.png`), posicionando árvores 3D instanciadas (`data/tree.obj` + `trees.txt`), separando a visualização de satélite (`F10`) da câmera livre (`F11`) e disponibilizando a ferramenta C++ `tools/terrain_psd_tool` (`--init` / `--apply`) para edição manual em camadas PSD.

---

# Extração viária pura por visão computacional sobre o satélite, ajuste de árvore e execução manual do PSD

PROMPT: Refaça a segmentação do asfalto em `tools/generate_terrain_assets.py` utilizando exclusivamente a imagem de satélite `tools/bruto_2012_mascara.tiff` (removendo qualquer dependência do OpenStreetMap), com desobstrução direcional sob copas de árvores e inclusão da estrada florestal sudeste. Remova a execução automática do `terrain_psd_tool.exe` ao iniciar o simulador (documentando a execução manual com `--apply` no `README.md`), elimine a exclusão automática de árvores em raio de 4m do asfalto e aumente a densidade da vegetação gerada.

---

# Suavização geodésica estrita no leito viário preservando pistas paralelas e encostas

PROMPT: Desfaça a suavização de terreno com halo externo sobre as bordas e salve backups do heightmap original (`vale_backup.png` e `vale_processed_backup.png`). Implemente uma difusão geodésica 3x3 restrita exclusivamente aos pixels internos do asfalto (com barreira em bordas e degraus verticais) para eliminar ondulações sem alterar taludes adjacentes nem nivelar pistas paralelas em cotas diferentes.

---

# Meio-fio 3D geométrico com quinas de 90°, rebaixamento do asfalto em 10 cm e iluminação dedicada

PROMPT: Implemente a geração procedural de geometria 3D para o meio-fio ao longo da borda da camada de asfalto principal (`splatmap_0.r`), recortando automaticamente as interseções com o asfalto secundário (`splatmap_0.g`). Extraia o contorno via Marching Squares com suavização de Taubin e simplificação Ramer-Douglas-Peucker para produzir segmentos retilíneos contínuos, com faces separadas formando ângulos retos de 90 graus entre o topo, a parede vertical e o asfalto. Mantenha o topo do meio-fio nivelado à cota natural do terreno adjacente, aplicando rebaixamento físico de -10 cm exclusivamente no interior do asfalto, e adicione um material dedicado no fragment shader (`object_id == CURB`) com pintura branca pré-moldada isolada do entorno e reflexão especular/difusa para Sol, postes e faróis do ônibus.

