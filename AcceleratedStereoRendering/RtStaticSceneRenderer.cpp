/*
 * Authors: Niko Wissmann
 */

#include "Framework.h"
#include "RtStaticSceneRenderer.h"


RtStaticSceneRenderer::SharedPtr RtStaticSceneRenderer::create(RtScene::SharedPtr pScene)
{
    return SharedPtr(new RtStaticSceneRenderer(pScene));
}

static bool setVertexBuffer(ParameterBlockReflection::BindLocation bindLocation, uint32_t vertexLoc, const Vao* pVao, GraphicsVars* pVars)
{
    if (bindLocation.setIndex != ProgramReflection::kInvalidLocation)
    {
        const auto& elemDesc = pVao->getElementIndexByLocation(vertexLoc);
        if (elemDesc.elementIndex == Vao::ElementDesc::kInvalidIndex)
        {
            pVars->getDefaultBlock()->setSrv(bindLocation, 0, nullptr);
        }
        else
        {
            assert(elemDesc.elementIndex == 0);
            pVars->getDefaultBlock()->setSrv(bindLocation, 0, pVao->getVertexBuffer(elemDesc.vbIndex)->getSRV());
            return true;
        }
    }
    return false;
}

void RtStaticSceneRenderer::renderScene(RenderContext * pContext, std::shared_ptr<RtProgramVars> pRtVars, std::shared_ptr<RtState> pState, uvec2 targetDim, Camera * pCamera)
{
    renderScene(pContext, pRtVars, pState, uvec3(targetDim, 1), pCamera);
}

void RtStaticSceneRenderer::renderScene(RenderContext * pContext, std::shared_ptr<RtProgramVars> pRtVars, std::shared_ptr<RtState> pState, uvec3 targetDim, Camera * pCamera)
{


    InstanceData data;
    data.currentData.pCamera = pCamera == nullptr ? mpScene->getActiveCamera().get() : pCamera;
    uint32_t hitCount = pRtVars->getHitProgramsCount();
    if (hitCount)
    {
        updateVariableOffsets(pState->getProgram()->getHitProgram(0)->getReflector().get()); // Using the local+global reflector, some resources are `shared`
        initializeMeshBufferLocation(pState->getProgram()->getHitProgram(0)->getLocalReflector().get()); // Using the local reflector only
    }

    setRayGenShaderData(pRtVars.get(), data);
    setGlobalData(pRtVars.get(), data);

    if (mUpdateShaderState)
    {
        // Set the miss-shader data
        for (data.progId = 0; data.progId < pRtVars->getMissProgramsCount(); data.progId++)
        {
            if (pRtVars->getMissVars(data.progId))
            {
                setMissShaderData(pRtVars.get(), data);
            }
        }

        // Set the hit-shader data
        for (data.progId = 0; data.progId < hitCount; data.progId++)
        {
            if (pRtVars->getHitVars(data.progId).empty()) continue;
            for (data.model = 0; data.model < mpScene->getModelCount(); data.model++)
            {
                const Model* pModel = mpScene->getModel(data.model).get();
                data.currentData.pModel = pModel;
                for (data.modelInstance = 0; data.modelInstance < mpScene->getModelInstanceCount(data.model); data.modelInstance++)
                {
                    for (data.mesh = 0; data.mesh < pModel->getMeshCount(); data.mesh++)
                    {
                        const Mesh* pMesh = pModel->getMesh(data.mesh).get();
                        for (data.meshInstance = 0; data.meshInstance < pModel->getMeshInstanceCount(data.mesh); data.meshInstance++)
                        {
                            setHitShaderData(pRtVars.get(), data);
                        }
                    }
                }
            }
        }
    }

    if (!pRtVars->apply(pContext, pState->getRtso().get(), mUpdateShaderState))
    {
        logError("RtSceneRenderer::renderScene() - applying RtProgramVars failed, most likely because we ran out of descriptors.", true);
        assert(false);
    }

    mUpdateShaderState = false;

    pContext->raytrace(pRtVars, pState, targetDim.x, targetDim.y, targetDim.z);
}

void RtStaticSceneRenderer::setPerFrameData(RtProgramVars * pRtVars, InstanceData & data)
{
    SceneRenderer::setPerFrameData(data.currentData);
}

bool RtStaticSceneRenderer::setPerMeshInstanceData(const CurrentWorkingData & currentData, const Scene::ModelInstance * pModelInstance, const Model::MeshInstance * pMeshInstance, uint32_t drawInstanceID)
{
    const Mesh* pMesh = pMeshInstance->getObject().get();
    const Vao* pVao = pModelInstance->getObject()->getMeshVao(pMesh).get();
    auto& pVars = currentData.pVars;

    if (mMeshBufferLocations.indices.setIndex != ProgramReflection::kInvalidLocation)
    {
        auto pSrv = pVao->getIndexBuffer() ? pVao->getIndexBuffer()->getSRV() : nullptr;
        pVars->getDefaultBlock()->setSrv(mMeshBufferLocations.indices, 0, pSrv);
    }

    setVertexBuffer(mMeshBufferLocations.lightmapUVs, VERTEX_LIGHTMAP_UV_LOC, pVao, pVars);
    setVertexBuffer(mMeshBufferLocations.texC, VERTEX_TEXCOORD_LOC, pVao, pVars);
    setVertexBuffer(mMeshBufferLocations.normal, VERTEX_NORMAL_LOC, pVao, pVars);
    setVertexBuffer(mMeshBufferLocations.position, VERTEX_POSITION_LOC, pVao, pVars);
    setVertexBuffer(mMeshBufferLocations.bitangent, VERTEX_BITANGENT_LOC, pVao, pVars);

    // Bind vertex buffer for previous positions if it exists. If not, we bind the current positions.
    if (!setVertexBuffer(mMeshBufferLocations.prevPosition, VERTEX_PREV_POSITION_LOC, pVao, pVars))
    {
        setVertexBuffer(mMeshBufferLocations.prevPosition, VERTEX_POSITION_LOC, pVao, pVars);
    }

    return SceneRenderer::setPerMeshInstanceData(currentData, pModelInstance, pMeshInstance, drawInstanceID);
}

void RtStaticSceneRenderer::setHitShaderData(RtProgramVars * pRtVars, InstanceData & data)
{
    const RtScene* pScene = dynamic_cast<RtScene*>(mpScene.get());
    uint32_t instanceId = pScene->getInstanceId(data.model, data.modelInstance, data.mesh, data.meshInstance);
    data.currentData.pVars = pRtVars->getHitVars(data.progId)[instanceId].get();
    if (data.currentData.pVars)
    {
        const Model* pModel = mpScene->getModel(data.model).get();
        const Scene::ModelInstance* pModelInstance = mpScene->getModelInstance(data.model, data.modelInstance).get();
        const Mesh* pMesh = pModel->getMesh(data.mesh).get();
        const Model::MeshInstance* pMeshInstance = pModel->getMeshInstance(data.mesh, data.meshInstance).get();

        setPerFrameData(pRtVars, data);
        setPerModelData(data.currentData);
        setPerModelInstanceData(data.currentData, pModelInstance, data.modelInstance);
        setPerMeshData(data.currentData, pMesh);
        setPerMeshInstanceData(data.currentData, pModelInstance, pMeshInstance, 0);
        setPerMaterialData(data.currentData, pMesh->getMaterial().get());
    }
}

void RtStaticSceneRenderer::setMissShaderData(RtProgramVars * pRtVars, InstanceData & data)
{
    data.currentData.pVars = pRtVars->getMissVars(data.progId).get();
    if (data.currentData.pVars)
    {
        setPerFrameData(pRtVars, data);
    }
}

void RtStaticSceneRenderer::setRayGenShaderData(RtProgramVars * pRtVars, InstanceData & data)
{
    data.currentData.pVars = pRtVars->getRayGenVars().get();
    setPerFrameData(pRtVars, data);
}

void RtStaticSceneRenderer::setGlobalData(RtProgramVars * pRtVars, InstanceData & data)
{
    data.currentData.pVars = pRtVars->getGlobalVars().get();

    GraphicsVars* pVars = data.currentData.pVars;
    ParameterBlockReflection::BindLocation loc = pVars->getReflection()->getDefaultParameterBlock()->getResourceBinding("gRtScene");
    if (loc.setIndex != ProgramReflection::kInvalidLocation)
    {
        RtScene* pRtScene = dynamic_cast<RtScene*>(mpScene.get());
        pVars->getDefaultBlock()->setSrv(loc, 0, pRtScene->getTlasSrv(pRtVars->getHitProgramsCount()));
    }

    ConstantBuffer::SharedPtr pDxrPerFrame = pVars->getConstantBuffer("DxrPerFrame");
    if (pDxrPerFrame)
    {
        pDxrPerFrame["hitProgramCount"] = pRtVars->getHitProgramsCount();
    }
    SceneRenderer::setPerFrameData(data.currentData);
}

void RtStaticSceneRenderer::initializeMeshBufferLocation(const ProgramReflection * pReflection)
{
    mMeshBufferLocations.indices = pReflection->getDefaultParameterBlock()->getResourceBinding("gIndices");
    mMeshBufferLocations.texC = pReflection->getDefaultParameterBlock()->getResourceBinding("gTexCrds");
    mMeshBufferLocations.lightmapUVs = pReflection->getDefaultParameterBlock()->getResourceBinding("gLightMapUVs");
    mMeshBufferLocations.normal = pReflection->getDefaultParameterBlock()->getResourceBinding("gNormals");
    mMeshBufferLocations.position = pReflection->getDefaultParameterBlock()->getResourceBinding("gPositions");
    mMeshBufferLocations.prevPosition = pReflection->getDefaultParameterBlock()->getResourceBinding("gPrevPositions");
    mMeshBufferLocations.bitangent = pReflection->getDefaultParameterBlock()->getResourceBinding("gBitangents");
}
