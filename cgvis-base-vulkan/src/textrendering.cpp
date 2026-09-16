// ===========================================================================
// textrendering.cpp -- Rasterização de texto com Vulkan
// ===========================================================================
//
// Based on http://hamelot.io/visualization/opengl-text-without-any-external-libraries/
//   and on https://github.com/rougier/freetype-gl
//
// FONTE: a lógica de posicionamento dos glifos (o laço dentro de
// TextRendering_PrintString(), o cálculo de x0/y0/x1/y1 e de s0/t0/s1/t1, e as
// funções TextRendering_Print*) foi mantida idêntica à do código base em
// OpenGL da disciplina INF01047 (arquivo "src/textrendering.cpp" do repositório
// https://github.com/cgvis-inf-ufrgs/cgvis-base-trabalho-final). O que foi
// reescrito aqui é apenas a parte de comunicação com a API gráfica, que passou
// de OpenGL para Vulkan.
//
// A interface pública deste arquivo é EXATAMENTE a mesma do código base, de
// modo que o "main.cpp" possa chamar TextRendering_PrintString() da mesma
// forma que chamava em OpenGL.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "utils.h"
#include "vkcontext.h"
#include "dejavufont.h"

// Número máximo de letras que podem ser desenhadas em um único quadro. Cada
// letra ocupa 6 vértices * 4 floats = 96 bytes no buffer de vértices.
#define TEXT_MAX_GLYPHS_PER_FRAME 8192

#define TEXT_VERTICES_PER_GLYPH 6
#define TEXT_FLOATS_PER_VERTEX  4
#define TEXT_BYTES_PER_GLYPH    (TEXT_VERTICES_PER_GLYPH * TEXT_FLOATS_PER_VERTEX * sizeof(float))

// Cada vértice do texto carrega a posição em NDC (x,y) e a coordenada de
// textura dentro do atlas de glifos (s,t).
struct TextVertex
{
    float x, y, s, t;
};

// --- Recursos Vulkan utilizados pelo renderizador de texto -----------------

// Atlas de glifos (equivalente à textura do código base).
static VkImage        g_TextFontImage        = VK_NULL_HANDLE;
static VkDeviceMemory g_TextFontImageMemory  = VK_NULL_HANDLE;
static VkImageView    g_TextFontImageView    = VK_NULL_HANDLE;
static VkSampler      g_TextFontSampler      = VK_NULL_HANDLE;

// Descriptor set que liga o atlas de glifos ao Fragment Shader.
static VkDescriptorSetLayout g_TextDescriptorSetLayout = VK_NULL_HANDLE;
static VkDescriptorPool      g_TextDescriptorPool      = VK_NULL_HANDLE;
static VkDescriptorSet       g_TextDescriptorSet       = VK_NULL_HANDLE;

// Pipeline gráfico (equivalente ao "programa de GPU" do código base).
static VkPipelineLayout g_TextPipelineLayout = VK_NULL_HANDLE;
static VkPipeline       g_TextPipeline       = VK_NULL_HANDLE;

// Buffers de vértices dinâmicos, um por quadro "em voo". Ficam permanentemente
// mapeados na memória da CPU, de modo que escrever um glifo é apenas um
// memcpy(). É o equivalente ao glBufferSubData() feito a cada letra no código
// base em OpenGL.
static VkBuffer        g_TextVertexBuffers[MAX_FRAMES_IN_FLIGHT]       = {};
static VkDeviceMemory  g_TextVertexBufferMemories[MAX_FRAMES_IN_FLIGHT] = {};
static void*           g_TextVertexBufferMapped[MAX_FRAMES_IN_FLIGHT]  = {};

// Quantos glifos já foram gravados no buffer do quadro atual, e em qual quadro
// estamos. Quando o número do quadro muda, o contador é zerado.
static uint32_t g_TextGlyphsUsed   = 0;
static uint64_t g_TextLastFrame    = 0;
static bool     g_TextOverflowWarned = false;

// ===========================================================================
// Criação do atlas de glifos
// ===========================================================================

static void TextRendering_CreateFontTexture()
{
    VkDeviceSize image_size = (VkDeviceSize)(dejavufont.tex_width * dejavufont.tex_height);

    // 1) Copiamos os pixels da fonte para um "staging buffer" visível pela CPU.
    VkBuffer       staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;

    Vulkan_CreateBuffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &staging_buffer,
        &staging_memory
    );

    void* mapped = NULL;
    VK_CHECK(vkMapMemory(g_Vk.device, staging_memory, 0, image_size, 0, &mapped));
    memcpy(mapped, dejavufont.tex_data, (size_t)image_size);
    vkUnmapMemory(g_Vk.device, staging_memory);

    // 2) Criamos a imagem na GPU. O atlas possui um único canal de 8 bits, que
    //    no código base era o formato GL_R8.
    Vulkan_CreateImage(
        (uint32_t)dejavufont.tex_width,
        (uint32_t)dejavufont.tex_height,
        1,
        VK_FORMAT_R8_UNORM,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        &g_TextFontImage,
        &g_TextFontImageMemory
    );

    // 3) Transferimos os pixels do staging buffer para a imagem.
    Vulkan_TransitionImageLayout(g_TextFontImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    Vulkan_CopyBufferToImage(staging_buffer, g_TextFontImage, (uint32_t)dejavufont.tex_width, (uint32_t)dejavufont.tex_height);
    Vulkan_TransitionImageLayout(g_TextFontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

    vkDestroyBuffer(g_Vk.device, staging_buffer, NULL);
    vkFreeMemory(g_Vk.device, staging_memory, NULL);

    g_TextFontImageView = Vulkan_CreateImageView(g_TextFontImage, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1);

    // 4) Criamos o amostrador. Os parâmetros abaixo são os mesmos utilizados
    //    pelo código base: GL_CLAMP_TO_EDGE e GL_LINEAR (sem mipmaps).
    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter     = VK_FILTER_LINEAR;
    sampler_info.minFilter     = VK_FILTER_LINEAR;
    sampler_info.addressModeU  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW  = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy    = 1.0f;
    sampler_info.borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp     = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode    = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.mipLodBias    = 0.0f;
    sampler_info.minLod        = 0.0f;
    sampler_info.maxLod        = 0.0f;

    VK_CHECK(vkCreateSampler(g_Vk.device, &sampler_info, NULL, &g_TextFontSampler));
}

// ===========================================================================
// Descriptor set
// ===========================================================================

static void TextRendering_CreateDescriptorSet()
{
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info = {};
    layout_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = 1;
    layout_info.pBindings    = &binding;

    VK_CHECK(vkCreateDescriptorSetLayout(g_Vk.device, &layout_info, NULL, &g_TextDescriptorSetLayout));

    VkDescriptorPoolSize pool_size = {};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;
    pool_info.maxSets       = 1;

    VK_CHECK(vkCreateDescriptorPool(g_Vk.device, &pool_info, NULL, &g_TextDescriptorPool));

    VkDescriptorSetAllocateInfo alloc_info = {};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool     = g_TextDescriptorPool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &g_TextDescriptorSetLayout;

    VK_CHECK(vkAllocateDescriptorSets(g_Vk.device, &alloc_info, &g_TextDescriptorSet));

    VkDescriptorImageInfo image_info = {};
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView   = g_TextFontImageView;
    image_info.sampler     = g_TextFontSampler;

    VkWriteDescriptorSet write = {};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = g_TextDescriptorSet;
    write.dstBinding      = 0;
    write.dstArrayElement = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &image_info;

    vkUpdateDescriptorSets(g_Vk.device, 1, &write, 0, NULL);
}

// ===========================================================================
// Pipeline gráfico
// ===========================================================================

static void TextRendering_CreatePipeline()
{
    VkShaderModule vertex_module   = Vulkan_CreateShaderModuleFromFile("../shaders/shader_text.vert.spv");
    VkShaderModule fragment_module = Vulkan_CreateShaderModuleFromFile("../shaders/shader_text.frag.spv");

    if (vertex_module == VK_NULL_HANDLE || fragment_module == VK_NULL_HANDLE)
    {
        fprintf(stderr, "ERROR: Não foi possível carregar os shaders de texto.\n");
        std::exit(EXIT_FAILURE);
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vertex_binding = {};
    vertex_binding.binding   = 0;
    vertex_binding.stride    = sizeof(TextVertex);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vertex_attribute = {};
    vertex_attribute.location = 0;
    vertex_attribute.binding  = 0;
    vertex_attribute.format   = VK_FORMAT_R32G32B32A32_SFLOAT;
    vertex_attribute.offset   = 0;

    VkPipelineVertexInputStateCreateInfo vertex_input = {};
    vertex_input.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount   = 1;
    vertex_input.pVertexBindingDescriptions      = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 1;
    vertex_input.pVertexAttributeDescriptions    = &vertex_attribute;

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
    input_assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state = {};
    viewport_state.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth   = 1.0f;
    // O texto é desenhado de frente para a câmera; não há o que descartar.
    rasterizer.cullMode    = VK_CULL_MODE_NONE;
    rasterizer.frontFace   = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // O código base chamava glDepthFunc(GL_ALWAYS) antes de desenhar o texto,
    // isto é, o texto sempre aparece por cima da cena. Aqui simplesmente
    // desligamos o teste de profundidade.
    VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
    depth_stencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable  = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    depth_stencil.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

    // Equivalente a:
    //   glEnable(GL_BLEND);
    //   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    VkPipelineColorBlendAttachmentState color_blend_attachment = {};
    color_blend_attachment.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                               | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    color_blend_attachment.blendEnable         = VK_TRUE;
    color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    color_blend_attachment.colorBlendOp        = VK_BLEND_OP_ADD;
    color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    color_blend_attachment.alphaBlendOp        = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo color_blending = {};
    color_blending.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments    = &color_blend_attachment;

    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamic_state = {};
    dynamic_state.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates    = dynamic_states;

    VkPipelineLayoutCreateInfo layout_info = {};
    layout_info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts    = &g_TextDescriptorSetLayout;

    VK_CHECK(vkCreatePipelineLayout(g_Vk.device, &layout_info, NULL, &g_TextPipelineLayout));

    VkGraphicsPipelineCreateInfo pipeline_info = {};
    pipeline_info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount          = 2;
    pipeline_info.pStages             = stages;
    pipeline_info.pVertexInputState   = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState      = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState   = &multisampling;
    pipeline_info.pDepthStencilState  = &depth_stencil;
    pipeline_info.pColorBlendState    = &color_blending;
    pipeline_info.pDynamicState       = &dynamic_state;
    pipeline_info.layout              = g_TextPipelineLayout;
    pipeline_info.renderPass          = g_Vk.render_pass;
    pipeline_info.subpass             = 0;

    VK_CHECK(vkCreateGraphicsPipelines(g_Vk.device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &g_TextPipeline));

    // Os módulos de shader podem ser destruídos assim que o pipeline é criado.
    vkDestroyShaderModule(g_Vk.device, vertex_module, NULL);
    vkDestroyShaderModule(g_Vk.device, fragment_module, NULL);
}

// ===========================================================================
// Buffers de vértices dinâmicos
// ===========================================================================

static void TextRendering_CreateVertexBuffers()
{
    VkDeviceSize buffer_size = (VkDeviceSize)(TEXT_MAX_GLYPHS_PER_FRAME * TEXT_BYTES_PER_GLYPH);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        Vulkan_CreateBuffer(
            buffer_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &g_TextVertexBuffers[i],
            &g_TextVertexBufferMemories[i]
        );

        // Mantemos o buffer permanentemente mapeado: escrever uma letra passa a
        // ser apenas um memcpy() na memória da CPU.
        VK_CHECK(vkMapMemory(g_Vk.device, g_TextVertexBufferMemories[i], 0, buffer_size, 0, &g_TextVertexBufferMapped[i]));
    }
}

// ===========================================================================
// Interface pública
// ===========================================================================

void TextRendering_Init()
{
    TextRendering_CreateFontTexture();
    TextRendering_CreateDescriptorSet();
    TextRendering_CreatePipeline();
    TextRendering_CreateVertexBuffers();

    g_TextGlyphsUsed = 0;
    g_TextLastFrame  = 0;
}

void TextRendering_Destroy()
{
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
    {
        if (g_TextVertexBufferMemories[i] != VK_NULL_HANDLE)
        {
            vkUnmapMemory(g_Vk.device, g_TextVertexBufferMemories[i]);
            vkDestroyBuffer(g_Vk.device, g_TextVertexBuffers[i], NULL);
            vkFreeMemory(g_Vk.device, g_TextVertexBufferMemories[i], NULL);
        }
    }

    vkDestroyPipeline(g_Vk.device, g_TextPipeline, NULL);
    vkDestroyPipelineLayout(g_Vk.device, g_TextPipelineLayout, NULL);
    vkDestroyDescriptorPool(g_Vk.device, g_TextDescriptorPool, NULL);
    vkDestroyDescriptorSetLayout(g_Vk.device, g_TextDescriptorSetLayout, NULL);

    vkDestroySampler(g_Vk.device, g_TextFontSampler, NULL);
    vkDestroyImageView(g_Vk.device, g_TextFontImageView, NULL);
    vkDestroyImage(g_Vk.device, g_TextFontImage, NULL);
    vkFreeMemory(g_Vk.device, g_TextFontImageMemory, NULL);
}

float textscale = 1.5f;

void TextRendering_PrintString(GLFWwindow* window, const std::string &str, float x, float y, float scale = 1.0f)
{
    // O buffer de vértices é reaproveitado a cada quadro. Como existe um buffer
    // por quadro "em voo", e o VulkanContext_BeginFrame() espera a fence do
    // quadro antes de continuar, é seguro sobrescrevê-lo aqui.
    if (g_TextLastFrame != g_Vk.frame_counter)
    {
        g_TextLastFrame  = g_Vk.frame_counter;
        g_TextGlyphsUsed = 0;
    }

    VkCommandBuffer command_buffer = g_Vk.command_buffers[g_Vk.current_frame];
    uint32_t        frame          = g_Vk.current_frame;

    scale *= textscale;
    int width, height;
    glfwGetWindowSize(window, &width, &height);

    if (width <= 0 || height <= 0)
        return;

    float sx = scale / width;
    float sy = scale / height;

    // Ligamos o pipeline e o descriptor set uma única vez por chamada (o código
    // base em OpenGL fazia glUseProgram() a cada letra).
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_TextPipeline);
    vkCmdBindDescriptorSets(
        command_buffer,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        g_TextPipelineLayout,
        0, 1, &g_TextDescriptorSet,
        0, NULL
    );

    for (size_t i = 0; i < str.size(); i++)
    {
        // Find the glyph for the character we are looking for
        texture_glyph_t *glyph = 0;
        for (size_t j = 0; j < dejavufont.glyphs_count; ++j)
        {
            if (dejavufont.glyphs[j].codepoint == (uint32_t)str[i])
            {
                glyph = &dejavufont.glyphs[j];
                break;
            }
        }
        if (!glyph) {
            continue;
        }

        if (g_TextGlyphsUsed >= TEXT_MAX_GLYPHS_PER_FRAME)
        {
            if (!g_TextOverflowWarned)
            {
                fprintf(stderr,
                    "WARNING: mais de %d letras desenhadas em um único quadro.\n"
                    "         Aumente TEXT_MAX_GLYPHS_PER_FRAME em textrendering.cpp.\n",
                    TEXT_MAX_GLYPHS_PER_FRAME);
                g_TextOverflowWarned = true;
            }
            return;
        }

        x += glyph->kerning[0].kerning;
        float x0 = (float) (x + glyph->offset_x * sx);
        float y0 = (float) (y + glyph->offset_y * sy);
        float x1 = (float) (x0 + glyph->width * sx);
        float y1 = (float) (y0 - glyph->height * sy);

        float s0 = glyph->s0 - 0.5f/dejavufont.tex_width;
        float t0 = glyph->t0 - 0.5f/dejavufont.tex_height;
        float s1 = glyph->s1 - 0.5f/dejavufont.tex_width;
        float t1 = glyph->t1 - 0.5f/dejavufont.tex_height;

        TextVertex data[TEXT_VERTICES_PER_GLYPH] = {
            { x0, y0, s0, t0 },
            { x0, y1, s0, t1 },
            { x1, y1, s1, t1 },
            { x0, y0, s0, t0 },
            { x1, y1, s1, t1 },
            { x1, y0, s1, t0 }
        };

        // Gravamos os 6 vértices desta letra na sua "fatia" do buffer.
        VkDeviceSize offset = (VkDeviceSize)(g_TextGlyphsUsed * TEXT_BYTES_PER_GLYPH);

        memcpy((char*)g_TextVertexBufferMapped[frame] + offset, data, sizeof(data));

        vkCmdBindVertexBuffers(command_buffer, 0, 1, &g_TextVertexBuffers[frame], &offset);
        vkCmdDraw(command_buffer, TEXT_VERTICES_PER_GLYPH, 1, 0, 0);

        g_TextGlyphsUsed += 1;

        x += (glyph->advance_x * sx);
    }
}

float TextRendering_LineHeight(GLFWwindow* window)
{
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    return dejavufont.height / height * textscale;
}

float TextRendering_CharWidth(GLFWwindow* window)
{
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    return dejavufont.glyphs[32].advance_x / width * textscale;
}

void TextRendering_PrintMatrix(GLFWwindow* window, glm::mat4 M, float x, float y, float scale = 1.0f)
{
    char buffer[40];
    float lineheight = TextRendering_LineHeight(window) * scale;

    snprintf(buffer, 40, "[%+0.2f %+0.2f %+0.2f %+0.2f]", M[0][0], M[1][0], M[2][0], M[3][0]);
    TextRendering_PrintString(window, buffer, x, y, scale);
    snprintf(buffer, 40, "[%+0.2f %+0.2f %+0.2f %+0.2f]", M[0][1], M[1][1], M[2][1], M[3][1]);
    TextRendering_PrintString(window, buffer, x, y - lineheight, scale);
    snprintf(buffer, 40, "[%+0.2f %+0.2f %+0.2f %+0.2f]", M[0][2], M[1][2], M[2][2], M[3][2]);
    TextRendering_PrintString(window, buffer, x, y - 2*lineheight, scale);
    snprintf(buffer, 40, "[%+0.2f %+0.2f %+0.2f %+0.2f]", M[0][3], M[1][3], M[2][3], M[3][3]);
    TextRendering_PrintString(window, buffer, x, y - 3*lineheight, scale);
}

void TextRendering_PrintVector(GLFWwindow* window, glm::vec4 v, float x, float y, float scale = 1.0f)
{
    char buffer[10];
    float lineheight = TextRendering_LineHeight(window) * scale;

    snprintf(buffer, 10, "[%+0.2f]", v.x);
    TextRendering_PrintString(window, buffer, x, y, scale);
    snprintf(buffer, 10, "[%+0.2f]", v.y);
    TextRendering_PrintString(window, buffer, x, y - lineheight, scale);
    snprintf(buffer, 10, "[%+0.2f]", v.z);
    TextRendering_PrintString(window, buffer, x, y - 2*lineheight, scale);
    snprintf(buffer, 10, "[%+0.2f]", v.w);
    TextRendering_PrintString(window, buffer, x, y - 3*lineheight, scale);
}

void TextRendering_PrintMatrixVectorProduct(GLFWwindow* window, glm::mat4 M, glm::vec4 v, float x, float y, float scale = 1.0f)
{
    char buffer[70];
    float lineheight = TextRendering_LineHeight(window) * scale;

    auto r = M*v;
    snprintf(buffer, 70, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f]     [%+0.2f]\n", M[0][0], M[1][0], M[2][0], M[3][0], v[0], r[0]);
    TextRendering_PrintString(window, buffer, x, y, scale);
    snprintf(buffer, 70, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f]     [%+0.2f]\n", M[0][1], M[1][1], M[2][1], M[3][1], v[1], r[1]);
    TextRendering_PrintString(window, buffer, x, y - lineheight, scale);
    snprintf(buffer, 70, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f] --> [%+0.2f]\n", M[0][2], M[1][2], M[2][2], M[3][2], v[2], r[2]);
    TextRendering_PrintString(window, buffer, x, y - 2*lineheight, scale);
    snprintf(buffer, 70, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f]     [%+0.2f]\n", M[0][3], M[1][3], M[2][3], M[3][3], v[3], r[3]);
    TextRendering_PrintString(window, buffer, x, y - 3*lineheight, scale);
}

void TextRendering_PrintMatrixVectorProductMoreDigits(GLFWwindow* window, glm::mat4 M, glm::vec4 v, float x, float y, float scale = 1.0f)
{
    char buffer[70];
    float lineheight = TextRendering_LineHeight(window) * scale;

    auto r = M*v;
    snprintf(buffer, 70, "[%5.1f %5.1f %5.1f %5.1f][%5.2f]     [%+6.1f]\n", M[0][0], M[1][0], M[2][0], M[3][0], v[0], r[0]);
    TextRendering_PrintString(window, buffer, x, y, scale);
    snprintf(buffer, 70, "[%5.1f %5.1f %5.1f %5.1f][%5.2f]     [%+6.1f]\n", M[0][1], M[1][1], M[2][1], M[3][1], v[1], r[1]);
    TextRendering_PrintString(window, buffer, x, y - lineheight, scale);
    snprintf(buffer, 70, "[%5.1f %5.1f %5.1f %5.1f][%5.2f] --> [%+6.1f]\n", M[0][2], M[1][2], M[2][2], M[3][2], v[2], r[2]);
    TextRendering_PrintString(window, buffer, x, y - 2*lineheight, scale);
    snprintf(buffer, 70, "[%5.1f %5.1f %5.1f %5.1f][%5.2f]     [%+6.1f]\n", M[0][3], M[1][3], M[2][3], M[3][3], v[3], r[3]);
    TextRendering_PrintString(window, buffer, x, y - 3*lineheight, scale);
}

void TextRendering_PrintMatrixVectorProductDivW(GLFWwindow* window, glm::mat4 M, glm::vec4 v, float x, float y, float scale = 1.0f)
{
    auto r = M*v;
    auto w = r[3];

    char buffer[90];
    float lineheight = TextRendering_LineHeight(window) * scale;

    snprintf(buffer, 90, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f]     [%+0.2f]        [%+0.2f]\n", M[0][0], M[1][0], M[2][0], M[3][0], v[0], r[0], r[0]/w);
    TextRendering_PrintString(window, buffer, x, y, scale);
    snprintf(buffer, 90, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f]     [%+0.2f] div. w [%+0.2f]\n", M[0][1], M[1][1], M[2][1], M[3][1], v[1], r[1], r[1]/w);
    TextRendering_PrintString(window, buffer, x, y - lineheight, scale);
    snprintf(buffer, 90, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f] --> [%+0.2f] -----> [%+0.2f]\n", M[0][2], M[1][2], M[2][2], M[3][2], v[2], r[2], r[2]/w);
    TextRendering_PrintString(window, buffer, x, y - 2*lineheight, scale);
    snprintf(buffer, 90, "[%+0.2f %+0.2f %+0.2f %+0.2f][%+0.2f]     [%+0.2f]        [%+0.2f]\n", M[0][3], M[1][3], M[2][3], M[3][3], v[3], r[3], r[3]/w);
    TextRendering_PrintString(window, buffer, x, y - 3*lineheight, scale);
}

// vim: set spell spelllang=pt_br :
