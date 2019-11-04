#include "DeferredRenderer.h"

#include "Scene/Scene.h"
#include "Renderer/Samplers.h"
#include <bigg.hpp>
#include <bx/string.h>
#include <glm/matrix.hpp>
#include <glm/gtc/type_ptr.hpp>

bgfx::VertexDecl DeferredRenderer::PosVertex::decl;

DeferredRenderer::DeferredRenderer(const Scene* scene) :
    Renderer(scene),
    pointLightVertexBuffer(BGFX_INVALID_HANDLE),
    pointLightIndexBuffer(BGFX_INVALID_HANDLE),
    gBufferTextures {
        { BGFX_INVALID_HANDLE, "Diffuse + roughness" },
        { BGFX_INVALID_HANDLE, "Normal"              },
        { BGFX_INVALID_HANDLE, "F0 + metallic"       },
        { BGFX_INVALID_HANDLE, "Depth"               },
        { BGFX_INVALID_HANDLE, nullptr               }
    },
    gBufferTextureUnits {
        Samplers::DEFERRED_DIFFUSE_A,
        Samplers::DEFERRED_NORMAL,
        Samplers::DEFERRED_F0_METALLIC,
        Samplers::DEFERRED_DEPTH
    },
    gBufferSamplerNames {
        "s_texDiffuseA",  
        "s_texNormal",
        "s_texF0Metallic",
        "s_texDepth"
    },
    gBuffer(BGFX_INVALID_HANDLE),
    lightDepthTexture(BGFX_INVALID_HANDLE),
    accumFrameBuffer(BGFX_INVALID_HANDLE),
    lightIndexVecUniform(BGFX_INVALID_HANDLE),
    geometryProgram(BGFX_INVALID_HANDLE),
    pointLightProgram(BGFX_INVALID_HANDLE)
{
    for(bgfx::UniformHandle& handle : gBufferSamplers)
    {
        handle = BGFX_INVALID_HANDLE;
    }
    buffers = gBufferTextures;
}

DeferredRenderer::~DeferredRenderer()
{
}

bool DeferredRenderer::supported()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    return Renderer::supported() &&
           // blitting depth texture after geometry pass
           (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0 &&
           // fragment depth available in fragment shader
           (caps->supported & BGFX_CAPS_FRAGMENT_DEPTH) != 0 &&
           // render target for G-Buffer diffuse and material
           (caps->formats[bgfx::TextureFormat::BGRA8]   & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) != 0 &&
           // render target for G-Buffer normals
           (caps->formats[bgfx::TextureFormat::RGB10A2] & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) != 0 &&
           // multiple render targets
           caps->limits.maxFBAttachments >= GBufferAttachment::Count; // does depth count as an attachment?
}

void DeferredRenderer::onInitialize()
{
    PosVertex::init();

    for(size_t i = 0; i < BX_COUNTOF(gBufferSamplers); i++)
    {
        gBufferSamplers[i] = bgfx::createUniform(gBufferSamplerNames[i], bgfx::UniformType::Sampler);
    }
    lightIndexVecUniform = bgfx::createUniform("u_lightIndexVec", bgfx::UniformType::Vec4);

    // axis-aligned bounding box used as light geometry for light culling
    constexpr float LEFT = -1.0f, RIGHT = 1.0f, BOTTOM = -1.0f, TOP = 1.0f, FRONT = -1.0f, BACK = 1.0f;
    const PosVertex vertices[8] = {
        { LEFT, BOTTOM, FRONT }, { RIGHT, BOTTOM, FRONT }, { LEFT, TOP, FRONT }, { RIGHT, TOP, FRONT },
        { LEFT, BOTTOM, BACK  }, { RIGHT, BOTTOM, BACK  }, { LEFT, TOP, BACK  }, { RIGHT, TOP, BACK  },
    };
    const uint16_t indices[6 * 6] = { // CCW
        0, 1, 3, 3, 2, 0, // front
        5, 4, 6, 6, 7, 5, // back
        4, 0, 2, 2, 6, 4, // left
        1, 5, 7, 7, 3, 1, // right
        2, 3, 7, 7, 6, 2, // top
        4, 5, 1, 1, 0, 4  // bottom
    };

    pointLightVertexBuffer = bgfx::createVertexBuffer(bgfx::copy(&vertices, sizeof(vertices)), PosVertex::decl);
    pointLightIndexBuffer = bgfx::createIndexBuffer(bgfx::copy(&indices, sizeof(indices)));

    char vsName[128], fsName[128];

    bx::snprintf(vsName, BX_COUNTOF(vsName), "%s%s", shaderDir(), "vs_deferred_geometry.bin");
    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_deferred_geometry.bin");
    geometryProgram = bigg::loadProgram(vsName, fsName);

    bx::snprintf(vsName, BX_COUNTOF(vsName), "%s%s", shaderDir(), "vs_deferred_light.bin");
    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_deferred_pointlight.bin");
    pointLightProgram = bigg::loadProgram(vsName, fsName);

    bx::snprintf(vsName, BX_COUNTOF(vsName), "%s%s", shaderDir(), "vs_forward.bin");
    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_forward.bin");
    transparencyProgram = bigg::loadProgram(vsName, fsName);
}

void DeferredRenderer::onReset()
{
    if(!bgfx::isValid(gBuffer))
    {
        gBuffer = createGBuffer();

        for(size_t i = 0; i < GBufferAttachment::Count; i++)
        {
            gBufferTextures[i].handle = bgfx::getTexture(gBuffer, (uint8_t)i);
        }
    }

    if(!bgfx::isValid(accumFrameBuffer))
    {
        // we can't use the G-Buffer's depth texture in the light pass framebuffer
        // binding a texture for reading in the shader and attaching it to a framebuffer
        // at the same time is undefined behaviour in most APIs
        // https://www.khronos.org/opengl/wiki/Memory_Model#Framebuffer_objects
        // we use a different depth texture and just blit it between the geometry and light pass
        // OpenGL does not like BGFX_TEXTURE_RT_WRITE_ONLY here
        // why? we're not attaching or reading it back, just blitting to it
        const uint64_t flags = BGFX_TEXTURE_RT | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_MIN_POINT |
                               BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
                               BGFX_SAMPLER_V_CLAMP;
        bgfx::TextureFormat::Enum depthFormat = findDepthFormat(flags);
        lightDepthTexture = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, depthFormat, flags);

        const bgfx::TextureHandle textures[] = {
            bgfx::getTexture(frameBuffer, 0),
            lightDepthTexture
        };
        accumFrameBuffer = bgfx::createFrameBuffer(BX_COUNTOF(textures), textures); // don't destroy textures
    }
}

void DeferredRenderer::onRender(float dt)
{
    enum : bgfx::ViewId
    {
        vGeometry = 0,
        vLight,
        vTransparent
    };

    const uint32_t BLACK = 0x000000FF;

    bgfx::setViewClear(vGeometry, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, BLACK, 1.0f);
    bgfx::setViewRect(vGeometry, 0, 0, width, height);
    bgfx::setViewFrameBuffer(vGeometry, gBuffer);
    bgfx::touch(vGeometry);

    bgfx::setViewClear(vLight, BGFX_CLEAR_COLOR, clearColor);
    bgfx::setViewRect(vLight, 0, 0, width, height);
    bgfx::setViewFrameBuffer(vLight, accumFrameBuffer);
    bgfx::touch(vLight);

    bgfx::setViewClear(vTransparent, BGFX_CLEAR_NONE);
    bgfx::setViewRect(vTransparent, 0, 0, width, height);
    bgfx::setViewFrameBuffer(vTransparent, accumFrameBuffer);
    bgfx::touch(vTransparent);

    if(!scene->loaded)
        return;

    setViewProjection(vGeometry);
    setViewProjection(vLight);
    setViewProjection(vTransparent);

    bgfx::setViewName(vGeometry, "Deferred geometry pass");

    // render geometry, write to G-Buffer

    const uint64_t state = BGFX_STATE_DEFAULT & ~BGFX_STATE_CULL_MASK;

    for(const Mesh& mesh : scene->meshes)
    {
        const Material& mat = scene->materials[mesh.material];
        // transparent materials are rendered in a seperate forward pass (view vTransparent)
        if(!mat.blend)
        {
            glm::mat4 model = glm::identity<glm::mat4>();
            bgfx::setTransform(glm::value_ptr(model));
            setNormalMatrix(model);
            bgfx::setVertexBuffer(0, mesh.vertexBuffer);
            bgfx::setIndexBuffer(mesh.indexBuffer);
            uint64_t materialState = pbr.bindMaterial(mat);
            bgfx::setState(state | materialState);
            bgfx::submit(vGeometry, geometryProgram);
        }
    }

    bgfx::setViewName(vLight, "Deferred light pass");

    // render lights to framebuffer
    // cull with light geometry
    //   - axis-aligned bounding box (TODO? sphere for point lights)
    //   - read depth from geometry pass
    //   - reverse depth test
    //   - render backfaces
    //   - this shades all pixels between camera and backfaces
    // accumulate light contributions (blend mode add)
    // TODO? tiled-deferred is probably faster for small lights
    // https://software.intel.com/sites/default/files/m/d/4/1/d/8/lauritzen_deferred_shading_siggraph_2010.pdf

    // copy G-Buffer depth texture to depth attachment for light pass
    // we can't attach it to the frame buffer and read it in the shader (unprojecting world position) at the same time
    // blit happens before any compute or draw calls
    bgfx::blit(vLight, lightDepthTexture, 0, 0, bgfx::getTexture(gBuffer, GBufferAttachment::Depth));

    // TODO ambient light
    // full screen quad

    // point lights

    for(size_t i = 0; i < scene->pointLights.lights.size(); i++)
    {
        // position light geometry (bounding box)
        const PointLight& light = scene->pointLights.lights[i];
        float radius = light.calculateRadius();
        glm::mat4 scale = glm::scale(glm::identity<glm::mat4>(), glm::vec3(radius));
        glm::mat4 translate = glm::translate(glm::identity<glm::mat4>(), light.position);
        glm::mat4 model = translate * scale;
        bgfx::setTransform(glm::value_ptr(model));
        bgfx::setVertexBuffer(0, pointLightVertexBuffer);
        bgfx::setIndexBuffer(pointLightIndexBuffer);
        float lightIndexVec[4] = { (float)i };
        bgfx::setUniform(lightIndexVecUniform, lightIndexVec);
        for(size_t i = 0; i < GBufferAttachment::Count; i++)
        {
            bgfx::setTexture(gBufferTextureUnits[i], gBufferSamplers[i], bgfx::getTexture(gBuffer, (uint8_t)i));
        }
        lights.bindLights(scene);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_GEQUAL | BGFX_STATE_CULL_CCW | BGFX_STATE_BLEND_ADD);
        bgfx::submit(vLight, pointLightProgram);
    }

    bgfx::setViewName(vTransparent, "Transparent forward pass");

    for(const Mesh& mesh : scene->meshes)
    {
        const Material& mat = scene->materials[mesh.material];
        if(mat.blend)
        {
            glm::mat4 model = glm::identity<glm::mat4>();
            bgfx::setTransform(glm::value_ptr(model));
            setNormalMatrix(model);
            bgfx::setVertexBuffer(0, mesh.vertexBuffer);
            bgfx::setIndexBuffer(mesh.indexBuffer);
            uint64_t materialState = pbr.bindMaterial(mat);
            lights.bindLights(scene);
            bgfx::setState(state | materialState);
            bgfx::submit(vTransparent, transparencyProgram);
        }
    }
}

void DeferredRenderer::onShutdown()
{
    bgfx::destroy(geometryProgram);
    bgfx::destroy(pointLightProgram);
    bgfx::destroy(transparencyProgram);
    for(bgfx::UniformHandle& handle : gBufferSamplers)
    {
        bgfx::destroy(handle);
        handle = BGFX_INVALID_HANDLE;
    }
    bgfx::destroy(pointLightVertexBuffer);
    bgfx::destroy(pointLightIndexBuffer);
    if(bgfx::isValid(lightDepthTexture))
        bgfx::destroy(lightDepthTexture);
    if(bgfx::isValid(gBuffer))
        bgfx::destroy(gBuffer);
    if(bgfx::isValid(accumFrameBuffer))
        bgfx::destroy(accumFrameBuffer);

    geometryProgram = pointLightProgram = transparencyProgram = BGFX_INVALID_HANDLE;
    lightIndexVecUniform = BGFX_INVALID_HANDLE;
    pointLightVertexBuffer = BGFX_INVALID_HANDLE;
    pointLightIndexBuffer = BGFX_INVALID_HANDLE;
    lightDepthTexture = BGFX_INVALID_HANDLE;
    gBuffer = BGFX_INVALID_HANDLE;
    accumFrameBuffer = BGFX_INVALID_HANDLE;
}

bgfx::FrameBufferHandle DeferredRenderer::createGBuffer()
{
    bgfx::TextureHandle textures[GBufferAttachment::Count];

    const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
                                  BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;

    bgfx::TextureFormat::Enum attachmentFormats[] =
    {
        bgfx::TextureFormat::BGRA8,
        bgfx::TextureFormat::RGB10A2,
        bgfx::TextureFormat::BGRA8
    };

    for(size_t i = 0; i < GBufferAttachment::Depth; i++)
    {
        assert(bgfx::isTextureValid(0, false, 1, attachmentFormats[i], BGFX_TEXTURE_RT | samplerFlags));
        textures[i] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, attachmentFormats[i], BGFX_TEXTURE_RT | samplerFlags);
    }

    // not write only
    bgfx::TextureFormat::Enum depthFormat = findDepthFormat(BGFX_TEXTURE_RT | samplerFlags);
    assert(depthFormat != bgfx::TextureFormat::Count);
    textures[Depth] = bgfx::createTexture2D(bgfx::BackbufferRatio::Equal, false, 1, depthFormat, BGFX_TEXTURE_RT | samplerFlags);

    bgfx::FrameBufferHandle gb = bgfx::createFrameBuffer((uint8_t)GBufferAttachment::Count, textures, true);

    if(!bgfx::isValid(gb))
        Log->error("Failed to create G-Buffer");
    else
        bgfx::setName(gb, "G-Buffer");

    return gb;
}
