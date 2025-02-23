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

#pragma once

#include <vulkan/vulkan.h>

#include "renderer/materialDescriptor.h"

namespace GfxRenderEngine
{
    class VK_MaterialDescriptor: public MaterialDescriptor
    {

    public:

        VK_MaterialDescriptor(MaterialType materialType);
        VK_MaterialDescriptor(MaterialType materialType, std::vector<std::shared_ptr<Buffer>>& buffers);
        VK_MaterialDescriptor(MaterialType materialType, std::vector<std::shared_ptr<Texture>>& textures);
        VK_MaterialDescriptor(MaterialType materialType, std::vector<std::shared_ptr<Texture>>& textures, std::vector<std::shared_ptr<Buffer>>& buffers);
        VK_MaterialDescriptor(MaterialType materialType, std::shared_ptr<Cubemap> const& cubemap);

        VK_MaterialDescriptor(VK_MaterialDescriptor const& other);

        virtual ~VK_MaterialDescriptor();

    public:

        virtual MaterialDescriptor::MaterialType GetMaterialType() const override;
        const VkDescriptorSet& GetDescriptorSet() const;
        const VkDescriptorSet& GetShadowDescriptorSet() const;

    private:

        MaterialDescriptor::MaterialType m_MaterialType;
        VkDescriptorSet m_DescriptorSet;
        VkDescriptorSet m_ShadowDescriptorSet;
    };
}
