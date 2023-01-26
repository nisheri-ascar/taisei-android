/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
*/

#pragma once
#include "taisei.h"

typedef enum FileWatchEvent {
	FILEWATCH_FILE_UPDATED,
	FILEWATCH_FILE_DELETED,
} FileWatchEvent;

typedef struct FileWatch FileWatch;

void filewatch_init(void);
void filewatch_shutdown(void);

FileWatch *filewatch_watch(const char *syspath);
void filewatch_unwatch(FileWatch *watch);
