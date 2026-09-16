#ifndef _VKCONTEXT_H
#define _VKCONTEXT_H

// ===========================================================================
// vkcontext.h -- "Boilerplate" do Vulkan
// ===========================================================================
//
// No código base em OpenGL, a criação do contexto gráfico era feita em duas
// linhas:
//
//     glfwMakeContextCurrent(window);
//     gladLoadGLLoader((GLADloadproc) glfwGetProcAddress);
//
// O Vulkan é uma API explícita: não existe "contexto global" nem "máquina de
// estados". Tudo que o driver de OpenGL fazia por baixo dos panos (escolher a
// GPU, criar a fila de comandos, criar os buffers da tela, sincronizar o
// desenho com a apresentação na tela, alocar memória, ...) precisa ser feito
// manualmente. Todo esse código está isolado neste módulo, para que o arquivo
// "main.cpp" permaneça o mais parecido possível com o do código base.
//
// Referência da API: https://registry.khronos.org/vulkan/specs/1.3/html/
//
#include <vector>

#include <vulkan/vulkan.h>

struct GLFWwindow;

// Número de quadros que podem estar "em voo" (sendo processados pela GPU) ao
// mesmo tempo. Com 2, a CPU pode preparar o quadro N+1 enquanto a GPU ainda
// desenha o quadro N.
#define MAX_FRAMES_IN_FLIGHT 2

// Estrutura que guarda TODO o estado global do Vulkan utilizado pela
// aplicação. No OpenGL este estado ficava escondido dentro do driver.
struct VulkanContext
{
    // --- Instância, dispositivo lógico e filas ----------------------------
    VkInstance                 instance;
    VkDebugUtilsMessengerEXT   debug_messenger;
    bool                       validation_enabled;

    VkSurfaceKHR               surface;   // "tela" onde vamos desenhar (criada pela GLFW)

    VkPhysicalDevice           physical_device;           // a GPU escolhida
    VkPhysicalDeviceProperties physical_device_properties;
    VkPhysicalDeviceFeatures   physical_device_features;

    uint32_t                   graphics_queue_family;
    uint32_t                   present_queue_family;

    VkDevice                   device;          // dispositivo lógico
    VkQueue                    graphics_queue;  // fila onde submetemos comandos de desenho
    VkQueue                    present_queue;   // fila que apresenta a imagem na tela

    // --- Swapchain (conjunto de framebuffers da janela) -------------------
    VkSwapchainKHR             swapchain;
    VkFormat                   swapchain_image_format;
    VkExtent2D                 swapchain_extent;
    std::vector<VkImage>       swapchain_images;
    std::vector<VkImageView>   swapchain_image_views;
    std::vector<VkFramebuffer> swapchain_framebuffers;

    // --- Z-buffer (depth buffer) ------------------------------------------
    VkFormat                   depth_format;
    VkImage                    depth_image;
    VkDeviceMemory             depth_image_memory;
    VkImageView                depth_image_view;

    // --- Render pass ------------------------------------------------------
    // Descreve os "anexos" (attachments) utilizados durante a renderização de
    // um quadro: a imagem de cor e o Z-buffer, e o que fazer com eles no
    // início (limpar) e no fim (apresentar) do quadro.
    VkRenderPass               render_pass;

    // --- Command buffers e objetos de sincronização -----------------------
    VkCommandPool              command_pool;
    VkCommandBuffer            command_buffers[MAX_FRAMES_IN_FLIGHT];
    VkSemaphore                image_available_semaphores[MAX_FRAMES_IN_FLIGHT];
    std::vector<VkSemaphore>   render_finished_semaphores; // um por imagem do swapchain
    VkFence                    in_flight_fences[MAX_FRAMES_IN_FLIGHT];

    // --- Estado do laço de renderização -----------------------------------
    uint32_t                   current_frame;       // 0 .. MAX_FRAMES_IN_FLIGHT-1
    uint32_t                   current_image_index; // índice da imagem do swapchain deste quadro
    uint64_t                   frame_counter;       // número total de quadros iniciados
    bool                       framebuffer_resized; // setado pelo callback de redimensionamento
};

// Contexto global, definido em "vkcontext.cpp". Utilizamos uma variável global
// (assim como o código base em OpenGL utiliza várias) para manter o código
// didático e simples de ler.
extern VulkanContext g_Vk;

// --- Ciclo de vida do contexto -------------------------------------------
void VulkanContext_Init(GLFWwindow* window, bool enable_validation);
void VulkanContext_Destroy();
void VulkanContext_RecreateSwapchain(GLFWwindow* window);

// Inicia um quadro: espera a GPU liberar os recursos, adquire uma imagem do
// swapchain, começa a gravação do command buffer e inicia o render pass
// (limpando a tela com a cor "clear_color").
//
// Retorna "false" quando o quadro deve ser pulado (por exemplo, quando a
// janela foi redimensionada ou minimizada e o swapchain precisou ser
// recriado). Nesse caso, NÃO chame VulkanContext_EndFrame().
//
// Em caso de sucesso, "*out_command_buffer" recebe o command buffer onde os
// comandos de desenho deste quadro devem ser gravados.
bool VulkanContext_BeginFrame(GLFWwindow* window, const float clear_color[4], VkCommandBuffer* out_command_buffer);

// Finaliza o render pass, submete o command buffer para a GPU e apresenta a
// imagem na tela. É o equivalente ao glfwSwapBuffers() do código base.
void VulkanContext_EndFrame(GLFWwindow* window);

// --- Funções auxiliares ---------------------------------------------------

// Encontra um tipo de memória da GPU que satisfaça os requisitos pedidos.
uint32_t Vulkan_FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties);

// Cria um buffer (equivalente a um VBO/EBO/UBO do OpenGL) e aloca sua memória.
void Vulkan_CreateBuffer(
    VkDeviceSize          size,
    VkBufferUsageFlags    usage,
    VkMemoryPropertyFlags properties,
    VkBuffer*             out_buffer,
    VkDeviceMemory*       out_memory
);

// Copia "size" bytes de um buffer para outro, utilizando a GPU.
void Vulkan_CopyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size);

// Cria um buffer na memória da GPU (device-local) já preenchido com os dados
// apontados por "data". Internamente utiliza um "staging buffer". É o
// equivalente ao par glBufferData()/glBufferSubData() do código base.
void Vulkan_CreateBufferWithData(
    const void*        data,
    VkDeviceSize       size,
    VkBufferUsageFlags usage,
    VkBuffer*          out_buffer,
    VkDeviceMemory*    out_memory
);

// Começa/termina a gravação de um command buffer temporário, utilizado para
// operações que acontecem uma única vez (cópias de memória, transições de
// layout de imagens, geração de mipmaps, ...).
VkCommandBuffer Vulkan_BeginSingleTimeCommands();
void            Vulkan_EndSingleTimeCommands(VkCommandBuffer command_buffer);

// Cria uma imagem (equivalente a uma textura do OpenGL) e aloca sua memória.
void Vulkan_CreateImage(
    uint32_t              width,
    uint32_t              height,
    uint32_t              mip_levels,
    VkFormat              format,
    VkImageTiling         tiling,
    VkImageUsageFlags     usage,
    VkMemoryPropertyFlags properties,
    VkImage*              out_image,
    VkDeviceMemory*       out_memory
);

VkImageView Vulkan_CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip_levels);

// Muda o "layout" de uma imagem. Em Vulkan, uma imagem precisa estar no layout
// adequado para cada uso (destino de cópia, leitura por shader, ...).
void Vulkan_TransitionImageLayout(
    VkImage       image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    uint32_t      mip_levels
);

void Vulkan_CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);

// Gera os níveis de mipmap de uma imagem (equivalente ao glGenerateMipmap()).
// Ao final, a imagem fica no layout VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
void Vulkan_GenerateMipmaps(VkImage image, VkFormat format, int32_t width, int32_t height, uint32_t mip_levels);

// Retorna "true" se o formato suporta filtragem linear em operações de blit,
// o que é necessário para a geração de mipmaps feita acima.
bool Vulkan_FormatSupportsLinearBlit(VkFormat format);

// Lê um arquivo SPIR-V (.spv) do disco e cria um VkShaderModule. É o
// equivalente ao par glCreateShader()/glCompileShader() do código base --
// com a diferença de que o Vulkan não aceita código GLSL diretamente: o GLSL
// precisa ser compilado para SPIR-V *antes*, pelo compilador glslc.
VkShaderModule Vulkan_CreateShaderModuleFromFile(const char* spirv_filename);

#endif // _VKCONTEXT_H
