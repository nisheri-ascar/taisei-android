/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#pragma once
#include "taisei.h"

#include "resource/shader_object.h"
#include "opengl.h"

struct ShaderObject {
	GLuint gl_handle;
	ShaderStage stage;
	char debug_label[R_DEBUG_LABEL_SIZE];
	uint num_attribs;
	GLSLAttribute *attribs;
};

bool gl33_shader_language_supported(const ShaderLangInfo *lang, ShaderLangInfo *out_alternative);

ShaderObject *gl33_shader_object_compile(ShaderSource *source);
void gl33_shader_object_destroy(ShaderObject *shobj);
void gl33_shader_object_set_debug_label(ShaderObject *shobj, const char *label);
const char *gl33_shader_object_get_debug_label(ShaderObject *shobj);
bool gl33_shader_object_transfer(ShaderObject *dst, ShaderObject *src);
