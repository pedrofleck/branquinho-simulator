# Compilação

Este projeto usa **Vulkan** (e não OpenGL). Por isso, além das bibliotecas que
o código base da disciplina já exigia, é necessário instalar o **Vulkan SDK**,
que fornece:

- os *headers* do Vulkan (`vulkan/vulkan.h`);
- a biblioteca do *loader* (`vulkan-1.lib` / `libvulkan.so`);
- o compilador de shaders **`glslc`**, que traduz GLSL para SPIR-V;
- as *validation layers*, que verificam em tempo de execução se a aplicação
  está usando a API corretamente (habilitadas automaticamente em builds
  de `Debug`).

> [!IMPORTANT]
> O Vulkan SDK é necessário para **compilar**. Para **executar** o programa já
> compilado, basta um driver de placa de vídeo com suporte a Vulkan.

> [!NOTE]
> Se a sua placa de vídeo não suporta Vulkan, ou se você está em uma máquina
> virtual, é possível rodar a aplicação por software instalando o **SwiftShader**
> ou o **lavapipe** (Mesa). O desempenho será baixo, mas suficiente para testes.

## Instalação do Vulkan SDK

### Windows

1. Baixe e instale o SDK em <https://vulkan.lunarg.com/sdk/home#windows>.
2. O instalador define a variável de ambiente `VULKAN_SDK` automaticamente.
   Abra um **novo** terminal depois da instalação para que ela seja visível.

### Linux (Ubuntu / Debian)

```bash
sudo apt-get install vulkan-tools libvulkan-dev vulkan-validationlayers-dev \
                     spirv-tools glslc
```

Se o pacote `glslc` não existir na sua distribuição, ele também é fornecido
pelo pacote `shaderc`, ou pelo SDK oficial da LunarG
(<https://vulkan.lunarg.com/sdk/home#linux>).

Você também precisa das bibliotecas de janela que o código base já usava:

```bash
sudo apt-get install build-essential make cmake libx11-dev libxrandr-dev \
                     libxinerama-dev libxcursor-dev libxcb1-dev libxext-dev \
                     libxrender-dev libxfixes-dev libxau-dev libxdmcp-dev
```

Se você usa Linux Mint, talvez seja necessário instalar mais algumas bibliotecas:

```bash
sudo apt-get install libmesa-dev libxxf86vm-dev
```

### macOS

O Vulkan em macOS roda sobre o Metal, através do **MoltenVK**. Instale o SDK em
<https://vulkan.lunarg.com/sdk/home#mac>. Você também precisará da GLFW:

```zsh
brew install glfw
```

> [!WARNING]
> O `CMakeLists.txt` deste projeto foi testado apenas em Windows e Linux. Em
> macOS será necessário ajustar o trecho que localiza a biblioteca GLFW, e
> possivelmente habilitar a extensão de portabilidade
> `VK_KHR_portability_enumeration` em `CreateInstance()`, no arquivo
> `src/vkcontext.cpp`.

## Compilando e executando

### Com CMake (melhor opção, em qualquer sistema)

Abra um terminal, navegue até a pasta onde está este código fonte, e execute:

```bash
cmake --workflow --preset configure-build-run
```

isso é equivalente aos comandos:

```bash
cmake -B build -S .          # Cria e configura diretório de build
cmake --build build          # Faz a compilação (inclusive dos shaders)
cmake --build build -- run   # Executa o código compilado
```

> [!TIP]
> Esta é a melhor opção, pois o CMake gera automaticamente uma configuração
> que reutiliza arquivos previamente compilados, e recompila somente os
> arquivos que foram modificados -- incluindo os shaders.

Também é possível remover todos os arquivos compilados com:

```bash
cmake --build build -- clean
```

### Linux com Makefile

Execute `make` para compilar e `make run` para executar.

### Com VSCode

1. Instale o VSCode seguindo as instruções em <https://code.visualstudio.com/>.

2. No Windows, instale o compilador GCC seguindo as instruções em
   <https://code.visualstudio.com/docs/cpp/config-mingw#_installing-the-mingww64-toolchain>,
   ou use o Visual Studio (MSVC).

3. Instale o CMake seguindo as instruções em <https://cmake.org/download/>.

4. Instale as extensões `ms-vscode.cpptools` e `ms-vscode.cmake-tools` no
   VSCode. Se você abrir o diretório deste projeto no VSCode, automaticamente
   será sugerida a instalação destas extensões (pois estão listadas no arquivo
   `.vscode/extensions.json`).

5. Clique no botão de "Play" *NA BARRA INFERIOR* do VSCode para compilar e
   executar o projeto. Na primeira compilação, a extensão do CMake irá
   perguntar qual compilador você quer utilizar.

## Onde ficam os arquivos gerados

```
cgvis-base-vulkan/
+-- bin/
|   +-- Debug/   (ou Release/, ou Linux/)
|   |   +-- main        <- o executável
|   +-- shaders/
|       +-- shader_vertex.vert.spv
|       +-- shader_fragment.frag.spv
|       +-- shader_text.vert.spv
|       +-- shader_text.frag.spv
+-- build/       <- arquivos intermediários do CMake
```

O programa espera ser executado **a partir do diretório do executável**, pois
usa caminhos relativos: `../../data/` para modelos e texturas, e `../shaders/`
para os arquivos SPIR-V. O alvo `run` do CMake já faz isso automaticamente.

## Solução de problemas

**"O compilador de shaders glslc não foi encontrado"** durante o `cmake`:
instale o Vulkan SDK e abra um terminal novo, de modo que a variável de
ambiente `VULKAN_SDK` fique visível.

**"Nenhuma GPU com suporte a Vulkan foi encontrada"** ao executar: atualize o
driver da sua placa de vídeo. Você pode verificar se o Vulkan está funcionando
executando o programa `vulkaninfo`, que vem com o SDK.

**A janela abre preta**: rode uma build de `Debug` e observe o terminal. As
*validation layers* imprimem mensagens detalhadas sobre qualquer uso incorreto
da API.

**Erros de compilação no Windows com GCC**: cuide para extrair o código em um
caminho que não contenha espaços no nome de algum diretório.

- Caminho OK: `C:\Users\JohnDoe\Documents\CGVis\TrabalhoFinal`
- Caminho NÃO OK: `C:\Users\JohnDoe\Documents\Fundamentos de CG\TrabalhoFinal`
