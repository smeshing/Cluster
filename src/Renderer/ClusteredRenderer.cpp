#include "ClusteredRenderer.h"

#include "Scene/Scene.h"
#include <bigg.hpp>
#include <bx/string.h>
#include <glm/matrix.hpp>
#include <glm/gtc/type_ptr.hpp>

ClusteredRenderer::ClusteredRenderer(const Scene* scene) :
    Renderer(scene),
    clusterBuildingComputeProgram(BGFX_INVALID_HANDLE),
    lightCullingComputeProgram(BGFX_INVALID_HANDLE),
    lightingProgram(BGFX_INVALID_HANDLE),
    debugVisProgram(BGFX_INVALID_HANDLE)
{
}

bool ClusteredRenderer::supported()
{
    const bgfx::Caps* caps = bgfx::getCaps();
    return Renderer::supported() &&
           // compute shader
           (caps->supported & BGFX_CAPS_COMPUTE) != 0 &&
           // 32-bit index buffers, used for light grid structure
           ((caps->supported & BGFX_CAPS_INDEX32) != 0) &&
           // fragment depth available in fragment shader
           (caps->supported & BGFX_CAPS_FRAGMENT_DEPTH) != 0;
}

void ClusteredRenderer::onInitialize()
{
    // OpenGL backend: uniforms must be created before loading shaders
    clusters.initialize();

    char csName[128], vsName[128], fsName[128];

    bx::snprintf(csName, BX_COUNTOF(csName), "%s%s", shaderDir(), "cs_clustered_clusterbuilding.bin");
    clusterBuildingComputeProgram = bgfx::createProgram(bigg::loadShader(csName), true);

    bx::snprintf(csName, BX_COUNTOF(csName), "%s%s", shaderDir(), "cs_clustered_lightculling.bin");
    lightCullingComputeProgram = bgfx::createProgram(bigg::loadShader(csName), true);

    bx::snprintf(vsName, BX_COUNTOF(vsName), "%s%s", shaderDir(), "vs_clustered.bin");
    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_clustered.bin");
    lightingProgram = bigg::loadProgram(vsName, fsName);

    bx::snprintf(fsName, BX_COUNTOF(fsName), "%s%s", shaderDir(), "fs_clustered_debug_vis.bin");
    debugVisProgram = bigg::loadProgram(vsName, fsName);
}

void ClusteredRenderer::onRender(float dt)
{
    enum : bgfx::ViewId
    {
        vClusterBuilding = 0,
        vLightCulling,
        vLighting
    };
        
    bgfx::setViewName(vClusterBuilding, "Cluster building pass (compute)");
    bgfx::setViewClear(vClusterBuilding, BGFX_CLEAR_NONE);
    // set u_viewRect for screen2Eye to work correctly
    bgfx::setViewRect(vClusterBuilding, 0, 0, width, height);
    // this could be set by a different renderer, reset it (D3D12 cares and crashes)
    bgfx::setViewFrameBuffer(vClusterBuilding, BGFX_INVALID_HANDLE);
    //bgfx::touch(vClusterBuilding);

    bgfx::setViewName(vLightCulling, "Clustered light culling pass (compute)");
    bgfx::setViewClear(vLightCulling, BGFX_CLEAR_NONE);
    bgfx::setViewRect(vLightCulling, 0, 0, width, height);
    bgfx::setViewFrameBuffer(vLightCulling, BGFX_INVALID_HANDLE);
    //bgfx::touch(vLightCulling);

    bgfx::setViewName(vLighting, "Clustered lighting pass");
    bgfx::setViewClear(vLighting, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, clearColor, 1.0f, 0);
    bgfx::setViewRect(vLighting, 0, 0, width, height);
    bgfx::setViewFrameBuffer(vLighting, frameBuffer);
    bgfx::touch(vLighting);

    if(!scene->loaded)
        return;

    clusters.setUniforms(scene, width, height);

    // cluster building needs u_invProj to transform screen coordinates to eye space
    setViewProjection(vClusterBuilding);
    // light culling needs u_view to transform lights to eye space
    setViewProjection(vLightCulling);
    setViewProjection(vLighting);

    // cluster building

    clusters.bindBuffers(false); // write access, all buffers

    bgfx::dispatch(vClusterBuilding,
                    clusterBuildingComputeProgram,
                    ClusterShader::CLUSTERS_X,
                    ClusterShader::CLUSTERS_Y,
                    ClusterShader::CLUSTERS_Z);

    // light culling

    lights.bindLights(scene);
    clusters.bindBuffers(false); // write access, all buffers

    bgfx::dispatch(vLightCulling,
                    lightCullingComputeProgram,
                    1,
                    1,
                    ClusterShader::CLUSTERS_Z / ClusterShader::CLUSTERS_Z_THREADS);

    // lighting    

    uint64_t state = BGFX_STATE_DEFAULT & ~BGFX_STATE_CULL_MASK;

    bool debugVis = variables["DEBUG_VIS"] == "true";
    bgfx::ProgramHandle program = debugVis ? debugVisProgram : lightingProgram;

    for(const Mesh& mesh : scene->meshes)
    {
        glm::mat4 model = glm::identity<glm::mat4>();
        bgfx::setTransform(glm::value_ptr(model));
        setNormalMatrix(model);
        bgfx::setVertexBuffer(0, mesh.vertexBuffer);
        bgfx::setIndexBuffer(mesh.indexBuffer);
        const Material& mat = scene->materials[mesh.material];
        uint64_t materialState = pbr.bindMaterial(mat);
        lights.bindLights(scene);
        clusters.bindBuffers();
        bgfx::setState(state | materialState);
        bgfx::submit(vLighting, program);
    }
}

void ClusteredRenderer::onShutdown()
{
    clusters.shutdown();

    bgfx::destroy(clusterBuildingComputeProgram);
    bgfx::destroy(lightCullingComputeProgram);
    bgfx::destroy(lightingProgram);
    bgfx::destroy(debugVisProgram);

    clusterBuildingComputeProgram = lightCullingComputeProgram = lightingProgram = debugVisProgram =
        BGFX_INVALID_HANDLE;
}
