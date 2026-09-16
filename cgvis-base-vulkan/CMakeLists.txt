# Este arquivo CMakeLists.txt foi adaptado a partir do arquivo homônimo do
# código base da disciplina INF01047 (que por sua vez foi adaptado do projeto
# castor do PET INF/UFRGS, https://github.com/petcomputacaoufrgs/castor-fcg,
# com modificações vindas do CMakeLists.txt criado pelos alunos Luis Melo e
# Santiago Gonzaga em 2023/1).
#
# As mudanças em relação ao original são:
#   * o arquivo "src/glad.c" não é mais compilado (GLAD só existe para OpenGL);
#   * passamos a procurar e linkar o Vulkan;
#   * passamos a compilar os shaders GLSL para SPIR-V com o compilador glslc;
#   * passamos a compilar "src/vkcontext.cpp" e "src/collisions.cpp".

cmake_minimum_required(VERSION 3.16)

project(CGVIS_BASE_VULKAN VERSION 1.0.0 LANGUAGES CXX)

# Arquivos fonte C++. Inclua nesta lista todos os arquivos que devem
# ser compilados.
set(SOURCES
  src/main.cpp
  src/vkcontext.cpp
  src/collisions.cpp
  src/correcao.cpp
  src/textrendering.cpp
  src/tiny_obj_loader.cpp
  src/stb_image.cpp
)

# Shaders GLSL que serão compilados para SPIR-V.
set(SHADERS
  src/shader_vertex.vert
  src/shader_fragment.frag
  src/shader_text.vert
  src/shader_text.frag
)

set(CMAKE_CXX_STANDARD          11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(WIN32)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${PROJECT_SOURCE_DIR}/bin/Debug")
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_SOURCE_DIR}/bin/Release")
  # Geradores de configuração única (MinGW Makefiles, Ninja) usam a variável
  # sem sufixo.
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY         "${PROJECT_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
elseif(UNIX)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/Linux")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()
message(STATUS
  "Build type: ${CMAKE_BUILD_TYPE}

               Change the build type on the command line with

                   -DCMAKE_BUILD_TYPE=type

               for type in {Release, Debug, RelWithDebInfo}.
")

set(EXECUTABLE_NAME main)

# Verifica se todos os arquivos fonte estão presentes no diretório
# atual. Se não estão, avisa sobre CMakeLists mal configurado.
foreach(source_file IN LISTS SOURCES SHADERS)
  if(NOT EXISTS ${PROJECT_SOURCE_DIR}/${source_file})
    message(FATAL_ERROR "
O arquivo ${PROJECT_SOURCE_DIR}/${source_file} não existe.
Por favor, atualize a lista de arquivos fonte no arquivo CMakeLists.txt.")
    break()
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Vulkan
# ---------------------------------------------------------------------------

find_package(Vulkan REQUIRED)

message(STATUS "Vulkan encontrado: ${Vulkan_LIBRARY}")

# O compilador de shaders glslc acompanha o Vulkan SDK. Versões do CMake a
# partir da 3.19 já o localizam sozinhas na variável Vulkan_GLSLC_EXECUTABLE;
# nas versões anteriores, procuramos manualmente.
if(NOT Vulkan_GLSLC_EXECUTABLE)
  find_program(Vulkan_GLSLC_EXECUTABLE
    NAMES glslc
    HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
  )
endif()

if(NOT Vulkan_GLSLC_EXECUTABLE)
  message(FATAL_ERROR "
O compilador de shaders \"glslc\" não foi encontrado.

Diferente do OpenGL, o Vulkan não aceita código GLSL diretamente: os shaders
precisam ser compilados para o formato binário SPIR-V antes de serem usados.
Quem faz essa compilação é o programa glslc, que vem junto com o Vulkan SDK.

Instale o Vulkan SDK (https://vulkan.lunarg.com/sdk/home) e garanta que a
variável de ambiente VULKAN_SDK esteja definida. No Linux, alternativamente,
instale o pacote da sua distribuição que contém o glslc (por exemplo,
\"glslc\" ou \"shaderc\").")
endif()

message(STATUS "glslc encontrado: ${Vulkan_GLSLC_EXECUTABLE}")

# ---------------------------------------------------------------------------
# Compilação dos shaders GLSL para SPIR-V
# ---------------------------------------------------------------------------
#
# Os arquivos ".spv" são gerados em "bin/shaders/". O executável os procura em
# "../shaders/" (caminho relativo a "bin/Debug/", "bin/Release/" ou
# "bin/Linux/"), assim como já procura os modelos e texturas em "../../data/".

set(SHADER_OUTPUT_DIR "${PROJECT_SOURCE_DIR}/bin/shaders")
file(MAKE_DIRECTORY "${SHADER_OUTPUT_DIR}")

set(SPIRV_FILES "")

foreach(shader_file IN LISTS SHADERS)
  get_filename_component(shader_name ${shader_file} NAME)
  set(spirv_file "${SHADER_OUTPUT_DIR}/${shader_name}.spv")

  add_custom_command(
    OUTPUT  ${spirv_file}
    COMMAND ${Vulkan_GLSLC_EXECUTABLE} "${PROJECT_SOURCE_DIR}/${shader_file}" -o "${spirv_file}"
    DEPENDS "${PROJECT_SOURCE_DIR}/${shader_file}"
    COMMENT "Compilando shader ${shader_name} para SPIR-V"
    VERBATIM
  )

  list(APPEND SPIRV_FILES ${spirv_file})
endforeach()

add_custom_target(shaders ALL DEPENDS ${SPIRV_FILES})

# ---------------------------------------------------------------------------
# Executável
# ---------------------------------------------------------------------------

add_executable(${EXECUTABLE_NAME} ${SOURCES})

# Garante que os shaders sejam compilados antes (ou junto) do executável.
add_dependencies(${EXECUTABLE_NAME} shaders)

target_include_directories(${EXECUTABLE_NAME} BEFORE PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(${EXECUTABLE_NAME} Vulkan::Vulkan)

if(WIN32)

  if(MINGW)

    # Aqui tentamos descobrir qual libc do Widows está sendo usada
    # pelo compilador MinGW: msvcrt (antiga) ou ucrt (nova). Também
    # diferenciamos entre um compilador 32-bits (antigo) ou 64-bits.
    # Para isso, buscamos pela ocorrência de algumas strings
    # específicas no output do comando "-v" do GCC, que lista os
    # parâmetros de configuração do compilador.
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} "-v"
      ERROR_VARIABLE  COMPILER_VERSION_OUTPUT
      RESULT_VARIABLE COMPILER_VERSION_RESULT
    )

    if (COMPILER_VERSION_RESULT EQUAL 0)
      # NOTE: É importante que o primeiro teste seja buscando pela
      # string ucrt64 no output do compilador, pois a string "mingw64"
      # sempre aparece no output (mesmo quando ucrt64 é a libc utilizada).
      if (COMPILER_VERSION_OUTPUT MATCHES "ucrt64")
        set(LIBGLFW ${PROJECT_SOURCE_DIR}/lib-ucrt-64/libglfw3.a)
      elseif (COMPILER_VERSION_OUTPUT MATCHES "mingw64")
        set(LIBGLFW ${PROJECT_SOURCE_DIR}/lib-mingw-64/libglfw3.a)
      else()
        set(LIBGLFW ${PROJECT_SOURCE_DIR}/lib-mingw-32/libglfw3.a)
      endif()
    else()
      message(FATAL_ERROR "Failed to get MinGW compiler version.")
    endif()

  elseif(MSVC)
    set(LIBGLFW ${PROJECT_SOURCE_DIR}/lib-vc2022/glfw3.lib)
  else()
    message(FATAL_ERROR "This CMakeLists.txt file only supports MINGW or MSVC toolchain on Windows.")
  endif()

  message(STATUS "LIBGLFW = ${LIBGLFW}")

  # NOTE: não linkamos mais com "opengl32", pois esta aplicação não utiliza
  # OpenGL. A GLFW carrega a opengl32.dll dinamicamente apenas quando um
  # contexto OpenGL é pedido, o que não é o nosso caso (usamos GLFW_NO_API).
  target_link_libraries(${EXECUTABLE_NAME} ${LIBGLFW} gdi32)

elseif(UNIX)

  target_compile_options(${EXECUTABLE_NAME} PRIVATE -Wall -Wno-unused-function)

  find_package(X11 REQUIRED)
  find_library(MATH_LIBRARY m)
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)

  target_link_libraries(${EXECUTABLE_NAME}
    ${CMAKE_DL_LIBS}
    ${MATH_LIBRARY}
    ${PROJECT_SOURCE_DIR}/lib-linux/libglfw3.a
    ${CMAKE_THREAD_LIBS_INIT}
    ${X11_LIBRARIES}
    ${X11_Xrandr_LIB}
    ${X11_Xcursor_LIB}
    ${X11_Xinerama_LIB}
  )

endif()

# Alvo "run": compila (se necessário) e executa o programa a partir do
# diretório onde o executável foi gerado, que é o que os caminhos relativos
# "../../data/" e "../shaders/" do código esperam.
add_custom_target(run
    COMMAND ${CMAKE_COMMAND} -E chdir $<TARGET_FILE_DIR:${EXECUTABLE_NAME}> $<TARGET_FILE:${EXECUTABLE_NAME}>
    DEPENDS ${EXECUTABLE_NAME}
    USES_TERMINAL
)
