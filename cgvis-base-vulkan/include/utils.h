#ifndef _UTILS_H
#define _UTILS_H

#include <cstdio>
#include <cstdlib>

#include <vulkan/vulkan.h>

// Converte um código de erro VkResult para uma string legível. O Vulkan, ao
// contrário do OpenGL, não possui uma função como glGetError(): TODAS as
// funções que podem falhar retornam um VkResult, e cabe à aplicação verificar
// esse retorno. A macro VK_CHECK() logo abaixo automatiza essa verificação.
static const char* VulkanResultString(VkResult result)
{
    switch (result)
    {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT:    return "VK_ERROR_VALIDATION_FAILED_EXT";
        default:                                return "VK_ERROR_UNKNOWN";
    }
}

// Verifica o retorno de uma chamada Vulkan. Em caso de erro, imprime o nome da
// chamada, o arquivo e a linha, e encerra o programa.
//
// Esta função é o equivalente, em Vulkan, da função glCheckError() que existia
// no código base em OpenGL. A diferença é que aqui a verificação é feita
// chamada-a-chamada (e não "a qualquer momento"), pois o Vulkan não mantém um
// estado global de erro.
static VkResult VulkanCheckError_(VkResult result, const char* call, const char* file, int line)
{
    if (result != VK_SUCCESS)
    {
        fprintf(stderr, "ERROR: Vulkan \"%s\" retornou \"%s\" em \"%s\" (linha %d)\n",
                call, VulkanResultString(result), file, line);
        std::exit(EXIT_FAILURE);
    }
    return result;
}

#define VK_CHECK(call) VulkanCheckError_((call), #call, __FILE__, __LINE__)

#endif // _UTILS_H
