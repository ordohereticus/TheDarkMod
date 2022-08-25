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
// Contains common formulas for computing interaction.
// Includes: illumination model, fetching surface and light properties
// Excludes: shadows

#pragma tdm_include "tdm_lightproject.glsl"
#pragma tdm_include "tdm_interaction.glsl"
#pragma tdm_include "tdm_utils.glsl"

in vec4 attr_Position;
in vec4 attr_TexCoord;
in vec3 attr_Tangent;
in vec3 attr_Bitangent;
in vec3 attr_Normal;
in vec4 attr_Color;
in int attr_DrawId;

#pragma tdm_include "stages/interaction/interaction.params.glsl"

flat out int var_DrawId;
out vec2 var_TexDiffuse;
out vec2 var_TexNormal;
out vec2 var_TexSpecular;
out vec4 var_TexLight;
out vec4 var_Color;
out mat3 var_TangentBitangentNormalMatrix; 
out vec3 var_LightDirLocal;  
out vec3 var_ViewDirLocal;  
out vec3 var_WorldLightDir;

uniform vec3 u_globalViewOrigin;
uniform vec3 u_globalLightOrigin;

void interactionProcessVertex() {
	// transform vertex position into homogenous clip-space
	gl_Position = objectPosToClip(attr_Position, params[attr_DrawId].modelViewMatrix, u_projectionMatrix);

	// surface texcoords, tangent space, and color generation
	generateSurfaceProperties(
		attr_TexCoord, attr_Color, 
		attr_Tangent, attr_Bitangent, attr_Normal,
		params[attr_DrawId].bumpMatrix, params[attr_DrawId].diffuseMatrix, params[attr_DrawId].specularMatrix,
		params[attr_DrawId].colorModulate, params[attr_DrawId].colorAdd,
		var_TexNormal, var_TexDiffuse, var_TexSpecular,
		var_Color, var_TangentBitangentNormalMatrix
	);

	// light projection texgen
	var_TexLight = computeLightTex(params[attr_DrawId].lightProjectionFalloff, attr_Position);


	var_DrawId = attr_DrawId;

	var_LightDirLocal = (worldPosToObject(u_globalLightOrigin, params[attr_DrawId].modelMatrix) - attr_Position.xyz) * var_TangentBitangentNormalMatrix;
	var_ViewDirLocal = (worldPosToObject(u_globalViewOrigin, params[attr_DrawId].modelMatrix) - attr_Position.xyz) * var_TangentBitangentNormalMatrix;	

	// light->fragment vector in world coordinates
	var_WorldLightDir = (params[attr_DrawId].modelMatrix * attr_Position).xyz - u_globalLightOrigin;
}
