#version 330 core

#include "lib/render_context.glslh"
#include "interface/tower_light.glslh"

void main(void) {
	gl_Position = r_projectionMatrix * r_modelViewMatrix * vec4(position, 1.0);
	texCoord = (r_textureMatrix * vec4(texCoordRawIn, 0.0, 1.0)).xy;
	normal = normalIn;
	l = lightvec - (r_modelViewMatrix*vec4(position,1.0)).xyz;
}
