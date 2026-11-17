# Tutorial Completo: Edição do Terreno, Máscaras no Affinity Photo e Resumo do Projeto

Este guia foi feito para você conseguir editar o asfalto e os materiais do **Branquinho: Rota do Vale** diretamente no **Affinity Photo**, testar no simulador e entender tudo o que já foi implementado nas últimas etapas.

---

## Parte 1 — Como editar as máscaras do terreno no Affinity Photo

### 1. Qual arquivo abrir
Abra no **Affinity Photo** o arquivo:
> **`data/terrain/terreno_editavel.psd`** (`2048 × 2048` pixels)

Ele já vem alinhado **1:1** com o relevo do Campus do Vale e com a "base" detectada automaticamente pelo script.

---

### 2. O que é cada camada (de cima para baixo no painel *Layers*)

| Posição | Camada | Cor visual no editor | O que ela representa no jogo |
| :---: | :--- | :--- | :--- |
| 🔝 Topo | **`SATELITE_GUIA_TOPO`** | Foto de satélite (45% opacidade) | **Guia por cima das máscaras** para você enxergar as ruas reais enquanto pinta. *Pode ligar/desligar ou mudar a opacidade dela à vontade — ela não vira textura no chão.* |
| **1** | **`0_Asfalto_Principal`** | 🔴 Vermelho (`#E53935`) | **Asfalto Principal** (Anel Viário, Av. Bento Gonçalves) |
| **2** | **`1_Asfalto_Secundario`** | 🟠 Laranja (`#FB8C00`) | **Asfalto Secundário** (estacionamentos, pátios de manobra, acessos menores) |
| **3** | **`2_Calcadas`** | 🔵 Ciano (`#00ACC1`) | **Calçadas e pisos pavimentados** |
| **4** | **`7_Predios`** | 🟣 Roxo (`#8E24AA`) | **Base / Fundação dos prédios** |
| **5** | **`6_Agua_Lagoa`** | 🔹 Azul (`#1E88E5`) | **Água** (Lagoa do Vale) |
| **6** | **`5_Terra_Barro`** | 🟤 Marrom (`#8D6E63`) | **Terra exposta / Taludes / Saibro** |
| **7** | **`4_Mata_Arvores`** | 🌲 Verde Escuro (`#1B5E20`) | **Solo de mata** (serapilheira debaixo das árvores) |
| **8** | **`3_Grama_Base`** | 🌱 Verde Claro (`#7CB342`) | **Grama base** — preenche toda a área válida do mapa por baixo de tudo, garantindo que nada fique com "buraco" ao apagar camadas de cima |
| 🔻 Fundo | **`SATELITE_BASE_FUNDO`** | Foto de satélite (100% opacidade) | **Fundo de referência opaco** para comparação |

---

### 3. Regra de ouro: como funciona a prioridade de camadas

A pilha foi desenhada para que **a camada de cima sempre vença a de baixo automaticamente**:

- Se você pintar vermelho em **`0_Asfalto_Principal`** por cima de onde havia grama ou mata, o asfalto cobre tudo embaixo automaticamente — **você NÃO precisa ir nas camadas de baixo apagar a grama ou a mata**.
- Se você **apagar** um trecho de **`0_Asfalto_Principal`** com a borracha, o que estava exatamente embaixo (como calçada, mata ou **`3_Grama_Base`**) aparece sozinho no jogo!
- **O programa lê apenas a opacidade (Alpha) de cada camada de material**: não importa qual cor esteja selecionada no seu pincel (vermelho, branco, preto), qualquer traço opaco pinta aquele material e a borracha remove. *(Dica: use `Alt` + Clique em cima da cor da camada só para manter o visual bonito e fácil de distinguir no Affinity Photo).*

---

### 4. Passo a passo para corrigir o asfalto no Affinity Photo

1. **Ative o guia**: deixe **`SATELITE_GUIA_TOPO`** visível (se quiser enxergar melhor o asfalto ou a máscara, ajuste a opacidade dela entre `30%` e `70%` no painel *Layers*).
2. **Selecione a camada certa**: clique em **`0_Asfalto_Principal`** (ou **`1_Asfalto_Secundario`** se for estacionamento/acesso menor).
3. **Para ADICIONAR asfalto onde faltou**:
   - Pegue o **Pincel (`B`)** (*Paint Brush Tool*).
   - Ajuste **Hardness** para **`70% – 85%`** (dá uma transição suave e natural na beirada da pista em vez de pixels serrilhados).
   - Pinte seguindo a rua na foto de satélite por cima. *(Dica do Affinity Photo: clique num ponto, segure **`Shift`** e clique mais à frente para traçar trechos retos bem limpos).*
4. **Para REMOVER asfalto onde sobrou / ficou errado**:
   - Na mesma camada (`0_Asfalto_Principal` ou `1_Asfalto_Secundario`), pegue a **Borracha (`E`)** (*Erase Brush Tool*) e apague o trecho errado.
   - Confira se não há resto de laranja em `1_Asfalto_Secundario` por baixo caso o erro apareça em dois tons.
5. **Cuidado nas bordas pretas fora do campus**:
   - Os cantos vazios fora do recorte válido estão transparentes em **todas** as camadas de material (`3_Grama_Base` inclusive). Não pinte nessas bordas externas para o mapa não esticar fora da área recortada.
6. **Não apague nem renomeie as camadas** de material (`0_...` até `7_...`), e mantenha todas as 8 camadas de material **com o olhinho ligado (visíveis) e em `100%` de opacidade** antes de exportar.

---

### 5. Como salvar no Affinity Photo e aplicar no jogo

1. No Affinity Photo, salve/exporte sobrescrevendo o próprio PSD:
   - **`File` -> `Export...` -> `PSD`** (pode usar o preset padrão *Preserve Editability* ou *Final Cut*), salvando em **`data/terrain/terreno_editavel.psd`**.
   - *(Obs.: `File -> Save` no Affinity Photo salva como `.afphoto`; se você quiser guardar o projeto com histórico do Affinity, salve o `.afphoto` para você, mas lembre-se sempre de exportar por cima de `data/terrain/terreno_editavel.psd` para o jogo enxergar).*
2. **Aplique o PSD manualmente antes de rodar o simulador**:
   - Como `terreno_editavel.psd` e `*.exe` ficam no `.gitignore`, **o jogo não roda a ferramenta sozinho**. Após salvar/exportar o PSD, aplique as alterações nos splatmaps executando na raiz do repositório:
     ```powershell
     .\tools\terrain_psd_tool.exe --apply
     ```
   - Isso atualiza `data/terrain/splatmap_0.png` e `data/terrain/splatmap_1.png` (as texturas no chão do jogo) sem remover árvores de `data/terrain/trees.txt`.
3. Em seguida, **rode o simulador normalmente** para ver as novas máscaras aplicadas.

#### Se você for editar em outro computador:
O Git ignora `*.exe` e `terreno_editavel.psd` (`.gitignore`), então os splatmaps atualizados, o `terrain_psd_tool.cpp` e este tutorial vão normalmente no `git push`/`git pull`, sem travar o GitHub. No computador novo, basta compilar a ferramenta uma vez e gerar o `.psd` em 1 segundo a partir dos splatmaps atuais:
```powershell
g++ -O3 -Wall -static tools/terrain_psd_tool.cpp -Icgvis-base-opengl/include -lz -o tools/terrain_psd_tool.exe
.\tools\terrain_psd_tool.exe --init
```
*(Aviso: no mesmo PC onde você já editou o PSD, **não** rode `--init` depois de editar, pois `--init` recria o `.psd` a partir dos splatmaps salvos e sobrescreve edições ainda não aplicadas).*

---

## Parte 2 — Resumo de tudo o que foi explicado e implementado na conversa

Caso queira revisar o que foi feito no código antes de você partir para a colisão e para a textura de satélite final, aqui está o resumo direto:

### 1. Altura e escala do terreno 16-bit (`vale.png` / `vale_processed.png`)
- **Escala métrica real (QGIS SIRGAS 2000 / UTM 22S)**: a imagem `vale.png` tem `1930 × 1703` pixels e foi mapeada em **`1 pixel = 1.0 metro`** no OpenGL (`1930 m × 1703 m` centralizados na origem).
- **Por que parecia baixo na escala 1:1**: os pixels de 16 bits em `vale.png` vão realmente do mínimo (`1`) ao máximo (`65535`), mapeando exatamente de `33.0 m` até `137.043 m` (`deltaH = 104.043 m`). Porém, o campus central ocupa a baixada (entre ~15% e 35% dessa altura), enquanto `137 m` é apenas o pico do Morro Santana; além disso, em perspectiva de monitor (`FOV = 60°`), 104 m de altura em ~2 km de largura parecem mais planos que ao vivo.
  - Se quiser **exagero vertical** (como no Google Earth ou Slowroads), basta multiplicar o 5º argumento (`104.043f`) em `BuildHeightmapAndAddToVirtualScene(...)` dentro de `cgvis-base-opengl/src/main.cpp` por `1.5f` ou `2.0f`. A física do ônibus (`GetTerrainHeight(x, z)`) e as normais se ajustam sozinhas.

### 2. Por que a imagem de satélite gigante (`55536 × 49019`) não abre direto inteira
- Uma imagem de `55536 × 49019` tem **2,72 Gigapixels** (~`8,2 GB` descomprimida em RAM + `10,9 GB` com Mipmaps na VRAM), estourando o limite de inteiros de 32 bits do `stb_image` (`2 GB`) e o limite máximo de textura de placa de vídeo (`GL_MAX_TEXTURE_SIZE`, geralmente `16384` ou `8192`).
- **Você NÃO precisa recortar nem perder pedaços do mapa**: quando for gerar a textura final em alta resolução (caso vá usar textura de satélite), o ideal é:
  - **Opção simples**: exportar mantendo toda a extensão do mapa redimensionada para **`8192 × 7230`** (~23 cm/pixel) ou **`16384 × 14461`** (~12 cm/pixel);
  - **Opção quadrantes (`2×2` ou `3×3`)**: dividir em 4 ou 9 quadrantes de até `4K`/`8K` cada (evitando passar de ~`2 GB` de VRAM total e de `100 MB` por arquivo no GitHub).
- Por enquanto, o simulador carrega `data/satelite_2012_low.png` (`1930 × 1703`) apenas quando você aperta **`F10`** para depuração visual.

### 3. Câmera Livre e Todos os Atalhos do Simulador
- **`W / A / S / D`**: Dirigem o ônibus (nos modos normais) ou voam pelo mapa (quando na câmera livre).
- **`C`**: Alterna entre câmera **Orbit** (terceira pessoa) e **Bumper** (para-choque).
- **`F11`**: Ativa/desativa a **câmera livre (`Free`)** a `120 m/s` e desliga o **fog** temporariamente para inspecionar o terreno inteiro de longe (ao sair no `F11`, volta para onde o ônibus estava e religa o fog).
- **`F10`**: Alterna a visualização do chão entre o **modo jogável** (8 materiais dos splatmaps) e a imagem de **satélite** (`satelite_2012_low.png`).
- **`L`**: Ciclo de 3 estágios dos faróis frontais:
  1. 1º toque: **Luz menor do meio** (`_tex_electric__emissive_`);
  2. 2º toque: **Farol principal maior assimétrico** (`_tex_low_beam__emissive_` — lado direito mais aberto e elevado para iluminar calçada/paradas; lado esquerdo contido);
  3. 3º toque: **Desliga** (luzes laterais removidas).
- **`P`**: Farol separado de **luz alta** (`_tex_high_beam__emissive_`) — toque rápido **pisca**; segurar por `>= 0.25s` **trava aceso** até apertar `P` de novo.
- **`J` / `K`**: Avança/retrocede o ângulo do Sol (dia, pôr do sol e noite).
- **`O`**: Alterna projeção Perspectiva / Ortográfica.
- **`V`**: Liga / desliga **V-Sync**.
- **`R`**: Recarrega os shaders em tempo real (*Hot-Reload*) sem fechar o jogo.

### 4. Como faremos a colisão na borda do asfalto (próximo passo)
Depois que você revisar e deixar o traçado do asfalto bem certinho no `terreno_editavel.psd`:
1. Criaremos o módulo separado `collisions.h` e `collisions.cpp` (regra da disciplina).
2. Carregaremos na CPU o peso de asfalto gerado pelas suas camadas (`splatmap_0.r + splatmap_0.g`, com interpolação bilinear).
3. Modelaremos o ônibus (`branquinho.obj`) como uma **OBB** (caixa orientada pelo `yaw`) e testaremos os **4 cantos** dele contra a borda do asfalto:
   - Se algum canto tentar sair para fora do asfalto, calculamos a **normal da borda** usando os pixels vizinhos do mapa de asfalto, empurramos o ônibus de volta para a pista e zeramos só a componente da velocidade contra a "parede invisível" — fazendo o ônibus **deslizar suavemente pela guia** em vez de travar seco nas curvas.
4. Na mesma arquitetura, colocaremos **OBB vs OBB** (para outros veículos futuramente) e **Esfera vs OBB/AABB** (para paradas/checkpoints), completando os testes de intersecção da especificação.
