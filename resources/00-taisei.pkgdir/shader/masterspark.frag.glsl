#version 330 core

#include "lib/util.glslh"
#include "interface/standard.glslh"

UNIFORM(1) float t;

float smootherstep(float x, float width) {
	// step function with a very slow decay towards zero

	return x < 0 ? 1 / (x*x/width/width + 1) : 1;
}

void main(void) {
	vec2 r = vec2(texCoord.x - 0.5, flip_native_to_bottomleft(texCoord.y));

	// globally distort the coordinate system a little bit for a more interesting/dynamic shape
	r.x -= 0.005 * sin(r.y * 13.32 - t * 0.53);

	// this is > 0 in the basic core region of the spark. to make the shape more interesting, it is modulated with additional terms later.
	//
	// The terms after abs(r.x) diverge to -∞ (= invisible later) assuring that
	// there are no hard edges. This is necessary because I use this debil
	// smootherstep function which never actually is zero for finite x. But it looks
	// better, mate. Glow!
	float coreRegion = 0.4 * pow(r.y, 0.25) - pow(abs(r.x), 1.0)*1.0 - pow(0.1/(0.5 - abs(r.x))+0.1*pow(abs(r.x), 1.5)/r.y, 2);

	// smootherstep(coreRegion, 0) would be a mask for the bare cone shape of the spark. By adding extra terms it is distorted.
	// sin adds a wavy edge, the fraction with t takes care of fading in/out

	float colorRegion = coreRegion - 0.01 * sin(r.y * 30 - t * 0.7) * (1-1/(t+1)) - 0.05;

	float whiteRegion = coreRegion - 0.15 * pow(r.y, 0.5) - 0.2 / (1 + t * 0.1);

	vec3 tint = hsv2rgb(vec3(t / 100.0 - pow(r.y, 0.25), 1.0, 1.0));

	fragColor = mix(vec4(tint, 0) * smootherstep(colorRegion, 0.05), vec4(vec3(1), 0), 0.5 * smootherstep(whiteRegion, 0.02));
}
