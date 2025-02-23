/* Engine Copyright (c) 2023 Engine Development Team 
   https://github.com/beaumanvienna/vulkan

   Permission is hereby granted, free of charge, to any person
   obtaining a copy of this software and associated documentation files
   (the "Software"), to deal in the Software without restriction,
   including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software,
   and to permit persons to whom the Software is furnished to do so,
   subject to the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS 
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "core.h"
#include "engine.h"
#include "resources/resources.h"
#include "auxiliary/file.h"

#include "shadowMapping.h"
#include "VKrenderer.h"
#include "VKwindow.h"
#include "VKshader.h"

namespace GfxRenderEngine
{
    std::shared_ptr<Texture> gTextureSpritesheet;
    std::shared_ptr<Texture> gTextureFontAtlas;

    std::unique_ptr<VK_DescriptorPool> VK_Renderer::m_DescriptorPool;

    VK_Renderer::VK_Renderer(VK_Window* window)
        : m_Window{window}, m_FrameCounter{0},
          m_CurrentImageIndex{0}, m_AmbientLightIntensity{0.0f},
          m_CurrentFrameIndex{0}, m_ShowDebugShadowMap{false},
          m_FrameInProgress{false}, m_ShadersCompiled{false},
          m_Device{VK_Core::m_Device}
    {
        CompileShaders();  // runs in a parallel thread and sets m_ShadersCompiled
    }

    bool VK_Renderer::Init()
    {
        if (!m_ShadersCompiled) return m_ShadersCompiled;

        RecreateSwapChain();
        RecreateRenderpass();
        RecreateShadowMaps();
        CreateCommandBuffers();

        for (uint i = 0; i < m_ShadowUniformBuffers0.size(); i++)
        {
            m_ShadowUniformBuffers0[i] = std::make_unique<VK_Buffer>
            (
                sizeof(ShadowUniformBuffer),
                1,                                      // uint instanceCount
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                m_Device->m_Properties.limits.minUniformBufferOffsetAlignment
            );
            m_ShadowUniformBuffers0[i]->Map();
        }

        for (uint i = 0; i < m_ShadowUniformBuffers1.size(); i++)
        {
            m_ShadowUniformBuffers1[i] = std::make_unique<VK_Buffer>
            (
                sizeof(ShadowUniformBuffer),
                1,                                      // uint instanceCount
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                m_Device->m_Properties.limits.minUniformBufferOffsetAlignment
            );
            m_ShadowUniformBuffers1[i]->Map();
        }

        for (uint i = 0; i < m_UniformBuffers.size(); i++)
        {
            m_UniformBuffers[i] = std::make_unique<VK_Buffer>
            (
                sizeof(GlobalUniformBuffer),
                1,                                      // uint instanceCount
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                m_Device->m_Properties.limits.minUniformBufferOffsetAlignment
            );
            m_UniformBuffers[i]->Map();
        }

        // create a global pool for desciptor sets
        static constexpr uint POOL_SIZE = 10000;
        m_DescriptorPool = 
            VK_DescriptorPool::Builder()
            .SetMaxSets(VK_SwapChain::MAX_FRAMES_IN_FLIGHT * POOL_SIZE)
            .AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SwapChain::MAX_FRAMES_IN_FLIGHT * 50)
            .AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SwapChain::MAX_FRAMES_IN_FLIGHT * 7500)
            .AddPoolSize(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SwapChain::MAX_FRAMES_IN_FLIGHT * 2450)
            .Build();

        std::unique_ptr<VK_DescriptorSetLayout> shadowUniformBufferDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .Build();

        m_ShadowMapDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> globalDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // spritesheet
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // font atlas
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS)
                    .Build();
        m_GlobalDescriptorSetLayout = globalDescriptorSetLayout->GetDescriptorSetLayout();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> animationDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> animationInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> instanceDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseSADescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseSAInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> emissiveInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> emissiveTextureDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // emissive map
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> emissiveTextureInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // emissive map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalSADescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalSAInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalRoughnessMetallicDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // roughness metallic map
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalRoughnessMetallic2DescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // roughness map
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // metallic map
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalRoughnessMetallic2InstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // roughness map
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // metallic map
                    .AddBinding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalRoughnessMetallicSADescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // roughness metallic map
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for animation
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> noMapInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> diffuseNormalRoughnessMetallicInstancedDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // color map
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // normal map
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // roughness metallic map
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_ALL_GRAPHICS) // shader data for instances
                    .Build();

        m_LightingDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // g buffer position input attachment
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // g buffer normal input attachment
                    .AddBinding(2, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // g buffer color input attachment
                    .AddBinding(3, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // g buffer material input attachment
                    .AddBinding(4, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // g buffer emissive input attachment
                    .Build();

        m_PostProcessingDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // color input attachment
                    .AddBinding(1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, VK_SHADER_STAGE_FRAGMENT_BIT) // g buffer emissive input attachment
                    .Build();

        std::unique_ptr<VK_DescriptorSetLayout> cubemapDescriptorSetLayout = VK_DescriptorSetLayout::Builder()
                    .AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_ALL_GRAPHICS) // cubemap
                    .Build();

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDefaultDiffuse =
        {
            m_GlobalDescriptorSetLayout
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuse =
        {
            m_GlobalDescriptorSetLayout,
            diffuseDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseSA =
        {
            m_GlobalDescriptorSetLayout,
            diffuseSADescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseSAInstanced =
        {
            m_GlobalDescriptorSetLayout,
            diffuseSAInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsEmissiveInstanced =
        {
            m_GlobalDescriptorSetLayout,
            emissiveInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsEmissiveTexture =
        {
            m_GlobalDescriptorSetLayout,
            emissiveTextureDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsEmissiveTextureInstanced =
        {
            m_GlobalDescriptorSetLayout,
            emissiveTextureInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormal =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalSA =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalSADescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalSAInstanced =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalSAInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalRoughnessMetallic =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalRoughnessMetallicDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalRoughnessMetallic2 =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalRoughnessMetallic2DescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalRoughnessMetallic2Instanced =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalRoughnessMetallic2InstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalRoughnessMetallicSA =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalRoughnessMetallicSADescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseInstanced =
        {
            m_GlobalDescriptorSetLayout,
            diffuseInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsNoMapInstanced =
        {
            m_GlobalDescriptorSetLayout,
            noMapInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalInstanced =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDiffuseNormalRoughnessMetallicInstanced =
        {
            m_GlobalDescriptorSetLayout,
            diffuseNormalRoughnessMetallicInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsLighting =
        {
            m_GlobalDescriptorSetLayout,
            m_LightingDescriptorSetLayout->GetDescriptorSetLayout(),
            m_ShadowMapDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsPostProcessing =
        {
            m_GlobalDescriptorSetLayout,
            m_PostProcessingDescriptorSetLayout->GetDescriptorSetLayout(),
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsCubemap =
        {
            m_GlobalDescriptorSetLayout,
            cubemapDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsShadow =
        {
            shadowUniformBufferDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsShadowInstanced =
        {
            shadowUniformBufferDescriptorSetLayout->GetDescriptorSetLayout(),
            instanceDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsShadowAnimated =
        {
            shadowUniformBufferDescriptorSetLayout->GetDescriptorSetLayout(),
            animationDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsShadowAnimatedInstanced =
        {
            shadowUniformBufferDescriptorSetLayout->GetDescriptorSetLayout(),
            animationInstancedDescriptorSetLayout->GetDescriptorSetLayout()
        };

        std::vector<VkDescriptorSetLayout> descriptorSetLayoutsDebug =
        {
            m_ShadowMapDescriptorSetLayout->GetDescriptorSetLayout()
        };

        size_t fileSize;
        auto data = (const uchar*) ResourceSystem::GetDataPointer(fileSize, "/images/atlas/atlas.png", IDB_ATLAS, "PNG");
        auto textureSpritesheet = std::make_shared<VK_Texture>(true);
        textureSpritesheet->Init(data, fileSize, Texture::USE_SRGB);
        textureSpritesheet->SetFilename("spritesheet");

        gTextureSpritesheet = textureSpritesheet; // copy from VK_Texture to Texture
        VkDescriptorImageInfo imageInfo0 = textureSpritesheet->GetDescriptorImageInfo();

        data = (const uchar*) ResourceSystem::GetDataPointer(fileSize, "/images/atlas/fontAtlas.png", IDB_FONTS_RETRO, "PNG");
        auto textureFontAtlas = std::make_shared<VK_Texture>(true);
        textureFontAtlas->Init(data, fileSize, Texture::USE_SRGB);
        textureFontAtlas->SetFilename("font atlas");

        gTextureFontAtlas = textureFontAtlas; // copy from VK_Texture to Texture
        VkDescriptorImageInfo imageInfo1 = textureFontAtlas->GetDescriptorImageInfo();

        for (uint i = 0; i < VK_SwapChain::MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorBufferInfo shadowUBObufferInfo = m_ShadowUniformBuffers0[i]->DescriptorInfo();
            VK_DescriptorWriter(*shadowUniformBufferDescriptorSetLayout, *m_DescriptorPool)
                .WriteBuffer(0, shadowUBObufferInfo)
                .Build(m_ShadowDescriptorSets0[i]);
        }

        for (uint i = 0; i < VK_SwapChain::MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorBufferInfo shadowUBObufferInfo = m_ShadowUniformBuffers1[i]->DescriptorInfo();
            VK_DescriptorWriter(*shadowUniformBufferDescriptorSetLayout, *m_DescriptorPool)
                .WriteBuffer(0, shadowUBObufferInfo)
                .Build(m_ShadowDescriptorSets1[i]);
        }

        for (uint i = 0; i < VK_SwapChain::MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorBufferInfo bufferInfo = m_UniformBuffers[i]->DescriptorInfo();
            VK_DescriptorWriter(*globalDescriptorSetLayout, *m_DescriptorPool)
                .WriteBuffer(0, bufferInfo)
                .WriteImage(1, imageInfo0)
                .WriteImage(2, imageInfo1)
                .Build(m_GlobalDescriptorSets[i]);
        }

        m_RenderSystemShadow                            = std::make_unique<VK_RenderSystemShadow>
                                                          (
                                                              m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowRenderPass(),
                                                              m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowRenderPass(),
                                                              descriptorSetLayoutsShadow
                                                          );
        m_RenderSystemShadowInstanced                   = std::make_unique<VK_RenderSystemShadowInstanced>
                                                          (
                                                              m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowRenderPass(),
                                                              m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowRenderPass(),
                                                              descriptorSetLayoutsShadowInstanced
                                                          );
        m_RenderSystemShadowAnimated                    = std::make_unique<VK_RenderSystemShadowAnimated>
                                                          (
                                                              m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowRenderPass(),
                                                              m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowRenderPass(),
                                                              descriptorSetLayoutsShadowAnimated
                                                          );
        m_RenderSystemShadowAnimatedInstanced           = std::make_unique<VK_RenderSystemShadowAnimatedInstanced>
                                                          (
                                                              m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowRenderPass(),
                                                              m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowRenderPass(),
                                                              descriptorSetLayoutsShadowAnimatedInstanced
                                                          );
        m_LightSystem                                             = std::make_unique<VK_LightSystem>(m_Device, m_RenderPass->Get3DRenderPass(), *globalDescriptorSetLayout);
        m_RenderSystemSpriteRenderer                              = std::make_unique<VK_RenderSystemSpriteRenderer>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuse);
        m_RenderSystemSpriteRenderer2D                            = std::make_unique<VK_RenderSystemSpriteRenderer2D>(m_RenderPass->GetGUIRenderPass(), *globalDescriptorSetLayout);
        m_RenderSystemGUIRenderer                                 = std::make_unique<VK_RenderSystemGUIRenderer>(m_RenderPass->GetGUIRenderPass(), *globalDescriptorSetLayout);
        m_RenderSystemCubemap                                     = std::make_unique<VK_RenderSystemCubemap>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsCubemap);

        m_RenderSystemPbrNoMap                                    = std::make_unique<VK_RenderSystemPbrNoMap>(m_RenderPass->Get3DRenderPass(), *globalDescriptorSetLayout);
        m_RenderSystemPbrDiffuse                                  = std::make_unique<VK_RenderSystemPbrDiffuse>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuse);
        m_RenderSystemPbrEmissive                                 = std::make_unique<VK_RenderSystemPbrEmissive>(m_RenderPass->Get3DRenderPass(), *globalDescriptorSetLayout);
        m_RenderSystemPbrDiffuseSA                                = std::make_unique<VK_RenderSystemPbrDiffuseSA>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseSA);
        m_RenderSystemPbrDiffuseNormal                            = std::make_unique<VK_RenderSystemPbrDiffuseNormal>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormal);
        m_RenderSystemPbrNoMapInstanced                           = std::make_unique<VK_RenderSystemPbrNoMapInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsNoMapInstanced);
        m_RenderSystemPbrDiffuseNormalSA                          = std::make_unique<VK_RenderSystemPbrDiffuseNormalSA>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalSA);
        m_RenderSystemPbrEmissiveTexture                          = std::make_unique<VK_RenderSystemPbrEmissiveTexture>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsEmissiveTexture);
        m_RenderSystemPbrDiffuseInstanced                         = std::make_unique<VK_RenderSystemPbrDiffuseInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseInstanced);
        m_RenderSystemPbrEmissiveInstanced                        = std::make_unique<VK_RenderSystemPbrEmissiveInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsEmissiveInstanced);
        m_RenderSystemPbrDiffuseSAInstanced                       = std::make_unique<VK_RenderSystemPbrDiffuseSAInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseSAInstanced);
        m_RenderSystemPbrDiffuseNormalInstanced                   = std::make_unique<VK_RenderSystemPbrDiffuseNormalInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalInstanced);
        m_RenderSystemPbrDiffuseNormalSAInstanced                 = std::make_unique<VK_RenderSystemPbrDiffuseNormalSAInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalSAInstanced);
        m_RenderSystemPbrEmissiveTextureInstanced                 = std::make_unique<VK_RenderSystemPbrEmissiveTextureInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsEmissiveTextureInstanced);
        m_RenderSystemPbrDiffuseNormalRoughnessMetallic           = std::make_unique<VK_RenderSystemPbrDiffuseNormalRoughnessMetallic>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalRoughnessMetallic);
        m_RenderSystemPbrDiffuseNormalRoughnessMetallic2          = std::make_unique<VK_RenderSystemPbrDiffuseNormalRoughnessMetallic2>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalRoughnessMetallic2);
        m_RenderSystemPbrDiffuseNormalRoughnessMetallicSA         = std::make_unique<VK_RenderSystemPbrDiffuseNormalRoughnessMetallicSA>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalRoughnessMetallicSA);
        m_RenderSystemPbrDiffuseNormalRoughnessMetallicInstanced  = std::make_unique<VK_RenderSystemPbrDiffuseNormalRoughnessMetallicInstanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalRoughnessMetallicInstanced);
        m_RenderSystemPbrDiffuseNormalRoughnessMetallic2Instanced = std::make_unique<VK_RenderSystemPbrDiffuseNormalRoughnessMetallic2Instanced>(m_RenderPass->Get3DRenderPass(), descriptorSetLayoutsDiffuseNormalRoughnessMetallic2Instanced);

        CreateShadowMapDescriptorSets();
        CreateLightingDescriptorSets();
        CreatePostProcessingDescriptorSets();

        m_RenderSystemDeferredShading                 = std::make_unique<VK_RenderSystemDeferredShading>
        (
            m_RenderPass->Get3DRenderPass(),
            descriptorSetLayoutsLighting,
            m_LightingDescriptorSets.data(),
            m_ShadowMapDescriptorSets.data()
        );

        CreateRenderSystemBloom();

        m_RenderSystemPostProcessing                 = std::make_unique<VK_RenderSystemPostProcessing>
        (
            m_RenderPass->GetPostProcessingRenderPass(),
            descriptorSetLayoutsPostProcessing,
            m_PostProcessingDescriptorSets.data()
        );

        m_RenderSystemDebug                             = std::make_unique<VK_RenderSystemDebug>
        (
            m_RenderPass->Get3DRenderPass(),
            descriptorSetLayoutsDebug,
            m_ShadowMapDescriptorSets.data()
        );

        m_Imgui = Imgui::Create(m_RenderPass->GetGUIRenderPass(), static_cast<uint>(m_SwapChain->ImageCount()));
        return m_ShadersCompiled;
    }

    void VK_Renderer::CreateRenderSystemBloom()
    {
        m_RenderSystemBloom = std::make_unique<VK_RenderSystemBloom>
        (
            *m_RenderPass,
            *m_DescriptorPool
        );
    }

    void VK_Renderer::CreateShadowMapDescriptorSets()
    {
        for (uint i = 0; i < VK_SwapChain::MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorImageInfo shadowMapInfo0 = m_ShadowMap[ShadowMaps::HIGH_RES]->GetDescriptorImageInfo();
            VkDescriptorImageInfo shadowMapInfo1 = m_ShadowMap[ShadowMaps::LOW_RES]->GetDescriptorImageInfo();

            VkDescriptorBufferInfo shadowUBObufferInfo0 = m_ShadowUniformBuffers0[i]->DescriptorInfo();
            VkDescriptorBufferInfo shadowUBObufferInfo1 = m_ShadowUniformBuffers1[i]->DescriptorInfo();

            VK_DescriptorWriter(*m_ShadowMapDescriptorSetLayout, *m_DescriptorPool)
                .WriteImage(0, shadowMapInfo0)
                .WriteImage(1, shadowMapInfo1)
                .WriteBuffer(2, shadowUBObufferInfo0)
                .WriteBuffer(3, shadowUBObufferInfo1)
                .Build(m_ShadowMapDescriptorSets[i]);
        }
    }

    void VK_Renderer::CreateLightingDescriptorSets()
    {
        for (uint i = 0; i < VK_SwapChain::MAX_FRAMES_IN_FLIGHT; i++)
        {
            VkDescriptorImageInfo imageInfoGBufferPositionInputAttachment {};
            imageInfoGBufferPositionInputAttachment.imageView   = m_RenderPass->GetImageViewGBufferPosition();
            imageInfoGBufferPositionInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo imageInfoGBufferNormalInputAttachment {};
            imageInfoGBufferNormalInputAttachment.imageView   = m_RenderPass->GetImageViewGBufferNormal();
            imageInfoGBufferNormalInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo imageInfoGBufferColorInputAttachment {};
            imageInfoGBufferColorInputAttachment.imageView   = m_RenderPass->GetImageViewGBufferColor();
            imageInfoGBufferColorInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo imageInfoGBufferMaterialInputAttachment {};
            imageInfoGBufferMaterialInputAttachment.imageView   = m_RenderPass->GetImageViewGBufferMaterial();
            imageInfoGBufferMaterialInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo imageInfoGBufferEmissionInputAttachment {};
            imageInfoGBufferEmissionInputAttachment.imageView   = m_RenderPass->GetImageViewGBufferEmission();
            imageInfoGBufferEmissionInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VK_DescriptorWriter(*m_LightingDescriptorSetLayout, *m_DescriptorPool)
                .WriteImage(0, imageInfoGBufferPositionInputAttachment)
                .WriteImage(1, imageInfoGBufferNormalInputAttachment)
                .WriteImage(2, imageInfoGBufferColorInputAttachment)
                .WriteImage(3, imageInfoGBufferMaterialInputAttachment)
                .WriteImage(4, imageInfoGBufferEmissionInputAttachment)
                .Build(m_LightingDescriptorSets[i]);
        }
    }

    void VK_Renderer::CreatePostProcessingDescriptorSets()
    {
        for (uint frameIndex = 0; frameIndex < VK_SwapChain::MAX_FRAMES_IN_FLIGHT; frameIndex++)
        {
            VkDescriptorImageInfo imageInfoColorInputAttachment {};
            imageInfoColorInputAttachment.imageView   = m_RenderPass->GetImageViewColorAttachment();
            imageInfoColorInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo imageInfoGBufferEmissionInputAttachment {};
            imageInfoGBufferEmissionInputAttachment.imageView   = m_RenderPass->GetImageViewGBufferEmission();
            imageInfoGBufferEmissionInputAttachment.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VK_DescriptorWriter(*m_PostProcessingDescriptorSetLayout, *m_DescriptorPool)
                .WriteImage(0, imageInfoColorInputAttachment)
                .WriteImage(1, imageInfoGBufferEmissionInputAttachment)
                .Build(m_PostProcessingDescriptorSets[frameIndex]);
        }
    }

    VK_Renderer::~VK_Renderer()
    {
        FreeCommandBuffers();
    }

    void VK_Renderer::RecreateSwapChain()
    {
        auto extent = m_Window->GetExtent();
        while (extent.width == 0 || extent.height == 0)
        {
            extent = m_Window->GetExtent();
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(m_Device->Device());

        // create the swapchain
        if (m_SwapChain == nullptr)
        {
            m_SwapChain = std::make_unique<VK_SwapChain>(extent);
        }
        else
        {
            LOG_CORE_INFO("recreating swapchain at frame {0}", m_FrameCounter);
            std::shared_ptr<VK_SwapChain> oldSwapChain = std::move(m_SwapChain);
            m_SwapChain = std::make_unique<VK_SwapChain>(extent, oldSwapChain);
            if (!oldSwapChain->CompareSwapFormats(*m_SwapChain.get()))
            {
                LOG_CORE_CRITICAL("swap chain image or depth format has changed");
            }
        }
    }

    void VK_Renderer::RecreateRenderpass()
    {
        m_RenderPass = std::make_unique<VK_RenderPass>(m_SwapChain.get());
    }

    void VK_Renderer::RecreateShadowMaps()
    {
        // create shadow maps
        m_ShadowMap[ShadowMaps::HIGH_RES] = std::make_unique<VK_ShadowMap>(SHADOW_MAP_HIGH_RES);
        m_ShadowMap[ShadowMaps::LOW_RES]  = std::make_unique<VK_ShadowMap>(SHADOW_MAP_LOW_RES);
    }

    void VK_Renderer::CreateCommandBuffers()
    {
        m_CommandBuffers.resize(VK_SwapChain::MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandPool = m_Device->GetCommandPool();
        allocateInfo.commandBufferCount = static_cast<uint>(m_CommandBuffers.size());

        if (vkAllocateCommandBuffers(m_Device->Device(), &allocateInfo, m_CommandBuffers.data()) != VK_SUCCESS)
        {
            LOG_CORE_CRITICAL("failed to allocate command buffers");
        }
    }

    void VK_Renderer::FreeCommandBuffers()
    {
        vkFreeCommandBuffers
        (
            m_Device->Device(),
            m_Device->GetCommandPool(),
            static_cast<uint>(m_CommandBuffers.size()),
            m_CommandBuffers.data()
        );
        m_CommandBuffers.clear();
    }

    VkCommandBuffer VK_Renderer::GetCurrentCommandBuffer() const
    {
        ASSERT(m_FrameInProgress);
        return m_CommandBuffers[m_CurrentFrameIndex];
    }

    VkCommandBuffer VK_Renderer::BeginFrame()
    {
        ASSERT(!m_FrameInProgress);

        auto result = m_SwapChain->AcquireNextImage(&m_CurrentImageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            Recreate();
            return nullptr;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        {
            LOG_CORE_CRITICAL("failed to acquire next swap chain image");
        }

        m_FrameInProgress = true;
        m_FrameCounter++;

        auto commandBuffer = GetCurrentCommandBuffer();

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
        {
            LOG_CORE_CRITICAL("failed to begin recording command buffer!");
        }

        return commandBuffer;
    }

    void VK_Renderer::EndFrame()
    {
        ASSERT(m_FrameInProgress);

        auto commandBuffer = GetCurrentCommandBuffer();

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
        {
            LOG_CORE_CRITICAL("recording of command buffer failed");
        }
        auto result = m_SwapChain->SubmitCommandBuffers(&commandBuffer, &m_CurrentImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            m_Window->ResetWindowResizedFlag();
            Recreate();
        }
        else if (result != VK_SUCCESS)
        {
            LOG_CORE_WARN("failed to present swap chain image");
        }
        m_FrameInProgress = false;
        m_CurrentFrameIndex = (m_CurrentFrameIndex + 1) % VK_SwapChain::MAX_FRAMES_IN_FLIGHT;
    }

    void VK_Renderer::Recreate()
    {
        RecreateSwapChain();
        RecreateRenderpass();
        CreateLightingDescriptorSets();
        CreateRenderSystemBloom();
        CreatePostProcessingDescriptorSets();
    }

    void VK_Renderer::BeginShadowRenderPass0(VkCommandBuffer commandBuffer)
    {
        ASSERT(m_FrameInProgress);
        ASSERT(commandBuffer == GetCurrentCommandBuffer());

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowRenderPass();
        renderPassInfo.framebuffer = m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowFrameBuffer();

        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowMapExtent();

        std::array<VkClearValue, static_cast<uint>(VK_ShadowMap::ShadowRenderTargets::NUMBER_OF_ATTACHMENTS)> clearValues{};
        clearValues[0].depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = static_cast<uint>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowMapExtent().width);
        viewport.height = static_cast<float>(m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowMapExtent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, m_ShadowMap[ShadowMaps::HIGH_RES]->GetShadowMapExtent()};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    }

    void VK_Renderer::BeginShadowRenderPass1(VkCommandBuffer commandBuffer)
    {
        ASSERT(m_FrameInProgress);
        ASSERT(commandBuffer == GetCurrentCommandBuffer());

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowRenderPass();
        renderPassInfo.framebuffer = m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowFrameBuffer();

        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowMapExtent();

        std::array<VkClearValue, static_cast<uint>(VK_ShadowMap::ShadowRenderTargets::NUMBER_OF_ATTACHMENTS)> clearValues{};
        clearValues[0].depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = static_cast<uint>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowMapExtent().width);
        viewport.height = static_cast<float>(m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowMapExtent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, m_ShadowMap[ShadowMaps::LOW_RES]->GetShadowMapExtent()};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void VK_Renderer::SubmitShadows(entt::registry& registry, const std::vector<DirectionalLightComponent*>& directionalLights)
    {
        // this function supports one directional light 
        // with a high-resolution and
        // with a low-resolution component
        // --> either both or none must be provided
        if (directionalLights.size() == 2)
        {
            {
                ShadowUniformBuffer ubo{};
                ubo.m_Projection = directionalLights[0]->m_LightView->GetProjectionMatrix();
                ubo.m_View = directionalLights[0]->m_LightView->GetViewMatrix();
                m_ShadowUniformBuffers0[m_CurrentFrameIndex]->WriteToBuffer(&ubo);
                m_ShadowUniformBuffers0[m_CurrentFrameIndex]->Flush();
            }
            {
                ShadowUniformBuffer ubo{};
                ubo.m_Projection = directionalLights[1]->m_LightView->GetProjectionMatrix();
                ubo.m_View = directionalLights[1]->m_LightView->GetViewMatrix();
                m_ShadowUniformBuffers1[m_CurrentFrameIndex]->WriteToBuffer(&ubo);
                m_ShadowUniformBuffers1[m_CurrentFrameIndex]->Flush();
            }

            BeginShadowRenderPass0(m_CurrentCommandBuffer);
            m_RenderSystemShadow->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[0],
                0 /* shadow pass 0*/,
                m_ShadowDescriptorSets0[m_CurrentFrameIndex]
            );
            m_RenderSystemShadowInstanced->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[0],
                0 /* shadow pass 0*/,
                m_ShadowDescriptorSets0[m_CurrentFrameIndex]
            );
            m_RenderSystemShadowAnimated->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[0],
                0 /* shadow pass 0*/,
                m_ShadowDescriptorSets0[m_CurrentFrameIndex]
            );
            m_RenderSystemShadowAnimatedInstanced->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[0],
                0 /* shadow pass 0*/,
                m_ShadowDescriptorSets0[m_CurrentFrameIndex]
            );
            EndRenderPass(m_CurrentCommandBuffer);

            BeginShadowRenderPass1(m_CurrentCommandBuffer);
            m_RenderSystemShadow->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[1],
                1 /* shadow pass 1*/,
                m_ShadowDescriptorSets1[m_CurrentFrameIndex]
            );
            m_RenderSystemShadowInstanced->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[1],
                1 /* shadow pass 1*/,
                m_ShadowDescriptorSets1[m_CurrentFrameIndex]
            );
            m_RenderSystemShadowAnimated->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[1],
                1 /* shadow pass 1*/,
                m_ShadowDescriptorSets1[m_CurrentFrameIndex]
            );
            m_RenderSystemShadowAnimatedInstanced->RenderEntities
            (
                m_FrameInfo,
                registry,
                directionalLights[1],
                1 /* shadow pass 1*/,
                m_ShadowDescriptorSets1[m_CurrentFrameIndex]
            );
            EndRenderPass(m_CurrentCommandBuffer);
        }
        else
        {
            // we still have to clear the shadow map depth buffers
            // because the lighting shader expects values
            BeginShadowRenderPass0(m_CurrentCommandBuffer);
            EndRenderPass(m_CurrentCommandBuffer);
            BeginShadowRenderPass1(m_CurrentCommandBuffer);
            EndRenderPass(m_CurrentCommandBuffer);
        }
    }

    void VK_Renderer::Begin3DRenderPass(VkCommandBuffer commandBuffer)
    {
        ASSERT(m_FrameInProgress);
        ASSERT(commandBuffer == GetCurrentCommandBuffer());

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_RenderPass->Get3DRenderPass();
        renderPassInfo.framebuffer = m_RenderPass->Get3DFrameBuffer(m_CurrentImageIndex);

        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_SwapChain->GetSwapChainExtent();

        std::array<VkClearValue, static_cast<uint>(VK_RenderPass::RenderTargets3D::NUMBER_OF_ATTACHMENTS)> clearValues{};
        clearValues[0].color = {{0.01f, 0.01f, 0.01f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        clearValues[2].color = {{0.1f, 0.1f, 0.1f, 1.0f}};
        clearValues[3].color = {{0.5f, 0.5f, 0.1f, 1.0f}};
        clearValues[4].color = {{0.5f, 0.1f, 0.5f, 1.0f}};
        clearValues[5].color = {{0.5f, 0.7f, 0.2f, 1.0f}};
        clearValues[6].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        renderPassInfo.clearValueCount = static_cast<uint>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_SwapChain->GetSwapChainExtent().width);
        viewport.height = static_cast<float>(m_SwapChain->GetSwapChainExtent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, m_SwapChain->GetSwapChainExtent()};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void VK_Renderer::BeginPostProcessingRenderPass(VkCommandBuffer commandBuffer)
    {
        ASSERT(m_FrameInProgress);
        ASSERT(commandBuffer == GetCurrentCommandBuffer());

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_RenderPass->GetPostProcessingRenderPass();
        renderPassInfo.framebuffer = m_RenderPass->GetPostProcessingFrameBuffer(m_CurrentImageIndex);

        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_SwapChain->GetSwapChainExtent();

        std::array<VkClearValue, static_cast<uint>(VK_RenderPass::RenderTargetsPostProcessing::NUMBER_OF_ATTACHMENTS)> clearValues{};
        clearValues[0].color = {{0.01f, 0.01f, 0.01f, 1.0f}};
        renderPassInfo.clearValueCount = static_cast<uint>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_SwapChain->GetSwapChainExtent().width);
        viewport.height = static_cast<float>(m_SwapChain->GetSwapChainExtent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, m_SwapChain->GetSwapChainExtent()};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void VK_Renderer::BeginGUIRenderPass(VkCommandBuffer commandBuffer)
    {
        ASSERT(m_FrameInProgress);
        ASSERT(commandBuffer == GetCurrentCommandBuffer());

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = m_RenderPass->GetGUIRenderPass();
        renderPassInfo.framebuffer = m_RenderPass->GetGUIFrameBuffer(m_CurrentImageIndex);

        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = m_SwapChain->GetSwapChainExtent();
        renderPassInfo.clearValueCount = 0;
        renderPassInfo.pClearValues = nullptr;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_SwapChain->GetSwapChainExtent().width);
        viewport.height = static_cast<float>(m_SwapChain->GetSwapChainExtent().height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, m_SwapChain->GetSwapChainExtent()};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

    void VK_Renderer::EndRenderPass(VkCommandBuffer commandBuffer)
    {
        ASSERT(m_FrameInProgress);
        ASSERT(commandBuffer == GetCurrentCommandBuffer());

        vkCmdEndRenderPass(commandBuffer);
    }

    void VK_Renderer::BeginFrame(Camera* camera)
    {
        m_CurrentCommandBuffer = BeginFrame();
        if (m_CurrentCommandBuffer)
        {
            m_FrameInfo =
            {
                m_CurrentFrameIndex,
                m_CurrentImageIndex,
                0.0f, /* m_FrameTime */
                m_CurrentCommandBuffer, 
                camera,
                m_GlobalDescriptorSets[m_CurrentFrameIndex]
            };
        }
    }

    void VK_Renderer::Renderpass3D(entt::registry& registry)
    {
        if (m_CurrentCommandBuffer)
        {
            GlobalUniformBuffer ubo{};
            ubo.m_Projection = m_FrameInfo.m_Camera->GetProjectionMatrix();
            ubo.m_View = m_FrameInfo.m_Camera->GetViewMatrix();
            ubo.m_AmbientLightColor = {1.0f, 1.0f, 1.0f, m_AmbientLightIntensity};
            m_LightSystem->Update(m_FrameInfo, ubo, registry);
            m_UniformBuffers[m_CurrentFrameIndex]->WriteToBuffer(&ubo);
            m_UniformBuffers[m_CurrentFrameIndex]->Flush();

            Begin3DRenderPass(m_CurrentCommandBuffer);
        }
    }

    void VK_Renderer::UpdateTransformCache(Scene& scene, uint const nodeIndex, glm::mat4 const& parentMat4, bool parentDirtyFlag)
    {
        TreeNode& node = scene.GetTreeNode(nodeIndex);
        entt::entity gameObject = node.GetGameObject();
        auto& transform = scene.GetRegistry().get<TransformComponent>(gameObject);
        bool dirtyFlag = transform.GetDirtyFlag() || parentDirtyFlag;

        if (dirtyFlag)
        {
            transform.SetMat4Global(parentMat4);
            const glm::mat4& mat4Global = transform.GetMat4Global();
            for (uint index = 0; index < node.Children(); index++)
            {
                UpdateTransformCache(scene, node.GetChild(index), mat4Global, true);
            }
        }
        else
        {
            const glm::mat4& mat4Global = transform.GetMat4Global();
            for (uint index = 0; index < node.Children(); ++index)
            {
                UpdateTransformCache(scene, node.GetChild(index), mat4Global, false);
            }
        }
    }

    void VK_Renderer::Submit(Scene& scene)
    {
        if (m_CurrentCommandBuffer)
        {
            UpdateTransformCache(scene, SceneGraph::ROOT_NODE, glm::mat4(1.0f), false);

            auto& registry = scene.GetRegistry();

            // 3D objects
            m_RenderSystemPbrNoMap->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuse->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseSA->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormal->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrNoMapInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalSA->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseSAInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalSAInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalRoughnessMetallic->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalRoughnessMetallic2->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalRoughnessMetallicSA->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalRoughnessMetallicInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrDiffuseNormalRoughnessMetallic2Instanced->RenderEntities(m_FrameInfo, registry);

            // the emissive pipelines need to go last
            // they do not write to the depth buffer
            m_RenderSystemPbrEmissive->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrEmissiveTexture->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrEmissiveInstanced->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemPbrEmissiveTextureInstanced->RenderEntities(m_FrameInfo, registry);
        }
    }

    void VK_Renderer::LightingPass()
    {
        if (m_CurrentCommandBuffer)
        {
            m_RenderSystemDeferredShading->LightingPass(m_FrameInfo);
        }
    }

    void VK_Renderer::TransparencyPass(entt::registry& registry, ParticleSystem* particleSystem)
    {
        if (m_CurrentCommandBuffer)
        {
            // sprites
            m_RenderSystemCubemap->RenderEntities(m_FrameInfo, registry);
            m_RenderSystemSpriteRenderer->RenderEntities(m_FrameInfo, registry);
            if (particleSystem) m_RenderSystemSpriteRenderer->DrawParticles(m_FrameInfo, particleSystem);
            m_LightSystem->Render(m_FrameInfo, registry);
            m_RenderSystemDebug->RenderEntities(m_FrameInfo, m_ShowDebugShadowMap);
        }
    }

    void VK_Renderer::PostProcessingRenderpass()
    {
        if (m_CurrentCommandBuffer)
        {
            EndRenderPass(m_CurrentCommandBuffer); // end 3D renderpass
            m_RenderSystemBloom->RenderBloom(m_FrameInfo);
            BeginPostProcessingRenderPass(m_CurrentCommandBuffer);
            m_RenderSystemPostProcessing->PostProcessingPass(m_FrameInfo);
        }
    }

    void VK_Renderer::NextSubpass()
    {
        if (m_CurrentCommandBuffer)
        {
            vkCmdNextSubpass(m_CurrentCommandBuffer, VK_SUBPASS_CONTENTS_INLINE);
        }
    }

    void VK_Renderer::GUIRenderpass(Camera* camera)
    {
        if (m_CurrentCommandBuffer)
        {
            EndRenderPass(m_CurrentCommandBuffer); // end post processing renderpass
            BeginGUIRenderPass(m_CurrentCommandBuffer);

            // set up orthogonal camera
            m_GUIViewProjectionMatrix = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        }
    }

    void VK_Renderer::Submit2D(Camera* camera, entt::registry& registry)
    {
        if (m_CurrentCommandBuffer)
        {
            m_RenderSystemSpriteRenderer2D->RenderEntities(m_FrameInfo, registry, camera);
        }
    }

    void VK_Renderer::EndScene()
    {
        if (m_CurrentCommandBuffer)
        {
            // built-in editor GUI runs last
            m_Imgui->NewFrame();
            m_Imgui->Run();
            m_Imgui->Render(m_CurrentCommandBuffer);

            EndRenderPass(m_CurrentCommandBuffer); // end GUI render pass
            EndFrame();
        }
    }

    int VK_Renderer::GetFrameIndex() const
    {
        ASSERT(m_FrameInProgress);
        return m_CurrentFrameIndex;
    }

    void VK_Renderer::UpdateAnimations(entt::registry& registry, const Timestep& timestep)
    { 
        auto view = registry.view<MeshComponent, TransformComponent, SkeletalAnimationTag>();
        for (auto entity : view)
        {
            auto& mesh = view.get<MeshComponent>(entity);
            if (mesh.m_Enabled)
            {
                static_cast<VK_Model*>(mesh.m_Model.get())->UpdateAnimation(timestep, m_FrameCounter);
            }
        }
    }

    void VK_Renderer::CompileShaders()
    {

        std::thread shaderCompileThread([this]()
        {
            if (!EngineCore::FileExists("bin-int"))
            {
                LOG_CORE_WARN("creating bin directory for spirv files");
                EngineCore::CreateDirectory("bin-int");
            }
            std::vector<std::string> shaderFilenames = 
            {
                // 2D
                "spriteRenderer.vert",
                "spriteRenderer.frag",
                "spriteRenderer2D.frag",
                "spriteRenderer2D.vert",
                "guiShader.frag",
                "guiShader.vert",
                "guiShader2.frag",
                "guiShader2.vert",
                // 3D
                "pointLight.vert",
                "pointLight.frag",
                "pbrNoMap.vert",
                "pbrNoMap.frag",
                "pbrDiffuse.vert",
                "pbrDiffuse.frag",
                "pbrDiffuseSA.vert",
                "pbrDiffuseSA.frag",
                "pbrDiffuseNormal.vert",
                "pbrDiffuseNormal.frag",
                "pbrNoMapInstanced.vert",
                "pbrNoMapInstanced.frag",
                "pbrDiffuseNormalSA.vert",
                "pbrDiffuseNormalSA.frag",
                "pbrDiffuseInstanced.vert",
                "pbrDiffuseInstanced.frag",
                "pbrDiffuseSAInstanced.vert",
                "pbrDiffuseSAInstanced.frag",
                "pbrDiffuseNormalInstanced.vert",
                "pbrDiffuseNormalInstanced.frag",
                "pbrDiffuseNormalSAInstanced.vert",
                "pbrDiffuseNormalSAInstanced.frag",
                "pbrDiffuseNormalRoughnessMetallic.vert",
                "pbrDiffuseNormalRoughnessMetallic.frag",
                "pbrDiffuseNormalRoughnessMetallic2.vert",
                "pbrDiffuseNormalRoughnessMetallic2.frag",
                "pbrDiffuseNormalRoughnessMetallicSA.vert",
                "pbrDiffuseNormalRoughnessMetallicSA.frag",
                "pbrDiffuseNormalRoughnessMetallicInstanced.vert",
                "pbrDiffuseNormalRoughnessMetallicInstanced.frag",
                "pbrDiffuseNormalRoughnessMetallic2Instanced.vert",
                "pbrDiffuseNormalRoughnessMetallic2Instanced.frag",
                "pbrEmissive.vert",
                "pbrEmissive.frag",
                "pbrEmissiveInstanced.vert",
                "pbrEmissiveInstanced.frag",
                "pbrEmissiveTexture.vert",
                "pbrEmissiveTexture.frag",
                "pbrEmissiveTextureInstanced.vert",
                "pbrEmissiveTextureInstanced.frag",
                "deferredShading.vert",
                "deferredShading.frag",
                "skybox.vert",
                "skybox.frag",
                "shadowShader.vert",
                "shadowShader.frag",
                "shadowShaderAnimated.vert",
                "shadowShaderAnimated.frag",
                "shadowShaderAnimatedInstanced.vert",
                "shadowShaderAnimatedInstanced.frag",
                "shadowShaderInstanced.vert",
                "shadowShaderInstanced.frag",
                "debug.vert",
                "debug.frag",
                "postprocessing.vert",
                "postprocessing.frag",
                "bloomUp.vert",
                "bloomUp.frag",
                "bloomDown.vert",
                "bloomDown.frag",
            };

            for (auto& filename : shaderFilenames)
            {
                std::string spirvFilename = std::string("bin-int/") + filename + std::string(".spv");
                if (!EngineCore::FileExists(spirvFilename))
                {
                    std::string name = std::string("engine/platform/Vulkan/shaders/") + filename;
                    VK_Shader shader{name, spirvFilename};
                }
            }
            m_ShadersCompiled = true;
        });
        shaderCompileThread.detach();
    }

    void VK_Renderer::DrawWithTransform(const Sprite& sprite, const glm::mat4& transform)
    {
        if (m_CurrentCommandBuffer)
        {
            m_RenderSystemGUIRenderer->RenderSprite(m_FrameInfo, sprite, m_GUIViewProjectionMatrix * transform);
        }
    }

    void VK_Renderer::Draw(const Sprite& sprite, const glm::mat4& position, const glm::vec4& color, const float textureID)
    {
        if (m_CurrentCommandBuffer)
        {
            m_RenderSystemGUIRenderer->RenderSprite(m_FrameInfo, sprite, position, color, textureID);
        }
    }
}
