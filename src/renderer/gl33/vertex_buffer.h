/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#pragma once
#include "taisei.h"

#include "common_buffer.h"

typedef struct VertexBuffer {
	CommonBuffer cbuf;
} VertexBuffer;

VertexBuffer* gl33_vertex_buffer_create(size_t capacity, void *data);
const char* gl33_vertex_buffer_get_debug_label(VertexBuffer *vbuf);
void gl33_vertex_buffer_set_debug_label(VertexBuffer *vbuf, const char *label);
void gl33_vertex_buffer_destroy(VertexBuffer *vbuf);
void gl33_vertex_buffer_invalidate(VertexBuffer *vbuf);
SDL_RWops* gl33_vertex_buffer_get_stream(VertexBuffer *vbuf);
void gl33_vertex_buffer_flush(VertexBuffer *vbuf);
