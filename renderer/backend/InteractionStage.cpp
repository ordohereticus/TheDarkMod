/*****************************************************************************
The Dark Mod GPL Source Code

This file is part of the The Dark Mod Source Code, originally based
on the Doom 3 GPL Source Code as published in 2011.

The Dark Mod Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version. For details, see LICENSE.TXT.

Project: The Dark Mod (http://www.thedarkmod.com/)

******************************************************************************/

#include "precompiled.h"

#include "InteractionStage.h"

#include "RenderBackend.h"
#include "../glsl.h"
#include "../GLSLProgramManager.h"
#include "../FrameBuffer.h"
#include "../FrameBufferManager.h"
#include "../AmbientOcclusionStage.h"
#include "DrawBatchExecutor.h"

// NOTE: must match struct in shader, beware of std140 layout requirements and alignment!
struct InteractionStage::ShaderParams {
	idMat4 modelMatrix;
	idMat4 modelViewMatrix;
	idVec4 bumpMatrix[2];
	idVec4 diffuseMatrix[2];
	idVec4 specularMatrix[2];
	idMat4 lightProjectionFalloff;
	idVec4 lightTextureMatrix[2];
	idVec4 colorModulate;
	idVec4 colorAdd;
	idVec4 diffuseColor;
	idVec4 specularColor;
	idVec4 hasTextureDNS;
	int useBumpmapLightTogglingFix;
	float RGTC;
	idVec2 padding_2;
};

namespace {
	struct InteractionUniforms: GLSLUniformGroup {
		UNIFORM_GROUP_DEF( InteractionUniforms )

		DEFINE_UNIFORM( sampler, normalTexture )
		DEFINE_UNIFORM( sampler, diffuseTexture )
		DEFINE_UNIFORM( sampler, specularTexture )
		DEFINE_UNIFORM( sampler, lightProjectionTexture )
		DEFINE_UNIFORM( sampler, lightProjectionCubemap )
		DEFINE_UNIFORM( sampler, lightFalloffTexture )
		DEFINE_UNIFORM( int, useNormalIndexedDiffuse )
		DEFINE_UNIFORM( int, useNormalIndexedSpecular )
		DEFINE_UNIFORM( sampler, lightDiffuseCubemap )
		DEFINE_UNIFORM( sampler, lightSpecularCubemap )
		DEFINE_UNIFORM( sampler, ssaoTexture )
		DEFINE_UNIFORM( vec3, globalViewOrigin )
		DEFINE_UNIFORM( vec3, globalLightOrigin )

		DEFINE_UNIFORM( int, cubic )
		DEFINE_UNIFORM( float, gamma )
		DEFINE_UNIFORM( float, minLevel )
		DEFINE_UNIFORM( int, ssaoEnabled )
		DEFINE_UNIFORM( vec2, renderResolution )

		DEFINE_UNIFORM( int, shadows )
		DEFINE_UNIFORM( int, softShadowsQuality )
		DEFINE_UNIFORM( float, softShadowsRadius )
		DEFINE_UNIFORM( vec4, shadowRect )
		DEFINE_UNIFORM( int, shadowMapCullFront )
		DEFINE_UNIFORM( sampler, stencilTexture )
		DEFINE_UNIFORM( sampler, depthTexture )
		DEFINE_UNIFORM( sampler, shadowMap )
		DEFINE_UNIFORM( ivec2, stencilMipmapsLevel )
		DEFINE_UNIFORM( vec4, stencilMipmapsScissor )
		DEFINE_UNIFORM( sampler, stencilMipmapsTexture )
	};

	enum TextureUnits {
		TU_NORMAL = 0,
		TU_DIFFUSE = 1,
		TU_SPECULAR = 2,
		TU_LIGHT_PROJECT = 3,
		TU_LIGHT_PROJECT_CUBE = 4,
		TU_LIGHT_FALLOFF = 5,
		TU_LIGHT_DIFFUSE_CUBE = 6,
		TU_LIGHT_SPECULAR_CUBE = 7,
		TU_SSAO = 8,
		TU_SHADOW_MAP = 9,
		TU_SHADOW_DEPTH = 10,
		TU_SHADOW_STENCIL = 11,
		TU_SHADOW_STENCIL_MIPMAPS = 12,
	};
}

void InteractionStage::LoadInteractionShader( GLSLProgram *shader, const idStr &baseName ) {
	idHashMapDict defines;
	defines.Set( "MAX_SHADER_PARAMS", idStr::Fmt( "%d", maxShaderParamsArraySize ) );
	shader->LoadFromFiles( "stages/interaction/" + baseName + ".vs.glsl", "stages/interaction/" + baseName + ".fs.glsl", defines );
	InteractionUniforms *uniforms = shader->GetUniformGroup<InteractionUniforms>();
	uniforms->lightProjectionCubemap.Set( TU_LIGHT_PROJECT_CUBE );
	uniforms->lightProjectionTexture.Set( TU_LIGHT_PROJECT );
	uniforms->lightFalloffTexture.Set( TU_LIGHT_FALLOFF );
	uniforms->lightDiffuseCubemap.Set( TU_LIGHT_DIFFUSE_CUBE );
	uniforms->lightSpecularCubemap.Set( TU_LIGHT_SPECULAR_CUBE );
	uniforms->ssaoTexture.Set( TU_SSAO );
	uniforms->stencilTexture.Set( TU_SHADOW_STENCIL );
	uniforms->stencilMipmapsTexture.Set( TU_SHADOW_STENCIL_MIPMAPS );
	uniforms->depthTexture.Set( TU_SHADOW_DEPTH );
	uniforms->shadowMap.Set( TU_SHADOW_MAP );
	uniforms->normalTexture.Set( TU_NORMAL );
	uniforms->diffuseTexture.Set( TU_DIFFUSE );
	uniforms->specularTexture.Set( TU_SPECULAR );
	shader->BindUniformBlockLocation( 0, "ViewParamsBlock" );
	shader->BindUniformBlockLocation( 1, "PerDrawCallParamsBlock" );
	shader->BindUniformBlockLocation( 2, "ShadowSamplesBlock" );
}


InteractionStage::InteractionStage( DrawBatchExecutor *drawBatchExecutor )
	: drawBatchExecutor( drawBatchExecutor ), interactionShader( nullptr )
{}

void InteractionStage::Init() {
	maxShaderParamsArraySize = drawBatchExecutor->MaxShaderParamsArraySize<ShaderParams>();
	
	ambientInteractionShader = programManager->LoadFromGenerator( "interaction_ambient", 
		[this](GLSLProgram *shader) { LoadInteractionShader( shader, "interaction.ambient" ); } );
	stencilInteractionShader = programManager->LoadFromGenerator( "interaction_stencil", 
		[this](GLSLProgram *shader) { LoadInteractionShader( shader, "interaction.stencil" ); } );
	shadowMapInteractionShader = programManager->LoadFromGenerator( "interaction_shadowmap", 
		[this](GLSLProgram *shader) { LoadInteractionShader( shader, "interaction.shadowmap" ); } );

	qglGenBuffers( 1, &poissonSamplesUbo );
	qglBindBuffer( GL_UNIFORM_BUFFER, poissonSamplesUbo );
	qglBufferData( GL_UNIFORM_BUFFER, 0, nullptr, GL_STATIC_DRAW );
}

void InteractionStage::Shutdown() {
	qglDeleteBuffers( 1, &poissonSamplesUbo );
	poissonSamplesUbo = 0;
	poissonSamples.ClearFree();
}

void InteractionStage::DrawInteractions( viewLight_t *vLight, const drawSurf_t *interactionSurfs, const TiledCustomMipmapStage *stencilShadowMipmaps ) {
	if ( !interactionSurfs ) {
		return;
	}

	TRACE_GL_SCOPE( "DrawInteractions" );

	PreparePoissonSamples();

	// if using float buffers, alpha values are not clamped and can stack up quite high, since most interactions add 1 to its value
	// this in turn causes issues with some shader stage materials that use DST_ALPHA blending.
	// masking the alpha channel for interactions seems to fix those issues, but only do it for float buffers in case it has
	// unwanted side effects
	int alphaMask = r_fboColorBits.GetInteger() == 64 ? GLS_ALPHAMASK : 0;

	// perform setup here that will be constant for all interactions
	GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHMASK | alphaMask | backEnd.depthFunc );
	backEnd.currentSpace = NULL; // ambient/interaction shaders conflict

	backEnd.currentScissor = vLight->scissorRect;
	FB_ApplyScissor();

	// bind the vertex and fragment program
	ChooseInteractionProgram( vLight, interactionSurfs == backEnd.vLight->translucentInteractions );
	InteractionUniforms *uniforms = interactionShader->GetUniformGroup<InteractionUniforms>();
	uniforms->cubic.Set( vLight->lightShader->IsCubicLight() ? 1 : 0 );
	uniforms->globalLightOrigin.Set( vLight->globalLightOrigin );
	uniforms->globalViewOrigin.Set( backEnd.viewDef->renderView.vieworg );
	uniforms->renderResolution.Set( frameBuffers->activeFbo->Width(), frameBuffers->activeFbo->Height() );

	idList<const drawSurf_t *> drawSurfs;
	for ( const drawSurf_t *surf = interactionSurfs; surf; surf = surf->nextOnLight) {
		drawSurfs.AddGrow( surf );
	}
	std::sort( drawSurfs.begin(), drawSurfs.end(), [](const drawSurf_t *a, const drawSurf_t *b) {
		if ( a->ambientCache.isStatic != b->ambientCache.isStatic )
			return a->ambientCache.isStatic;
		if ( a->indexCache.isStatic != b->indexCache.isStatic )
			return a->indexCache.isStatic;

		return a->material < b->material;
	} );

	if ( vLight->lightShader->IsAmbientLight() ) {
		// stgatilov #6090: ambient lights can have cubemaps indexed by surface normal
		// separately for diffuse and specular

		idImage *cubemap = vLight->lightShader->LightAmbientDiffuse();
		if ( cubemap ) {
			uniforms->useNormalIndexedDiffuse.Set( true );
			GL_SelectTexture( TU_LIGHT_DIFFUSE_CUBE );
			cubemap->Bind();
		}
		else {
			uniforms->useNormalIndexedDiffuse.Set( false );
		}

		cubemap = vLight->lightShader->LightAmbientSpecular();
		if ( cubemap ) {
			uniforms->useNormalIndexedSpecular.Set( true );
			GL_SelectTexture( TU_LIGHT_SPECULAR_CUBE );
			cubemap->Bind();
		}
		else {
			uniforms->useNormalIndexedSpecular.Set( false );
		}
	}

	GL_SelectTexture( TU_LIGHT_FALLOFF );
	vLight->falloffImage->Bind();

	if ( r_softShadowsQuality.GetBool() && !backEnd.viewDef->IsLightGem() || vLight->shadows == LS_MAPS )
		BindShadowTexture( stencilShadowMipmaps );

	if( vLight->lightShader->IsAmbientLight() && ambientOcclusion->ShouldEnableForCurrentView() ) {
		ambientOcclusion->BindSSAOTexture( TU_SSAO );
	}

	if ( stencilShadowMipmaps && stencilShadowMipmaps->IsFilled() ) {
		GL_SelectTexture( TU_SHADOW_STENCIL_MIPMAPS );
		stencilShadowMipmaps->BindMipmapTexture();
		// note: correct level is evaluated when mipmaps object is created
		int useLevel = stencilShadowMipmaps->GetMaxLevel();
		int baseLevel = stencilShadowMipmaps->GetBaseLevel();
		idScreenRect scissor = stencilShadowMipmaps->GetScissorAtLevel( useLevel );
		uniforms->stencilMipmapsLevel.Set( useLevel, baseLevel );
		uniforms->stencilMipmapsScissor.Set(
			(scissor.x1 + 0.5f) * (1 << useLevel),
			(scissor.y1 + 0.5f) * (1 << useLevel),
			(scissor.x2 + 0.5f) * (1 << useLevel),
			(scissor.y2 + 0.5f) * (1 << useLevel)
		);
	}
	else {
		uniforms->stencilMipmapsLevel.Set( -1, -1 );
	}

	const idMaterial	*lightShader = vLight->lightShader;
	const float			*lightRegs = vLight->shaderRegisters;
	for ( int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++ ) {
		const shaderStage_t	*lightStage = lightShader->GetStage( lightStageNum );

		// ignore stages that fail the condition
		if ( !lightRegs[lightStage->conditionRegister] ) {
			continue;
		}

		if ( vLight->lightShader->IsCubicLight() ) {
			GL_SelectTexture( TU_LIGHT_PROJECT_CUBE );
			lightStage->texture.image->Bind();
		}
		else {
			GL_SelectTexture( TU_LIGHT_PROJECT );
			lightStage->texture.image->Bind();
		}
		// careful - making bindless textures resident could bind an arbitrary texture to the currently active
		// slot, so reset this to something that is safe to override in bindless mode!
		GL_SelectTexture(TU_NORMAL);

		BeginDrawBatch();
		const drawSurf_t *curBatchCaches = drawSurfs[0];
		for ( const drawSurf_t *surf : drawSurfs ) {
			if ( !surf->ambientCache.IsValid() || !surf->indexCache.IsValid() ) {
#ifdef _DEBUG
				common->Printf( "InteractionStage: missing vertex or index cache\n" );
#endif
				continue;
			}

			if ( surf->ambientCache.isStatic != curBatchCaches->ambientCache.isStatic || surf->indexCache.isStatic != curBatchCaches->indexCache.isStatic ) {
				ExecuteDrawCalls();
			}

			if ( surf->space->weaponDepthHack ) {
				// GL state change, need to execute previous draw calls
				ExecuteDrawCalls();
				RB_EnterWeaponDepthHack();
			}

			curBatchCaches = surf;
			ProcessSingleSurface( vLight, lightStage, surf );

			if ( surf->space->weaponDepthHack ) {
				ExecuteDrawCalls();
				RB_LeaveDepthHack();
			}
		}
		ExecuteDrawCalls();
	}

	GL_SelectTexture( 0 );

	GLSLProgram::Deactivate();
}

void InteractionStage::BindShadowTexture( const TiledCustomMipmapStage *stencilShadowMipmaps ) {
	if ( backEnd.vLight->shadowMapPage.width > 0 ) {
		GL_SelectTexture( TU_SHADOW_MAP );
		globalImages->shadowAtlas->Bind();
	} else {
		GL_SelectTexture( TU_SHADOW_DEPTH );
		globalImages->currentDepthImage->Bind();

		GL_SelectTexture( TU_SHADOW_STENCIL );
		globalImages->shadowDepthFbo->Bind();
		if (globalImages->shadowDepthFbo->texnum != idImage::TEXTURE_NOT_LOADED)
			qglTexParameteri( GL_TEXTURE_2D, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX );
	}
}

void InteractionStage::ChooseInteractionProgram( viewLight_t *vLight, bool translucent ) {
	if ( vLight->lightShader->IsAmbientLight() ) {
		interactionShader = ambientInteractionShader;
		Uniforms::Interaction *uniforms = interactionShader->GetUniformGroup<Uniforms::Interaction>();
		uniforms->ambient = true;
	} else if ( vLight->shadowMapPage.width > 0 ) {
		interactionShader = shadowMapInteractionShader;
	} else {
		interactionShader = stencilInteractionShader;
	}
	interactionShader->Activate();

	InteractionUniforms *uniforms = interactionShader->GetUniformGroup<InteractionUniforms>();
	uniforms->gamma.Set( backEnd.viewDef->IsLightGem() ? 1 : r_ambientGamma.GetFloat() );
	uniforms->minLevel.Set( r_ambientMinLevel.GetFloat() );
	uniforms->ssaoEnabled.Set( ambientOcclusion->ShouldEnableForCurrentView() ? 1 : 0 );

	bool doShadows = !vLight->noShadows && vLight->lightShader->LightCastsShadows(); 
	if ( doShadows && vLight->shadows == LS_MAPS ) {
		// FIXME shadowmap only valid when globalInteractions not empty, otherwise garbage
		doShadows = vLight->globalInteractions != NULL;
	}
	if ( doShadows ) {
		uniforms->shadows.Set( vLight->shadows );
		const renderCrop_t &page = vLight->shadowMapPage;
		// https://stackoverflow.com/questions/5879403/opengl-texture-coordinates-in-pixel-space
		idVec4 v( page.x, page.y, 0, page.width-1 );
		v.ToVec2() = (v.ToVec2() * 2 + idVec2( 1, 1 )) / (2 * 6 * r_shadowMapSize.GetInteger());
		v.w /= 6 * r_shadowMapSize.GetFloat();
		uniforms->shadowRect.Set( v );
	} else {
		uniforms->shadows.Set(0);
	}
	uniforms->shadowMapCullFront.Set( r_shadowMapCullFront );

	if ( !translucent && ( vLight->globalShadows || vLight->localShadows ) && !backEnd.viewDef->IsLightGem() ) {
		uniforms->softShadowsQuality.Set( r_softShadowsQuality.GetInteger() );
	} else {
		uniforms->softShadowsQuality.Set( 0 );
	}
	uniforms->softShadowsRadius.Set( GetEffectiveLightRadius() ); // for soft stencil and all shadow maps
}

void InteractionStage::ProcessSingleSurface( viewLight_t *vLight, const shaderStage_t *lightStage, const drawSurf_t *surf ) {
	const idMaterial	*material = surf->material;
	const float			*surfaceRegs = surf->shaderRegisters;
	const idMaterial	*lightShader = vLight->lightShader;
	const float			*lightRegs = vLight->shaderRegisters;
	drawInteraction_t	inter;

	if ( !surf->ambientCache.IsValid() ) {
		return;
	}

	inter.surf = surf;

	inter.cubicLight = lightShader->IsCubicLight(); // nbohr1more #3881: cubemap lights
	inter.ambientLight = lightShader->IsAmbientLight();

	// the base projections may be modified by texture matrix on light stages
	idPlane lightProject[4];
	R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[0], lightProject[0] );
	R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[1], lightProject[1] );
	R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[2], lightProject[2] );
	R_GlobalPlaneToLocal( surf->space->modelMatrix, vLight->lightProject[3], lightProject[3] );

	memcpy( inter.lightProjection, lightProject, sizeof( inter.lightProjection ) );

	float lightTexMatrix[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	if ( lightStage->texture.hasMatrix )
		RB_GetShaderTextureMatrix( lightRegs, &lightStage->texture, lightTexMatrix );
	// stgatilov: we no longer merge two transforms together, since we need light-volume coords in fragment shader
	//RB_BakeTextureMatrixIntoTexgen( reinterpret_cast<class idPlane *>(inter.lightProjection), lightTexMatrix );
	inter.lightTextureMatrix[0].Set( lightTexMatrix[0], lightTexMatrix[4], 0, lightTexMatrix[12] );
	inter.lightTextureMatrix[1].Set( lightTexMatrix[1], lightTexMatrix[5], 0, lightTexMatrix[13] );

	inter.bumpImage = NULL;
	inter.specularImage = NULL;
	inter.diffuseImage = NULL;
	inter.diffuseColor[0] = inter.diffuseColor[1] = inter.diffuseColor[2] = inter.diffuseColor[3] = 0;
	inter.specularColor[0] = inter.specularColor[1] = inter.specularColor[2] = inter.specularColor[3] = 0;

	// backEnd.lightScale is calculated so that lightColor[] will never exceed
	// tr.backEndRendererMaxLight
	float lightColor[4] = {
		lightColor[0] = backEnd.lightScale * lightRegs[lightStage->color.registers[0]],
		lightColor[1] = backEnd.lightScale * lightRegs[lightStage->color.registers[1]],
		lightColor[2] = backEnd.lightScale * lightRegs[lightStage->color.registers[2]],
		lightColor[3] = lightRegs[lightStage->color.registers[3]]
	};

	// go through the individual stages
	for ( int surfaceStageNum = 0; surfaceStageNum < material->GetNumStages(); surfaceStageNum++ ) {
		const shaderStage_t	*surfaceStage = material->GetStage( surfaceStageNum );

		if ( !surfaceRegs[ surfaceStage->conditionRegister ] ) // ignore stage that fails the condition
			continue;


		void R_SetDrawInteraction( const shaderStage_t *surfaceStage, const float *surfaceRegs, idImage **image, idVec4 matrix[2], float color[4] );

		switch ( surfaceStage->lighting ) {
		case SL_AMBIENT: {
			// ignore ambient stages while drawing interactions
			break;
		}
		case SL_BUMP: {				
			if ( !r_skipBump.GetBool() ) {
				PrepareDrawCommand( &inter ); // draw any previous interaction
				inter.diffuseImage = NULL;
				inter.specularImage = NULL;
				R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.bumpImage, inter.bumpMatrix, NULL );
			}
			break;
		}
		case SL_DIFFUSE: {
			if ( inter.diffuseImage ) {
				PrepareDrawCommand( &inter );
			}
			R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.diffuseImage,
								  inter.diffuseMatrix, inter.diffuseColor.ToFloatPtr() );
			inter.diffuseColor[0] *= lightColor[0];
			inter.diffuseColor[1] *= lightColor[1];
			inter.diffuseColor[2] *= lightColor[2];
			inter.diffuseColor[3] *= lightColor[3];
			inter.vertexColor = surfaceStage->vertexColor;
			break;
		}
		case SL_SPECULAR: {
			// nbohr1more: #4292 nospecular and nodiffuse fix
			if ( vLight->noSpecular ) {
				break;
			}
			if ( inter.specularImage ) {
				PrepareDrawCommand( &inter );
			}
			R_SetDrawInteraction( surfaceStage, surfaceRegs, &inter.specularImage,
								  inter.specularMatrix, inter.specularColor.ToFloatPtr() );
			inter.specularColor[0] *= lightColor[0];
			inter.specularColor[1] *= lightColor[1];
			inter.specularColor[2] *= lightColor[2];
			inter.specularColor[3] *= lightColor[3];
			inter.vertexColor = surfaceStage->vertexColor;
			break;
		}
		}
	}

	// draw the final interaction
	PrepareDrawCommand( &inter );
}

void InteractionStage::PrepareDrawCommand( drawInteraction_t *din ) {
	if ( !din->bumpImage ) {
		if ( !r_skipBump.GetBool() )
			return;
		din->bumpImage = globalImages->flatNormalMap;		
	}

	if ( !din->diffuseImage || r_skipDiffuse.GetBool() ) {
		din->diffuseImage = globalImages->blackImage;
	}

	if ( !din->specularImage || r_skipSpecular.GetBool() ) {
		din->specularImage = globalImages->blackImage;
	}

	if (currentIndex > 0) {
		if (!din->bumpImage->IsBound( TU_NORMAL )
			|| !din->diffuseImage->IsBound( TU_DIFFUSE )
			|| !din->specularImage->IsBound( TU_SPECULAR ) ) {

			// change in textures, execute previous draw calls
			ExecuteDrawCalls();
		}
	}

	if (currentIndex == 0) {
		// bind new material textures
		GL_SelectTexture( TU_NORMAL );
		din->bumpImage->Bind();
		GL_SelectTexture( TU_DIFFUSE );
		din->diffuseImage->Bind();
		GL_SelectTexture( TU_SPECULAR );
		din->specularImage->Bind();
	}

	ShaderParams &params = drawBatch.shaderParams[currentIndex];

	memcpy( params.modelMatrix.ToFloatPtr(), din->surf->space->modelMatrix, sizeof(idMat4) );
	memcpy( params.modelViewMatrix.ToFloatPtr(), din->surf->space->modelViewMatrix, sizeof(idMat4) );
	params.bumpMatrix[0] = din->bumpMatrix[0];
	params.bumpMatrix[1] = din->bumpMatrix[1];
	params.diffuseMatrix[0] = din->diffuseMatrix[0];
	params.diffuseMatrix[1] = din->diffuseMatrix[1];
	params.specularMatrix[0] = din->specularMatrix[0];
	params.specularMatrix[1] = din->specularMatrix[1];
	memcpy( params.lightProjectionFalloff.ToFloatPtr(), din->lightProjection, sizeof(idMat4) );
	memcpy( params.lightTextureMatrix->ToFloatPtr(), din->lightTextureMatrix, sizeof(idVec4) * 2 );
	switch ( din->vertexColor ) {
	case SVC_IGNORE:
		params.colorModulate = idVec4(0, 0, 0, 0);
		params.colorAdd = idVec4(1, 1, 1, 1);
		break;
	case SVC_MODULATE:
		params.colorModulate = idVec4(1, 1, 1, 1);
		params.colorAdd = idVec4(0, 0, 0, 0);
		break;
	case SVC_INVERSE_MODULATE:
		params.colorModulate = idVec4(-1, -1, -1, -1);
		params.colorAdd = idVec4(1, 1, 1, 1);
		break;
	}
	params.diffuseColor = din->diffuseColor;
	params.specularColor = din->specularColor;
	if ( !din->bumpImage ) {
		params.hasTextureDNS = idVec4(1, 0, 1, 0);
	} else {
		params.hasTextureDNS = idVec4(1, 1, 1, 0);
	}
	params.useBumpmapLightTogglingFix = r_useBumpmapLightTogglingFix.GetBool() && !din->surf->material->ShouldCreateBackSides();
	params.RGTC = din->bumpImage->internalFormat == GL_COMPRESSED_RG_RGTC2;

	drawBatch.surfs[currentIndex] = din->surf;

	++currentIndex;
	if (currentIndex == drawBatch.maxBatchSize) {
		ExecuteDrawCalls();
	}
}

void InteractionStage::BeginDrawBatch() {
	currentIndex = 0;
	drawBatch = drawBatchExecutor->BeginBatch<ShaderParams>();
}

void InteractionStage::ExecuteDrawCalls() {
	if (currentIndex == 0) {
		return;
	}
	drawBatchExecutor->ExecuteDrawVertBatch( currentIndex );
	BeginDrawBatch();
}

void InteractionStage::PreparePoissonSamples() {
	int sampleK = r_softShadowsQuality.GetInteger();
	if ( sampleK > 0 && poissonSamples.Num() != sampleK ) {
		GeneratePoissonDiskSampling( poissonSamples, sampleK );
		// note: due to std140 buffer requirements, array members must be aligned with vec4 size
		// so we have to copy our samples to a vec4 array for upload :-/
		idList<idVec4> uploadSamples;
		uploadSamples.SetNum( poissonSamples.Num() );
		for ( int i = 0; i < poissonSamples.Num(); ++i ) {
			uploadSamples[i].x = poissonSamples[i].x;
			uploadSamples[i].y = poissonSamples[i].y;
		}
		size_t size = uploadSamples.Num() * sizeof(idVec4);
		qglBindBuffer( GL_UNIFORM_BUFFER, poissonSamplesUbo );
		qglBufferData( GL_UNIFORM_BUFFER, size, uploadSamples.Ptr(), GL_STATIC_DRAW );
	}
	qglBindBufferBase( GL_UNIFORM_BUFFER, 2, poissonSamplesUbo );
}
