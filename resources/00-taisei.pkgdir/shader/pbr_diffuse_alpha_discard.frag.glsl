#version 330 core

#define PBR_UNCONDITIONAL_FEATURES (PBR_FEATURE_DIFFUSE_MAP | PBR_FEATURE_DIFFUSE_ALPHA_DISCARD)

#include "lib/pbr_generic.frag.glslh"
