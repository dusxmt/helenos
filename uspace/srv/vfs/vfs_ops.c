/*
 * Copyright (c) 2008 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup fs
 * @{
 */

/**
 * @file vfs_ops.c
 * @brief Operations that VFS offers to its clients.
 */

#include "vfs.h"
#include <macros.h>
#include <stdint.h>
#include <async.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <str.h>
#include <stdbool.h>
#include <fibril_synch.h>
#include <adt/list.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <assert.h>
#include <vfs/canonify.h>
#include <vfs/vfs_mtab.h>

FIBRIL_MUTEX_INITIALIZE(mtab_list_lock);
LIST_INITIALIZE(mtab_list);
static size_t mtab_size = 0;

/* Forward declarations of static functions. */
static int vfs_truncate_internal(fs_handle_t, service_id_t, fs_index_t,
    aoff64_t);

/**
 * This rwlock prevents the race between a triplet-to-VFS-node resolution and a
 * concurrent VFS operation which modifies the file system namespace.
 */
FIBRIL_RWLOCK_INITIALIZE(namespace_rwlock);

vfs_node_t *root = NULL;

static int vfs_mount_internal(ipc_callid_t rid, service_id_t service_id,
    fs_handle_t fs_handle, char *mp, char *opts)
{
	vfs_lookup_res_t mp_res;
	vfs_lookup_res_t mr_res;
	vfs_node_t *mp_node = NULL;
	vfs_node_t *mr_node;
	fs_index_t rindex;
	aoff64_t rsize;
	async_exch_t *exch;
	sysarg_t rc;
	aid_t msg;
	ipc_call_t answer;
	
	/* Resolve the path to the mountpoint. */
	fibril_rwlock_write_lock(&namespace_rwlock);
	if (root == NULL) {
		/* We still don't have the root file system mounted. */
		if (str_cmp(mp, "/") != 0) {
			/*
			 * We can't resolve this without the root filesystem
			 * being mounted first.
			 */
			fibril_rwlock_write_unlock(&namespace_rwlock);
			async_answer_0(rid, ENOENT);
			return ENOENT;
		}
		
		/*
		 * For this simple, but important case,
		 * we are almost done.
		 */
			
		/* Tell the mountee that it is being mounted. */
		exch = vfs_exchange_grab(fs_handle);
		msg = async_send_1(exch, VFS_OUT_MOUNTED,
		    (sysarg_t) service_id, &answer);
		/* Send the mount options */
		rc = async_data_write_start(exch, (void *)opts,
		    str_size(opts));
		vfs_exchange_release(exch);
			
		if (rc != EOK) {
			async_forget(msg);
			fibril_rwlock_write_unlock(&namespace_rwlock);
			async_answer_0(rid, rc);
			return rc;
		}
		async_wait_for(msg, &rc);
			
		if (rc != EOK) {
			fibril_rwlock_write_unlock(&namespace_rwlock);
			async_answer_0(rid, rc);
			return rc;
		}

		rindex = (fs_index_t) IPC_GET_ARG1(answer);
		rsize = (aoff64_t) MERGE_LOUP32(IPC_GET_ARG2(answer),
		    IPC_GET_ARG3(answer));
		
		mr_res.triplet.fs_handle = fs_handle;
		mr_res.triplet.service_id = service_id;
		mr_res.triplet.index = rindex;
		mr_res.size = rsize;
		mr_res.type = VFS_NODE_DIRECTORY;
			
		/* Add reference to the mounted root. */
		root = vfs_node_get(&mr_res); 
		assert(root);
			
		fibril_rwlock_write_unlock(&namespace_rwlock);
		async_answer_0(rid, rc);
		return rc;
	}
	
	/* We already have the root FS. */
	if (str_cmp(mp, "/") == 0) {
		/* Trying to mount root FS over root FS */
		fibril_rwlock_write_unlock(&namespace_rwlock);
		async_answer_0(rid, EBUSY);
		return EBUSY;
	}
	
	rc = vfs_lookup_internal((vfs_triplet_t *) root, mp, L_DIRECTORY, &mp_res);
	if (rc != EOK) {
		/* The lookup failed. */
		fibril_rwlock_write_unlock(&namespace_rwlock);
		async_answer_0(rid, rc);
		return rc;
	}
	
	mp_node = vfs_node_get(&mp_res);
	if (!mp_node) {
		fibril_rwlock_write_unlock(&namespace_rwlock);
		async_answer_0(rid, ENOMEM);
		return ENOMEM;
	}
	
	/*
	 * Now we hold a reference to mp_node.
	 * It will be dropped upon the corresponding VFS_IN_UNMOUNT.
	 * This prevents the mount point from being deleted.
	 */
	
	/*
	 * At this point, we have all necessary pieces: file system handle
	 * and service ID, and we know the mount point VFS node.
	 */
	
	async_exch_t *mountee_exch = vfs_exchange_grab(fs_handle);
	assert(mountee_exch);
	
	exch = vfs_exchange_grab(mp_res.triplet.fs_handle);
	msg = async_send_4(exch, VFS_OUT_MOUNT,
	    (sysarg_t) mp_res.triplet.service_id,
	    (sysarg_t) mp_res.triplet.index,
	    (sysarg_t) fs_handle,
	    (sysarg_t) service_id, &answer);
	
	/* Send connection */
	rc = async_exchange_clone(exch, mountee_exch);
	vfs_exchange_release(mountee_exch);
	
	if (rc != EOK) {
		vfs_exchange_release(exch);
		async_forget(msg);
		
		/* Mount failed, drop reference to mp_node. */
		if (mp_node)
			vfs_node_put(mp_node);
		
		async_answer_0(rid, rc);
		fibril_rwlock_write_unlock(&namespace_rwlock);
		return rc;
	}
	
	/* send the mount options */
	rc = async_data_write_start(exch, (void *) opts, str_size(opts));
	if (rc != EOK) {
		vfs_exchange_release(exch);
		async_forget(msg);
		
		/* Mount failed, drop reference to mp_node. */
		if (mp_node)
			vfs_node_put(mp_node);
		
		fibril_rwlock_write_unlock(&namespace_rwlock);
		async_answer_0(rid, rc);
		return rc;
	}
	
	/*
	 * Wait for the answer before releasing the exchange to avoid deadlock
	 * in case the answer depends on further calls to the same file system.
	 * Think of a case when mounting a FS on a file_bd backed by a file on
	 * the same FS. 
	 */
	async_wait_for(msg, &rc);
	vfs_exchange_release(exch);
	
	if (rc == EOK) {
		rindex = (fs_index_t) IPC_GET_ARG1(answer);
		rsize = (aoff64_t) MERGE_LOUP32(IPC_GET_ARG2(answer),
		    IPC_GET_ARG3(answer));
		
		mr_res.triplet.fs_handle = fs_handle;
		mr_res.triplet.service_id = service_id;
		mr_res.triplet.index = rindex;
		mr_res.size = rsize;
		mr_res.type = VFS_NODE_DIRECTORY;
		
		/* Add reference to the mounted root. */
		mr_node = vfs_node_get(&mr_res); 
		assert(mr_node);
	} else {
		/* Mount failed, drop reference to mp_node. */
		if (mp_node)
			vfs_node_put(mp_node);
	}
	
	async_answer_0(rid, rc);
	fibril_rwlock_write_unlock(&namespace_rwlock);
	return rc;
}

void vfs_mount(ipc_callid_t rid, ipc_call_t *request)
{
	service_id_t service_id;

	/*
	 * We expect the library to do the device-name to device-handle
	 * translation for us, thus the device handle will arrive as ARG1
	 * in the request.
	 */
	service_id = (service_id_t) IPC_GET_ARG1(*request);
	
	/*
	 * Mount flags are passed as ARG2.
	 */
	unsigned int flags = (unsigned int) IPC_GET_ARG2(*request);
	
	/*
	 * Instance number is passed as ARG3.
	 */
	unsigned int instance = IPC_GET_ARG3(*request);

	/* We want the client to send us the mount point. */
	char *mp;
	int rc = async_data_write_accept((void **) &mp, true, 0, MAX_PATH_LEN,
	    0, NULL);
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}
	
	/* Now we expect to receive the mount options. */
	char *opts;
	rc = async_data_write_accept((void **) &opts, true, 0, MAX_MNTOPTS_LEN,
	    0, NULL);
	if (rc != EOK) {
		free(mp);
		async_answer_0(rid, rc);
		return;
	}
	
	/*
	 * Now, we expect the client to send us data with the name of the file
	 * system.
	 */
	char *fs_name;
	rc = async_data_write_accept((void **) &fs_name, true, 0,
	    FS_NAME_MAXLEN, 0, NULL);
	if (rc != EOK) {
		free(mp);
		free(opts);
		async_answer_0(rid, rc);
		return;
	}
	
	/*
	 * Wait for VFS_IN_PING so that we can return an error if we don't know
	 * fs_name.
	 */
	ipc_call_t data;
	ipc_callid_t callid = async_get_call(&data);
	if (IPC_GET_IMETHOD(data) != VFS_IN_PING) {
		async_answer_0(callid, ENOTSUP);
		async_answer_0(rid, ENOTSUP);
		free(mp);
		free(opts);
		free(fs_name);
		return;
	}

	/*
	 * Check if we know a file system with the same name as is in fs_name.
	 * This will also give us its file system handle.
	 */
	fibril_mutex_lock(&fs_list_lock);
	fs_handle_t fs_handle;
recheck:
	fs_handle = fs_name_to_handle(instance, fs_name, false);
	if (!fs_handle) {
		if (flags & IPC_FLAG_BLOCKING) {
			fibril_condvar_wait(&fs_list_cv, &fs_list_lock);
			goto recheck;
		}
		
		fibril_mutex_unlock(&fs_list_lock);
		async_answer_0(callid, ENOENT);
		async_answer_0(rid, ENOENT);
		free(mp);
		free(fs_name);
		free(opts);
		return;
	}
	fibril_mutex_unlock(&fs_list_lock);

	/* Add the filesystem info to the list of mounted filesystems */
	mtab_ent_t *mtab_ent = malloc(sizeof(mtab_ent_t));
	if (!mtab_ent) {
		async_answer_0(callid, ENOMEM);
		async_answer_0(rid, ENOMEM);
		free(mp);
		free(fs_name);
		free(opts);
		return;
	}

	/* Do the mount */
	rc = vfs_mount_internal(rid, service_id, fs_handle, mp, opts);
	if (rc != EOK) {
		async_answer_0(callid, ENOTSUP);
		async_answer_0(rid, ENOTSUP);
		free(mtab_ent);
		free(mp);
		free(opts);
		free(fs_name);
		return;
	}

	/* Add the filesystem info to the list of mounted filesystems */

	str_cpy(mtab_ent->mp, MAX_PATH_LEN, mp);
	str_cpy(mtab_ent->fs_name, FS_NAME_MAXLEN, fs_name);
	str_cpy(mtab_ent->opts, MAX_MNTOPTS_LEN, opts);
	mtab_ent->instance = instance;
	mtab_ent->service_id = service_id;

	link_initialize(&mtab_ent->link);

	fibril_mutex_lock(&mtab_list_lock);
	list_append(&mtab_ent->link, &mtab_list);
	mtab_size++;
	fibril_mutex_unlock(&mtab_list_lock);

	free(mp);
	free(fs_name);
	free(opts);

	/* Acknowledge that we know fs_name. */
	async_answer_0(callid, EOK);
}

void vfs_unmount(ipc_callid_t rid, ipc_call_t *request)
{
	int rc;
	char *mp;
	vfs_lookup_res_t mp_res;
	vfs_lookup_res_t mr_res;
	vfs_node_t *mr_node;
	async_exch_t *exch;
	
	/*
	 * Receive the mount point path.
	 */
	rc = async_data_write_accept((void **) &mp, true, 0, MAX_PATH_LEN,
	    0, NULL);
	if (rc != EOK)
		async_answer_0(rid, rc);
	
	/*
	 * Taking the namespace lock will do two things for us. First, it will
	 * prevent races with other lookup operations. Second, it will stop new
	 * references to already existing VFS nodes and creation of new VFS
	 * nodes. This is because new references are added as a result of some
	 * lookup operation or at least of some operation which is protected by
	 * the namespace lock.
	 */
	fibril_rwlock_write_lock(&namespace_rwlock);
	
	/*
	 * Lookup the mounted root and instantiate it.
	 */
	rc = vfs_lookup_internal((vfs_triplet_t *) root, mp, 0, &mr_res);
	if (rc != EOK) {
		fibril_rwlock_write_unlock(&namespace_rwlock);
		free(mp);
		async_answer_0(rid, rc);
		return;
	}
	mr_node = vfs_node_get(&mr_res);
	if (!mr_node) {
		fibril_rwlock_write_unlock(&namespace_rwlock);
		free(mp);
		async_answer_0(rid, ENOMEM);
		return;
	}
	
	/*
	 * Count the total number of references for the mounted file system. We
	 * are expecting at least two. One which we got above and one which we
	 * got when the file system was mounted. If we find more, it means that
	 * the file system cannot be gracefully unmounted at the moment because
	 * someone is working with it.
	 */
	if (vfs_nodes_refcount_sum_get(mr_node->fs_handle,
	    mr_node->service_id) != 2) {
		fibril_rwlock_write_unlock(&namespace_rwlock);
		vfs_node_put(mr_node);
		free(mp);
		async_answer_0(rid, EBUSY);
		return;
	}
	
	if (str_cmp(mp, "/") == 0) {
		
		/*
		 * Unmounting the root file system.
		 *
		 * In this case, there is no mount point node and we send
		 * VFS_OUT_UNMOUNTED directly to the mounted file system.
		 */
		
		exch = vfs_exchange_grab(mr_node->fs_handle);
		rc = async_req_1_0(exch, VFS_OUT_UNMOUNTED,
		    mr_node->service_id);
		vfs_exchange_release(exch);
		
		if (rc != EOK) {
			fibril_rwlock_write_unlock(&namespace_rwlock);
			free(mp);
			vfs_node_put(mr_node);
			async_answer_0(rid, rc);
			return;
		}
		
		root = NULL;
	} else {
		
		/*
		 * Unmounting a non-root file system.
		 *
		 * We have a regular mount point node representing the parent
		 * file system, so we delegate the operation to it.
		 */
		
		rc = vfs_lookup_internal((vfs_triplet_t *) root, mp, L_MP, &mp_res);
		if (rc != EOK) {
			fibril_rwlock_write_unlock(&namespace_rwlock);
			free(mp);
			vfs_node_put(mr_node);
			async_answer_0(rid, rc);
			return;
		}
		
		vfs_node_t *mp_node = vfs_node_get(&mp_res);
		if (!mp_node) {
			fibril_rwlock_write_unlock(&namespace_rwlock);
			free(mp);
			vfs_node_put(mr_node);
			async_answer_0(rid, ENOMEM);
			return;
		}
		
		exch = vfs_exchange_grab(mp_node->fs_handle);
		rc = async_req_2_0(exch, VFS_OUT_UNMOUNT,
		    mp_node->service_id, mp_node->index);
		vfs_exchange_release(exch);
		
		if (rc != EOK) {
			fibril_rwlock_write_unlock(&namespace_rwlock);
			free(mp);
			vfs_node_put(mp_node);
			vfs_node_put(mr_node);
			async_answer_0(rid, rc);
			return;
		}
		
		/* Drop the reference we got above. */
		vfs_node_put(mp_node);
		/* Drop the reference from when the file system was mounted. */
		vfs_node_put(mp_node);
	}
	
	/*
	 * All went well, the mounted file system was successfully unmounted.
	 * The only thing left is to forget the unmounted root VFS node.
	 */
	vfs_node_forget(mr_node);
	fibril_rwlock_write_unlock(&namespace_rwlock);

	fibril_mutex_lock(&mtab_list_lock);

	int found = 0;

	list_foreach(mtab_list, cur) {
		mtab_ent_t *mtab_ent = list_get_instance(cur, mtab_ent_t,
		    link);

		if (str_cmp(mtab_ent->mp, mp) == 0) {
			list_remove(&mtab_ent->link);
			mtab_size--;
			free(mtab_ent);
			found = 1;
			break;
		}
	}
	assert(found);
	fibril_mutex_unlock(&mtab_list_lock);

	free(mp);

	async_answer_0(rid, EOK);
}

static inline bool walk_flags_valid(int flags)
{
	if ((flags&~WALK_ALL_FLAGS) != 0) {
		return false;
	}
	if ((flags&WALK_MAY_CREATE) && (flags&WALK_MUST_CREATE)) {
		return false;
	}
	if ((flags&WALK_REGULAR) && (flags&WALK_DIRECTORY)) {
		return false;
	}
	if ((flags&WALK_MAY_CREATE) || (flags&WALK_MUST_CREATE)) {
		if (!(flags&WALK_DIRECTORY) && !(flags&WALK_REGULAR)) {
			return false;
		}
	}
	return true;
}

static inline int walk_lookup_flags(int flags)
{
	int lflags = 0;
	if (flags&WALK_MAY_CREATE || flags&WALK_MUST_CREATE) {
		lflags |= L_CREATE;
	}
	if (flags&WALK_MUST_CREATE) {
		lflags |= L_EXCLUSIVE;
	}
	if (flags&WALK_REGULAR) {
		lflags |= L_FILE;
	}
	if (flags&WALK_DIRECTORY) {
		lflags |= L_DIRECTORY;
	}
	return lflags;
}

void vfs_walk(ipc_callid_t rid, ipc_call_t *request)
{
	/*
	 * Parent is our relative root for file lookup.
	 * For defined flags, see <ipc/vfs.h>.
	 */
	int parentfd = IPC_GET_ARG1(*request);
	int flags = IPC_GET_ARG2(*request);
	
	if (!walk_flags_valid(flags)) {
		async_answer_0(rid, EINVAL);
		return;
	}
	
	char *path;
	int rc = async_data_write_accept((void **)&path, true, 0, 0, 0, NULL);
	
	/* Lookup the file structure corresponding to the file descriptor. */
	vfs_file_t *parent = NULL;
	vfs_node_t *parent_node = root;
	// TODO: Client-side root.
	if (parentfd != -1) {
		parent = vfs_file_get(parentfd);
		if (!parent) {
			free(path);
			async_answer_0(rid, EBADF);
			return;
		}
		parent_node = parent->node;
	}
	
	fibril_rwlock_read_lock(&namespace_rwlock);
	
	vfs_lookup_res_t lr;
	rc = vfs_lookup_internal((vfs_triplet_t *) parent_node, path, walk_lookup_flags(flags), &lr);
	free(path);

	if (rc != EOK) {
		fibril_rwlock_read_unlock(&namespace_rwlock);
		if (parent) {
			vfs_file_put(parent);
		}
		async_answer_0(rid, rc);
		return;
	}
	
	vfs_node_t *node = vfs_node_get(&lr);
	
	int fd = vfs_fd_alloc(false);
	if (fd < 0) {
		vfs_node_put(node);
		if (parent) {
			vfs_file_put(parent);
		}
		async_answer_0(rid, fd);
		return;
	}
	
	vfs_file_t *file = vfs_file_get(fd);
	assert(file != NULL);
	
	file->node = node;
	if (parent) {
		file->permissions = parent->permissions;
	} else {
		file->permissions = MODE_READ | MODE_WRITE | MODE_APPEND;
	}
	file->open_read = false;
	file->open_write = false;
	
	vfs_file_put(file);
	if (parent) {
		vfs_file_put(parent);
	}
	
	fibril_rwlock_read_unlock(&namespace_rwlock);

	async_answer_1(rid, EOK, fd);
}

void vfs_open2(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = IPC_GET_ARG1(*request);
	int flags = IPC_GET_ARG2(*request);

	if (flags == 0) {
		async_answer_0(rid, EINVAL);
		return;
	}

	vfs_file_t *file = vfs_file_get(fd);
	if (!file) {
		async_answer_0(rid, EBADF);
		return;
	}
	
	if ((flags & ~file->permissions) != 0) {
		vfs_file_put(file);
		async_answer_0(rid, EPERM);
		return;
	}
	
	file->open_read = (flags & MODE_READ) != 0;
	file->open_write = (flags & (MODE_WRITE | MODE_APPEND)) != 0;
	file->append = (flags & MODE_APPEND) != 0;
	
	if (!file->open_read && !file->open_write) {
		vfs_file_put(file);
		async_answer_0(rid, EINVAL);
		return;
	}
	
	if (file->node->type == VFS_NODE_DIRECTORY && file->open_write) {
		file->open_read = file->open_write = false;
		vfs_file_put(file);
		async_answer_0(rid, EINVAL);
		return;
	}
	
	int rc = vfs_open_node_remote(file->node);
	if (rc != EOK) {
		file->open_read = file->open_write = false;
		vfs_file_put(file);
		async_answer_0(rid, rc);
		return;
	}
	
	vfs_file_put(file);
	async_answer_0(rid, EOK);
}

void vfs_sync(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = IPC_GET_ARG1(*request);
	
	/* Lookup the file structure corresponding to the file descriptor. */
	vfs_file_t *file = vfs_file_get(fd);
	if (!file) {
		async_answer_0(rid, ENOENT);
		return;
	}
	
	/*
	 * Lock the open file structure so that no other thread can manipulate
	 * the same open file at a time.
	 */
	fibril_mutex_lock(&file->lock);
	async_exch_t *fs_exch = vfs_exchange_grab(file->node->fs_handle);
	
	/* Make a VFS_OUT_SYMC request at the destination FS server. */
	aid_t msg;
	ipc_call_t answer;
	msg = async_send_2(fs_exch, VFS_OUT_SYNC, file->node->service_id,
	    file->node->index, &answer);
	
	vfs_exchange_release(fs_exch);
	
	/* Wait for reply from the FS server. */
	sysarg_t rc;
	async_wait_for(msg, &rc);
	
	fibril_mutex_unlock(&file->lock);
	
	vfs_file_put(file);
	async_answer_0(rid, rc);
}

void vfs_close(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = IPC_GET_ARG1(*request);
	int ret = vfs_fd_free(fd);
	async_answer_0(rid, ret);
}

static void vfs_rdwr(ipc_callid_t rid, ipc_call_t *request, bool read)
{
	/*
	 * The following code strongly depends on the fact that the files data
	 * structure can be only accessed by a single fibril and all file
	 * operations are serialized (i.e. the reads and writes cannot
	 * interleave and a file cannot be closed while it is being read).
	 *
	 * Additional synchronization needs to be added once the table of
	 * open files supports parallel access!
	 */
	
	int fd = IPC_GET_ARG1(*request);
	
	/* Lookup the file structure corresponding to the file descriptor. */
	vfs_file_t *file = vfs_file_get(fd);
	if (!file) {
		async_answer_0(rid, ENOENT);
		return;
	}
	
	/*
	 * Lock the open file structure so that no other thread can manipulate
	 * the same open file at a time.
	 */
	fibril_mutex_lock(&file->lock);
	
	if ((read && !file->open_read) || (!read && !file->open_write)) {
		fibril_mutex_unlock(&file->lock);
		async_answer_0(rid, EINVAL);
		return;
	}
	
	vfs_info_t *fs_info = fs_handle_to_info(file->node->fs_handle);
	assert(fs_info);
	
	/*
	 * Lock the file's node so that no other client can read/write to it at
	 * the same time unless the FS supports concurrent reads/writes and its
	 * write implementation does not modify the file size.
	 */
	if ((read) ||
	    ((fs_info->concurrent_read_write) && (fs_info->write_retains_size)))
		fibril_rwlock_read_lock(&file->node->contents_rwlock);
	else
		fibril_rwlock_write_lock(&file->node->contents_rwlock);
	
	if (file->node->type == VFS_NODE_DIRECTORY) {
		/*
		 * Make sure that no one is modifying the namespace
		 * while we are in readdir().
		 */
		assert(read);
		fibril_rwlock_read_lock(&namespace_rwlock);
	}
	
	async_exch_t *fs_exch = vfs_exchange_grab(file->node->fs_handle);
	
	/*
	 * Make a VFS_READ/VFS_WRITE request at the destination FS server
	 * and forward the IPC_M_DATA_READ/IPC_M_DATA_WRITE request to the
	 * destination FS server. The call will be routed as if sent by
	 * ourselves. Note that call arguments are immutable in this case so we
	 * don't have to bother.
	 */
	sysarg_t rc;
	ipc_call_t answer;
	if (read) {
		rc = async_data_read_forward_4_1(fs_exch, VFS_OUT_READ,
		    file->node->service_id, file->node->index,
		    LOWER32(file->pos), UPPER32(file->pos), &answer);
	} else {
		if (file->append)
			file->pos = vfs_node_get_size(file->node);
		
		rc = async_data_write_forward_4_1(fs_exch, VFS_OUT_WRITE,
		    file->node->service_id, file->node->index,
		    LOWER32(file->pos), UPPER32(file->pos), &answer);
	}
	
	vfs_exchange_release(fs_exch);
	
	size_t bytes = IPC_GET_ARG1(answer);
	
	if (file->node->type == VFS_NODE_DIRECTORY) {
		fibril_rwlock_read_unlock(&namespace_rwlock);
	}
	
	/* Unlock the VFS node. */
	if ((read) ||
	    ((fs_info->concurrent_read_write) && (fs_info->write_retains_size)))
		fibril_rwlock_read_unlock(&file->node->contents_rwlock);
	else {
		/* Update the cached version of node's size. */
		if (rc == EOK)
			file->node->size = MERGE_LOUP32(IPC_GET_ARG2(answer),
			    IPC_GET_ARG3(answer));
		fibril_rwlock_write_unlock(&file->node->contents_rwlock);
	}
	
	/* Update the position pointer and unlock the open file. */
	if (rc == EOK)
		file->pos += bytes;
	fibril_mutex_unlock(&file->lock);
	vfs_file_put(file);	

	/*
	 * FS server's reply is the final result of the whole operation we
	 * return to the client.
	 */
	async_answer_1(rid, rc, bytes);
}

void vfs_read(ipc_callid_t rid, ipc_call_t *request)
{
	vfs_rdwr(rid, request, true);
}

void vfs_write(ipc_callid_t rid, ipc_call_t *request)
{
	vfs_rdwr(rid, request, false);
}

void vfs_seek(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = (int) IPC_GET_ARG1(*request);
	off64_t off = (off64_t) MERGE_LOUP32(IPC_GET_ARG2(*request),
	    IPC_GET_ARG3(*request));
	int whence = (int) IPC_GET_ARG4(*request);
	
	/* Lookup the file structure corresponding to the file descriptor. */
	vfs_file_t *file = vfs_file_get(fd);
	if (!file) {
		async_answer_0(rid, ENOENT);
		return;
	}
	
	fibril_mutex_lock(&file->lock);
	
	off64_t newoff;
	switch (whence) {
	case SEEK_SET:
		if (off >= 0) {
			file->pos = (aoff64_t) off;
			fibril_mutex_unlock(&file->lock);
			vfs_file_put(file);
			async_answer_1(rid, EOK, off);
			return;
		}
		break;
	case SEEK_CUR:
		if ((off >= 0) && (file->pos + off < file->pos)) {
			fibril_mutex_unlock(&file->lock);
			vfs_file_put(file);
			async_answer_0(rid, EOVERFLOW);
			return;
		}
		
		if ((off < 0) && (file->pos < (aoff64_t) -off)) {
			fibril_mutex_unlock(&file->lock);
			vfs_file_put(file);
			async_answer_0(rid, EOVERFLOW);
			return;
		}
		
		file->pos += off;
		newoff = (file->pos > OFF64_MAX) ?  OFF64_MAX : file->pos;
		
		fibril_mutex_unlock(&file->lock);
		vfs_file_put(file);
		async_answer_2(rid, EOK, LOWER32(newoff),
		    UPPER32(newoff));
		return;
	case SEEK_END:
		fibril_rwlock_read_lock(&file->node->contents_rwlock);
		aoff64_t size = vfs_node_get_size(file->node);
		
		if ((off >= 0) && (size + off < size)) {
			fibril_rwlock_read_unlock(&file->node->contents_rwlock);
			fibril_mutex_unlock(&file->lock);
			vfs_file_put(file);
			async_answer_0(rid, EOVERFLOW);
			return;
		}
		
		if ((off < 0) && (size < (aoff64_t) -off)) {
			fibril_rwlock_read_unlock(&file->node->contents_rwlock);
			fibril_mutex_unlock(&file->lock);
			vfs_file_put(file);
			async_answer_0(rid, EOVERFLOW);
			return;
		}
		
		file->pos = size + off;
		newoff = (file->pos > OFF64_MAX) ?  OFF64_MAX : file->pos;
		
		fibril_rwlock_read_unlock(&file->node->contents_rwlock);
		fibril_mutex_unlock(&file->lock);
		vfs_file_put(file);
		async_answer_2(rid, EOK, LOWER32(newoff), UPPER32(newoff));
		return;
	}
	
	fibril_mutex_unlock(&file->lock);
	vfs_file_put(file);
	async_answer_0(rid, EINVAL);
}

int vfs_truncate_internal(fs_handle_t fs_handle, service_id_t service_id,
    fs_index_t index, aoff64_t size)
{
	async_exch_t *exch = vfs_exchange_grab(fs_handle);
	sysarg_t rc = async_req_4_0(exch, VFS_OUT_TRUNCATE,
	    (sysarg_t) service_id, (sysarg_t) index, LOWER32(size),
	    UPPER32(size));
	vfs_exchange_release(exch);
	
	return (int) rc;
}

void vfs_truncate(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = IPC_GET_ARG1(*request);
	aoff64_t size = (aoff64_t) MERGE_LOUP32(IPC_GET_ARG2(*request),
	    IPC_GET_ARG3(*request));
	int rc;

	vfs_file_t *file = vfs_file_get(fd);
	if (!file) {
		async_answer_0(rid, ENOENT);
		return;
	}
	fibril_mutex_lock(&file->lock);

	fibril_rwlock_write_lock(&file->node->contents_rwlock);
	rc = vfs_truncate_internal(file->node->fs_handle,
	    file->node->service_id, file->node->index, size);
	if (rc == EOK)
		file->node->size = size;
	fibril_rwlock_write_unlock(&file->node->contents_rwlock);

	fibril_mutex_unlock(&file->lock);
	vfs_file_put(file);
	async_answer_0(rid, (sysarg_t)rc);
}

void vfs_fstat(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = IPC_GET_ARG1(*request);
	sysarg_t rc;

	vfs_file_t *file = vfs_file_get(fd);
	if (!file) {
		async_answer_0(rid, ENOENT);
		return;
	}

	ipc_callid_t callid;
	if (!async_data_read_receive(&callid, NULL)) {
		vfs_file_put(file);
		async_answer_0(callid, EINVAL);
		async_answer_0(rid, EINVAL);
		return;
	}

	fibril_mutex_lock(&file->lock);

	async_exch_t *exch = vfs_exchange_grab(file->node->fs_handle);
	
	aid_t msg;
	msg = async_send_3(exch, VFS_OUT_STAT, file->node->service_id,
	    file->node->index, true, NULL);
	async_forward_fast(callid, exch, 0, 0, 0, IPC_FF_ROUTE_FROM_ME);
	
	vfs_exchange_release(exch);
	
	async_wait_for(msg, &rc);
	
	fibril_mutex_unlock(&file->lock);
	vfs_file_put(file);
	async_answer_0(rid, rc);
}

void vfs_unlink2(ipc_callid_t rid, ipc_call_t *request)
{
	int rc;
	char *path;
	vfs_file_t *parent = NULL;
	vfs_file_t *expect = NULL;
	vfs_node_t *parent_node = root;
	
	int parentfd = IPC_GET_ARG1(*request);
	int expectfd = IPC_GET_ARG2(*request);
	int wflag = IPC_GET_ARG3(*request);
	
	rc = async_data_write_accept((void **) &path, true, 0, 0, 0, NULL);
	if (rc != EOK) {
		async_answer_0(rid, rc);
		return;
	}
	
	fibril_rwlock_write_lock(&namespace_rwlock);
	
	int lflag = (wflag&WALK_DIRECTORY) ? L_DIRECTORY: 0;

	if (parentfd >= 0) {
		parent = vfs_file_get(parentfd);
		if (!parent) {
			rc = ENOENT;
			goto exit;
		}
		parent_node = parent->node;
	}
	
	if (expectfd >= 0) {
		expect = vfs_file_get(expectfd);
		if (!expect) {
			rc = ENOENT;
			goto exit;
		}
		
		vfs_lookup_res_t lr;
		rc = vfs_lookup_internal((vfs_triplet_t *) parent_node, path, lflag, &lr);
		if (rc != EOK) {
			goto exit;
		}
		
		if (__builtin_memcmp(&lr.triplet, expect->node, sizeof(vfs_triplet_t)) != 0) {
			rc = ENOENT;
			goto exit;
		}
		
		vfs_file_put(expect);
		expect = NULL;
	}
	
	vfs_lookup_res_t lr;
	rc = vfs_lookup_internal((vfs_triplet_t *) parent_node, path, lflag | L_UNLINK, &lr);
	if (rc != EOK) {
		goto exit;
	}

	/*
	 * The name has already been unlinked by vfs_lookup_internal().
	 * We have to get and put the VFS node to ensure that it is
	 * VFS_OUT_DESTROY'ed after the last reference to it is dropped.
	 */
	vfs_node_put(vfs_node_get(&lr));

exit:
	if (path) {
		free(path);
	}
	if (parent) {
		vfs_file_put(parent);
	}
	if (expect) {
		vfs_file_put(expect);
	}
	fibril_rwlock_write_unlock(&namespace_rwlock);
	async_answer_0(rid, rc);
}

static size_t shared_path(char *a, char *b)
{
	size_t res = 0;
	
	while (a[res] == b[res] && a[res] != 0) {
		res++;
	}
	
	if (a[res] == b[res]) {
		return res;
	}
	
	res--;
	while (a[res] != '/') {
		res--;
	}
	return res;
}

static int vfs_rename_internal(vfs_triplet_t *base, char *old, char *new)
{
	assert(base != NULL);
	assert(old != NULL);
	assert(new != NULL);
	
	vfs_lookup_res_t base_lr;
	vfs_lookup_res_t old_lr;
	vfs_lookup_res_t new_lr_orig;
	bool orig_unlinked = false;
	
	int rc;
	
	size_t shared = shared_path(old, new);
	
	/* Do not allow one path to be a prefix of the other. */
	if (old[shared] == 0 || new[shared] == 0) {
		return EINVAL;
	}
	assert(old[shared] == '/');
	assert(new[shared] == '/');
	
	fibril_rwlock_write_lock(&namespace_rwlock);
	
	/* Resolve the shared portion of the path first. */
	if (shared != 0) {
		old[shared] = 0;
		rc = vfs_lookup_internal(base, old, L_DIRECTORY, &base_lr);
		if (rc != EOK) {
			fibril_rwlock_write_unlock(&namespace_rwlock);
			return rc;
		}
		
		base = &base_lr.triplet;
		old[shared] = '/';
		old += shared;
		new += shared;
	}
	
	
	rc = vfs_lookup_internal(base, new, L_UNLINK | L_DISABLE_MOUNTS, &new_lr_orig);
	if (rc == EOK) {
		orig_unlinked = true;
	} else if (rc != ENOENT) {
		fibril_rwlock_write_unlock(&namespace_rwlock);
		return rc;
	}
	
	rc = vfs_lookup_internal(base, old, L_UNLINK | L_DISABLE_MOUNTS, &old_lr);
	if (rc != EOK) {
		if (orig_unlinked) {
			vfs_link_internal(base, new, &new_lr_orig.triplet);
		}
		fibril_rwlock_write_unlock(&namespace_rwlock);
		return rc;
	}
	
	rc = vfs_link_internal(base, new, &old_lr.triplet);
	if (rc != EOK) {
		vfs_link_internal(base, old, &old_lr.triplet);
		if (orig_unlinked) {
			vfs_link_internal(base, new, &new_lr_orig.triplet);
		}
		fibril_rwlock_write_unlock(&namespace_rwlock);
		return rc;
	}
	
	if (orig_unlinked) {
		vfs_node_put(vfs_node_get(&new_lr_orig));
	}
	
	fibril_rwlock_write_unlock(&namespace_rwlock);
	return EOK;
}

void vfs_rename(ipc_callid_t rid, ipc_call_t *request)
{
	/* The common base directory. */
	int basefd;
	char *old = NULL;
	char *new = NULL;
	vfs_file_t *base = NULL;
	int rc;
	
	basefd = IPC_GET_ARG1(*request);
	
	/* Retrieve the old path. */
	rc = async_data_write_accept((void **) &old, true, 0, 0, 0, NULL);
	if (rc != EOK) {
		goto out;
	}
	
	/* Retrieve the new path. */
	rc = async_data_write_accept((void **) &new, true, 0, 0, 0, NULL);
	if (rc != EOK) {
		goto out;
	}
	
	size_t olen;
	size_t nlen;
	char *oldc = canonify(old, &olen);
	char *newc = canonify(new, &nlen);
	
	if ((!oldc) || (!newc)) {
		rc = EINVAL;
		goto out;
	}
	
	assert(oldc[olen] == '\0');
	assert(newc[nlen] == '\0');
	
	/* Lookup the file structure corresponding to the file descriptor. */
	vfs_node_t *base_node = root;
	// TODO: Client-side root.
	if (basefd != -1) {
		base = vfs_file_get(basefd);
		if (!base) {
			rc = EBADF;
			goto out;
		}
		base_node = base->node;
	}
	
	rc = vfs_rename_internal((vfs_triplet_t *) base_node, oldc, newc);

out:
	async_answer_0(rid, rc);

	if (old) {
		free(old);
	}
	if (new) {
		free(new);
	}
	if (base) {
		vfs_file_put(base);
	}
}

void vfs_dup(ipc_callid_t rid, ipc_call_t *request)
{
	int oldfd = IPC_GET_ARG1(*request);
	int newfd = IPC_GET_ARG2(*request);
	
	/* If the file descriptors are the same, do nothing. */
	if (oldfd == newfd) {
		async_answer_1(rid, EOK, newfd);
		return;
	}
	
	/* Lookup the file structure corresponding to oldfd. */
	vfs_file_t *oldfile = vfs_file_get(oldfd);
	if (!oldfile) {
		async_answer_0(rid, EBADF);
		return;
	}
	
	/*
	 * Lock the open file structure so that no other thread can manipulate
	 * the same open file at a time.
	 */
	fibril_mutex_lock(&oldfile->lock);
	
	/* Make sure newfd is closed. */
	(void) vfs_fd_free(newfd);
	
	/* Assign the old file to newfd. */
	int ret = vfs_fd_assign(oldfile, newfd);
	fibril_mutex_unlock(&oldfile->lock);
	vfs_file_put(oldfile);
	
	if (ret != EOK)
		async_answer_0(rid, ret);
	else
		async_answer_1(rid, EOK, newfd);
}

void vfs_wait_handle(ipc_callid_t rid, ipc_call_t *request)
{
	int fd = vfs_wait_handle_internal();
	async_answer_1(rid, EOK, fd);
}

void vfs_get_mtab(ipc_callid_t rid, ipc_call_t *request)
{
	ipc_callid_t callid;
	ipc_call_t data;
	sysarg_t rc = EOK;
	size_t len;

	fibril_mutex_lock(&mtab_list_lock);

	/* Send to the caller the number of mounted filesystems */
	callid = async_get_call(&data);
	if (IPC_GET_IMETHOD(data) != VFS_IN_PING) {
		rc = ENOTSUP;
		async_answer_0(callid, rc);
		goto exit;
	}
	async_answer_1(callid, EOK, mtab_size);

	list_foreach(mtab_list, cur) {
		mtab_ent_t *mtab_ent = list_get_instance(cur, mtab_ent_t,
		    link);

		rc = ENOTSUP;

		if (!async_data_read_receive(&callid, &len)) {
			async_answer_0(callid, rc);
			goto exit;
		}

		(void) async_data_read_finalize(callid, mtab_ent->mp,
		    str_size(mtab_ent->mp));

		if (!async_data_read_receive(&callid, &len)) {
			async_answer_0(callid, rc);
			goto exit;
		}

		(void) async_data_read_finalize(callid, mtab_ent->opts,
		    str_size(mtab_ent->opts));

		if (!async_data_read_receive(&callid, &len)) {
			async_answer_0(callid, rc);
			goto exit;
		}

		(void) async_data_read_finalize(callid, mtab_ent->fs_name,
		    str_size(mtab_ent->fs_name));

		callid = async_get_call(&data);

		if (IPC_GET_IMETHOD(data) != VFS_IN_PING) {
			async_answer_0(callid, rc);
			goto exit;
		}

		rc = EOK;
		async_answer_2(callid, rc, mtab_ent->instance,
		    mtab_ent->service_id);
	}

exit:
	fibril_mutex_unlock(&mtab_list_lock);
	async_answer_0(rid, rc);
}

/**
 * @}
 */
