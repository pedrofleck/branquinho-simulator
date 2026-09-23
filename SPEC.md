# Especificação da Implementação

## Integrantes da dupla

- **Aluno 1 - Nome**: Eric Peracchi Pisoni
- **Aluno 1 - Cartão UFRGS**: 00318500

- **Aluno 2 - Nome**: Pedro Henrique Freitas Fleck
- **Aluno 2 - Cartão UFRGS**: 00233700

## Detalhes do que será implementado

- **Título do trabalho**: Branquinho: Rota do Vale
- **Parágrafo curto descrevendo o que será implementado**: Um simulador do ônibus circular do Campus do Vale

## Especificação visual

### Vídeo - Link

[OMSI The Bus Simulator](https://youtu.be/TE3m94hVeD8)

### Vídeo - Timestamp

- **Timestamp inicial**: 03:30
- **Timestamp final**: 04:00

### Imagens

#### Imagem 1

- **Descrição**: Inspiração de como deve ser a câmera Dashboard em primeira pessoa, com o painel do veículo visível.

![Imagem 1](images/spec/image1.png)

#### Imagem 2

- **Descrição**: Câmera Orbit que pode ser controlada lateralmente pelo usuário, mostrando o exterior do ônibus com os passageiros aguardando no ponto para embarcar.

![Imagem 2](images/spec/image2.png)

#### Imagem 3 - Slowroads (gráficos):

- **Descrição**: Outra referência de gráficos que buscamos alcançar é o jogo Slowroads, que pode ser jogado diretamente do navegador através do link https://slowroads.io/

![Imagem 3](images/spec/image3.jpg)

## Especificação textual

### Malhas poligonais complexas
- Serão implementados diversos modelos 3D com malhas poligonais complexas.
- O veículo que será conduzido pelo jogador, que é uma representação do ônibus Mascarello Gran Midi 2005 usado na linha circular do Campus do Vale.
- O cenário do jogo terá elementos que remetem ao Campus do Vale, como prédios, pórtico de entrada e outras estruturas.

### Transformações geométricas controladas pelo usuário
- O veículo virtual será conduzido pelo usuário através de transformações de translação e rotação, que serão aplicadas com base em um modelo simples que simula o comportamento de um automóvel.

### Diferentes tipos de câmeras
O jogo terá dois tipos de câmera:
1. Dashboard cam - visão em primeira pessoa, no assento do motorista olhando para frente, com visão do painel do veículo.
2. Orbit cam - visão em terceira pessoa elevada que pode ser controlada para olhar para os lados.

### Instâncias de objetos
- Alguns objetos, como árvores e postes, serão copiados em múltiplas instâncias para montar o cenário do jogo.

### Testes de intersecção
- Para evitar que o veículo atravesse paredes ou objetos, será implementado testes de colisão entre o veículo e o cenário.
Também haverá *checkpoints* que o jogador deve atingir.

### Modelos de Iluminação em todos os objetos
- Será implementado o sistema de iluminação *Disney BRDF* como iluminação global.
- 
Comentário Professor: Implementem os seguintes efeitos presentes na referência visual: névoa volumétrica, iluminação dos postes, vidros transparentes e espelhos retrovisores.

### Mapeamento de texturas em todos os objetos
- As texturas aplicadas aos objetos serão adquiridas a partir de fotografias em alta resolução de diferentes pontos do Campus do Vale.

### Movimentação com curva Bézier cúbica
- Os pedestres que caminham na calçada terão sua movimentação definida por curvas de Bézier.

### Animações baseadas no tempo ($\Delta t$)
- As animações dos pedestres e passageiros caminhando serão baseadas no tempo.
- O veículo tera física se deslocando por terrenos com relevo, como a subida e descida, quebra-molas e desníveis de terreno, isso também se baseará em tempo.

### Funcionalidade extra obrigatória

**Sistema de partículas/fumaça:**
- Os pneus do veículo produzem efeitos de fumaça quando submetido a acelerações bruscas, como arrancadas, frenagens e curvas em alta velocidade.

## Limitações esperadas

- Dashboard cam: possivelmente encontraremos problemas na implementação de uma câmera interna que mostra o painel do veículo.
Caso isso aconteça, utilizaremos uma Bumper cam, que é uma câmera localizada no exterior do ônibus, diretamente à frente do para-choque.
- Buscamos utilizar fotografias reais para atingir um visual um pouco mais fotorrealista, mas caso os resultados não atinjam nossas expectativas, vamos optar por um visual mais minimalista, com texturas mais simples.
- Animações dos pedestres: reconhecemos que fazer animações realistas de pessoas caminhando seria um desafio consideravelmente difícil, então optamos por animações mais simples para os pedestres.
