/*
 * This software is licensed under the terms of the MIT License.
 * See COPYING for further information.
 * ---
 * Copyright (c) 2011-2019, Lukas Weber <laochailan@web.de>.
 * Copyright (c) 2012-2019, Andrei Alexeyev <akari@taisei-project.org>.
 */

#include "taisei.h"

#include "resource.h"

#include "config.h"
#include "events.h"
#include "filewatch/filewatch.h"
#include "menu/mainmenu.h"
#include "renderer/common/backend.h"
#include "taskmanager.h"
#include "video.h"

#include "animation.h"
#include "bgm.h"
#include "font.h"
#include "material.h"
#include "model.h"
#include "postprocess.h"
#include "sfx.h"
#include "shader_object.h"
#include "shader_program.h"
#include "sprite.h"
#include "texture.h"

#define DEBUG_LOCKS 0

ResourceHandler *_handlers[] = {
	[RES_ANIM]              = &animation_res_handler,
	[RES_BGM]               = &bgm_res_handler,
	[RES_FONT]              = &font_res_handler,
	[RES_MATERIAL]          = &material_res_handler,
	[RES_MODEL]             = &model_res_handler,
	[RES_POSTPROCESS]       = &postprocess_res_handler,
	[RES_SFX]               = &sfx_res_handler,
	[RES_SHADER_OBJECT]     = &shader_object_res_handler,
	[RES_SHADER_PROGRAM]    = &shader_program_res_handler,
	[RES_SPRITE]            = &sprite_res_handler,
	[RES_TEXTURE]           = &texture_res_handler,
};

typedef enum ResourceStatus {
	RES_STATUS_LOADING,
	RES_STATUS_LOADED,
	RES_STATUS_FAILED,
} ResourceStatus;

typedef enum LoadStatus {
	LOAD_NONE,
	LOAD_OK,
	LOAD_FAILED,
	LOAD_CONT_ON_MAIN,
	LOAD_CONT,
} LoadStatus;

typedef struct InternalResource InternalResource;
typedef struct InternalResLoadState InternalResLoadState;

typedef DYNAMIC_ARRAY(InternalResource*) IResPtrArray;

#define HT_SUFFIX                      ires_counted_set
#define HT_KEY_TYPE                    InternalResource*
#define HT_VALUE_TYPE                  uint32_t
#define HT_FUNC_HASH_KEY(key)          htutil_hashfunc_uint64((uintptr_t)(key))
#define HT_FUNC_COPY_KEY(dst, src)     (*(dst) = (InternalResource*)(src))
#define HT_KEY_FMT                     "p"
#define HT_KEY_PRINTABLE(key)          (key)
#define HT_VALUE_FMT                   "i"
#define HT_VALUE_PRINTABLE(val)        (val)
#define HT_KEY_CONST
#define HT_DECL
#define HT_IMPL
#include "hashtable_incproxy.inc.h"

#define HT_SUFFIX                      watch2iresset
#define HT_KEY_TYPE                    FileWatch*
#define HT_VALUE_TYPE                  ht_ires_counted_set_t
#define HT_FUNC_HASH_KEY(key)          htutil_hashfunc_uint64((uintptr_t)(key))
#define HT_FUNC_COPY_KEY(dst, src)     (*(dst) = (FileWatch*)(src))
#define HT_KEY_FMT                     "p"
#define HT_KEY_PRINTABLE(key)          (key)
#define HT_VALUE_FMT                   "p"
#define HT_VALUE_PRINTABLE(val)        (&(val))
#define HT_KEY_CONST
#define HT_THREAD_SAFE
#define HT_DECL
#define HT_IMPL
#include "hashtable_incproxy.inc.h"

typedef struct WatchedPath {
	char *vfs_path;
	FileWatch *watch;
} WatchedPath;

struct InternalResource {
	Resource res;
	SDL_mutex *mutex;
	SDL_cond *cond;
	InternalResLoadState *load;
	char *name;
	ResourceStatus status;

	// Reloading works by allocating a temporary InternalResource, attempting to load the resource
	// into it, and, if succeeded, replacing the original resource with the new one.
	// `is_transient_reloader` indicates whether this instance is the transient or persistent one.
	// The transient-persistent pairs point to each other via the `reload_buddy` field.
	bool is_transient_reloader;
	InternalResource *reload_buddy;

	// List of monitored VFS paths. If any of those files changes, a reload is triggered.
	DYNAMIC_ARRAY(WatchedPath) watched_paths;

	// Other resources that this one depends on.
	// When a dependecy gets reloaded, this resource will get reloaded as well.
	IResPtrArray dependencies;

	// Reverse dependencies, i.e. which resources depend on this one.
	// This needs to be tracked to avoid a brute force search when propagating reloads.
	// For simplicity of implementation, this is a counted set (a multiset)
	ht_ires_counted_set_t dependents;

#if DEBUG_LOCKS
	SDL_atomic_t num_locks;
#endif
};

struct InternalResLoadState {
	ResourceLoadState st;
	InternalResource *ires;
	Task *async_task;
	ResourceLoadProc continuation;
	LoadStatus status;
	bool ready_to_finalize;
};

typedef struct FileWatchHandlerData {
	IResPtrArray temp_ires_array;
} FileWatchHandlerData;

static struct {
	hrtime_t frame_threshold;
	uchar loaded_this_frame : 1;
	struct {
		uchar no_async_load : 1;
		uchar no_preload : 1;
		uchar no_unload : 1;
		uchar preload_required : 1;
	} env;
	SDL_SpinLock ires_freelist_lock;
	InternalResource *ires_freelist;

	// Maps FileWatch pointers to sets of resources that are monitoring them.
	// This needs to be tracked to avoid a brute force search when handling TE_FILEWATCH events.
	// This is a hashtable where keys are FileWatch* and values are ht_ires_counted_set_t.
	// ht_ires_counted_set_t is in turn another hashtable (InternalResource* -> uint32_t)
	// implementing a counted set. This is because a resource may track the same FileWatch pointer
	// multiple times.
	ht_watch2iresset_t watch_to_iresset;

	// Static data for filewatch event handler
	FileWatchHandlerData fw_handler_data;
} res_gstate;

INLINE ResourceHandler *get_handler(ResourceType type) {
	return _handlers[type];
}

INLINE ResourceHandler *get_ires_handler(InternalResource *ires) {
	return get_handler(ires->res.type);
}

static inline InternalResLoadState *loadstate_internal(ResourceLoadState *st) {
	return UNION_CAST(ResourceLoadState*, InternalResLoadState*, st);
}

static InternalResource *preload_resource_internal(ResourceType type, const char *name, ResourceFlags flags);

#if DEBUG_LOCKS

#define ires_update_num_locks(_ires, _n) \
	log_debug("%p  :: %u ::  [%s '%s']   %s:%i", \
		(_ires), \
		SDL_AtomicAdd(&(_ires)->num_locks, _n), \
		get_ires_handler(_ires)->typename, \
		ires->name, \
		dbg.func, dbg.line \
	)

#define ires_lock(ires) ires_lock(ires, _DEBUG_INFO_)
static void (ires_lock)(InternalResource *ires, DebugInfo dbg) {
	SDL_LockMutex(ires->mutex);
	ires_update_num_locks(ires, 1);
}

#define ires_unlock(ires) ires_unlock(ires, _DEBUG_INFO_)
static void (ires_unlock)(InternalResource *ires, DebugInfo dbg) {
	ires_update_num_locks(ires, -1);
	SDL_UnlockMutex(ires->mutex);
}

#define ires_cond_wait(ires) ires_cond_wait(ires, _DEBUG_INFO_)
static void (ires_cond_wait)(InternalResource *ires, DebugInfo dbg) {
	ires_update_num_locks(ires, -1);
	SDL_CondWait(ires->cond, ires->mutex);
	ires_update_num_locks(ires, 1);
}

#else

INLINE void ires_lock(InternalResource *ires) {
	SDL_LockMutex(ires->mutex);
}

INLINE void ires_unlock(InternalResource *ires) {
	SDL_UnlockMutex(ires->mutex);
}

INLINE void ires_cond_wait(InternalResource *ires) {
	SDL_CondWait(ires->cond, ires->mutex);
}

#endif  // DEBUG_LOCKS

static void ires_cond_broadcast(InternalResource *ires) {
	SDL_CondBroadcast(ires->cond);
}

attr_returns_nonnull
static InternalResource *ires_alloc(ResourceType rtype) {
	SDL_AtomicLock(&res_gstate.ires_freelist_lock);
	InternalResource *ires = res_gstate.ires_freelist;
	if(ires) {
		res_gstate.ires_freelist = ires->reload_buddy;
	}
	SDL_AtomicUnlock(&res_gstate.ires_freelist_lock);

	if(ires) {
		ires_lock(ires);
		ires->reload_buddy = NULL;
		ires->res.type = rtype;
		ires_unlock(ires);
	} else {
		ires = ALLOC(typeof(*ires));
		ires->mutex = SDL_CreateMutex();
		ires->cond = SDL_CreateCond();
		ires->res.type = rtype;
	}

	return ires;
}

attr_nonnull_all
static void ires_free_meta(InternalResource *ires) {
	assert(ires->load == NULL);
	assert(ires->watched_paths.num_elements == 0);
	assert(ires->dependents.num_elements_occupied == 0);

	dynarray_free_data(&ires->watched_paths);
	dynarray_free_data(&ires->dependencies);
	ht_ires_counted_set_destroy(&ires->dependents);
}

attr_nonnull_all
static void ires_release(InternalResource *ires) {
	ires_lock(ires);
	ires_free_meta(ires);

	ires->res = (Resource) { };
	ires->name = NULL;
	ires->status = RES_STATUS_LOADING;
	ires->is_transient_reloader = false;
	ires->dependents = (ht_ires_counted_set_t) { };

	SDL_AtomicLock(&res_gstate.ires_freelist_lock);
	ires->reload_buddy = res_gstate.ires_freelist;
	res_gstate.ires_freelist = ires;
	SDL_AtomicUnlock(&res_gstate.ires_freelist_lock);
	ires_unlock(ires);
}

attr_nonnull_all
static void ires_free(InternalResource *ires) {
	ires_free_meta(ires);
	SDL_DestroyMutex(ires->mutex);
	SDL_DestroyCond(ires->cond);
	mem_free(ires);
}

attr_nonnull_all attr_returns_nonnull
static InternalResource *ires_get_persistent(InternalResource *ires) {
	if(ires->is_transient_reloader) {
		return NOT_NULL(ires->reload_buddy);
	}

	return ires;
}

void res_load_failed(ResourceLoadState *st) {
	InternalResLoadState *ist = loadstate_internal(st);
	ist->status = LOAD_FAILED;
}

void res_load_finished(ResourceLoadState *st, void *res) {
	InternalResLoadState *ist = loadstate_internal(st);
	ist->status = LOAD_OK;
	ist->st.opaque = res;
}

void res_load_continue_on_main(ResourceLoadState *st, ResourceLoadProc callback, void *opaque) {
	InternalResLoadState *ist = loadstate_internal(st);
	ist->status = LOAD_CONT_ON_MAIN;
	ist->st.opaque = opaque;
	ist->continuation = callback;
}

void res_load_continue_after_dependencies(ResourceLoadState *st, ResourceLoadProc callback, void *opaque) {
	InternalResLoadState *ist = loadstate_internal(st);
	ist->status = LOAD_CONT;
	ist->st.opaque = opaque;
	ist->continuation = callback;
}

static uint32_t ires_make_dependent_one(InternalResource *ires, InternalResource *dep);

void res_load_dependency(ResourceLoadState *st, ResourceType type, const char *name) {
	InternalResLoadState *ist = loadstate_internal(st);
	InternalResource *dep = preload_resource_internal(type, name, st->flags & ~RESF_RELOAD);
	InternalResource *ires = ist->ires;
	ires_lock(ires);
	*dynarray_append(&ires->dependencies) = dep;
	ires_make_dependent_one(ires_get_persistent(ires), dep);
	ires_unlock(ires);
}

static uint32_t ires_counted_set_inc(ht_ires_counted_set_t *set, InternalResource *ires) {
	if(!set->elements) {
		ht_ires_counted_set_create(set);
	}

	uint *counter;
	bool initialized = ht_ires_counted_set_get_ptr_unsafe(set, ires, &counter, true);
	assume(counter != NULL);

	if(!initialized) {
		*counter = 1;
	} else {
		++(*counter);
	}

	return *counter;
}

static uint32_t ires_counted_set_dec(ht_ires_counted_set_t *set, InternalResource *ires) {
	assert(set->elements != NULL);

	uint *counter;
	attr_unused bool initialized = ht_ires_counted_set_get_ptr_unsafe(set, ires, &counter, false);
	assume(initialized);
	assume(counter != NULL);
	assume(*counter > 0);

	if(--(*counter) == 0) {
		ht_ires_counted_set_unset(set, ires);
		return 0;
	}

	return *counter;
}

static void associate_ires_watch(InternalResource *ires, FileWatch *w) {
	ht_watch2iresset_t *map = &res_gstate.watch_to_iresset;
	ht_ires_counted_set_t *set;

	ht_watch2iresset_write_lock(map);

	bool initialized = ht_watch2iresset_get_ptr_unsafe(map, w, &set, true);
	assume(set != NULL);

	if(!initialized) {
		ht_ires_counted_set_create(set);
	}

	ires_counted_set_inc(set, ires);
	ht_watch2iresset_write_unlock(map);
}

static void unassociate_ires_watch(InternalResource *ires, FileWatch *w) {
	ht_watch2iresset_t *map = &res_gstate.watch_to_iresset;
	ht_ires_counted_set_t *set;

	ht_watch2iresset_write_lock(map);

	bool set_exists = ht_watch2iresset_get_ptr_unsafe(map, w, &set, false);

	if(set_exists) {
		ires_counted_set_dec(NOT_NULL(set), ires);

		// TODO make this a public hashtable API
		if(set->num_elements_occupied == 0) {
			ht_ires_counted_set_destroy(set);
			ht_watch2iresset_unset_unsafe(map, w);
		}
	}

	ht_watch2iresset_write_unlock(map);
}

static void register_watched_path(InternalResource *ires, const char *vfspath, FileWatch *w) {
	WatchedPath *free_slot = NULL;

	ires_lock(ires);
	associate_ires_watch(ires, w);

	dynarray_foreach_elem(&ires->watched_paths, WatchedPath *wp, {
		if(!free_slot && !wp->vfs_path) {
			free_slot = wp;
		}

		if(!strcmp(wp->vfs_path, vfspath)) {
			filewatch_unwatch(wp->watch);
			unassociate_ires_watch(ires, wp->watch);
			wp->watch = w;
			goto done;
		}
	});

	if(!free_slot) {
		free_slot = dynarray_append(&ires->watched_paths);
	}

	free_slot->vfs_path = strdup(vfspath);
	free_slot->watch = w;

done:
	ires_unlock(ires);
}

static void cleanup_watched_path(InternalResource *ires, WatchedPath *wp) {
	if(wp->vfs_path) {
		mem_free(wp->vfs_path);
		wp->vfs_path = NULL;
	}

	if(wp->watch) {
		unassociate_ires_watch(ires, wp->watch);
		filewatch_unwatch(wp->watch);
		wp->watch = NULL;
	}
}

static void refresh_watched_path(InternalResource *ires, WatchedPath *wp) {
	FileWatch *old_watch = wp->watch;

	// FIXME: we probably need a better API to obtain the underlying syspath
	// the returned path here may not actually be valid (e.g. in case it's inside a zip package)
	char *syspath = vfs_repr(NOT_NULL(wp->vfs_path), true);

	if(syspath != NULL) {
		wp->watch = filewatch_watch(syspath);
		mem_free(syspath);
		associate_ires_watch(ires, wp->watch);
	} else {
		wp->watch = NULL;
	}

	if(old_watch) {
		unassociate_ires_watch(ires, old_watch);
		filewatch_unwatch(old_watch);
	}
}

static void get_ires_list_for_watch(FileWatch *watch, IResPtrArray *output) {
	ht_watch2iresset_t *map = &res_gstate.watch_to_iresset;
	ht_ires_counted_set_t *set;

	output->num_elements = 0;

	ht_watch2iresset_write_lock(map);

	if(ht_watch2iresset_get_ptr_unsafe(map, watch, &set, false)) {
		ht_ires_counted_set_iter_t iter;
		ht_ires_counted_set_iter_begin(set, &iter);

		for(;iter.has_data; ht_ires_counted_set_iter_next(&iter)) {
			assert(iter.value > 0);
			*dynarray_append(output) = iter.key;
		}

		ht_ires_counted_set_iter_end(&iter);
	}

	ht_watch2iresset_write_unlock(map);
}

static void ires_remove_watched_paths(InternalResource *ires) {
	ires_lock(ires);

	dynarray_foreach_elem(&ires->watched_paths, WatchedPath *wp, {
		cleanup_watched_path(ires, wp);
	});

	dynarray_free_data(&ires->watched_paths);
	ires_unlock(ires);
}

static void ires_migrate_watched_paths(InternalResource *dst, InternalResource *src) {
	ires_lock(dst);
	ires_lock(src);

	dynarray_foreach_elem(&dst->watched_paths, WatchedPath *wp, {
		cleanup_watched_path(dst, wp);
	});

	dst->watched_paths.num_elements = 0;

	dynarray_foreach_elem(&src->watched_paths, WatchedPath *src_wp, {
		WatchedPath *dst_wp = dynarray_append(&dst->watched_paths);
		*dst_wp = *src_wp;
		FileWatch *w = dst_wp->watch;

		associate_ires_watch(dst, w);
		unassociate_ires_watch(src, w);
	});

	dynarray_free_data(&src->watched_paths);

	ires_unlock(src);
	ires_unlock(dst);
}

static uint32_t ires_make_dependent_one(InternalResource *ires, InternalResource *dep) {
	ires_lock(dep);

	assert(dep->name != NULL);
	assert(!dep->is_transient_reloader);
	assert(ires->name != NULL);
	assert(!ires->is_transient_reloader);

	uint32_t refs = ires_counted_set_inc(&dep->dependents, ires);

	ires_unlock(dep);
	return refs;
}

static uint32_t ires_unmake_dependent_one(InternalResource *ires, InternalResource *dep) {
	ires_lock(dep);

	assert(dep->name != NULL);
	assert(!dep->is_transient_reloader);
	assert(ires->name != NULL);
	assert(!ires->is_transient_reloader);

	uint32_t refs = ires_counted_set_dec(&dep->dependents, ires);

	ires_unlock(dep);
	return refs;
}

static void ires_unmake_dependent(InternalResource *ires, IResPtrArray *dependencies) {
	ires_lock(ires);

	dynarray_foreach_elem(dependencies, InternalResource **pdep, {
		InternalResource *dep = *pdep;
		ires_unmake_dependent_one(ires, dep);
	});

	ires_unlock(ires);
}

SDL_RWops *res_open_file(ResourceLoadState *st, const char *path, VFSOpenMode mode) {
	SDL_RWops *rw = vfs_open(path, mode);

	if(UNLIKELY(!rw)) {
		return NULL;
	}

	InternalResLoadState *ist = loadstate_internal(st);
	InternalResource *ires = ist->ires;
	ResourceHandler *handler = get_ires_handler(ires);

	if(!handler->procs.transfer) {
		return rw;
	}

	// FIXME: we probably need a better API to obtain the underlying syspath
	char *syspath = vfs_repr(path, true);

	if(syspath == NULL) {
		return rw;
	}

	FileWatch *w = filewatch_watch(syspath);
	mem_free(syspath);

	if(w == NULL) {
		return rw;
	}

	register_watched_path(ires, path, w);
	return rw;
}

INLINE void alloc_handler(ResourceHandler *h) {
	assert(h != NULL);
	ht_create(&h->private.mapping);
}

INLINE const char *type_name(ResourceType type) {
	return get_handler(type)->typename;
}

struct valfunc_arg {
	ResourceType type;
};

static void *valfunc_begin_load_resource(void* arg) {
	ResourceType type = ((struct valfunc_arg*)arg)->type;
	return ires_alloc(type);
}

static bool try_begin_load_resource(ResourceType type, const char *name, hash_t hash, InternalResource **out_ires) {
	ResourceHandler *handler = get_handler(type);
	struct valfunc_arg arg = { type };
	return ht_try_set_prehashed(&handler->private.mapping, name, hash, &arg, valfunc_begin_load_resource, (void**)out_ires);
}

static void load_resource_finish(InternalResLoadState *st);

static ResourceStatus pump_or_wait_for_dependencies(InternalResLoadState *st, bool pump_only);

static ResourceStatus pump_dependencies(InternalResLoadState *st) {
	return pump_or_wait_for_dependencies(st, true);
}

static ResourceStatus wait_for_dependencies(InternalResLoadState *st) {
	return pump_or_wait_for_dependencies(st, false);
}

static ResourceStatus pump_or_wait_for_resource_load_nolock(
	InternalResource *ires, uint32_t want_flags, bool pump_only
) {
	assert(ires->name != NULL);

	char *orig_name = ires->name;
	attr_unused bool was_transient = ires->is_transient_reloader;
	InternalResLoadState *load_state = ires->load;

// 	log_debug("%p transient=%i load_state=%p", ires, ires->is_transient_reloader, ires->load);

	if(load_state) {
		assert(ires->status == RES_STATUS_LOADING);

		Task *task = load_state->async_task;

		if(task) {
			// If there's an async load task for this resource, wait for it to complete.
			// If it's not yet running, it will be offloaded to this thread instead.

			load_state->async_task = NULL;
			ires_unlock(ires);

			if(!task_finish(task, NULL)) {
				log_fatal("Internal error: async task failed");
			}

			ires_lock(ires);

			if(UNLIKELY(ires->name != orig_name)) {
				// ires got released (and possibly reused) while it was unlocked.
				// This should only happen for transient reloaders.
				// FIXME Unfortunately we can not know whether the load succeeded in this case.
				// A possible solution is to add a reference count, so that the ires is kept alive
				// as long as anything is waiting on it.
				assert(was_transient);
				ires_unlock(ires);
				return RES_STATUS_LOADED;
			}

			// It's important to refresh load_state here, because the task may have finalized the load.
			// This may happen if the resource doesn't need to be finalized on the main thread, or if we *are* the main thread.
			load_state = ires->load;
		}

		if(load_state) {
			ResourceStatus dep_status = pump_dependencies(load_state);

			if(!pump_only && is_main_thread()) {
				InternalResource *persistent = ires_get_persistent(ires);

				if(dep_status == RES_STATUS_LOADING) {
					wait_for_dependencies(load_state);
				}

				while(!load_state->ready_to_finalize) {
					ires_cond_wait(ires);
				}

				// May have been finalized while we were sleeping
				// Can happen if load fails
				load_state = ires->load;

				if(load_state) {
					load_resource_finish(load_state);
				}

				if(persistent != ires) {
					// This was a reload and ires has been moved to the freelist now.
					// All we can do with it now is unlock its mutex.
					ResourceStatus status = persistent->status;
					assert(status != RES_STATUS_LOADING);
					ires_unlock(ires);
					return status;
				}

				assert(ires->status != RES_STATUS_LOADING);
			}
		}
	}

	while(ires->status == RES_STATUS_LOADING && !pump_only) {
		// If we get to this point, then we're waiting for a resource that's awaiting finalization on the main thread.
		// If we *are* the main thread, there's no excuse for us to wait; we should've finalized the resource ourselves.
		assert(!is_main_thread());
		ires_cond_wait(ires);
	}

	ResourceStatus status = ires->status;

	if(status == RES_STATUS_LOADED) {
		uint32_t missing_flags = (want_flags & ~ires->res.flags) & RESF_PROMOTABLE_FLAGS;

		if(missing_flags) {
			uint32_t new_flags = ires->res.flags | want_flags;
			log_debug("Flags for %s at %p promoted from 0x%08x to 0x%08x", type_name(ires->res.type), (void*)ires, ires->res.flags, new_flags);
			ires->res.flags = new_flags;
		}

		if((want_flags & RESF_RELOAD) && ires->reload_buddy && !ires->is_transient_reloader) {
			// Wait for reload, if asked to.

			InternalResource *r = ires->reload_buddy;
			ires_unlock(ires);

			ires_lock(r);

			//
			if(r->name == ires->name && r->reload_buddy == ires) {
				assert(r->is_transient_reloader);
				assert(r->load != NULL);
				status = pump_or_wait_for_resource_load_nolock(r, want_flags & ~RESF_RELOAD, pump_only);
			}

			ires_unlock(r);

			// FIXME BRAINDEATH
			ires_lock(ires);
		}
	}

	return status;
}

static ResourceStatus pump_or_wait_for_resource_load(
	InternalResource *ires, uint32_t want_flags, bool pump_only
) {
	ires_lock(ires);
	ResourceStatus status = pump_or_wait_for_resource_load_nolock(ires, want_flags, pump_only);
	ires_unlock(ires);
	return status;
}

static ResourceStatus wait_for_resource_load(InternalResource *ires, uint32_t want_flags) {
	return pump_or_wait_for_resource_load(ires, want_flags, false);
}

static ResourceStatus pump_or_wait_for_dependencies(InternalResLoadState *st, bool pump_only) {
	ResourceStatus dep_status = RES_STATUS_LOADED;

	dynarray_foreach_elem(&st->ires->dependencies, InternalResource **dep, {
		ResourceStatus s = pump_or_wait_for_resource_load(*dep, st->st.flags, pump_only);

		switch(s) {
			case RES_STATUS_FAILED:
				return RES_STATUS_FAILED;

			case RES_STATUS_LOADING:
				dep_status = RES_STATUS_LOADING;
				break;

			case RES_STATUS_LOADED:
				break;

			default:
				UNREACHABLE;
		}
	});

	if(!pump_only) {
		assert(dep_status != RES_STATUS_LOADING);
	}

	return dep_status;
}

#define PROTECT_FLAGS(ist, ...) do { \
	attr_unused InternalResLoadState *_ist = (ist); \
	attr_unused ResourceFlags _orig_flags = _ist->st.flags; \
	{ __VA_ARGS__; } \
	assert(_ist->st.flags == _orig_flags); \
} while(0)

static void *load_resource_async_task(void *vdata) {
	InternalResLoadState *st = vdata;
	InternalResource *ires = st->ires;
	assume(st == ires->load);

	ResourceHandler *h = get_ires_handler(ires);

	st->status = LOAD_NONE;
	PROTECT_FLAGS(st, h->procs.load(&st->st));

retry:
	switch(st->status) {
		case LOAD_CONT: {
			ResourceStatus dep_status;

			if(is_main_thread()) {
				dep_status = pump_dependencies(st);

				if(dep_status == RES_STATUS_LOADING) {
					dep_status = wait_for_dependencies(st);
				}

				st->status = LOAD_NONE;
				PROTECT_FLAGS(st, st->continuation(&st->st));
				goto retry;
			} else {
				dep_status = pump_dependencies(st);

				if(dep_status == RES_STATUS_LOADING) {
					st->status = LOAD_CONT_ON_MAIN;
					st->ready_to_finalize = true;
					ires_cond_broadcast(ires);
					events_emit(TE_RESOURCE_ASYNC_LOADED, 0, ires, NULL);
					break;
				} else {
					st->status = LOAD_NONE;
					PROTECT_FLAGS(st, st->continuation(&st->st));
					goto retry;
				}
			}

			UNREACHABLE;
		}

		case LOAD_CONT_ON_MAIN:
			if(pump_dependencies(st) == RES_STATUS_LOADING || !is_main_thread()) {
				st->ready_to_finalize = true;
				ires_cond_broadcast(ires);
				events_emit(TE_RESOURCE_ASYNC_LOADED, 0, ires, NULL);
				break;
			}
		// fallthrough
		case LOAD_OK:
		case LOAD_FAILED:
			ires_lock(ires);
			st->ready_to_finalize = true;
			load_resource_finish(st);
			ires_unlock(ires);
			st = NULL;
			break;

		default:
			UNREACHABLE;
	}

	assume(ires->load == st);
	return st;
}

static SDLCALL int filter_asyncload_event(void *vires, SDL_Event *event) {
	return event->type == MAKE_TAISEI_EVENT(TE_RESOURCE_ASYNC_LOADED) && event->user.data1 == vires;
}

static bool unload_resource(InternalResource *ires) {
	assert(is_main_thread());

	ResourceHandler *handler = get_ires_handler(ires);
	const char *tname = handler->typename;

	ires_lock(ires);

	assert(!ires->is_transient_reloader);
	if(ires->reload_buddy) {
		assert(ires->reload_buddy->is_transient_reloader);
		assert(ires->reload_buddy->reload_buddy == ires);
	}

	if(ires->dependents.num_elements_occupied > 0) {
		log_warn(
			"Not unloading %s '%s' because it still has %i dependents",
			tname, ires->name, ires->dependents.num_elements_occupied
		);
		ires_unlock(ires);
		return false;
	}

	ht_unset(&handler->private.mapping, ires->name);
	attr_unused ResourceFlags flags = ires->res.flags;

	ires_unlock(ires);
	if(wait_for_resource_load(ires, RESF_RELOAD) == RES_STATUS_LOADED) {
		handler->procs.unload(ires->res.data);
	}
	ires_lock(ires);

	SDL_PumpEvents();
	SDL_FilterEvents(filter_asyncload_event, ires);

	ires_unmake_dependent(ires, &ires->dependencies);
	ires_remove_watched_paths(ires);

	char *name = ires->name;
	ires_release(ires);
	ires_unlock(ires);

	log_info(
		"Unloaded %s '%s' (%s)",
		tname, name,
		(flags & RESF_PERMANENT) ? "permanent" : "transient"
	);

	mem_free(name);

	return true;
}

static char *get_name_from_path(ResourceHandler *handler, const char *path) {
	if(!strstartswith(path, handler->subdir)) {
		return NULL;
	}

	return resource_util_basename(handler->subdir, path);
}

static bool should_defer_load(InternalResLoadState *st) {
	if(st->st.flags & RESF_RELOAD) {
		return false;
	}

	FrameTimes ft = eventloop_get_frame_times();

	if(ft.next != res_gstate.frame_threshold) {
		res_gstate.frame_threshold = ft.next;
		res_gstate.loaded_this_frame = false;
	}

	if(!res_gstate.loaded_this_frame) {
		return false;
	}

	hrtime_t t = time_get();

	if(ft.next < t || ft.next - t < ft.target / 2) {
		return true;
	}

	return false;
}

static bool resource_asyncload_handler(SDL_Event *evt, void *arg) {
	assert(is_main_thread());

	InternalResource *ires = evt->user.data1;
	InternalResLoadState *st = ires->load;

	if(st == NULL) {
		return true;
	}

	if(should_defer_load(st)) {
		events_defer(evt);
		return true;
	}

	ires_lock(ires);

	ResourceStatus dep_status = pump_dependencies(st);

	if(dep_status == RES_STATUS_LOADING) {
		// log_debug("Deferring %s '%s' because some dependencies are not satisfied", type_name(ires->res.type), st->st.name);

		// FIXME: Make this less braindead.
		// This will retry every frame until dependencies are satisfied.
		ires_unlock(ires);
		events_defer(evt);
		return true;
	}

	Task *task = st->async_task;
	assert(!task || ires->status == RES_STATUS_LOADING);
	st->async_task = NULL;

	if(task) {
		ires_unlock(ires);

		if(!task_finish(task, NULL)) {
			log_fatal("Internal error: async task failed");
		}

		ires_lock(ires);
		st = ires->load;
	}

	if(st) {
		load_resource_finish(ires->load);
		res_gstate.loaded_this_frame = true;
	}

	ires_unlock(ires);
	return true;
}

static InternalResLoadState *make_persistent_loadstate(InternalResLoadState *st_transient) {
	if(st_transient->ires->load != NULL) {
		assert(st_transient->ires->load == st_transient);
		return st_transient;
	}

	InternalResLoadState *st = memdup(st_transient, sizeof(*st));

	assume(st->st.name != NULL);
	assume(st->st.path != NULL);

	st->ires->load = st;
	return st;
}

static void load_resource_async(InternalResLoadState *st_transient) {
	InternalResLoadState *st = make_persistent_loadstate(st_transient);
	st->async_task = taskmgr_global_submit((TaskParams) { load_resource_async_task, st });
}

attr_nonnull(1, 2)
static void load_resource(
	InternalResource *ires,
	const char *name,
	ResourceFlags flags,
	bool async
) {
	ResourceHandler *handler = get_ires_handler(ires);
	const char *typename = type_name(handler->type);
	char *path = NULL;

	if(handler->type == RES_SFX || handler->type == RES_BGM) {
		// audio stuff is always optional.
		// loading may fail if the backend failed to initialize properly, even though the resource exists.
		// this is a less than ideal situation, but it doesn't render the game unplayable.
		flags |= RESF_OPTIONAL;
	}

	if(ires->name == NULL) {
		ires->name = strdup(name);
	} else {
		assume(flags & RESF_RELOAD);
	}

	name = ires->name;
	path = handler->procs.find(name);

	if(path) {
		assert(handler->procs.check(path));
	} else {
		if(!(flags & RESF_OPTIONAL)) {
			log_fatal("Required %s '%s' couldn't be located", typename, name);
		} else {
			log_error("Failed to locate %s '%s'", typename, name);
		}

		ires->status = RES_STATUS_FAILED;
	}

	InternalResLoadState st = {
		.ires = ires,
		.st = {
			.name = name,
			.path = path,
			.flags = flags,
		},
	};

	if(ires->status == RES_STATUS_FAILED) {
		st.ready_to_finalize = true;
		load_resource_finish(&st);
	} else {
		ires_unmake_dependent(ires, &ires->dependencies);
		ires->dependencies.num_elements = 0;

		if(async) {
			load_resource_async(&st);
		} else {
			st.status = LOAD_NONE;
			PROTECT_FLAGS(&st, handler->procs.load(&st.st));

			retry: switch(st.status) {
				case LOAD_OK:
				case LOAD_FAILED:
					st.ready_to_finalize = true;
					load_resource_finish(&st);
					break;
				case LOAD_CONT:
				case LOAD_CONT_ON_MAIN:
					wait_for_dependencies(&st);
					st.status = LOAD_NONE;
					PROTECT_FLAGS(&st, st.continuation(&st.st));
					goto retry;
				default: UNREACHABLE;
			}
		}
	}
}

static bool reload_resource(InternalResource *ires, ResourceFlags flags, bool async) {
	ResourceHandler *handler = get_ires_handler(ires);
	const char *typename = type_name(handler->type);

	if(!handler->procs.transfer) {
		log_debug("Can't reload %s '%s'", typename, ires->name);
		return false;
	}

	flags |= RESF_RELOAD | RESF_OPTIONAL;

	ires_lock(ires);
	assert(!ires->is_transient_reloader);

	if(ires->status != RES_STATUS_LOADED) {
		ires_unlock(ires);
		log_warn("Tried to reload %s '%s' that is not loaded", typename, ires->name);
		return false;
	}

	if(ires->reload_buddy) {
		ires_unlock(ires);
		log_warn("Tried to reload %s '%s' that is currently reloading", typename, ires->name);
		return false;
	}

	log_info("Reloading %s '%s'", typename, ires->name);

	InternalResource *transient = ires_alloc(ires->res.type);
	transient->name = ires->name;
	transient->status = RES_STATUS_LOADING;
	transient->reload_buddy = ires;
	transient->is_transient_reloader = true;

	ires_lock(transient);
	ires->reload_buddy = transient;
	load_resource(transient, ires->name, flags, async);
	ires_unlock(transient);

	InternalResource *dependents[ires->dependents.num_elements_occupied];

	ht_ires_counted_set_iter_t iter;
	ht_ires_counted_set_iter_begin(&ires->dependents, &iter);
	for(int i = 0; iter.has_data; ht_ires_counted_set_iter_next(&iter), ++i) {
		assert(i < ARRAY_SIZE(dependents));
		dependents[i] = iter.key;
	}
	ht_ires_counted_set_iter_end(&iter);

	ires_unlock(ires);

	for(int i = 0; i < ARRAY_SIZE(dependents); ++i) {
		reload_resource(dependents[i], 0, !res_gstate.env.no_async_load);
	}

	return true;
}

static void load_resource_finish(InternalResLoadState *st) {
	void *raw = NULL;
	InternalResource *ires = st->ires;

	assert(ires->status == RES_STATUS_LOADING || ires->status == RES_STATUS_FAILED);
	assert(st->ready_to_finalize);

	if(ires->status != RES_STATUS_FAILED) {
		retry: switch(st->status) {
			case LOAD_CONT:
			case LOAD_CONT_ON_MAIN:
				st->status = LOAD_NONE;
				st->continuation(&st->st);
				goto retry;

			case LOAD_OK:
				raw = NOT_NULL(st->st.opaque);
				break;

			case LOAD_FAILED:
				break;

			default:
				UNREACHABLE;
		}
	}

	const char *name = NOT_NULL(st->st.name);
	const char *path = st->st.path ? st->st.path : "<path unknown>";
	char *allocated_source = vfs_repr(path, true);
	const char *source = allocated_source ? allocated_source : path;
	const char *typename = type_name(ires->res.type);

	ires->res.flags = st->st.flags;
	ires->res.data = raw;

	if(ires->res.type == RES_MODEL || res_gstate.env.no_unload) {
		// FIXME: models can't be safely unloaded at runtime yet
		ires->res.flags |= RESF_PERMANENT;
	}

	bool success = ires->res.data;

	if(success) {
		ires->status = RES_STATUS_LOADED;
		log_info("Loaded %s '%s' from '%s' (%s)", typename, name, source, (ires->res.flags & RESF_PERMANENT) ? "permanent" : "transient");
	} else {
		ires->status = RES_STATUS_FAILED;

		if(ires->res.flags & RESF_OPTIONAL) {
			log_error("Failed to load %s '%s' from '%s'", typename, name, source);
		} else {
			log_fatal("Required %s '%s' couldn't be loaded from '%s'", typename, name, source);
		}
	}

	mem_free(allocated_source);
	mem_free((char*)st->st.path);

	assume(!ires->load || ires->load == st);

	Task *async_task = st->async_task;
	if(async_task) {
		st->async_task = NULL;
		task_detach(async_task);
	}

	mem_free(ires->load);
	ires->load = NULL;

	ires_cond_broadcast(ires);
	assert(ires->status != RES_STATUS_LOADING);

	// If we are reloading, ires points to a transient resource that will be copied into the
	// persistent version at the end of this function.
	// In case of a regular load, persistent == ires.
	InternalResource *persistent = ires_get_persistent(ires);

	if(persistent == ires) {
		return;
	}

	// Handle reload.

	ResourceHandler *handler = get_ires_handler(ires);
	assert(handler->procs.transfer != NULL);

	ires_lock(persistent);

	if(
		!success ||
		!handler->procs.transfer(NOT_NULL(persistent->res.data), ires->res.data)
	) {
		log_error("Failed to reload %s '%s'", typename, persistent->name);
		ires_remove_watched_paths(ires);
		ires_unmake_dependent(persistent, &ires->dependencies);
	} else {
		ires_migrate_watched_paths(persistent, ires);

		// Unassociate old dependencies
		// NOTE: New dependencies already associated during load (in res_load_dependency)
		ires_unmake_dependent(persistent, &persistent->dependencies);

		// Move dependecy list into persistent entry
		dynarray_free_data(&persistent->dependencies);
		persistent->dependencies = ires->dependencies;
		memset(&ires->dependencies, 0, sizeof(ires->dependencies));
	}

	persistent->reload_buddy = NULL;
	persistent->res.flags |= ires->res.flags & ~(RESF_RELOAD | RESF_OPTIONAL);
	assert(persistent->status == RES_STATUS_LOADED);
	ires_release(ires);

	ires_cond_broadcast(persistent);
	ires_unlock(persistent);
}

Resource *_get_resource(ResourceType type, const char *name, hash_t hash, ResourceFlags flags) {
	InternalResource *ires;
	Resource *res;

	if(try_begin_load_resource(type, name, hash, &ires)) {
		flags &= ~RESF_RELOAD;

		ires_lock(ires);

		if(!(flags & RESF_PRELOAD)) {
			log_warn("%s '%s' was not preloaded", type_name(type), name);

			if(res_gstate.env.preload_required) {
				log_fatal("Aborting due to TAISEI_PRELOAD_REQUIRED");
			}
		}

		load_resource(ires, name, flags, false);
		ires_cond_broadcast(ires);

		if(ires->status == RES_STATUS_FAILED) {
			res = NULL;
		} else {
			assert(ires->status == RES_STATUS_LOADED);
			assert(ires->res.data != NULL);
			res = &ires->res;
		}

		ires_unlock(ires);
		return res;
	} else {
		if(flags & RESF_RELOAD) {
			reload_resource(ires, flags, false);
		} else if(ires->status == RES_STATUS_LOADED) {
			assert(ires->res.data != NULL);
			return &ires->res;
		}

		ResourceStatus status = wait_for_resource_load(ires, flags);

		if(status == RES_STATUS_FAILED) {
			return NULL;
		}

		assert(status == RES_STATUS_LOADED);
		assert(ires->res.data != NULL);

		return &ires->res;
	}
}

void *_get_resource_data(ResourceType type, const char *name, hash_t hash, ResourceFlags flags) {
	Resource *res = _get_resource(type, name, hash, flags);

	if(res) {
		return res->data;
	}

	return NULL;
}

static InternalResource *preload_resource_internal(
	ResourceType type, const char *name, ResourceFlags flags
) {
	InternalResource *ires;

	if(try_begin_load_resource(type, name, ht_str2ptr_hash(name), &ires)) {
		ires_lock(ires);
		load_resource(ires, name, flags, !res_gstate.env.no_async_load);
		ires_unlock(ires);
	} else if(flags & RESF_RELOAD) {
		reload_resource(ires, flags, !res_gstate.env.no_async_load);
	}

	return ires;
}

void preload_resource(ResourceType type, const char *name, ResourceFlags flags) {
	if(!res_gstate.env.no_preload) {
		preload_resource_internal(type, name, flags | RESF_PRELOAD);
	}
}

void preload_resources(ResourceType type, ResourceFlags flags, const char *firstname, ...) {
	va_list args;
	va_start(args, firstname);

	for(const char *name = firstname; name; name = va_arg(args, const char*)) {
		preload_resource(type, name, flags);
	}

	va_end(args);
}

static void reload_resources(ResourceHandler *h) {
	if(!h->procs.transfer) {
		return;
	}

	ht_str2ptr_ts_iter_t iter;
	ht_iter_begin(&h->private.mapping, &iter);

	for(; iter.has_data; ht_iter_next(&iter)) {
		reload_resource(iter.value, 0, !res_gstate.env.no_async_load);
	}

	ht_iter_end(&iter);
}

void reload_all_resources(void) {
	for(uint i = 0; i < RES_NUMTYPES; ++i) {
		ResourceHandler *h = get_handler(i);
		assert(h != NULL);
		reload_resources(h);
	}
}

struct file_deleted_handler_task_args {
	InternalResource *ires;
	FileWatch *watch;
};

static void *file_deleted_handler_task(void *data) {
	struct file_deleted_handler_task_args *a = data;
	// See explanation in resource_filewatch_handler below…
	SDL_Delay(100);

	InternalResource *ires = a->ires;
	FileWatch *watch = a->watch;

	reload_resource(ires, 0, !res_gstate.env.no_async_load);

	ires_lock(ires);
	dynarray_foreach_elem(&ires->watched_paths, WatchedPath *wp, {
		if(wp->watch == watch) {
			refresh_watched_path(ires, wp);
		}
	});
	ires_unlock(ires);

	return NULL;
}

static bool resource_filewatch_handler(SDL_Event *e, void *a) {
	FileWatchHandlerData *hdata = NOT_NULL(a);
	FileWatch *watch = NOT_NULL(e->user.data1);
	FileWatchEvent fevent = e->user.code;

	get_ires_list_for_watch(watch, &hdata->temp_ires_array);
	dynarray_foreach_elem(&hdata->temp_ires_array, InternalResource **pires, {
		InternalResource *ires = *pires;

		if(ires->is_transient_reloader || ires->status == RES_STATUS_LOADING) {
			continue;
		}

		if(fevent == FILEWATCH_FILE_DELETED) {
			// The file was (potentially) deleted or moved from its original path.
			// Assume it will be replaced by a new file in short order — this is often done by text
			// editors. In this case we wait an arbitrarily short amount of time and reload the
			// resource, hoping the new file has been put in place.
			// Needless to say, this is a very dumb HACK.
			// An alternative is to monitor the parent directory for creation of the file of
			// interest, but filewatch does not do this yet. That approach comes with its own bag
			// of problems, e.g. symlinks, exacerbated by the nature of the VFS…

			struct file_deleted_handler_task_args a;
			a.ires = ires;
			a.watch = watch;

			if(res_gstate.env.no_async_load) {
				file_deleted_handler_task(&a);
			} else {
				task_detach(taskmgr_global_submit((TaskParams) {
					.callback = file_deleted_handler_task,
					.userdata = memdup(&a, sizeof(a)),
					.userdata_free_callback = mem_free,
				}));
			}
		} else {
			reload_resource(ires, 0, !res_gstate.env.no_async_load);
		}
	});

	return false;
}

void init_resources(void) {
	res_gstate.env.no_async_load = env_get("TAISEI_NOASYNC", false);
	res_gstate.env.no_preload = env_get("TAISEI_NOPRELOAD", false);
	res_gstate.env.no_unload = env_get("TAISEI_NOUNLOAD", false);
	res_gstate.env.preload_required = env_get("TAISEI_PRELOAD_REQUIRED", false);

	for(int i = 0; i < RES_NUMTYPES; ++i) {
		ResourceHandler *h = get_handler(i);
		alloc_handler(h);

		if(h->procs.init != NULL) {
			h->procs.init();
		}
	}

	ht_watch2iresset_create(&res_gstate.watch_to_iresset);

	if(!res_gstate.env.no_async_load) {
		events_register_handler(&(EventHandler) {
			.proc = resource_asyncload_handler,
			.priority = EPRIO_SYSTEM,
			.event_type = MAKE_TAISEI_EVENT(TE_RESOURCE_ASYNC_LOADED),
		});
	}

	events_register_handler(&(EventHandler) {
		.proc = resource_filewatch_handler,
		.priority = EPRIO_SYSTEM,
		.event_type = MAKE_TAISEI_EVENT(TE_FILEWATCH),
		.arg = &res_gstate.fw_handler_data,
	});
}

void resource_util_strip_ext(char *path) {
	char *dot = strrchr(path, '.');
	char *psep = strrchr(path, '/');

	if(dot && (!psep || dot > psep)) {
		*dot = 0;
	}
}

char *resource_util_basename(const char *prefix, const char *path) {
	assert(strstartswith(path, prefix));

	char *out = strdup(path + strlen(prefix));
	resource_util_strip_ext(out);

	return out;
}

const char *resource_util_filename(const char *path) {
	char *sep = strrchr(path, '/');

	if(sep) {
		return sep + 1;
	}

	return path;
}

static void preload_path(const char *path, ResourceType type, ResourceFlags flags) {
	if(_handlers[type]->procs.check(path)) {
		char *name = get_name_from_path(_handlers[type], path);
		if(name) {
			preload_resource(type, name, flags);
			mem_free(name);
		}
	}
}

static void *preload_shaders(const char *path, void *arg) {
	preload_path(path, RES_SHADER_PROGRAM, RESF_PERMANENT);
	return NULL;
}

static void *preload_all(const char *path, void *arg) {
	for(ResourceType t = 0; t < RES_NUMTYPES; ++t) {
		preload_path(path, t, RESF_PERMANENT | RESF_OPTIONAL);
	}

	return NULL;
}

void *resource_for_each(ResourceType type, void* (*callback)(const char *name, Resource *res, void *arg), void *arg) {
	ht_str2ptr_ts_iter_t iter;
	ht_iter_begin(&get_handler(type)->private.mapping, &iter);
	void *result = NULL;

	while(iter.has_data) {
		InternalResource *ires = iter.value;
		char *key = iter.key;

		ht_iter_next(&iter);

		if(ires->res.data == NULL) {
			continue;
		}

		if((result = callback(key, &ires->res, arg)) != NULL) {
			break;
		}
	}

	ht_iter_end(&iter);
	return result;
}

void load_resources(void) {
	for(uint i = 0; i < RES_NUMTYPES; ++i) {
		ResourceHandler *h = get_handler(i);
		assert(h != NULL);

		if(h->procs.post_init) {
			h->procs.post_init();
		}
	}

	menu_preload();

	if(env_get("TAISEI_PRELOAD_SHADERS", 0)) {
		log_info("Loading all shaders now due to TAISEI_PRELOAD_SHADERS");
		vfs_dir_walk(SHPROG_PATH_PREFIX, preload_shaders, NULL);
	}

	if(env_get("TAISEI_AGGRESSIVE_PRELOAD", 0)) {
		log_info("Attempting to load all resources now due to TAISEI_AGGRESSIVE_PRELOAD");
		vfs_dir_walk("res/", preload_all, NULL);
	}
}

static void _free_resources(IResPtrArray *tmp_ires_array, bool all) {
	ht_str2ptr_ts_iter_t iter;

	bool retry = false;

	for(ResourceType type = 0; type < RES_NUMTYPES; ++type) {
		ResourceHandler *handler = get_handler(type);
		InternalResource *ires;

		tmp_ires_array->num_elements = 0;
		ht_iter_begin(&handler->private.mapping, &iter);

		for(; iter.has_data; ht_iter_next(&iter)) {
			ires = iter.value;

			if(all || !(ires->res.flags & RESF_PERMANENT)) {
				*dynarray_append(tmp_ires_array) = ires;
			}
		}

		ht_iter_end(&iter);

		dynarray_foreach_elem(tmp_ires_array, InternalResource **pires, {
			if(!unload_resource(*pires)) {
				// TODO: This is a little dumb; maybe recursively unload dependents first
				retry = true;
			}
		});
	}

	if(retry) {
		// Tail call; hope your compiler agrees.
		_free_resources(tmp_ires_array, all);
	}
}

void free_resources(bool all) {
	// Wait for all resources to finish (re)loading first, and pray that nothing else starts loading
	// while we are in this function (FIXME)

	ht_str2ptr_ts_iter_t iter;

	for(ResourceType type = 0; type < RES_NUMTYPES; ++type) {
		ResourceHandler *handler = get_handler(type);

		ht_iter_begin(&handler->private.mapping, &iter);

		for(; iter.has_data; ht_iter_next(&iter)) {
			wait_for_resource_load(iter.value, RESF_RELOAD);
		}

		ht_iter_end(&iter);
	}

	IResPtrArray a = { };
	_free_resources(&a, all);
	dynarray_free_data(&a);
}

void shutdown_resources(void) {
	free_resources(true);

	for(ResourceType type = 0; type < RES_NUMTYPES; ++type) {
		ResourceHandler *handler = get_handler(type);

		if(handler->procs.shutdown != NULL) {
			handler->procs.shutdown();
		}

		ht_destroy(&handler->private.mapping);
	}

	for(InternalResource *ires = res_gstate.ires_freelist; ires;) {
		InternalResource *next = ires->reload_buddy;
		ires_free(ires);
		ires = next;
	}

	ht_watch2iresset_destroy(&res_gstate.watch_to_iresset);
	res_gstate.ires_freelist = NULL;

	if(!res_gstate.env.no_async_load) {
		events_unregister_handler(resource_asyncload_handler);
	}

	events_unregister_handler(resource_filewatch_handler);
}
