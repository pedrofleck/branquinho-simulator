// ===========================================================================
// vkcontext.cpp -- "Boilerplate" do Vulkan
// ===========================================================================
//
// Veja os comentários no arquivo "include/vkcontext.h" para uma explicação de
// alto nível sobre o que este módulo faz e por que ele existe.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <set>
#include <algorithm>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "utils.h"
#include "vkcontext.h"

// Contexto global (declarado como "extern" em vkcontext.h).
VulkanContext g_Vk = {};

// Camada de validação do Vulkan. Ela verifica, em tempo de execução, se a
// aplicação está utilizando a API corretamente, e imprime mensagens de erro
// bastante detalhadas. É o equivalente (muito mais poderoso) do glGetError()
// do OpenGL. Habilitada somente em builds de Debug.
static const char* const VALIDATION_LAYERS[] = {
    "VK_LAYER_KHRONOS_validation"
};

// Extensões que o dispositivo (GPU) precisa suportar. VK_KHR_swapchain é o que
// permite apresentar imagens em uma janela.
static const char* const DEVICE_EXTENSIONS[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// ===========================================================================
// Camada de validação e mensagens de debug
// ===========================================================================

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      message_severity,
    VkDebugUtilsMessageTypeFlagsEXT             message_type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
    void*                                       user_data
)
{
    (void)message_type;
    (void)user_data;

    // Ignoramos mensagens puramente informativas, para não poluir o terminal.
    if (message_severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return VK_FALSE;

    const char* severity = (message_severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                         ? "ERROR" : "WARNING";

    fprintf(stderr, "%s: Vulkan validation: %s\n", severity, callback_data->pMessage);
    fflush(stderr);

    return VK_FALSE;
}

static bool CheckValidationLayerSupport()
{
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, NULL);

    std::vector<VkLayerProperties> available(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available.data());

    for (size_t i = 0; i < sizeof(VALIDATION_LAYERS)/sizeof(VALIDATION_LAYERS[0]); ++i)
    {
        bool found = false;
        for (uint32_t j = 0; j < layer_count; ++j)
        {
            if (strcmp(VALIDATION_LAYERS[i], available[j].layerName) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }

    return true;
}

static void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info)
{
    info = {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = VulkanDebugCallback;
}

// As funções de debug do Vulkan são de uma extensão, e por isso precisam ser
// carregadas manualmente com vkGetInstanceProcAddr().
static void CreateDebugMessenger()
{
    VkDebugUtilsMessengerCreateInfoEXT info;
    PopulateDebugMessengerCreateInfo(info);

    PFN_vkCreateDebugUtilsMessengerEXT func = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(g_Vk.instance, "vkCreateDebugUtilsMessengerEXT");

    if (func != NULL)
        VK_CHECK(func(g_Vk.instance, &info, NULL, &g_Vk.debug_messenger));
    else
        fprintf(stderr, "WARNING: vkCreateDebugUtilsMessengerEXT não encontrada.\n");
}

static void DestroyDebugMessenger()
{
    if (g_Vk.debug_messenger == VK_NULL_HANDLE)
        return;

    PFN_vkDestroyDebugUtilsMessengerEXT func = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(g_Vk.instance, "vkDestroyDebugUtilsMessengerEXT");

    if (func != NULL)
        func(g_Vk.instance, g_Vk.debug_messenger, NULL);

    g_Vk.debug_messenger = VK_NULL_HANDLE;
}

// ===========================================================================
// Instância
// ===========================================================================

static void CreateInstance(bool enable_validation)
{
    if (enable_validation && !CheckValidationLayerSupport())
    {
        fprintf(stderr,
            "WARNING: Camada de validação \"VK_LAYER_KHRONOS_validation\" não\n"
            "         encontrada. Instale o Vulkan SDK para habilitá-la.\n"
            "         Continuando SEM validação.\n");
        enable_validation = false;
    }

    g_Vk.validation_enabled = enable_validation;

    VkApplicationInfo app_info = {};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "INF01047 - Trabalho Final";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName        = "cgvis-base-vulkan";
    app_info.engineVersion      = VK_MAKE_VERSION(1, 0, 0);
    // Utilizamos somente funcionalidades do núcleo do Vulkan 1.0 (mais a
    // extensão VK_KHR_swapchain), o que maximiza a compatibilidade com drivers
    // antigos.
    app_info.apiVersion         = VK_API_VERSION_1_0;

    // A GLFW nos informa quais extensões de instância são necessárias para
    // criar uma superfície de apresentação no sistema operacional atual
    // (VK_KHR_surface + VK_KHR_win32_surface, VK_KHR_xlib_surface, ...).
    uint32_t glfw_extension_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&glfw_extension_count);

    if (glfw_extensions == NULL)
    {
        fprintf(stderr,
            "ERROR: glfwGetRequiredInstanceExtensions() falhou. O sistema não\n"
            "       possui um loader do Vulkan instalado (vulkan-1.dll /\n"
            "       libvulkan.so). Atualize o driver da sua placa de vídeo.\n");
        std::exit(EXIT_FAILURE);
    }

    std::vector<const char*> extensions(glfw_extensions, glfw_extensions + glfw_extension_count);

    if (enable_validation)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo create_info = {};
    create_info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo        = &app_info;
    create_info.enabledExtensionCount   = (uint32_t)extensions.size();
    create_info.ppEnabledExtensionNames = extensions.data();

    // Este create info extra faz com que a validação também cubra as próprias
    // chamadas vkCreateInstance() e vkDestroyInstance().
    VkDebugUtilsMessengerCreateInfoEXT debug_info;

    if (enable_validation)
    {
        PopulateDebugMessengerCreateInfo(debug_info);
        create_info.enabledLayerCount   = (uint32_t)(sizeof(VALIDATION_LAYERS)/sizeof(VALIDATION_LAYERS[0]));
        create_info.ppEnabledLayerNames = VALIDATION_LAYERS;
        create_info.pNext               = &debug_info;
    }
    else
    {
        create_info.enabledLayerCount = 0;
    }

    VK_CHECK(vkCreateInstance(&create_info, NULL, &g_Vk.instance));
}

// ===========================================================================
// Escolha da GPU (physical device) e criação do dispositivo lógico
// ===========================================================================

// Procura, no dispositivo dado, uma família de filas que suporte comandos
// gráficos e uma que suporte apresentação na nossa superfície. Retorna "false"
// se alguma das duas não existir.
static bool FindQueueFamilies(VkPhysicalDevice device, uint32_t* out_graphics, uint32_t* out_present)
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);

    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    bool found_graphics = false;
    bool found_present  = false;

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!found_graphics && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            *out_graphics  = i;
            found_graphics = true;
        }

        VkBool32 present_support = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g_Vk.surface, &present_support);

        if (!found_present && present_support)
        {
            *out_present  = i;
            found_present = true;
        }

        // Preferimos usar a MESMA família para gráficos e apresentação, pois
        // isso evita ter que compartilhar as imagens do swapchain entre filas.
        if (present_support && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        {
            *out_graphics  = i;
            *out_present   = i;
            found_graphics = true;
            found_present  = true;
            break;
        }
    }

    return found_graphics && found_present;
}

static bool CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);

    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, available.data());

    for (size_t i = 0; i < sizeof(DEVICE_EXTENSIONS)/sizeof(DEVICE_EXTENSIONS[0]); ++i)
    {
        bool found = false;
        for (uint32_t j = 0; j < count; ++j)
        {
            if (strcmp(DEVICE_EXTENSIONS[i], available[j].extensionName) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }

    return true;
}

static bool IsDeviceSuitable(VkPhysicalDevice device, uint32_t* out_graphics, uint32_t* out_present)
{
    if (!FindQueueFamilies(device, out_graphics, out_present))
        return false;

    if (!CheckDeviceExtensionSupport(device))
        return false;

    // O dispositivo precisa suportar pelo menos um formato de imagem e um modo
    // de apresentação para a nossa superfície.
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, g_Vk.surface, &format_count, NULL);

    uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, g_Vk.surface, &present_mode_count, NULL);

    return format_count > 0 && present_mode_count > 0;
}

static void PickPhysicalDevice()
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(g_Vk.instance, &device_count, NULL);

    if (device_count == 0)
    {
        fprintf(stderr, "ERROR: Nenhuma GPU com suporte a Vulkan foi encontrada.\n");
        std::exit(EXIT_FAILURE);
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(g_Vk.instance, &device_count, devices.data());

    VkPhysicalDevice best          = VK_NULL_HANDLE;
    uint32_t         best_graphics = 0;
    uint32_t         best_present  = 0;
    int              best_score    = -1;

    for (uint32_t i = 0; i < device_count; ++i)
    {
        uint32_t graphics = 0;
        uint32_t present  = 0;

        if (!IsDeviceSuitable(devices[i], &graphics, &present))
            continue;

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);

        // Preferimos uma GPU dedicada a uma GPU integrada.
        int score = 1;
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score = 1000;
        else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score = 100;

        if (score > best_score)
        {
            best_score    = score;
            best          = devices[i];
            best_graphics = graphics;
            best_present  = present;
        }
    }

    if (best == VK_NULL_HANDLE)
    {
        fprintf(stderr,
            "ERROR: Nenhuma GPU adequada foi encontrada (é necessário suporte a\n"
            "       VK_KHR_swapchain e a apresentação em janela).\n");
        std::exit(EXIT_FAILURE);
    }

    g_Vk.physical_device       = best;
    g_Vk.graphics_queue_family = best_graphics;
    g_Vk.present_queue_family  = best_present;

    vkGetPhysicalDeviceProperties(g_Vk.physical_device, &g_Vk.physical_device_properties);
    vkGetPhysicalDeviceFeatures(g_Vk.physical_device, &g_Vk.physical_device_features);
}

static void CreateLogicalDevice()
{
    std::set<uint32_t> unique_families;
    unique_families.insert(g_Vk.graphics_queue_family);
    unique_families.insert(g_Vk.present_queue_family);

    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    float queue_priority = 1.0f;

    for (std::set<uint32_t>::iterator it = unique_families.begin(); it != unique_families.end(); ++it)
    {
        VkDeviceQueueCreateInfo info = {};
        info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = *it;
        info.queueCount       = 1;
        info.pQueuePriorities = &queue_priority;
        queue_infos.push_back(info);
    }

    // Habilitamos filtragem anisotrópica de texturas, se a GPU suportar. Isso
    // melhora bastante a qualidade das texturas vistas "de lado" (como o chão).
    VkPhysicalDeviceFeatures features = {};
    features.samplerAnisotropy = g_Vk.physical_device_features.samplerAnisotropy;

    VkDeviceCreateInfo create_info = {};
    create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount    = (uint32_t)queue_infos.size();
    create_info.pQueueCreateInfos       = queue_infos.data();
    create_info.pEnabledFeatures        = &features;
    create_info.enabledExtensionCount   = (uint32_t)(sizeof(DEVICE_EXTENSIONS)/sizeof(DEVICE_EXTENSIONS[0]));
    create_info.ppEnabledExtensionNames = DEVICE_EXTENSIONS;

    if (g_Vk.validation_enabled)
    {
        create_info.enabledLayerCount   = (uint32_t)(sizeof(VALIDATION_LAYERS)/sizeof(VALIDATION_LAYERS[0]));
        create_info.ppEnabledLayerNames = VALIDATION_LAYERS;
    }

    VK_CHECK(vkCreateDevice(g_Vk.physical_device, &create_info, NULL, &g_Vk.device));

    vkGetDeviceQueue(g_Vk.device, g_Vk.graphics_queue_family, 0, &g_Vk.graphics_queue);
    vkGetDeviceQueue(g_Vk.device, g_Vk.present_queue_family,  0, &g_Vk.present_queue);
}

// ===========================================================================
// Swapchain
// ===========================================================================

static VkSurfaceFormatKHR ChooseSwapSurfaceFormat()
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_Vk.physical_device, g_Vk.surface, &count, NULL);

    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_Vk.physical_device, g_Vk.surface, &count, formats.data());

    // IMPORTANTE: escolhemos um formato *UNORM* (e não *SRGB*) para a imagem
    // final da tela. O motivo é reproduzir exatamente o comportamento do código
    // base em OpenGL: lá, as texturas eram carregadas com o formato interno
    // GL_SRGB8 (portanto a GPU converte de sRGB para linear ao *ler* a textura),
    // a iluminação era calculada em espaço linear, e a correção gama final era
    // feita manualmente no fragment shader, com a linha:
    //
    //     color.rgb = pow(color.rgb, vec3(1.0,1.0,1.0)/2.2);
    //
    // Se usássemos um swapchain sRGB, a GPU aplicaria a conversão linear->sRGB
    // uma *segunda* vez, e a imagem ficaria lavada (clara demais).
    for (uint32_t i = 0; i < count; ++i)
    {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
             formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            return formats[i];
        }
    }

    fprintf(stderr,
        "WARNING: Nenhum formato UNORM disponível para o swapchain. As cores\n"
        "         podem ficar mais claras que o esperado (correção gama dupla).\n");

    return formats[0];
}

static VkPresentModeKHR ChooseSwapPresentMode()
{
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_Vk.physical_device, g_Vk.surface, &count, NULL);

    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_Vk.physical_device, g_Vk.surface, &count, modes.data());

    // VK_PRESENT_MODE_MAILBOX_KHR é o "triple buffering": não trava a aplicação
    // esperando o monitor, e mesmo assim não gera "screen tearing".
    for (uint32_t i = 0; i < count; ++i)
    {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            return VK_PRESENT_MODE_MAILBOX_KHR;
    }

    // VK_PRESENT_MODE_FIFO_KHR (v-sync) é o único cujo suporte é obrigatório.
    return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D ChooseSwapExtent(GLFWwindow* window, const VkSurfaceCapabilitiesKHR& capabilities)
{
    if (capabilities.currentExtent.width != 0xFFFFFFFFu)
        return capabilities.currentExtent;

    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);

    VkExtent2D extent;
    extent.width  = (uint32_t)width;
    extent.height = (uint32_t)height;

    if (extent.width < capabilities.minImageExtent.width)
        extent.width = capabilities.minImageExtent.width;
    if (extent.width > capabilities.maxImageExtent.width)
        extent.width = capabilities.maxImageExtent.width;

    if (extent.height < capabilities.minImageExtent.height)
        extent.height = capabilities.minImageExtent.height;
    if (extent.height > capabilities.maxImageExtent.height)
        extent.height = capabilities.maxImageExtent.height;

    return extent;
}

static void CreateSwapchain(GLFWwindow* window)
{
    VkSurfaceCapabilitiesKHR capabilities;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_Vk.physical_device, g_Vk.surface, &capabilities));

    VkSurfaceFormatKHR surface_format = ChooseSwapSurfaceFormat();
    VkPresentModeKHR   present_mode   = ChooseSwapPresentMode();
    VkExtent2D         extent         = ChooseSwapExtent(window, capabilities);

    uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount)
        image_count = capabilities.maxImageCount;

    VkSwapchainCreateInfoKHR create_info = {};
    create_info.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface          = g_Vk.surface;
    create_info.minImageCount    = image_count;
    create_info.imageFormat      = surface_format.format;
    create_info.imageColorSpace  = surface_format.colorSpace;
    create_info.imageExtent      = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    uint32_t families[2] = { g_Vk.graphics_queue_family, g_Vk.present_queue_family };

    if (g_Vk.graphics_queue_family != g_Vk.present_queue_family)
    {
        create_info.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices   = families;
    }
    else
    {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    create_info.preTransform   = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode    = present_mode;
    create_info.clipped        = VK_TRUE;
    create_info.oldSwapchain   = VK_NULL_HANDLE;

    VK_CHECK(vkCreateSwapchainKHR(g_Vk.device, &create_info, NULL, &g_Vk.swapchain));

    uint32_t actual_count = 0;
    vkGetSwapchainImagesKHR(g_Vk.device, g_Vk.swapchain, &actual_count, NULL);
    g_Vk.swapchain_images.resize(actual_count);
    vkGetSwapchainImagesKHR(g_Vk.device, g_Vk.swapchain, &actual_count, g_Vk.swapchain_images.data());

    g_Vk.swapchain_image_format = surface_format.format;
    g_Vk.swapchain_extent       = extent;

    g_Vk.swapchain_image_views.resize(actual_count);
    for (uint32_t i = 0; i < actual_count; ++i)
    {
        g_Vk.swapchain_image_views[i] = Vulkan_CreateImageView(
            g_Vk.swapchain_images[i],
            g_Vk.swapchain_image_format,
            VK_IMAGE_ASPECT_COLOR_BIT,
            1
        );
    }
}

// ===========================================================================
// Z-buffer (depth buffer)
// ===========================================================================

static VkFormat FindDepthFormat()
{
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT
    };

    for (size_t i = 0; i < sizeof(candidates)/sizeof(candidates[0]); ++i)
    {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(g_Vk.physical_device, candidates[i], &properties);

        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return candidates[i];
    }

    fprintf(stderr, "ERROR: Nenhum formato de Z-buffer suportado foi encontrado.\n");
    std::exit(EXIT_FAILURE);

    return VK_FORMAT_UNDEFINED;
}

static void CreateDepthResources()
{
    g_Vk.depth_format = FindDepthFormat();

    Vulkan_CreateImage(
        g_Vk.swapchain_extent.width,
        g_Vk.swapchain_extent.height,
        1,
        g_Vk.depth_format,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_Vk.depth_image,
        &g_Vk.depth_image_memory
    );

    g_Vk.depth_image_view = Vulkan_CreateImageView(
        g_Vk.depth_image,
        g_Vk.depth_format,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        1
    );
}

// ===========================================================================
// Render pass e framebuffers
// ===========================================================================

static void CreateRenderPass()
{
    // Anexo 0: a imagem de cor (uma das imagens do swapchain).
    VkAttachmentDescription color_attachment = {};
    color_attachment.format         = g_Vk.swapchain_image_format;
    color_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    // loadOp = CLEAR é o equivalente ao glClear(GL_COLOR_BUFFER_BIT).
    color_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    // Ao final do render pass, a imagem já fica pronta para ser apresentada.
    color_attachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Anexo 1: o Z-buffer.
    VkAttachmentDescription depth_attachment = {};
    depth_attachment.format         = g_Vk.depth_format;
    depth_attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    // loadOp = CLEAR é o equivalente ao glClear(GL_DEPTH_BUFFER_BIT).
    depth_attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // Não precisamos do Z-buffer depois que o quadro termina.
    depth_attachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_reference = {};
    color_reference.attachment = 0;
    color_reference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_reference = {};
    depth_reference.attachment = 1;
    depth_reference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;

    // Esta dependência garante que a GPU só comece a escrever na imagem depois
    // que ela tiver sido liberada pela apresentação do quadro anterior.
    VkSubpassDependency dependency = {};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                             | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[2] = { color_attachment, depth_attachment };

    VkRenderPassCreateInfo create_info = {};
    create_info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 2;
    create_info.pAttachments    = attachments;
    create_info.subpassCount    = 1;
    create_info.pSubpasses      = &subpass;
    create_info.dependencyCount = 1;
    create_info.pDependencies   = &dependency;

    VK_CHECK(vkCreateRenderPass(g_Vk.device, &create_info, NULL, &g_Vk.render_pass));
}

static void CreateFramebuffers()
{
    g_Vk.swapchain_framebuffers.resize(g_Vk.swapchain_image_views.size());

    for (size_t i = 0; i < g_Vk.swapchain_image_views.size(); ++i)
    {
        VkImageView attachments[2] = {
            g_Vk.swapchain_image_views[i],
            g_Vk.depth_image_view
        };

        VkFramebufferCreateInfo create_info = {};
        create_info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        create_info.renderPass      = g_Vk.render_pass;
        create_info.attachmentCount = 2;
        create_info.pAttachments    = attachments;
        create_info.width           = g_Vk.swapchain_extent.width;
        create_info.height          = g_Vk.swapchain_extent.height;
        create_info.layers          = 1;

        VK_CHECK(vkCreateFramebuffer(g_Vk.device, &create_info, NULL, &g_Vk.swapchain_framebuffers[i]));
    }
}

// ===========================================================================
// Command buffers e objetos de sincronização
// ===========================================================================

static void CreateCommandPoolAndBuffers()
{
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = g_Vk.graphics_queue_family;

    VK_CHECK(vkCreateCommandPool(g_Vk.device, &pool_info, NULL, &g_Vk.command_pool));

    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool        = g_Vk.command_pool;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;

    VK_CHECK(vkAllocateCommandBuffers(g_Vk.device, &alloc_info, g_Vk.command_buffers));
}

static void DestroySyncObjects()
{
    for (size_t i = 0; i < g_Vk.render_finished_semaphores.size(); ++i)
        vkDestroySemaphore(g_Vk.device, g_Vk.render_finished_semaphores[i], NULL);

    g_Vk.render_finished_semaphores.clear();
}

// Cria os semáforos que sinalizam "terminei de desenhar esta imagem". Existe um
// por imagem do swapchain (e não por quadro em voo), pois o semáforo é esperado
// pela operação de apresentação, que é indexada pela imagem.
static void CreateRenderFinishedSemaphores()
{
    DestroySyncObjects();

    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    g_Vk.render_finished_semaphores.resize(g_Vk.swapchain_images.size());

    for (size_t i = 0; i < g_Vk.render_finished_semaphores.size(); ++i)
        VK_CHECK(vkCreateSemaphore(g_Vk.device, &semaphore_info, NULL, &g_Vk.render_finished_semaphores[i]));
}

static void CreateFrameSyncObjects()
{
    VkSemaphoreCreateInfo semaphore_info = {};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Criamos a fence já sinalizada, para que o primeiro quadro não fique
    // esperando para sempre por uma GPU que ainda não desenhou nada.
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        VK_CHECK(vkCreateSemaphore(g_Vk.device, &semaphore_info, NULL, &g_Vk.image_available_semaphores[i]));
        VK_CHECK(vkCreateFence(g_Vk.device, &fence_info, NULL, &g_Vk.in_flight_fences[i]));
    }
}

// ===========================================================================
// Inicialização, destruição e recriação do swapchain
// ===========================================================================

void VulkanContext_Init(GLFWwindow* window, bool enable_validation)
{
    if (!glfwVulkanSupported())
    {
        fprintf(stderr,
            "ERROR: Vulkan não está disponível neste sistema.\n"
            "       Atualize o driver da sua placa de vídeo.\n");
        std::exit(EXIT_FAILURE);
    }

    CreateInstance(enable_validation);

    if (g_Vk.validation_enabled)
        CreateDebugMessenger();

    // A GLFW cria a superfície de apresentação de forma portável, escondendo as
    // diferenças entre Win32, X11, Wayland e Cocoa.
    VK_CHECK(glfwCreateWindowSurface(g_Vk.instance, window, NULL, &g_Vk.surface));

    PickPhysicalDevice();
    CreateLogicalDevice();

    CreateSwapchain(window);
    CreateDepthResources();
    CreateRenderPass();
    CreateFramebuffers();

    CreateCommandPoolAndBuffers();
    CreateFrameSyncObjects();
    CreateRenderFinishedSemaphores();

    g_Vk.current_frame       = 0;
    g_Vk.frame_counter       = 0;
    g_Vk.framebuffer_resized = false;
}

static void CleanupSwapchain()
{
    for (size_t i = 0; i < g_Vk.swapchain_framebuffers.size(); ++i)
        vkDestroyFramebuffer(g_Vk.device, g_Vk.swapchain_framebuffers[i], NULL);
    g_Vk.swapchain_framebuffers.clear();

    vkDestroyImageView(g_Vk.device, g_Vk.depth_image_view, NULL);
    vkDestroyImage(g_Vk.device, g_Vk.depth_image, NULL);
    vkFreeMemory(g_Vk.device, g_Vk.depth_image_memory, NULL);

    for (size_t i = 0; i < g_Vk.swapchain_image_views.size(); ++i)
        vkDestroyImageView(g_Vk.device, g_Vk.swapchain_image_views[i], NULL);
    g_Vk.swapchain_image_views.clear();

    vkDestroySwapchainKHR(g_Vk.device, g_Vk.swapchain, NULL);
    g_Vk.swapchain = VK_NULL_HANDLE;
}

void VulkanContext_RecreateSwapchain(GLFWwindow* window)
{
    // Se a janela estiver minimizada, seu tamanho é 0x0 e não é possível criar
    // um swapchain. Ficamos parados até a janela voltar a ter área visível.
    int width  = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    while (width == 0 || height == 0)
    {
        glfwGetFramebufferSize(window, &width, &height);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(g_Vk.device);

    CleanupSwapchain();

    CreateSwapchain(window);
    CreateDepthResources();
    CreateFramebuffers();
    // O número de imagens do swapchain pode ter mudado.
    CreateRenderFinishedSemaphores();

    g_Vk.framebuffer_resized = false;
}

void VulkanContext_Destroy()
{
    vkDeviceWaitIdle(g_Vk.device);

    CleanupSwapchain();
    DestroySyncObjects();

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        vkDestroySemaphore(g_Vk.device, g_Vk.image_available_semaphores[i], NULL);
        vkDestroyFence(g_Vk.device, g_Vk.in_flight_fences[i], NULL);
    }

    vkDestroyCommandPool(g_Vk.device, g_Vk.command_pool, NULL);
    vkDestroyRenderPass(g_Vk.device, g_Vk.render_pass, NULL);
    vkDestroyDevice(g_Vk.device, NULL);

    DestroyDebugMessenger();

    vkDestroySurfaceKHR(g_Vk.instance, g_Vk.surface, NULL);
    vkDestroyInstance(g_Vk.instance, NULL);
}

// ===========================================================================
// Laço de renderização
// ===========================================================================

bool VulkanContext_BeginFrame(GLFWwindow* window, const float clear_color[4], VkCommandBuffer* out_command_buffer)
{
    // Esperamos a GPU terminar de desenhar o quadro que usou estes mesmos
    // recursos (command buffer, semáforos, buffers de uniformes, ...).
    VK_CHECK(vkWaitForFences(g_Vk.device, 1, &g_Vk.in_flight_fences[g_Vk.current_frame], VK_TRUE, UINT64_MAX));

    if (g_Vk.framebuffer_resized)
    {
        VulkanContext_RecreateSwapchain(window);
        return false;
    }

    uint32_t image_index = 0;
    VkResult result = vkAcquireNextImageKHR(
        g_Vk.device,
        g_Vk.swapchain,
        UINT64_MAX,
        g_Vk.image_available_semaphores[g_Vk.current_frame],
        VK_NULL_HANDLE,
        &image_index
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        VulkanContext_RecreateSwapchain(window);
        return false;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        fprintf(stderr, "ERROR: vkAcquireNextImageKHR() retornou \"%s\".\n", VulkanResultString(result));
        std::exit(EXIT_FAILURE);
    }

    g_Vk.current_image_index = image_index;

    // Só resetamos a fence depois de termos certeza de que vamos submeter
    // trabalho para a GPU neste quadro (caso contrário, teríamos um deadlock).
    VK_CHECK(vkResetFences(g_Vk.device, 1, &g_Vk.in_flight_fences[g_Vk.current_frame]));

    VkCommandBuffer command_buffer = g_Vk.command_buffers[g_Vk.current_frame];

    VK_CHECK(vkResetCommandBuffer(command_buffer, 0));

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

    // Valores usados para "limpar" a tela e o Z-buffer no início do quadro.
    // Equivalente ao par glClearColor() + glClear() do código base.
    VkClearValue clear_values[2] = {};
    clear_values[0].color.float32[0] = clear_color[0];
    clear_values[0].color.float32[1] = clear_color[1];
    clear_values[0].color.float32[2] = clear_color[2];
    clear_values[0].color.float32[3] = clear_color[3];
    // Em Vulkan o Z-buffer vai de 0.0 (perto) a 1.0 (longe); em OpenGL ele ia
    // de -1.0 a 1.0. Veja Matrix_Vulkan_Clip() em "matrices.h".
    clear_values[1].depthStencil.depth   = 1.0f;
    clear_values[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo render_pass_info = {};
    render_pass_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass        = g_Vk.render_pass;
    render_pass_info.framebuffer       = g_Vk.swapchain_framebuffers[image_index];
    render_pass_info.renderArea.offset.x = 0;
    render_pass_info.renderArea.offset.y = 0;
    render_pass_info.renderArea.extent = g_Vk.swapchain_extent;
    render_pass_info.clearValueCount   = 2;
    render_pass_info.pClearValues      = clear_values;

    vkCmdBeginRenderPass(command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    // Viewport e scissor são estados dinâmicos do pipeline. Assim, quando a
    // janela é redimensionada, não precisamos recriar o pipeline gráfico --
    // basta regravar estes dois comandos. É o equivalente ao glViewport().
    VkViewport viewport = {};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = (float)g_Vk.swapchain_extent.width;
    viewport.height   = (float)g_Vk.swapchain_extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent   = g_Vk.swapchain_extent;
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);

    g_Vk.frame_counter += 1;

    *out_command_buffer = command_buffer;

    return true;
}

void VulkanContext_EndFrame(GLFWwindow* window)
{
    VkCommandBuffer command_buffer = g_Vk.command_buffers[g_Vk.current_frame];

    vkCmdEndRenderPass(command_buffer);
    VK_CHECK(vkEndCommandBuffer(command_buffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo submit_info = {};
    submit_info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount   = 1;
    submit_info.pWaitSemaphores      = &g_Vk.image_available_semaphores[g_Vk.current_frame];
    submit_info.pWaitDstStageMask    = &wait_stage;
    submit_info.commandBufferCount   = 1;
    submit_info.pCommandBuffers      = &command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores    = &g_Vk.render_finished_semaphores[g_Vk.current_image_index];

    VK_CHECK(vkQueueSubmit(g_Vk.graphics_queue, 1, &submit_info, g_Vk.in_flight_fences[g_Vk.current_frame]));

    // A apresentação é o equivalente ao glfwSwapBuffers() do código base.
    VkPresentInfoKHR present_info = {};
    present_info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = &g_Vk.render_finished_semaphores[g_Vk.current_image_index];
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &g_Vk.swapchain;
    present_info.pImageIndices      = &g_Vk.current_image_index;

    VkResult result = vkQueuePresentKHR(g_Vk.present_queue, &present_info);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || g_Vk.framebuffer_resized)
    {
        VulkanContext_RecreateSwapchain(window);
    }
    else if (result != VK_SUCCESS)
    {
        fprintf(stderr, "ERROR: vkQueuePresentKHR() retornou \"%s\".\n", VulkanResultString(result));
        std::exit(EXIT_FAILURE);
    }

    g_Vk.current_frame = (g_Vk.current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

// ===========================================================================
// Funções auxiliares: memória, buffers e imagens
// ===========================================================================

uint32_t Vulkan_FindMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(g_Vk.physical_device, &memory_properties);

    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if ((type_filter & (1u << i)) &&
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }

    fprintf(stderr, "ERROR: Nenhum tipo de memória adequado foi encontrado na GPU.\n");
    std::exit(EXIT_FAILURE);

    return 0;
}

void Vulkan_CreateBuffer(
    VkDeviceSize          size,
    VkBufferUsageFlags    usage,
    VkMemoryPropertyFlags properties,
    VkBuffer*             out_buffer,
    VkDeviceMemory*       out_memory
)
{
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size        = size;
    buffer_info.usage       = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateBuffer(g_Vk.device, &buffer_info, NULL, out_buffer));

    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g_Vk.device, *out_buffer, &requirements);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = requirements.size;
    alloc_info.memoryTypeIndex = Vulkan_FindMemoryType(requirements.memoryTypeBits, properties);

    VK_CHECK(vkAllocateMemory(g_Vk.device, &alloc_info, NULL, out_memory));
    VK_CHECK(vkBindBufferMemory(g_Vk.device, *out_buffer, *out_memory, 0));
}

VkCommandBuffer Vulkan_BeginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo alloc_info = {};
    alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandPool        = g_Vk.command_pool;
    alloc_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer;
    VK_CHECK(vkAllocateCommandBuffers(g_Vk.device, &alloc_info, &command_buffer));

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VK_CHECK(vkBeginCommandBuffer(command_buffer, &begin_info));

    return command_buffer;
}

void Vulkan_EndSingleTimeCommands(VkCommandBuffer command_buffer)
{
    VK_CHECK(vkEndCommandBuffer(command_buffer));

    VkSubmitInfo submit_info = {};
    submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers    = &command_buffer;

    VK_CHECK(vkQueueSubmit(g_Vk.graphics_queue, 1, &submit_info, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(g_Vk.graphics_queue));

    vkFreeCommandBuffers(g_Vk.device, g_Vk.command_pool, 1, &command_buffer);
}

void Vulkan_CopyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size)
{
    VkCommandBuffer command_buffer = Vulkan_BeginSingleTimeCommands();

    VkBufferCopy region = {};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size      = size;

    vkCmdCopyBuffer(command_buffer, src, dst, 1, &region);

    Vulkan_EndSingleTimeCommands(command_buffer);
}

void Vulkan_CreateBufferWithData(
    const void*        data,
    VkDeviceSize       size,
    VkBufferUsageFlags usage,
    VkBuffer*          out_buffer,
    VkDeviceMemory*    out_memory
)
{
    // 1) Criamos um "staging buffer" numa memória visível pela CPU e copiamos
    //    os dados para lá.
    VkBuffer       staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;

    Vulkan_CreateBuffer(
        size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging_buffer,
        &staging_memory
    );

    void* mapped = NULL;
    VK_CHECK(vkMapMemory(g_Vk.device, staging_memory, 0, size, 0, &mapped));
    memcpy(mapped, data, (size_t)size);
    vkUnmapMemory(g_Vk.device, staging_memory);

    // 2) Criamos o buffer final na memória rápida da GPU (device-local) e
    //    pedimos para a GPU copiar os dados do staging buffer para ele.
    Vulkan_CreateBuffer(
        size,
        usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        out_buffer,
        out_memory
    );

    Vulkan_CopyBuffer(staging_buffer, *out_buffer, size);

    vkDestroyBuffer(g_Vk.device, staging_buffer, NULL);
    vkFreeMemory(g_Vk.device, staging_memory, NULL);
}

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
)
{
    VkImageCreateInfo image_info = {};
    image_info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType     = VK_IMAGE_TYPE_2D;
    image_info.extent.width  = width;
    image_info.extent.height = height;
    image_info.extent.depth  = 1;
    image_info.mipLevels     = mip_levels;
    image_info.arrayLayers   = 1;
    image_info.format        = format;
    image_info.tiling        = tiling;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage         = usage;
    image_info.samples       = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    VK_CHECK(vkCreateImage(g_Vk.device, &image_info, NULL, out_image));

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g_Vk.device, *out_image, &requirements);

    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize  = requirements.size;
    alloc_info.memoryTypeIndex = Vulkan_FindMemoryType(requirements.memoryTypeBits, properties);

    VK_CHECK(vkAllocateMemory(g_Vk.device, &alloc_info, NULL, out_memory));
    VK_CHECK(vkBindImageMemory(g_Vk.device, *out_image, *out_memory, 0));
}

VkImageView Vulkan_CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip_levels)
{
    VkImageViewCreateInfo view_info = {};
    view_info.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image                           = image;
    view_info.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format                          = format;
    view_info.subresourceRange.aspectMask     = aspect;
    view_info.subresourceRange.baseMipLevel   = 0;
    view_info.subresourceRange.levelCount     = mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount     = 1;

    VkImageView image_view = VK_NULL_HANDLE;
    VK_CHECK(vkCreateImageView(g_Vk.device, &view_info, NULL, &image_view));

    return image_view;
}

void Vulkan_TransitionImageLayout(
    VkImage       image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    uint32_t      mip_levels
)
{
    VkCommandBuffer command_buffer = Vulkan_BeginSingleTimeCommands();

    VkImageMemoryBarrier barrier = {};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout                       = old_layout;
    barrier.newLayout                       = new_layout;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.image                           = image;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel   = 0;
    barrier.subresourceRange.levelCount     = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;

    VkPipelineStageFlags source_stage      = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED &&
        new_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
    {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        source_stage          = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destination_stage     = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    else if (old_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        source_stage          = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination_stage     = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    else
    {
        fprintf(stderr, "ERROR: Transição de layout de imagem não suportada.\n");
        std::exit(EXIT_FAILURE);
    }

    vkCmdPipelineBarrier(
        command_buffer,
        source_stage, destination_stage,
        0,
        0, NULL,
        0, NULL,
        1, &barrier
    );

    Vulkan_EndSingleTimeCommands(command_buffer);
}

void Vulkan_CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
    VkCommandBuffer command_buffer = Vulkan_BeginSingleTimeCommands();

    VkBufferImageCopy region = {};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset.x                   = 0;
    region.imageOffset.y                   = 0;
    region.imageOffset.z                   = 0;
    region.imageExtent.width               = width;
    region.imageExtent.height              = height;
    region.imageExtent.depth               = 1;

    vkCmdCopyBufferToImage(
        command_buffer,
        buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    Vulkan_EndSingleTimeCommands(command_buffer);
}

bool Vulkan_FormatSupportsLinearBlit(VkFormat format)
{
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(g_Vk.physical_device, format, &properties);

    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
}

void Vulkan_GenerateMipmaps(VkImage image, VkFormat format, int32_t width, int32_t height, uint32_t mip_levels)
{
    if (!Vulkan_FormatSupportsLinearBlit(format))
    {
        fprintf(stderr, "ERROR: O formato da textura não suporta geração de mipmaps por blit.\n");
        std::exit(EXIT_FAILURE);
    }

    VkCommandBuffer command_buffer = Vulkan_BeginSingleTimeCommands();

    VkImageMemoryBarrier barrier = {};
    barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image                           = image;
    barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount     = 1;
    barrier.subresourceRange.levelCount     = 1;

    int32_t mip_width  = width;
    int32_t mip_height = height;

    // Cada nível de mipmap é gerado reduzindo o nível anterior pela metade,
    // utilizando uma operação de "blit" com filtragem linear.
    for (uint32_t i = 1; i < mip_levels; ++i)
    {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier
        );

        VkImageBlit blit = {};
        blit.srcOffsets[0].x                 = 0;
        blit.srcOffsets[0].y                 = 0;
        blit.srcOffsets[0].z                 = 0;
        blit.srcOffsets[1].x                 = mip_width;
        blit.srcOffsets[1].y                 = mip_height;
        blit.srcOffsets[1].z                 = 1;
        blit.srcSubresource.aspectMask       = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel         = i - 1;
        blit.srcSubresource.baseArrayLayer   = 0;
        blit.srcSubresource.layerCount       = 1;
        blit.dstOffsets[0].x                 = 0;
        blit.dstOffsets[0].y                 = 0;
        blit.dstOffsets[0].z                 = 0;
        blit.dstOffsets[1].x                 = mip_width  > 1 ? mip_width  / 2 : 1;
        blit.dstOffsets[1].y                 = mip_height > 1 ? mip_height / 2 : 1;
        blit.dstOffsets[1].z                 = 1;
        blit.dstSubresource.aspectMask       = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel         = i;
        blit.dstSubresource.baseArrayLayer   = 0;
        blit.dstSubresource.layerCount       = 1;

        vkCmdBlitImage(
            command_buffer,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit,
            VK_FILTER_LINEAR
        );

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &barrier
        );

        if (mip_width  > 1) mip_width  /= 2;
        if (mip_height > 1) mip_height /= 2;
    }

    // O último nível de mipmap ainda está em TRANSFER_DST_OPTIMAL.
    barrier.subresourceRange.baseMipLevel = mip_levels - 1;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &barrier
    );

    Vulkan_EndSingleTimeCommands(command_buffer);
}

// ===========================================================================
// Shaders
// ===========================================================================

VkShaderModule Vulkan_CreateShaderModuleFromFile(const char* spirv_filename)
{
    std::ifstream file(spirv_filename, std::ios::ate | std::ios::binary);

    if (!file.is_open())
    {
        fprintf(stderr,
            "ERROR: Não foi possível abrir o arquivo SPIR-V \"%s\".\n"
            "       Os shaders GLSL precisam ser compilados para SPIR-V pelo\n"
            "       compilador \"glslc\" antes de serem utilizados. Isso é feito\n"
            "       automaticamente pelo CMake durante a compilação do projeto.\n",
            spirv_filename);
        return VK_NULL_HANDLE;
    }

    size_t file_size = (size_t)file.tellg();

    // O SPIR-V é um formato binário formado por palavras de 32 bits.
    if (file_size == 0 || (file_size % 4) != 0)
    {
        fprintf(stderr, "ERROR: \"%s\" não é um arquivo SPIR-V válido.\n", spirv_filename);
        return VK_NULL_HANDLE;
    }

    std::vector<char> code(file_size);
    file.seekg(0);
    file.read(code.data(), (std::streamsize)file_size);
    file.close();

    VkShaderModuleCreateInfo create_info = {};
    create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = file_size;
    create_info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shader_module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(g_Vk.device, &create_info, NULL, &shader_module);

    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "ERROR: vkCreateShaderModule(\"%s\") retornou \"%s\".\n",
                spirv_filename, VulkanResultString(result));
        return VK_NULL_HANDLE;
    }

    return shader_module;
}

// vim: set spell spelllang=pt_br :
