/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include <kernel.h>
#include <string.h>
#include <misc/printk.h>
#include <misc/rb.h>
#include <kernel_structs.h>
#include <sys_io.h>
#include <ksched.h>
#include <syscall.h>
#include <syscall_handler.h>
#include <device.h>
#include <init.h>
#include <logging/sys_log.h>

#define MAX_THREAD_BITS		(CONFIG_MAX_THREAD_BYTES * 8)

const char *otype_to_str(enum k_objects otype)
{
	/* -fdata-sections doesn't work right except in very very recent
	 * GCC and these literal strings would appear in the binary even if
	 * otype_to_str was omitted by the linker
	 */
#ifdef CONFIG_PRINTK
	switch (otype) {
	/* otype-to-str.h is generated automatically during build by
	 * gen_kobject_list.py
	 */
#include <otype-to-str.h>
	default:
		return "?";
	}
#else
	ARG_UNUSED(otype);
	return NULL;
#endif
}

struct perm_ctx {
	int parent_id;
	int child_id;
	struct k_thread *parent;
};

#ifdef CONFIG_DYNAMIC_OBJECTS
struct dyn_obj {
	struct _k_object kobj;
	struct rbnode node; /* must be immediately before data member */
	u8_t data[]; /* The object itself */
};

struct visit_ctx {
	_wordlist_cb_func_t func;
	void *original_context;
};

extern struct _k_object *_k_object_gperf_find(void *obj);
extern void _k_object_gperf_wordlist_foreach(_wordlist_cb_func_t func,
					     void *context);

static int node_lessthan(struct rbnode *a, struct rbnode *b);

static struct rbtree obj_rb_tree = {
	.lessthan_fn = node_lessthan
};

/* TODO: incorporate auto-gen with Leandro's patch */
static size_t obj_size_get(enum k_objects otype)
{
	switch (otype) {
	case K_OBJ_ALERT:
		return sizeof(struct k_alert);
	case K_OBJ_MSGQ:
		return sizeof(struct k_msgq);
	case K_OBJ_MUTEX:
		return sizeof(struct k_mutex);
	case K_OBJ_PIPE:
		return sizeof(struct k_pipe);
	case K_OBJ_SEM:
		return sizeof(struct k_sem);
	case K_OBJ_STACK:
		return sizeof(struct k_stack);
	case K_OBJ_THREAD:
		return sizeof(struct k_thread);
	case K_OBJ_TIMER:
		return sizeof(struct k_timer);
	default:
		return sizeof(struct device);
	}
}

static int node_lessthan(struct rbnode *a, struct rbnode *b)
{
	return a < b;
}

static inline struct dyn_obj *node_to_dyn_obj(struct rbnode *node)
{
	return CONTAINER_OF(node, struct dyn_obj, node);
}

static struct dyn_obj *dyn_object_find(void *obj)
{
	struct rbnode *node;
	struct dyn_obj *ret;
	int key;

	/* For any dynamically allocated kernel object, the object
	 * pointer is just a member of the conatining struct dyn_obj,
	 * so just a little arithmetic is necessary to locate the
	 * corresponding struct rbnode
	 */
	node = (struct rbnode *)((char *)obj - sizeof(struct rbnode));

	key = irq_lock();
	if (rb_contains(&obj_rb_tree, node)) {
		ret = node_to_dyn_obj(node);
	} else {
		ret = NULL;
	}
	irq_unlock(key);

	return ret;
}

void *k_object_alloc(enum k_objects otype)
{
	struct dyn_obj *dyn_obj;
	int key;

	/* Stacks are not supported, we don't yet have mem pool APIs
	 * to request memory that is aligned
	 */
	__ASSERT(otype > K_OBJ_ANY && otype < K_OBJ_LAST &&
		 otype != K_OBJ__THREAD_STACK_ELEMENT,
		 "bad object type requested");

	dyn_obj = k_malloc(sizeof(*dyn_obj) + obj_size_get(otype));
	if (!dyn_obj) {
		SYS_LOG_WRN("could not allocate kernel object");
		return NULL;
	}

	dyn_obj->kobj.name = (char *)&dyn_obj->data;
	dyn_obj->kobj.type = otype;
	dyn_obj->kobj.flags = 0;
	memset(dyn_obj->kobj.perms, 0, CONFIG_MAX_THREAD_BYTES);

	/* The allocating thread implicitly gets permission on kernel objects
	 * that it allocates
	 */
	_thread_perms_set(&dyn_obj->kobj, _current);

	key = irq_lock();
	rb_insert(&obj_rb_tree, &dyn_obj->node);
	irq_unlock(key);

	return dyn_obj->kobj.name;
}

void k_object_free(void *obj)
{
	struct dyn_obj *dyn_obj;
	int key;

	/* This function is intentionally not exposed to user mode.
	 * There's currently no robust way to track that an object isn't
	 * being used by some other thread
	 */

	key = irq_lock();
	dyn_obj = dyn_object_find(obj);
	if (dyn_obj) {
		rb_remove(&obj_rb_tree, &dyn_obj->node);
	}
	irq_unlock(key);

	if (dyn_obj) {
		k_free(dyn_obj);
	}
}

struct _k_object *_k_object_find(void *obj)
{
	struct _k_object *ret;

	ret = _k_object_gperf_find(obj);

	if (!ret) {
		struct dyn_obj *dyn_obj;

		dyn_obj = dyn_object_find(obj);
		if (dyn_obj) {
			ret = &dyn_obj->kobj;
		}
	}

	return ret;
}

static void visit_fn(struct rbnode *node, void *context)
{
	struct visit_ctx *vctx = context;

	vctx->func(&node_to_dyn_obj(node)->kobj, vctx->original_context);
}

void _k_object_wordlist_foreach(_wordlist_cb_func_t func, void *context)
{
	struct visit_ctx vctx;
	int key;

	_k_object_gperf_wordlist_foreach(func, context);

	vctx.func = func;
	vctx.original_context = context;

	key = irq_lock();
	rb_walk(&obj_rb_tree, visit_fn, &vctx);
	irq_unlock(key);
}
#endif /* CONFIG_DYNAMIC_OBJECTS */

static int thread_index_get(struct k_thread *t)
{
	struct _k_object *ko;

	ko = _k_object_find(t);

	if (!ko) {
		return -1;
	}

	return ko->data;
}

static void wordlist_cb(struct _k_object *ko, void *ctx_ptr)
{
	struct perm_ctx *ctx = (struct perm_ctx *)ctx_ptr;

	if (sys_bitfield_test_bit((mem_addr_t)&ko->perms, ctx->parent_id) &&
				  (struct k_thread *)ko->name != ctx->parent) {
		sys_bitfield_set_bit((mem_addr_t)&ko->perms, ctx->child_id);
	}
}

void _thread_perms_inherit(struct k_thread *parent, struct k_thread *child)
{
	struct perm_ctx ctx = {
		thread_index_get(parent),
		thread_index_get(child),
		parent
	};

	if ((ctx.parent_id != -1) && (ctx.child_id != -1)) {
		_k_object_wordlist_foreach(wordlist_cb, &ctx);
	}
}

void _thread_perms_set(struct _k_object *ko, struct k_thread *thread)
{
	int index = thread_index_get(thread);

	if (index != -1) {
		sys_bitfield_set_bit((mem_addr_t)&ko->perms, index);
	}
}

void _thread_perms_clear(struct _k_object *ko, struct k_thread *thread)
{
	int index = thread_index_get(thread);

	if (index != -1) {
		sys_bitfield_clear_bit((mem_addr_t)&ko->perms, index);
	}
}

static void clear_perms_cb(struct _k_object *ko, void *ctx_ptr)
{
	int id = (int)ctx_ptr;

	sys_bitfield_clear_bit((mem_addr_t)&ko->perms, id);
}

void _thread_perms_all_clear(struct k_thread *thread)
{
	int index = thread_index_get(thread);

	if (index != -1) {
		_k_object_wordlist_foreach(clear_perms_cb, (void *)index);
	}
}

static int thread_perms_test(struct _k_object *ko)
{
	int index;

	if (ko->flags & K_OBJ_FLAG_PUBLIC) {
		return 1;
	}

	index = thread_index_get(_current);
	if (index != -1) {
		return sys_bitfield_test_bit((mem_addr_t)&ko->perms, index);
	}
	return 0;
}

static void dump_permission_error(struct _k_object *ko)
{
	int index = thread_index_get(_current);
	printk("thread %p (%d) does not have permission on %s %p [",
	       _current, index,
	       otype_to_str(ko->type), ko->name);
	for (int i = CONFIG_MAX_THREAD_BYTES - 1; i >= 0; i--) {
		printk("%02x", ko->perms[i]);
	}
	printk("]\n");
}

void _dump_object_error(int retval, void *obj, struct _k_object *ko,
			enum k_objects otype)
{
	switch (retval) {
	case -EBADF:
		printk("%p is not a valid %s\n", obj, otype_to_str(otype));
		break;
	case -EPERM:
		dump_permission_error(ko);
		break;
	case -EINVAL:
		printk("%p used before initialization\n", obj);
		break;
	case -EADDRINUSE:
		printk("%p %s in use\n", obj, otype_to_str(otype));
	}
}

void _impl_k_object_access_grant(void *object, struct k_thread *thread)
{
	struct _k_object *ko = _k_object_find(object);

	if (ko) {
		_thread_perms_set(ko, thread);
	}
}

void _impl_k_object_access_revoke(void *object, struct k_thread *thread)
{
	struct _k_object *ko = _k_object_find(object);

	if (ko) {
		_thread_perms_clear(ko, thread);
	}
}

void k_object_access_all_grant(void *object)
{
	struct _k_object *ko = _k_object_find(object);

	if (ko) {
		ko->flags |= K_OBJ_FLAG_PUBLIC;
	}
}

int _k_object_validate(struct _k_object *ko, enum k_objects otype,
		       enum _obj_init_check init)
{
	if (unlikely(!ko || (otype != K_OBJ_ANY && ko->type != otype))) {
		return -EBADF;
	}

	/* Manipulation of any kernel objects by a user thread requires that
	 * thread be granted access first, even for uninitialized objects
	 */
	if (unlikely(!thread_perms_test(ko))) {
		return -EPERM;
	}

	/* Initialization state checks. _OBJ_INIT_ANY, we don't care */
	if (likely(init == _OBJ_INIT_TRUE)) {
		/* Object MUST be intialized */
		if (unlikely(!(ko->flags & K_OBJ_FLAG_INITIALIZED))) {
			return -EINVAL;
		}
	} else if (init < _OBJ_INIT_TRUE) { /* _OBJ_INIT_FALSE case */
		/* Object MUST NOT be initialized */
		if (unlikely(ko->flags & K_OBJ_FLAG_INITIALIZED)) {
			return -EADDRINUSE;
		}
	}

	return 0;
}

void _k_object_init(void *object)
{
	struct _k_object *ko;

	/* By the time we get here, if the caller was from userspace, all the
	 * necessary checks have been done in _k_object_validate(), which takes
	 * place before the object is initialized.
	 *
	 * This function runs after the object has been initialized and
	 * finalizes it
	 */

	ko = _k_object_find(object);
	if (!ko) {
		/* Supervisor threads can ignore rules about kernel objects
		 * and may declare them on stacks, etc. Such objects will never
		 * be usable from userspace, but we shouldn't explode.
		 */
		return;
	}

	/* Allows non-initialization system calls to be made on this object */
	ko->flags |= K_OBJ_FLAG_INITIALIZED;
}

void _k_object_uninit(void *object)
{
	struct _k_object *ko;

	/* See comments in _k_object_init() */
	ko = _k_object_find(object);
	if (!ko) {
		return;
	}

	ko->flags &= ~K_OBJ_FLAG_INITIALIZED;
}

static u32_t handler_bad_syscall(u32_t bad_id, u32_t arg2, u32_t arg3,
				  u32_t arg4, u32_t arg5, u32_t arg6, void *ssf)
{
	printk("Bad system call id %u invoked\n", bad_id);
	_arch_syscall_oops(ssf);
	CODE_UNREACHABLE;
}

static u32_t handler_no_syscall(u32_t arg1, u32_t arg2, u32_t arg3,
				 u32_t arg4, u32_t arg5, u32_t arg6, void *ssf)
{
	printk("Unimplemented system call\n");
	_arch_syscall_oops(ssf);
	CODE_UNREACHABLE;
}

#include <syscall_dispatch.c>

