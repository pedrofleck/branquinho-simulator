# Roteiro do Projeto: Branquinho Simulator

## Objetivo Imediato: Ônibus Andando e Câmera
- [ ] **Limpar `main.cpp`**: tirar sobras dos labs (coelho, esfera, braço/torso, cubos de Euler) e manter só o chão e o ônibus.
- [ ] **Ajustar a cena**: aumentar a escala do chão (`plane.obj`) e aumentar o `farplane` para a visão não cortar no fundo.
- [ ] **Tempo de quadro ($\Delta t$)**: calcular o tempo entre frames para movimentação estável independente do FPS.
- [ ] **Controle do ônibus (WASD)**:
  - `W` / `S`: acelerar para frente / ré e freio com desaceleração gradual.
  - `A` / `D`: esterçar/rotacionar o ônibus no plano $XZ$.
  - Atualizar posição usando vetor frente e velocidade calculada por $\Delta t$.
- [ ] **Câmera orbital com o mouse**: fazer o ponto focal da câmera seguir o ônibus enquanto o mouse orbita ao redor dele.

---

## Próximas Etapas

### 1. Câmeras
- Implementar a segunda câmera exigida (câmera no painel/motorista ou bumper frontal).
- Tecla para alternar entre terceira pessoa (órbita) e primeira pessoa.

### 2. Rodas e Partes Móveis
- Isolar os shapes das rodas dianteiras e traseiras no modelo.
- Fazer as rodas girarem com o deslocamento e as da frente virarem ao fazer curva.
- Animar abertura das portas.

### 3. Cenário e Instanciação
- Inserir a malha do Campus do Vale.
- Instanciar postes, árvores e paradas de ônibus reutilizando as mesmas malhas.

### 4. Iluminação e Texturas
- Configurar materiais reais do ônibus (`.mtl`) e texturas do cenário (mínimo de 3 texturas).
- Modelo de iluminação difusa + especular (Blinn-Phong / Disney BRDF).
- Tratar vidros com transparência e blending.

### 5. Pedestres com Curvas de Bézier
- Adicionar pedestres caminhando entre pontos de parada seguindo curvas de Bézier cúbicas parametrizadas pelo tempo.

### 6. Colisões (`collisions.cpp`)
- Criar caixas delimitadoras (AABB/OBB) para o ônibus.
- Tratar colisão com obstáculos do mapa (postes, guias, muros).
- Detectar parada nos pontos de embarque.

### 7. Sistema de Partículas (Funcionalidade Extra)
- Emissão de partículas de fumaça/poeira nos pneus durante frenagens e acelerações bruscas.
