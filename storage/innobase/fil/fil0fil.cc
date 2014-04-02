/*****************************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file fil/fil0fil.cc
The tablespace memory cache

Created 10/25/1995 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "fil0fil.h"
#include "mem0mem.h"
#include "hash0hash.h"
#include "os0file.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "log0recv.h"
#include "fsp0fsp.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "dict0dict.h"
#include "page0page.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "btr0btr.h"
#include "dict0boot.h"
#include "row0mysql.h"
#include "row0trunc.h"
#ifndef UNIV_HOTBACKUP
# include "buf0lru.h"
# include "ibuf0ibuf.h"
# include "sync0sync.h"
# include "os0event.h"
#else /* !UNIV_HOTBACKUP */
# include "srv0srv.h"
#endif /* !UNIV_HOTBACKUP */
#include "fsp0file.h"
#include "fsp0sysspace.h"

/*
		IMPLEMENTATION OF THE TABLESPACE MEMORY CACHE
		=============================================

The tablespace cache is responsible for providing fast read/write access to
tablespaces and logs of the database. File creation and deletion is done
in other modules which know more of the logic of the operation, however.

A tablespace consists of a chain of files. The size of the files does not
have to be divisible by the database block size, because we may just leave
the last incomplete block unused. When a new file is appended to the
tablespace, the maximum size of the file is also specified. At the moment,
we think that it is best to extend the file to its maximum size already at
the creation of the file, because then we can avoid dynamically extending
the file when more space is needed for the tablespace.

A block's position in the tablespace is specified with a 32-bit unsigned
integer. The files in the chain are thought to be catenated, and the block
corresponding to an address n is the nth block in the catenated file (where
the first block is named the 0th block, and the incomplete block fragments
at the end of files are not taken into account). A tablespace can be extended
by appending a new file at the end of the chain.

Our tablespace concept is similar to the one of Oracle.

To acquire more speed in disk transfers, a technique called disk striping is
sometimes used. This means that logical block addresses are divided in a
round-robin fashion across several disks. Windows NT supports disk striping,
so there we do not need to support it in the database. Disk striping is
implemented in hardware in RAID disks. We conclude that it is not necessary
to implement it in the database. Oracle 7 does not support disk striping,
either.

Another trick used at some database sites is replacing tablespace files by
raw disks, that is, the whole physical disk drive, or a partition of it, is
opened as a single file, and it is accessed through byte offsets calculated
from the start of the disk or the partition. This is recommended in some
books on database tuning to achieve more speed in i/o. Using raw disk
certainly prevents the OS from fragmenting disk space, but it is not clear
if it really adds speed. We measured on the Pentium 100 MHz + NT + NTFS file
system + EIDE Conner disk only a negligible difference in speed when reading
from a file, versus reading from a raw disk.

To have fast access to a tablespace or a log file, we put the data structures
to a hash table. Each tablespace and log file is given an unique 32-bit
identifier.

Some operating systems do not support many open files at the same time,
though NT seems to tolerate at least 900 open files. Therefore, we put the
open files in an LRU-list. If we need to open another file, we may close the
file at the end of the LRU-list. When an i/o-operation is pending on a file,
the file cannot be closed. We take the file nodes with pending i/o-operations
out of the LRU-list and keep a count of pending operations. When an operation
completes, we decrement the count and return the file node to the LRU-list if
the count drops to zero. */

/** When mysqld is run, the default directory "." is the mysqld datadir,
but in the MySQL Embedded Server Library and ibbackup it is not the default
directory, and we must set the base file path explicitly */
const char*	fil_path_to_mysql_datadir	= ".";

/** Common InnoDB file extentions */
const char* dot_ext[] = { "", ".ibd", ".isl", ".cfg" };

/** The number of fsyncs done to the log */
ulint	fil_n_log_flushes			= 0;

/** Number of pending redo log flushes */
ulint	fil_n_pending_log_flushes		= 0;
/** Number of pending tablespace flushes */
ulint	fil_n_pending_tablespace_flushes	= 0;

/** Number of files currently open */
ulint	fil_n_file_opened			= 0;

/** The null file address */
fil_addr_t	fil_addr_null = {FIL_NULL, 0};

/** File node of a tablespace or the log data space */
struct fil_node_t {
	fil_space_t*	space;	/*!< backpointer to the space where this node
				belongs */
	char*		name;	/*!< path to the file */
	bool		is_open;/*!< true if file is open */
	os_file_t	handle;	/*!< OS handle to the file, if file open */
	os_event_t	sync_event;/*!< Condition event to group and
				serialize calls to fsync */
	bool		is_raw_disk;/*!< true if the 'file' is actually a raw
				device or a raw disk partition */
	ulint		size;	/*!< size of the file in database pages, 0 if
				not known yet; the possible last incomplete
				megabyte may be ignored if space == 0 */
	ulint		n_pending;
				/*!< count of pending i/o's on this file;
				closing of the file is not allowed if
				this is > 0 */
	ulint		n_pending_flushes;
				/*!< count of pending flushes on this file;
				closing of the file is not allowed if
				this is > 0 */
	bool		being_extended;
				/*!< true if the node is currently
				being extended. */
	ib_int64_t	modification_counter;/*!< when we write to the file we
				increment this by one */
	ib_int64_t	flush_counter;/*!< up to what
				modification_counter value we have
				flushed the modifications to disk */
	UT_LIST_NODE_T(fil_node_t) chain;
				/*!< link field for the file chain */
	UT_LIST_NODE_T(fil_node_t) LRU;
				/*!< link field for the LRU list */
	ulint		magic_n;/*!< FIL_NODE_MAGIC_N */
};

/** Value of fil_node_t::magic_n */
#define	FIL_NODE_MAGIC_N	89389

/** Tablespace or log data space: let us call them by a common name space */
struct fil_space_t {
	char*		name;	/*!< space name = the path to the first file in
				it */
	ulint		id;	/*!< space id */
	ib_int64_t	tablespace_version;
				/*!< in DISCARD/IMPORT this timestamp
				is used to check if we should ignore
				an insert buffer merge request for a
				page because it actually was for the
				previous incarnation of the space */
	bool		stop_ios;/*!< true if we want to rename the
				.ibd file of tablespace and want to
				stop temporarily posting of new i/o
				requests on the file */
	bool		stop_new_ops;
				/*!< we set this true when we start
				deleting a single-table tablespace.
				When this is set following new ops
				are not allowed:
				* read IO request
				* ibuf merge
				* file flush
				Note that we can still possibly have
				new write operations because we don't
				check this flag when doing flush
				batches. */
	bool		is_being_truncated;
				/*!< this is set to true when we prepare to
				truncate a single-table tablespace and its
				.ibd file */
	fil_type_t	purpose;/*!< purpose */
	UT_LIST_BASE_NODE_T(fil_node_t) chain;
				/*!< base node for the file chain */
	ulint		size;	/*!< space size in pages; 0 if a single-table
				tablespace whose size we do not know yet;
				last incomplete megabytes in data files may be
				ignored if space == 0 */
	ulint		flags;	/*!< tablespace flags; see
				fsp_flags_is_valid(),
				page_size_t(ulint) (constructor) */
	ulint		n_reserved_extents;
				/*!< number of reserved free extents for
				ongoing operations like B-tree page split */
	ulint		n_pending_flushes; /*!< this is positive when flushing
				the tablespace to disk; dropping of the
				tablespace is forbidden if this is positive */
	ulint		n_pending_ops;/*!< this is positive when we
				have pending operations against this
				tablespace. The pending operations can
				be ibuf merges or lock validation code
				trying to read a block.
				Dropping of the tablespace is forbidden
				if this is positive */
	hash_node_t	hash;	/*!< hash chain node */
	hash_node_t	name_hash;/*!< hash chain the name_hash table */
#ifndef UNIV_HOTBACKUP
	rw_lock_t	latch;	/*!< latch protecting the file space storage
				allocation */
#endif /* !UNIV_HOTBACKUP */
	UT_LIST_NODE_T(fil_space_t) unflushed_spaces;
				/*!< list of spaces with at least one unflushed
				file we have written to */
	bool		is_in_unflushed_spaces;
				/*!< true if this space is currently in
				unflushed_spaces */
	UT_LIST_NODE_T(fil_space_t) space_list;
				/*!< list of all spaces */
	ulint		magic_n;/*!< FIL_SPACE_MAGIC_N */
};

/** Value of fil_space_t::magic_n */
#define	FIL_SPACE_MAGIC_N	89472

/** The tablespace memory cache; also the totality of logs (the log
data space) is stored here; below we talk about tablespaces, but also
the ib_logfiles form a 'space' and it is handled here */
struct fil_system_t {
#ifndef UNIV_HOTBACKUP
	ib_mutex_t	mutex;		/*!< The mutex protecting the cache */
#endif /* !UNIV_HOTBACKUP */
	hash_table_t*	spaces;		/*!< The hash table of spaces in the
					system; they are hashed on the space
					id */
	hash_table_t*	name_hash;	/*!< hash table based on the space
					name */
	UT_LIST_BASE_NODE_T(fil_node_t) LRU;
					/*!< base node for the LRU list of the
					most recently used open files with no
					pending i/o's; if we start an i/o on
					the file, we first remove it from this
					list, and return it to the start of
					the list when the i/o ends;
					log files and the system tablespace are
					not put to this list: they are opened
					after the startup, and kept open until
					shutdown */
	UT_LIST_BASE_NODE_T(fil_space_t) unflushed_spaces;
					/*!< base node for the list of those
					tablespaces whose files contain
					unflushed writes; those spaces have
					at least one file node where
					modification_counter > flush_counter */
	ulint		n_open;		/*!< number of files currently open */
	ulint		max_n_open;	/*!< n_open is not allowed to exceed
					this */
	ib_int64_t	modification_counter;/*!< when we write to a file we
					increment this by one */
	ulint		max_assigned_id;/*!< maximum space id in the existing
					tables, or assigned during the time
					mysqld has been up; at an InnoDB
					startup we scan the data dictionary
					and set here the maximum of the
					space id's of the tables there */
	ib_int64_t	tablespace_version;
					/*!< a counter which is incremented for
					every space object memory creation;
					every space mem object gets a
					'timestamp' from this; in DISCARD/
					IMPORT this is used to check if we
					should ignore an insert buffer merge
					request */
	UT_LIST_BASE_NODE_T(fil_space_t) space_list;
					/*!< list of all file spaces */
	bool		space_id_reuse_warned;
					/* !< true if fil_space_create()
					has issued a warning about
					potential space_id reuse */
};

/** The tablespace memory cache. This variable is NULL before the module is
initialized. */
static fil_system_t*	fil_system	= NULL;

#ifdef UNIV_HOTBACKUP
static ulint	srv_data_read;
static ulint	srv_data_written;
#endif /* UNIV_HOTBACKUP */

/** Determine if (i) is a user tablespace id or not. */
# define fil_is_user_tablespace_id(i)		\
	(((i) > srv_undo_tablespaces_open)	\
	 && ((i) != srv_tmp_space.space_id()))

/** Determine if user has explicitly disabled fsync(). */
#ifndef _WIN32
# define fil_buffering_disabled(s)	\
	((s)->purpose == FIL_TYPE_TABLESPACE	\
	 && srv_unix_file_flush_method	\
	 == SRV_UNIX_O_DIRECT_NO_FSYNC)
#else /* _WIN32 */
# define fil_buffering_disabled(s)	(0)
#endif /* __WIN32 */

#ifdef UNIV_DEBUG
/** Try fil_validate() every this many times */
# define FIL_VALIDATE_SKIP	17

/******************************************************************//**
Checks the consistency of the tablespace cache some of the time.
@return true if ok or the check was skipped */
static
bool
fil_validate_skip(void)
/*===================*/
{
	/** The fil_validate() call skip counter. Use a signed type
	because of the race condition below. */
	static int fil_validate_count = FIL_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly fil_validate() check
	in debug builds. */
	if (--fil_validate_count > 0) {
		return(true);
	}

	fil_validate_count = FIL_VALIDATE_SKIP;
	return(fil_validate());
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
Determines if a file node belongs to the least-recently-used list.
@return true if the file belongs to fil_system->LRU mutex. */
UNIV_INLINE
bool
fil_space_belongs_in_lru(
/*=====================*/
	const fil_space_t*	space)	/*!< in: file space */
{
	switch (space->purpose) {
	case FIL_TYPE_LOG:
		return(false);
	case FIL_TYPE_TABLESPACE:
	case FIL_TYPE_TEMPORARY:
		return(fil_is_user_tablespace_id(space->id));
	}

	ut_ad(0);
	return(false);
}

/********************************************************************//**
NOTE: you must call fil_mutex_enter_and_prepare_for_io() first!

Prepares a file node for i/o. Opens the file if it is closed. Updates the
pending i/o's field in the node and the system appropriately. Takes the node
off the LRU list if it is in the LRU list. The caller must hold the fil_sys
mutex.
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_prepare_for_io(
/*====================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space);	/*!< in: space */
/********************************************************************//**
Updates the data structures when an i/o operation finishes. Updates the
pending i/o's field in the node appropriately. */
static
void
fil_node_complete_io(
/*=================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	ulint		type);	/*!< in: OS_FILE_WRITE or OS_FILE_READ; marks
				the node as modified if
				type == OS_FILE_WRITE */
/*******************************************************************//**
Frees a space object from the tablespace memory cache. Closes the files in
the chain but does not delete them. There must not be any pending i/o's or
flushes on the files.
@return true on success */
static
bool
fil_space_free(
/*===========*/
	ulint		id,		/* in: space id */
	bool		x_latched);	/* in: true if caller has space->latch
					in X mode */

/** Reads data from a space to a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to read; this must not cross a
file boundary; in aio this must be a block size multiple
@param[in,out]	buf		buffer where to store data read; in aio this
must be appropriately aligned
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INLINE
dberr_t
fil_read(
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf)
{
	return(fil_io(OS_FILE_READ, true, page_id, page_size,
		      byte_offset, len, buf, NULL));
}

/** Writes data to a space from a buffer. Remember that the possible incomplete
blocks at the end of file are ignored: they are not taken into account when
calculating the byte offset within a space.
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to write; this must not cross
a file boundary; in aio this must be a block size multiple
@param[in]	buf		buffer from which to write; in aio this must
be appropriately aligned
@return DB_SUCCESS, or DB_TABLESPACE_DELETED if we are trying to do
i/o on a tablespace which does not exist */
UNIV_INLINE
dberr_t
fil_write(
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf)
{
	ut_ad(!srv_read_only_mode);

	return(fil_io(OS_FILE_WRITE, true, page_id, page_size,
		      byte_offset, len, buf, NULL));
}

/*******************************************************************//**
Returns the table space by a given id, NULL if not found. */
UNIV_INLINE
fil_space_t*
fil_space_get_by_id(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	ut_ad(mutex_own(&fil_system->mutex));

	HASH_SEARCH(hash, fil_system->spaces, id,
		    fil_space_t*, space,
		    ut_ad(space->magic_n == FIL_SPACE_MAGIC_N),
		    space->id == id);

	return(space);
}

/*******************************************************************//**
Returns the table space by a given name, NULL if not found. */
UNIV_INLINE
fil_space_t*
fil_space_get_by_name(
/*==================*/
	const char*	name)	/*!< in: space name */
{
	fil_space_t*	space;
	ulint		fold;

	ut_ad(mutex_own(&fil_system->mutex));

	fold = ut_fold_string(name);

	HASH_SEARCH(name_hash, fil_system->name_hash, fold,
		    fil_space_t*, space,
		    ut_ad(space->magic_n == FIL_SPACE_MAGIC_N),
		    !strcmp(name, space->name));

	return(space);
}

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Returns the version number of a tablespace, -1 if not found.
@return version number, -1 if the tablespace does not exist in the
memory cache */

ib_int64_t
fil_space_get_version(
/*==================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ib_int64_t	version		= -1;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space != NULL) {
		version = space->tablespace_version;
	}

	mutex_exit(&fil_system->mutex);

	return(version);
}

/** Returns the latch of a file space.
@param[in]	id	space id
@param[out]	flags	tablespace flags
@return latch protecting storage allocation */
rw_lock_t*
fil_space_get_latch(
	ulint	id,
	ulint*	flags)
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	if (flags) {
		*flags = space->flags;
	}

	mutex_exit(&fil_system->mutex);

	return(&(space->latch));
}

/** Gets the type of a file space.
@param[in]	id	tablespace identifier
@return file type */

fil_type_t
fil_space_get_type(
	ulint	id)
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	mutex_exit(&fil_system->mutex);

	return(space->purpose);
}

/** @brief Note that a tablespace has been imported.
It is initially marked as FIL_TYPE_TEMPORARY so that no logging is
done during the import process when the space ID is stamped to each page.
Now we change it to FIL_SPACE_TABLESPACE to start redo and undo logging.
NOTE: temporary tablespaces are never imported.
@param[in] id tablespace identifier */

void
fil_space_set_imported(
	ulint		id)
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);
	ut_ad(space->purpose == FIL_TYPE_TEMPORARY);
	space->purpose = FIL_TYPE_TABLESPACE;

	mutex_exit(&fil_system->mutex);
}
#endif /* !UNIV_HOTBACKUP */

/**********************************************************************//**
Checks if all the file nodes in a space are flushed. The caller must hold
the fil_system mutex.
@return true if all are flushed */
static
bool
fil_space_is_flushed(
/*=================*/
	fil_space_t*	space)	/*!< in: space */
{
	fil_node_t*	node;

	ut_ad(mutex_own(&fil_system->mutex));

	node = UT_LIST_GET_FIRST(space->chain);

	while (node) {
		if (node->modification_counter > node->flush_counter) {

			ut_ad(!fil_buffering_disabled(space));
			return(false);
		}

		node = UT_LIST_GET_NEXT(chain, node);
	}

	return(true);
}

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)

#include <sys/ioctl.h>
/** FusionIO atomic write control info */
#define DFS_IOCTL_ATOMIC_WRITE_SET	_IOW(0x95, 2, uint)

/**
Try and enable FusionIO atomic writes.
@param[in] file		OS file handle
@return true if successful */

bool
fil_fusionio_enable_atomic_write(os_file_t file)
{
	if (srv_unix_file_flush_method == SRV_UNIX_O_DIRECT) {

		uint	atomic = 1;

		ut_a(file != -1);

		if (ioctl(file, DFS_IOCTL_ATOMIC_WRITE_SET, &atomic) != -1) {

			return(true);
		}
	}

	return(false);
}
#endif /* !NO_FALLOCATE && UNIV_LINUX */

/*******************************************************************//**
Appends a new file to the chain of files of a space. File must be closed.
@return pointer to the file name, or NULL on error */

char*
fil_node_create(
/*============*/
	const char*	name,	/*!< in: file name (file must be closed) */
	ulint		size,	/*!< in: file size in database blocks, rounded
				downwards to an integer */
	fil_space_t*	space,	/*!< in,out: space where to append */
	bool		is_raw)	/*!< in: true if a raw device or
				a raw disk partition */
{
	fil_node_t*	node;

	ut_ad(name != NULL);
	ut_ad(fil_system != NULL);

	if (space == NULL) {
		return(NULL);
	}

	node = static_cast<fil_node_t*>(ut_zalloc(sizeof(fil_node_t)));

	node->name = mem_strdup(name);

	ut_a(!is_raw || srv_start_raw_disk_in_use);

	node->sync_event = os_event_create("fsync_event");
	node->is_raw_disk = is_raw;
	node->size = size;
	node->magic_n = FIL_NODE_MAGIC_N;

	space->size += size;

	node->space = space;

	UT_LIST_ADD_LAST(space->chain, node);

	return(node->name);
}

/********************************************************************//**
Opens a file of a node of a tablespace. The caller must own the fil_system
mutex.
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_open_file(
/*===============*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space)	/*!< in: space */
{
	os_offset_t	size_bytes;
	bool		success;
	byte*		buf2;
	byte*		page;
	ulint		space_id;
	ulint		flags;
	ulint		min_size;

	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->n_pending == 0);
	ut_a(!node->is_open);

	if (node->size == 0) {
		/* It must be a single-table tablespace and we do not know the
		size of the file yet. First we open the file in the normal
		mode, no async I/O here, for simplicity. Then do some checks,
		and close the file again.
		NOTE that we could not use the simple file read function
		os_file_read() in Windows to read from a file opened for
		async I/O! */

		node->handle = os_file_create_simple_no_error_handling(
			innodb_data_file_key, node->name, OS_FILE_OPEN,
			OS_FILE_READ_ONLY, &success);
		if (!success) {
			/* The following call prints an error message */
			os_file_get_last_error(true);

			ib_logf(IB_LOG_LEVEL_WARN, "Cannot open '%s'."
				" Have you deleted .ibd files under a"
				" running mysqld server?",
				node->name);

			return(false);
		}

		size_bytes = os_file_get_size(node->handle);
		ut_a(size_bytes != (os_offset_t) -1);
#ifdef UNIV_HOTBACKUP
		if (space->id == 0) {
			node->size = (ulint) (size_bytes / UNIV_PAGE_SIZE);
			os_file_close(node->handle);
			goto add_size;
		}
#endif /* UNIV_HOTBACKUP */
		ut_a(space->purpose != FIL_TYPE_LOG);
		ut_a(fil_is_user_tablespace_id(space->id));

		/* Read the first page of the tablespace */

		buf2 = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));
		/* Align the memory for file i/o if we might have O_DIRECT
		set */
		page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

		success = os_file_read(node->handle, page, 0, UNIV_PAGE_SIZE);
		space_id = fsp_header_get_space_id(page);
		flags = fsp_header_get_flags(page);

		::ut_free(buf2);

		/* Close the file now that we have read the space id from it */

		os_file_close(node->handle);

		const page_size_t	page_size(flags);

		min_size = FIL_IBD_FILE_INITIAL_SIZE * page_size.physical();

		if (size_bytes < min_size) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The size of single-table tablespace file %s,"
				" is only " UINT64PF ", should be at least %lu!",
				node->name, size_bytes, min_size);

			ut_error;
		}

		if (UNIV_UNLIKELY(space_id != space->id)) {
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Tablespace id is %lu in the data"
				" dictionary but in file %s it is %lu!",
				space->id, node->name, space_id);
		}

		if (UNIV_UNLIKELY(space_id == ULINT_UNDEFINED
				  || space_id == 0)) {
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Tablespace id %lu in file %s is not sensible",
				(ulong) space_id, node->name);
		}

		const page_size_t	space_page_size(space->flags);

		if (!page_size.equals_to(space_page_size)) {
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Tablespace file %s has page size "
				"(physical=" ULINTPF ", logical=" ULINTPF ") "
				"(flags=0x%lx) but the data "
				"dictionary expects page size "
				"(physical=" ULINTPF ", logical=" ULINTPF ") "
				"(flags=0x%lx)!",
				node->name,
				page_size.physical(),
				page_size.logical(),
				flags,
				space_page_size.physical(),
				space_page_size.logical(),
				space->flags);
		}

		if (UNIV_UNLIKELY(space->flags != flags)) {
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Table flags are 0x%lx in the"
				" data dictionary but the flags in file"
				" %s are 0x%lx!",
				space->flags, node->name, flags);
		}

		if (size_bytes >= 1024 * 1024) {
			/* Truncate the size to whole megabytes. */
			size_bytes = ut_2pow_round(size_bytes, 1024 * 1024);
		}

		node->size = (ulint) (size_bytes / page_size.physical());

#ifdef UNIV_HOTBACKUP
add_size:
#endif /* UNIV_HOTBACKUP */
		space->size += node->size;
	}

	/* printf("Opening file %s\n", node->name); */

	/* Open the file for reading and writing, in Windows normally in the
	unbuffered async I/O mode, though global variables may make
	os_file_create() to fall back to the normal file I/O mode. */

	if (space->purpose == FIL_TYPE_LOG) {
		node->handle = os_file_create(
			innodb_log_file_key, node->name, OS_FILE_OPEN,
			OS_FILE_AIO, OS_LOG_FILE, &success);
	} else if (node->is_raw_disk) {
		node->handle = os_file_create(
			innodb_data_file_key, node->name, OS_FILE_OPEN_RAW,
			OS_FILE_AIO, OS_DATA_FILE, &success);
	} else {
		node->handle = os_file_create(
			innodb_data_file_key, node->name, OS_FILE_OPEN,
			OS_FILE_AIO, OS_DATA_FILE, &success);
	}

	ut_a(success);

	node->is_open = true;

	system->n_open++;
	fil_n_file_opened++;

	if (fil_space_belongs_in_lru(space)) {

		/* Put the node to the LRU list */
		UT_LIST_ADD_FIRST(system->LRU, node);
	}

	return(true);
}

/**********************************************************************//**
Closes a file. */
static
void
fil_node_close_file(
/*================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system)	/*!< in: tablespace memory cache */
{
	bool	ret;

	ut_ad(node && system);
	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->is_open);
	ut_a(node->n_pending == 0);
	ut_a(node->n_pending_flushes == 0);
	ut_a(!node->being_extended);
#ifndef UNIV_HOTBACKUP
	ut_a(node->modification_counter == node->flush_counter
	     || node->space->purpose == FIL_TYPE_TEMPORARY
	     || srv_fast_shutdown == 2);
#endif /* !UNIV_HOTBACKUP */

	ret = os_file_close(node->handle);
	ut_a(ret);

	/* printf("Closing file %s\n", node->name); */

	node->is_open = false;
	ut_a(system->n_open > 0);
	system->n_open--;
	fil_n_file_opened--;

	if (fil_space_belongs_in_lru(node->space)) {

		ut_a(UT_LIST_GET_LEN(system->LRU) > 0);

		/* The node is in the LRU list, remove it */
		UT_LIST_REMOVE(system->LRU, node);
	}
}

/********************************************************************//**
Tries to close a file in the LRU list. The caller must hold the fil_sys
mutex.
@return true if success, false if should retry later; since i/o's
generally complete in < 100 ms, and as InnoDB writes at most 128 pages
from the buffer pool in a batch, and then immediately flushes the
files, there is a good chance that the next time we find a suitable
node from the LRU list */
static
bool
fil_try_to_close_file_in_LRU(
/*=========================*/
	bool	print_info)	/*!< in: if true, prints information why it
				cannot close a file */
{
	fil_node_t*	node;

	ut_ad(mutex_own(&fil_system->mutex));

	if (print_info) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"fil_sys open file LRU len %lu",
			(ulong) UT_LIST_GET_LEN(fil_system->LRU));
	}

	for (node = UT_LIST_GET_LAST(fil_system->LRU);
	     node != NULL;
	     node = UT_LIST_GET_PREV(LRU, node)) {

		if (node->modification_counter == node->flush_counter
		    && node->n_pending_flushes == 0
		    && !node->being_extended) {

			fil_node_close_file(node, fil_system);

			return(true);
		}

		if (!print_info) {
			continue;
		}

		if (node->n_pending_flushes > 0) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Cannot close file %s, because"
				" n_pending_flushes %lu",
				node->name,
				(ulong) node->n_pending_flushes);
		}

		if (node->modification_counter != node->flush_counter) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Cannot close file %s, because"
				" mod_count %ld != fl_count %ld",
				node->name,
				(long) node->modification_counter,
				(long) node->flush_counter);

		}

		if (node->being_extended) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Cannot close file %s, because it is being"
				" extended", node->name);
		}
	}

	return(false);
}

/*******************************************************************//**
Reserves the fil_system mutex and tries to make sure we can open at least one
file while holding it. This should be called before calling
fil_node_prepare_for_io(), because that function may need to open a file. */
static
void
fil_mutex_enter_and_prepare_for_io(
/*===============================*/
	ulint	space_id)	/*!< in: space id */
{
	fil_space_t*	space;
	bool		success;
	bool		print_info	= false;
	ulint		count		= 0;
	ulint		count2		= 0;

retry:
	mutex_enter(&fil_system->mutex);

	if (space_id == 0 || space_id >= SRV_LOG_SPACE_FIRST_ID) {
		/* We keep log files and system tablespace files always open;
		this is important in preventing deadlocks in this module, as
		a page read completion often performs another read from the
		insert buffer. The insert buffer is in tablespace 0, and we
		cannot end up waiting in this function. */

		return;
	}

	space = fil_space_get_by_id(space_id);

	if (space != NULL && space->stop_ios) {
		/* We are going to do a rename file and want to stop new i/o's
		for a while */

		if (count2 > 20000) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Tablespace %s has i/o ops stopped for a long"
				" time %lu", space->name, (ulong) count2);
		}

		mutex_exit(&fil_system->mutex);

#ifndef UNIV_HOTBACKUP

		/* Wake the i/o-handler threads to make sure pending
		i/o's are performed */
		os_aio_simulated_wake_handler_threads();

		/* The sleep here is just to give IO helper threads a
		bit of time to do some work. It is not required that
		all IO related to the tablespace being renamed must
		be flushed here as we do fil_flush() in
		fil_rename_tablespace() as well. */
		os_thread_sleep(20000);

#endif /* UNIV_HOTBACKUP */

		/* Flush tablespaces so that we can close modified
		files in the LRU list */
		fil_flush_file_spaces(FIL_TYPE_TABLESPACE);

		os_thread_sleep(20000);

		count2++;

		goto retry;
	}

	if (fil_system->n_open < fil_system->max_n_open) {

		return;
	}

	/* If the file is already open, no need to do anything; if the space
	does not exist, we handle the situation in the function which called
	this function */

	if (space == NULL || UT_LIST_GET_FIRST(space->chain)->is_open) {

		return;
	}

	if (count > 1) {
		print_info = true;
	}

	/* Too many files are open, try to close some */
close_more:
	success = fil_try_to_close_file_in_LRU(print_info);

	if (success && fil_system->n_open >= fil_system->max_n_open) {

		goto close_more;
	}

	if (fil_system->n_open < fil_system->max_n_open) {
		/* Ok */

		return;
	}

	if (count >= 2) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Too many (%lu) files stay open while the maximum"
			" allowed value would be %lu. You may need to raise"
			" the value of innodb_open_files in my.cnf.",
			(ulong) fil_system->n_open,
			(ulong) fil_system->max_n_open);

		return;
	}

	mutex_exit(&fil_system->mutex);

#ifndef UNIV_HOTBACKUP
	/* Wake the i/o-handler threads to make sure pending i/o's are
	performed */
	os_aio_simulated_wake_handler_threads();

	os_thread_sleep(20000);
#endif
	/* Flush tablespaces so that we can close modified files in the LRU
	list */

	fil_flush_file_spaces(FIL_TYPE_TABLESPACE);

	count++;

	goto retry;
}

/*******************************************************************//**
Frees a file node object from a tablespace memory cache. */
static
void
fil_node_free(
/*==========*/
	fil_node_t*	node,	/*!< in, own: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space)	/*!< in: space where the file node is chained */
{
	ut_ad(node && system && space);
	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->magic_n == FIL_NODE_MAGIC_N);
	ut_a(node->n_pending == 0);
	ut_a(!node->being_extended);

	if (node->is_open) {
		/* We fool the assertion in fil_node_close_file() to think
		there are no unflushed modifications in the file */

		node->modification_counter = node->flush_counter;
		os_event_set(node->sync_event);

		if (fil_buffering_disabled(space)) {

			ut_ad(!space->is_in_unflushed_spaces);
			ut_ad(fil_space_is_flushed(space));

		} else if (space->is_in_unflushed_spaces
			   && fil_space_is_flushed(space)) {

			space->is_in_unflushed_spaces = false;

			UT_LIST_REMOVE(system->unflushed_spaces, space);
		}

		fil_node_close_file(node, system);
	}

	space->size -= node->size;

	UT_LIST_REMOVE(space->chain, node);

	os_event_destroy(node->sync_event);
	::ut_free(node->name);
	::ut_free(node);
}

/** Creates a space memory object and puts it to the 'fil system' hash table.
If there is an error, prints an error message to the .err log.
@param[in]	space	name
@param[in]	id	space id
@param[in]	flags	space flags
@param[in]	purpose	space purpose
@retval pointer to the tablespace
@retval NULL on failure */

fil_space_t*
fil_space_create(
	const char*	name,
	ulint		id,
	ulint		flags,
	fil_type_t	purpose)
{
	fil_space_t*	space;

	DBUG_EXECUTE_IF("fil_space_create_failure", return(NULL););

	ut_a(fil_system);
	ut_a(fsp_flags_is_valid(flags));

	mutex_enter(&fil_system->mutex);

	/* Look for a matching tablespace and if found free it. */
	while ((space = fil_space_get_by_name(name)) != NULL) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Tablespace '%s' exists in the cache"
			" with id %lu != %lu",
			name, (ulong) space->id, (ulong) id);

		if (is_system_tablespace(id) || purpose == FIL_TYPE_LOG) {
			mutex_exit(&fil_system->mutex);
			return(NULL);
		}

		ib_logf(IB_LOG_LEVEL_WARN,
			"Freeing existing tablespace '%s' entry"
			" from the cache with id %lu",
			name, (ulong) id);

		bool	success = fil_space_free(space->id, false);
		ut_a(success);
	}

	space = fil_space_get_by_id(id);

	if (space != NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to add tablespace '%s' with id %lu"
			" to the tablespace memory cache, but tablespace '%s'"
			" with id %lu already exists in the cache!",
			name, (ulong) id, space->name, (ulong) space->id);

		mutex_exit(&fil_system->mutex);
		return(NULL);
	}

	space = static_cast<fil_space_t*>(ut_zalloc(sizeof(*space)));

	space->id = id;
	space->name = mem_strdup(name);

	UT_LIST_INIT(space->chain, &fil_node_t::chain);

	fil_system->tablespace_version++;
	space->tablespace_version = fil_system->tablespace_version;

	if ((purpose == FIL_TYPE_TABLESPACE || purpose == FIL_TYPE_TEMPORARY)
	    && !recv_recovery_on
	    && id > fil_system->max_assigned_id) {

		if (!fil_system->space_id_reuse_warned) {
			fil_system->space_id_reuse_warned = true;

			ib_logf(IB_LOG_LEVEL_WARN,
				"Allocated tablespace %lu, old maximum was %lu",
				(ulong) id,
				(ulong) fil_system->max_assigned_id);
		}

		fil_system->max_assigned_id = id;
	}

	space->purpose = purpose;
	space->flags = flags;

	space->magic_n = FIL_SPACE_MAGIC_N;

	rw_lock_create(fil_space_latch_key, &space->latch, SYNC_FSP);

	HASH_INSERT(fil_space_t, hash, fil_system->spaces, id, space);

	HASH_INSERT(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(name), space);

	UT_LIST_ADD_LAST(fil_system->space_list, space);

	if (id < SRV_LOG_SPACE_FIRST_ID && id > fil_system->max_assigned_id) {

		fil_system->max_assigned_id = id;
	}

	mutex_exit(&fil_system->mutex);

	return(space);
}

/*******************************************************************//**
Assigns a new space id for a new single-table tablespace. This works simply by
incrementing the global counter. If 4 billion id's is not enough, we may need
to recycle id's.
@return true if assigned, false if not */

bool
fil_assign_new_space_id(
/*====================*/
	ulint*	space_id)	/*!< in/out: space id */
{
	ulint	id;
	bool	success;

	mutex_enter(&fil_system->mutex);

	id = *space_id;

	if (id < fil_system->max_assigned_id) {
		id = fil_system->max_assigned_id;
	}

	id++;

	if (id > (SRV_LOG_SPACE_FIRST_ID / 2) && (id % 1000000UL == 0)) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"You are running out of new single-table tablespace"
			" id's. Current counter is %lu and it must not exceed"
			" %lu! To reset the counter to zero you have to dump"
			" all your tables and recreate the whole InnoDB"
			" installation.",
			(ulong) id,
			(ulong) SRV_LOG_SPACE_FIRST_ID);
	}

	success = (id < SRV_LOG_SPACE_FIRST_ID);

	if (success) {
		*space_id = fil_system->max_assigned_id = id;
	} else {
		ib_logf(IB_LOG_LEVEL_WARN,
			"You have run out of single-table tablespace id's!"
			" Current counter is %lu. To reset the counter to zero"
			" you have to dump all your tables and"
			" recreate the whole InnoDB installation.",
			(ulong) id);
		*space_id = ULINT_UNDEFINED;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/*******************************************************************//**
Frees a space object from the tablespace memory cache. Closes the files in
the chain but does not delete them. There must not be any pending i/o's or
flushes on the files.
@return true if success */
static
bool
fil_space_free(
/*===========*/
	ulint		id,		/* in: space id */
	bool		x_latched)	/* in: true if caller has space->latch
					in X mode */
{
	ut_ad(mutex_own(&fil_system->mutex));

	fil_space_t*	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to remove tablespace %lu"
			" from the cache but it is not there.",
			(ulong) id);

		return(false);
	}

	HASH_DELETE(fil_space_t, hash, fil_system->spaces, id, space);

	fil_space_t*	fnamespace = fil_space_get_by_name(space->name);

	ut_a(space == fnamespace);

	HASH_DELETE(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(space->name), space);

	if (space->is_in_unflushed_spaces) {

		ut_ad(!fil_buffering_disabled(space));
		space->is_in_unflushed_spaces = false;

		UT_LIST_REMOVE(fil_system->unflushed_spaces, space);
	}

	UT_LIST_REMOVE(fil_system->space_list, space);

	ut_a(space->magic_n == FIL_SPACE_MAGIC_N);
	ut_a(0 == space->n_pending_flushes);

	for (fil_node_t* fil_node = UT_LIST_GET_FIRST(space->chain);
	     fil_node != NULL;
	     fil_node = UT_LIST_GET_FIRST(space->chain)) {

		fil_node_free(fil_node, fil_system, space);
	}

	ut_a(0 == UT_LIST_GET_LEN(space->chain));

	if (x_latched) {
		rw_lock_x_unlock(&space->latch);
	}

	rw_lock_free(&(space->latch));

	::ut_free(space->name);
	::ut_free(space);

	return(true);
}

/*******************************************************************//**
Returns a pointer to the fil_space_t that is in the memory cache
associated with a space id. The caller must lock fil_system->mutex.
@return file_space_t pointer, NULL if space not found */
UNIV_INLINE
fil_space_t*
fil_space_get_space(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	fil_node_t*	node;

	ut_ad(fil_system);

	space = fil_space_get_by_id(id);
	if (space == NULL || space->size != 0) {
		return(space);
	}

	switch (space->purpose) {
	case FIL_TYPE_LOG:
		break;
	case FIL_TYPE_TEMPORARY:
	case FIL_TYPE_TABLESPACE:
		ut_a(id != 0);

		mutex_exit(&fil_system->mutex);

		/* It is possible that the space gets evicted at this point
		before the fil_mutex_enter_and_prepare_for_io() acquires
		the fil_system->mutex. Check for this after completing the
		call to fil_mutex_enter_and_prepare_for_io(). */
		fil_mutex_enter_and_prepare_for_io(id);

		/* We are still holding the fil_system->mutex. Check if
		the space is still in memory cache. */
		space = fil_space_get_by_id(id);
		if (space == NULL) {
			return(NULL);
		}

		/* The following code must change when InnoDB supports
		multiple datafiles per tablespace. */
		ut_a(1 == UT_LIST_GET_LEN(space->chain));

		node = UT_LIST_GET_FIRST(space->chain);

		/* It must be a single-table tablespace and we have not opened
		the file yet; the following calls will open it and update the
		size fields */

		if (!fil_node_prepare_for_io(node, fil_system, space)) {
			/* The single-table tablespace can't be opened,
			because the ibd file is missing. */
			return(NULL);
		}
		fil_node_complete_io(node, fil_system, OS_FILE_READ);
	}

	return(space);
}

/*******************************************************************//**
Returns the path from the first fil_node_t found for the space ID sent.
The caller is responsible for freeing the memory allocated here for the
value returned.
@return own: A copy of fil_node_t::path, NULL if space ID is zero
or not found. */

char*
fil_space_get_first_path(
/*=====================*/
	ulint		id)	/*!< in: space id */
{
	fil_space_t*	space;
	fil_node_t*	node;
	char*		path;

	ut_ad(fil_system);
	ut_a(id);

	fil_mutex_enter_and_prepare_for_io(id);

	space = fil_space_get_space(id);

	if (space == NULL) {
		mutex_exit(&fil_system->mutex);

		return(NULL);
	}

	ut_ad(mutex_own(&fil_system->mutex));

	node = UT_LIST_GET_FIRST(space->chain);

	path = mem_strdup(node->name);

	mutex_exit(&fil_system->mutex);

	return(path);
}

/*******************************************************************//**
Returns the size of the space in pages. The tablespace must be cached in the
memory cache.
@return space size, 0 if space not found */

ulint
fil_space_get_size(
/*===============*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint		size;

	ut_ad(fil_system);
	mutex_enter(&fil_system->mutex);

	space = fil_space_get_space(id);

	size = space ? space->size : 0;

	mutex_exit(&fil_system->mutex);

	return(size);
}

/*******************************************************************//**
Returns the flags of the space. The tablespace must be cached
in the memory cache.
@return flags, ULINT_UNDEFINED if space not found */

ulint
fil_space_get_flags(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;
	ulint		flags;

	ut_ad(fil_system);

	if (!id) {
		return(0);
	}

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_space(id);

	if (space == NULL) {
		mutex_exit(&fil_system->mutex);

		return(ULINT_UNDEFINED);
	}

	flags = space->flags;

	mutex_exit(&fil_system->mutex);

	return(flags);
}

/** Returns the page size of the space and whether it is compressed or not.
The tablespace must be cached in the memory cache.
@param[in]	id	space id
@param[out]	found	true if tablespace was found
@return page size */
const page_size_t
fil_space_get_page_size(
	ulint	id,
	bool*	found)
{
	const ulint	flags = fil_space_get_flags(id);

	if (flags == ULINT_UNDEFINED) {
		*found = false;
		return(page_size_t(0, 0, false));
	}

	*found = true;

	if (id == 0) {
		/* fil_space_get_flags() always returns flags=0 for space=0 */
		ut_ad(flags == 0);
		return(univ_page_size);
	}

	return(page_size_t(flags));
}

/*******************************************************************//**
Checks if the pair space, page_no refers to an existing page in a tablespace
file space. The tablespace must be cached in the memory cache.
@return true if the address is meaningful */

bool
fil_check_adress_in_tablespace(
/*===========================*/
	ulint	id,	/*!< in: space id */
	ulint	page_no)/*!< in: page number */
{
	return(fil_space_get_size(id) > page_no);
}

/****************************************************************//**
Initializes the tablespace memory cache. */

void
fil_init(
/*=====*/
	ulint	hash_size,	/*!< in: hash table size */
	ulint	max_n_open)	/*!< in: max number of open files */
{
	ut_a(fil_system == NULL);

	ut_a(hash_size > 0);
	ut_a(max_n_open > 0);

	fil_system = static_cast<fil_system_t*>(
		ut_zalloc(sizeof(*fil_system)));

	mutex_create("fil_system", &fil_system->mutex);

	fil_system->spaces = hash_create(hash_size);
	fil_system->name_hash = hash_create(hash_size);

	UT_LIST_INIT(fil_system->LRU, &fil_node_t::LRU);
	UT_LIST_INIT(fil_system->space_list, &fil_space_t::space_list);

	UT_LIST_INIT(fil_system->unflushed_spaces,
		     &fil_space_t::unflushed_spaces);

	fil_system->max_n_open = max_n_open;
}

/*******************************************************************//**
Opens all log files and system tablespace data files. They stay open until the
database server shutdown. This should be called at a server startup after the
space objects for the log and the system tablespace have been created. The
purpose of this operation is to make sure we never run out of file descriptors
if we need to read from the insert buffer or to write to the log. */

void
fil_open_log_and_system_tablespace_files(void)
/*==========================================*/
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	for (space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {

		fil_node_t*	node;

		if (fil_space_belongs_in_lru(space)) {

			continue;
		}

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (!node->is_open) {
				if (!fil_node_open_file(node, fil_system,
							space)) {
					/* This func is called during server's
					startup. If some file of log or system
					tablespace is missing, the server
					can't start successfully. So we should
					assert for it. */
					ut_a(0);
				}
			}

			if (fil_system->max_n_open < 10 + fil_system->n_open) {

				ib_logf(IB_LOG_LEVEL_WARN,
					"You must raise the value of"
					" innodb_open_files in my.cnf!"
					" Remember that InnoDB keeps all"
					" log files and all system"
					" tablespace files open"
					" for the whole time mysqld is"
					" running, and needs to open also"
					" some .ibd files if the"
					" file-per-table storage model is used."
					" Current open files %lu, max allowed"
					" open files %lu.",
					(ulong) fil_system->n_open,
					(ulong) fil_system->max_n_open);
			}
		}
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Closes all open files. There must not be any pending i/o's or not flushed
modifications in the files. */

void
fil_close_all_files(void)
/*=====================*/
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = UT_LIST_GET_FIRST(fil_system->space_list);

	while (space != NULL) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (node->is_open) {
				fil_node_close_file(node, fil_system);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);

		fil_space_free(prev_space->id, false);
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Closes the redo log files. There must not be any pending i/o's or not
flushed modifications in the files. */

void
fil_close_log_files(
/*================*/
	bool	free)	/*!< in: whether to free the memory object */
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = UT_LIST_GET_FIRST(fil_system->space_list);

	while (space != NULL) {
		fil_node_t*	node;
		fil_space_t*	prev_space = space;

		if (space->purpose != FIL_TYPE_LOG) {
			space = UT_LIST_GET_NEXT(space_list, space);
			continue;
		}

		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {

			if (node->is_open) {
				fil_node_close_file(node, fil_system);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);

		if (free) {
			fil_space_free(prev_space->id, false);
		}
	}

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Sets the max tablespace id counter if the given number is bigger than the
previous value. */

void
fil_set_max_space_id_if_bigger(
/*===========================*/
	ulint	max_id)	/*!< in: maximum known id */
{
	if (max_id >= SRV_LOG_SPACE_FIRST_ID) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Max tablespace id is too high, %lu",
			(ulong) max_id);
	}

	mutex_enter(&fil_system->mutex);

	if (fil_system->max_assigned_id < max_id) {

		fil_system->max_assigned_id = max_id;
	}

	mutex_exit(&fil_system->mutex);
}

/****************************************************************//**
Writes the flushed lsn and the latest archived log number to the page header
of the first page of a data file of the system tablespace (space 0),
which is uncompressed. */
static __attribute__((warn_unused_result))
dberr_t
fil_write_lsn_and_arch_no_to_file(
/*==============================*/
	ulint	space,		/*!< in: space to write to */
	ulint	sum_of_sizes,	/*!< in: combined size of previous files
				in space, in database pages */
	lsn_t	lsn,		/*!< in: lsn to write */
	ulint	arch_log_no __attribute__((unused)))
				/*!< in: archived log number to write */
{
	byte*	buf1;
	byte*	buf;
	dberr_t	err;

	buf1 = static_cast<byte*>(ut_malloc(2 * UNIV_PAGE_SIZE));
	buf = static_cast<byte*>(ut_align(buf1, UNIV_PAGE_SIZE));

	const page_id_t	page_id(space, sum_of_sizes);

	err = fil_read(page_id, univ_page_size, 0, univ_page_size.physical(),
		       buf);

	if (err == DB_SUCCESS) {
		mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN, lsn);

		err = fil_write(page_id, univ_page_size, 0,
				univ_page_size.physical(), buf);
	}

	::ut_free(buf1);

	return(err);
}

/****************************************************************//**
Writes the flushed lsn and the latest archived log number to the page
header of the first page of each data file in the system tablespace.
@return DB_SUCCESS or error number */

dberr_t
fil_write_flushed_lsn_to_data_files(
/*================================*/
	lsn_t	lsn,		/*!< in: lsn to write */
	ulint	arch_log_no)	/*!< in: latest archived log file number */
{
	mutex_enter(&fil_system->mutex);

	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {

		/* We only write the lsn to all existing data files which have
		been open during the lifetime of the mysqld process; they are
		represented by the space objects in the tablespace memory
		cache. Note that all data files in the system tablespace 0
		and the UNDO log tablespaces (if separate) are always open. */

		if (space->purpose == FIL_TYPE_TABLESPACE
		    && !fil_is_user_tablespace_id(space->id)) {
			ulint	sum_of_sizes = 0;

			for (fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
			     node != NULL;
			     node = UT_LIST_GET_NEXT(chain, node)) {

				dberr_t		err;

				mutex_exit(&fil_system->mutex);

				err = fil_write_lsn_and_arch_no_to_file(
					space->id, sum_of_sizes, lsn,
					arch_log_no);

				if (err != DB_SUCCESS) {

					return(err);
				}

				mutex_enter(&fil_system->mutex);

				sum_of_sizes += node->size;
			}
		}
	}

	mutex_exit(&fil_system->mutex);

	return(DB_SUCCESS);
}

/*================ SINGLE-TABLE TABLESPACES ==========================*/

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Increments the count of pending operation, if space is not being deleted
or truncated.
@return	true if being deleted/truncated, and operations should be skipped */

bool
fil_inc_pending_ops(
/*================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to do an operation on a dropped tablespace."
			" Name: %s,  Space ID: %lu",
			space->name, ulong(id));
	}

	bool skip_inc_pending_ops = (space == NULL
				     || space->stop_new_ops
				     || space->is_being_truncated);
	if (!skip_inc_pending_ops) {
		space->n_pending_ops++;
	}

	mutex_exit(&fil_system->mutex);

	return(skip_inc_pending_ops);
}

/*******************************************************************//**
Decrements the count of pending operations. */

void
fil_decr_pending_ops(
/*=================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	if (space == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Decrementing pending operation"
			" of a dropped tablespace %lu",
			(ulong) id);
	}

	if (space != NULL) {
		space->n_pending_ops--;
	}

	mutex_exit(&fil_system->mutex);
}
#endif /* !UNIV_HOTBACKUP */

/********************************************************//**
Creates the database directory for a table if it does not exist yet. */

void
fil_create_directory_for_tablename(
/*===============================*/
	const char*	name)	/*!< in: name in the standard
				'databasename/tablename' format */
{
	const char*	namend;
	char*		path;
	ulint		len;

	len = ::strlen(fil_path_to_mysql_datadir);
	namend = strchr(name, '/');
	ut_a(namend);
	path = static_cast<char*>(ut_malloc(len + (namend - name) + 2));

	memcpy(path, fil_path_to_mysql_datadir, len);
	path[len] = '/';
	memcpy(path + len + 1, name, namend - name);
	path[len + (namend - name) + 1] = 0;

	os_normalize_path_for_win(path);

	bool	success = os_file_create_directory(path, false);
	ut_a(success);

	::ut_free(path);
}

#ifndef UNIV_HOTBACKUP
/********************************************************//**
Writes a log record about an .ibd file
create/rename/delete/truncate. */
static
void
fil_op_write_log(
/*=============*/
	mlog_id_t	type,		/*!< in: MLOG_FILE_CREATE,
					MLOG_FILE_CREATE2,
					MLOG_FILE_DELETE, or
					MLOG_FILE_RENAME */
	ulint		space_id,	/*!< in: space id */
	ulint		log_flags,	/*!< in: redo log flags (stored
					in the page number field) */
	ulint		flags,		/*!< in: compressed page size
					and file format
					if type==MLOG_FILE_CREATE2, or 0 */
	const char*	name,		/*!< in: table name in the familiar
					'databasename/tablename' format, or
					the file path in the case of
					MLOG_FILE_DELETE */
	const char*	new_name,	/*!< in: if type is MLOG_FILE_RENAME,
					the new table name in the
					'databasename/tablename' format */
	mtr_t*		mtr)		/*!< in/out: mini-transaction handle */
{
	byte*		log_ptr;
	ulint		len;

	log_ptr = mlog_open(mtr, 11 + 2 + 1);

	if (log_ptr == NULL) {
		/* Logging in mtr is switched off during crash recovery:
		in that case mlog_open returns NULL */
		return;
	}

	log_ptr = mlog_write_initial_log_record_for_file_op(
		type, space_id, log_flags, log_ptr, mtr);

	if (type == MLOG_FILE_CREATE2) {
		mach_write_to_4(log_ptr, flags);
		log_ptr += 4;
	}
	/* Let us store the strings as null-terminated for easier readability
	and handling */

	len = ::strlen(name) + 1;

	mach_write_to_2(log_ptr, len);
	log_ptr += 2;
	mlog_close(mtr, log_ptr);

	mlog_catenate_string(mtr, (byte*) name, len);

	if (type == MLOG_FILE_RENAME) {
		len = ::strlen(new_name) + 1;
		log_ptr = mlog_open(mtr, 2 + len);
		ut_a(log_ptr);
		mach_write_to_2(log_ptr, len);
		log_ptr += 2;
		mlog_close(mtr, log_ptr);

		mlog_catenate_string(mtr, (byte*) new_name, len);
	}
}
#endif

/********************************************************//**
Recreates table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */

dberr_t
fil_recreate_table(
/*===============*/
	ulint		space_id,	/*!< in: space id */
	ulint		format_flags,	/*!< in: page format */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	name,		/*!< in: table name */
	truncate_t&	truncate)	/*!< in: The information of
					TRUNCATE log record */
{
	dberr_t			err = DB_SUCCESS;
	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(space_id,
								  &found));

	if (!found) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Missing .ibd file for table '%s' with tablespace %lu",
			name, space_id);
		return(DB_ERROR);
	}

	ut_ad(!truncate_t::s_fix_up_active);
	truncate_t::s_fix_up_active = true;

	/* Step-1: Scan for active indexes from REDO logs and drop
	all the indexes using low level function that take root_page_no
	and space-id. */
	truncate.drop_indexes(space_id);

	/* Step-2: Scan for active indexes and re-create them. */
	err = truncate.create_indexes(
		name, space_id, page_size, flags, format_flags);
	if (err != DB_SUCCESS) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Failed to create indexes for the table '%s' with"
			" tablespace %lu while fixing up truncate action",
			name, space_id);
		return(err);
	}

	truncate_t::s_fix_up_active = false;

	return(err);
}

/********************************************************//**
Recreates the tablespace and table indexes by applying
TRUNCATE log record during recovery.
@return DB_SUCCESS or error code */

dberr_t
fil_recreate_tablespace(
/*====================*/
	ulint		space_id,	/*!< in: space id */
	ulint		format_flags,	/*!< in: page format */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	name,		/*!< in: table name */
	truncate_t&	truncate,	/*!< in: The information of
					TRUNCATE log record */
	lsn_t		recv_lsn)	/*!< in: the end LSN of
						the log record */
{
	dberr_t		err = DB_SUCCESS;
	mtr_t		mtr;

	ut_ad(!truncate_t::s_fix_up_active);
	truncate_t::s_fix_up_active = true;

	/* Step-1: Invalidate buffer pool pages belonging to the tablespace
	to re-create. */
	buf_LRU_flush_or_remove_pages(space_id, BUF_REMOVE_ALL_NO_WRITE, 0);

	/* Remove all insert buffer entries for the tablespace */
	ibuf_delete_for_discarded_space(space_id);

	/* Step-2: truncate tablespace (reset the size back to original or
	default size) of tablespace. */
	err = truncate.truncate(
		space_id, truncate.get_dir_path(), name, flags, true);

	if (err != DB_SUCCESS) {

		ib_logf(IB_LOG_LEVEL_INFO,
			"Cannot access .ibd file for table '%s' with"
			" tablespace %lu while truncating", name, space_id);
		return(DB_ERROR);
	}

	bool			found;
	const page_size_t&	page_size =
		fil_space_get_page_size(space_id, &found);

	if (!found) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Missing .ibd file for table '%s' with tablespace %lu",
			name, space_id);
		return(DB_ERROR);
	}

	/* Step-3: Initialize Header. */
	if (page_size.is_compressed()) {
		byte*	buf;
		page_t*	page;

		buf = static_cast<byte*>(ut_zalloc(3 * UNIV_PAGE_SIZE));

		/* Align the memory for file i/o */
		page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

		flags = fsp_flags_set_page_size(flags, UNIV_PAGE_SIZE);

		fsp_header_init_fields(page, space_id, flags);

		mach_write_to_4(
			page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

		page_zip_des_t  page_zip;
		page_zip_set_size(&page_zip, page_size.physical());
		page_zip.data = page + UNIV_PAGE_SIZE;

#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
		page_zip.m_end = page_zip.m_nonempty = page_zip.n_blobs = 0;
		buf_flush_init_for_writing(page, &page_zip, 0);

		err = fil_write(page_id_t(space_id, 0), page_size, 0,
				page_size.physical(), page_zip.data);

		::ut_free(buf);

		if (err != DB_SUCCESS) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Failed to clean header of the table '%s' with"
				" tablespace %lu", name, space_id);
			return(err);
		}
	}

	mtr_start(&mtr);
	/* Don't log the operation while fixing up table truncate operation
	as crash at this level can still be sustained with recovery restarting
	from last checkpoint. */
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	/* Initialize the first extent descriptor page and
	the second bitmap page for the new tablespace. */
	fsp_header_init(space_id, FIL_IBD_FILE_INITIAL_SIZE, &mtr);
	mtr_commit(&mtr);

	/* Step-4: Re-Create Indexes to newly re-created tablespace.
	This operation will restore tablespace back to what it was
	when it was created during CREATE TABLE. */
	err = truncate.create_indexes(
		name, space_id, page_size, flags, format_flags);
	if (err != DB_SUCCESS) {
		return(err);
	}

	/* Step-5: Write new created pages into ibd file handle and
	flush it to disk for the tablespace, in case i/o-handler thread
	deletes the bitmap page from buffer. */
	mtr_start(&mtr);

	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	mutex_exit(&fil_system->mutex);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	for (ulint page_no = 0; page_no < node->size; ++page_no) {

		const page_id_t	cur_page_id(space_id, page_no);

		buf_block_t*	block = buf_page_get(cur_page_id, page_size,
						     RW_X_LATCH, &mtr);

		byte*	page = buf_block_get_frame(block);

		if (!fsp_flags_is_compressed(flags)) {

			ut_ad(!page_size.is_compressed());

			buf_flush_init_for_writing(page, NULL, recv_lsn);

			err = fil_write(cur_page_id, page_size, 0,
					page_size.physical(), page);
		} else {
			ut_ad(page_size.is_compressed());

			/* We don't want to rewrite empty pages. */

			if (fil_page_get_type(page) != 0) {
				page_zip_des_t*  page_zip =
					buf_block_get_page_zip(block);

				buf_flush_init_for_writing(
					page, page_zip, recv_lsn);

				err = fil_write(cur_page_id, page_size, 0,
						page_size.physical(),
						page_zip->data);
			} else {
#ifdef UNIV_DEBUG
				const byte*	data = block->page.zip.data;

				/* Make sure that the page is really empty */
				for (ulint i = 0;
				     i < page_size.physical();
				     ++i) {

					ut_a(data[i] == 0);
				}
#endif /* UNIV_DEBUG */
			}
		}

		if (err != DB_SUCCESS) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Cannot write page %lu into a .ibd file for"
				" table '%s' with tablespace %lu",
				page_no, name, space_id);
		}
	}

	mtr_commit(&mtr);

	truncate_t::s_fix_up_active = false;

	return(err);
}

/*******************************************************************//**
Parses the body of a log record written about an .ibd file operation. That is,
the log record part after the standard (type, space id, page no) header of the
log record.

If desired, also replays the delete or rename operation if the .ibd file
exists and the space id in it matches. Replays the create operation if a file
at that path does not exist yet. If the database directory for the file to be
created does not exist, then we create the directory, too.

Note that ibbackup --apply-log sets fil_path_to_mysql_datadir to point to the
datadir that we should use in replaying the file operations.

InnoDB recovery does not replay these fully since it always sets the space id
to zero. But ibbackup does replay them.  TODO: If remote tablespaces are used,
ibbackup will only create tables in the default directory since MLOG_FILE_CREATE
and MLOG_FILE_CREATE2 only know the tablename, not the path.

@return end of log record, or NULL if the record was not completely
contained between ptr and end_ptr */

byte*
fil_op_log_parse_or_replay(
/*=======================*/
	byte*		ptr,		/*!< in/out: buffer containing the log
					record body, or an initial segment
					of it, if the record does not fire
					completely between ptr and end_ptr */
	const byte*	end_ptr,	/*!< in: buffer end */
	mlog_id_t	type,		/*!< in: the type of this log record */
	ulint		space_id,	/*!< in: the space id of the tablespace
					in question */
	ulint		log_flags,	/*!< in: redo log flags
					(stored in the page number parameter) */
	bool		parse_only)	/*!< in: if true, parse the
					log record don't replay it. */
{
	const char*	name;
	ulint		name_len;
	ulint		tablespace_flags = 0;
	ulint		new_name_len;
	const char*	new_name = NULL;

	/* Step-1: Parse the log records. */

	/* Step-1a: Parse flags and name of table.
	Other fields (type, space-id, page-no) are parsed before
	invocation of this function */
	if (type == MLOG_FILE_CREATE2) {
		if (end_ptr < ptr + 4) {

			return(NULL);
		}

		tablespace_flags = mach_read_from_4(ptr);
		ptr += 4;
	}

	if (end_ptr < ptr + 2) {
		return(NULL);
	}

	name_len = mach_read_from_2(ptr);
	ptr += 2;
	if (end_ptr < ptr + name_len) {
		return(NULL);
	}

	name = (const char*) ptr;
	ptr += name_len;
	ut_ad(::strlen(name) == name_len - 1);

	/* Step-1b: Parse remaining field in type specific form. */
	if (type == MLOG_FILE_RENAME) {

		if (end_ptr < ptr + 2) {
			return(NULL);
		}

		new_name_len = mach_read_from_2(ptr);
		ptr += 2;

		if (end_ptr < ptr + new_name_len) {
			return(NULL);
		}

		new_name = (const char*) ptr;
		ptr += new_name_len;
		ut_ad(::strlen(new_name) == new_name_len - 1);
	}

	/* Condition to check if replay of log record is demanded by caller. */
	if (parse_only) {
		return(ptr);
	}

	/* Step-2: Replay log records. */

	/* MLOG_FILE_XXXX ops are not carried out on system tablespaces. */
	ut_ad(!is_system_tablespace(space_id));

	/* Let us try to perform the file operation, if sensible. Note that
	ibbackup has at this stage already read in all space id info to the
	fil0fil.cc data structures.

	NOTE that our algorithm is not guaranteed to work correctly if there
	were renames of tables during the backup. See ibbackup code for more
	on the problem. */

	switch (type) {
	case MLOG_FILE_DELETE:
		if (fil_tablespace_exists_in_mem(space_id)) {
			dberr_t	err = fil_delete_tablespace(
				space_id, BUF_REMOVE_FLUSH_NO_WRITE);
			ut_a(err == DB_SUCCESS);
		}

		break;

	case MLOG_FILE_RENAME:
		/* In order to replay the rename, the following must hold:
		* The new name is not already used.
		* A tablespace is open in memory with the old name.
		* The space ID for that tablepace matches this log entry.
		This will prevent unintended renames during recovery. */

		if (fil_get_space_id_for_table(new_name) == ULINT_UNDEFINED
		    && space_id == fil_get_space_id_for_table(name)) {
			/* Create the database directory for the new name, if
			it does not exist yet */
			fil_create_directory_for_tablename(new_name);

			if (!fil_rename_tablespace(name, space_id,
						   new_name, NULL)) {
				ut_error;
			}
		}

		break;

	case MLOG_FILE_CREATE:
	case MLOG_FILE_CREATE2:
		if (fil_tablespace_exists_in_mem(space_id)) {
			/* Do nothing */
		} else if (fil_get_space_id_for_table(name)
			   != ULINT_UNDEFINED) {
			/* Do nothing */
		} else if (log_flags & MLOG_FILE_FLAG_TEMP) {
			/* Temporary table, do nothing */
		} else {
			/* Create the database directory for name, if it does
			not exist yet */
			fil_create_directory_for_tablename(name);

			if (fil_create_new_single_table_tablespace(
				    space_id, name, NULL, tablespace_flags,
				    DICT_TF2_USE_FILE_PER_TABLE,
				    FIL_IBD_FILE_INITIAL_SIZE) != DB_SUCCESS) {
				ut_error;
			}
		}

		break;

	default:
		ut_error;
	}

	return(ptr);
}

/** File operations for tablespace */
enum fil_operation_t {
	FIL_OPERATION_DELETE,	/*!< delete a single-table tablespace */
	FIL_OPERATION_CLOSE,	/*!< close a single-table tablespace */
	FIL_OPERATION_TRUNCATE	/*!< truncate a single-table tablespace */
};

/*******************************************************************//**
Check for change buffer merges.
@return 0 if no merges else count + 1. */
static
ulint
fil_ibuf_check_pending_ops(
/*=======================*/
	fil_space_t*	space,	/*!< in/out: Tablespace to check */
	ulint		count)	/*!< in: number of attempts so far */
{
	ut_ad(mutex_own(&fil_system->mutex));

	if (space != 0 && space->n_pending_ops != 0) {

		if (count > 5000) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Trying to close/delete/truncate tablespace"
				" '%s' but there are %lu pending change"
				" buffer merges on it.",
				space->name,
				(ulong) space->n_pending_ops);
		}

		return(count + 1);
	}

	return(0);
}

/*******************************************************************//**
Check for pending IO.
@return 0 if no pending else count + 1. */
static
ulint
fil_check_pending_io(
/*=================*/
	fil_operation_t	operation,	/*!< in: File operation */
	fil_space_t*	space,		/*!< in/out: Tablespace to check */
	fil_node_t**	node,		/*!< out: Node in space list */
	ulint		count)		/*!< in: number of attempts so far */
{
	ut_ad(mutex_own(&fil_system->mutex));
	ut_a(space->n_pending_ops == 0);

	switch (operation) {
	case FIL_OPERATION_DELETE:
	case FIL_OPERATION_CLOSE:
		break;
	case FIL_OPERATION_TRUNCATE:
		space->is_being_truncated = true;
		break;
	}

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	*node = UT_LIST_GET_FIRST(space->chain);

	if (space->n_pending_flushes > 0 || (*node)->n_pending > 0) {

		ut_a(!(*node)->being_extended);

		if (count > 1000) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Trying to delete/close/truncate tablespace"
				" '%s' but there are %lu flushes"
				" and %lu pending i/o's on it.",
				space->name,
				(ulong) space->n_pending_flushes,
				(ulong) (*node)->n_pending);
		}

		return(count + 1);
	}

	return(0);
}

/*******************************************************************//**
Check pending operations on a tablespace.
@return DB_SUCCESS or error failure. */
static
dberr_t
fil_check_pending_operations(
/*=========================*/
	ulint		id,		/*!< in: space id */
	fil_operation_t	operation,	/*!< in: File operation */
	fil_space_t**	space,		/*!< out: tablespace instance
					in memory */
	char**		path)		/*!< out/own: tablespace path */
{
	ulint		count = 0;

	ut_a(!is_system_tablespace(id));
	ut_ad(space);

	*space = 0;

	mutex_enter(&fil_system->mutex);
	fil_space_t* sp = fil_space_get_by_id(id);
	if (sp) {
		sp->stop_new_ops = true;
	}
	mutex_exit(&fil_system->mutex);

	/* Check for pending change buffer merges. */

	do {
		mutex_enter(&fil_system->mutex);

		sp = fil_space_get_by_id(id);

		count = fil_ibuf_check_pending_ops(sp, count);

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

	} while (count > 0);

	/* Check for pending IO. */

	*path = 0;

	do {
		mutex_enter(&fil_system->mutex);

		sp = fil_space_get_by_id(id);

		if (sp == NULL) {
			mutex_exit(&fil_system->mutex);
			return(DB_TABLESPACE_NOT_FOUND);
		}

		fil_node_t*	node;

		count = fil_check_pending_io(operation, sp, &node, count);

		if (count == 0) {
			*path = mem_strdup(node->name);
		}

		mutex_exit(&fil_system->mutex);

		if (count > 0) {
			os_thread_sleep(20000);
		}

	} while (count > 0);

	ut_ad(sp);

	*space = sp;
	return(DB_SUCCESS);
}

/*******************************************************************//**
Closes a single-table tablespace. The tablespace must be cached in the
memory cache. Free all pages used by the tablespace.
@return DB_SUCCESS or error */

dberr_t
fil_close_tablespace(
/*=================*/
	trx_t*		trx,	/*!< in/out: Transaction covering the close */
	ulint		id)	/*!< in: space id */
{
	char*		path = 0;
	fil_space_t*	space = 0;
	dberr_t		err;

	ut_a(!is_system_tablespace(id));

	err = fil_check_pending_operations(id, FIL_OPERATION_CLOSE,
					   &space, &path);

	if (err != DB_SUCCESS) {
		return(err);
	}

	ut_a(space);
	ut_a(path != 0);

	rw_lock_x_lock(&space->latch);

#ifndef UNIV_HOTBACKUP
	/* Invalidate in the buffer pool all pages belonging to the
	tablespace. Since we have set space->stop_new_ops = true, readahead
	or ibuf merge can no longer read more pages of this tablespace to the
	buffer pool. Thus we can clean the tablespace out of the buffer pool
	completely and permanently. The flag stop_new_ops also prevents
	fil_flush() from being applied to this tablespace. */

	buf_LRU_flush_or_remove_pages(id, BUF_REMOVE_FLUSH_WRITE, trx);
#endif /* UNIV_HOTBACKUP */

	mutex_enter(&fil_system->mutex);

	/* If the free is successful, the X lock will be released before
	the space memory data structure is freed. */

	if (!fil_space_free(id, true)) {
		rw_lock_x_unlock(&space->latch);
		err = DB_TABLESPACE_NOT_FOUND;
	} else {
		err = DB_SUCCESS;
	}

	mutex_exit(&fil_system->mutex);

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */

	char*	cfg_name = fil_make_filepath(path, NULL, CFG, false);
	if (cfg_name != NULL) {
		os_file_delete_if_exists(innodb_data_file_key, cfg_name, NULL);
		::ut_free(cfg_name);
	}

	::ut_free(path);

	return(err);
}

/*******************************************************************//**
Deletes a single-table tablespace. The tablespace must be cached in the
memory cache.
@return DB_SUCCESS or error */

dberr_t
fil_delete_tablespace(
/*==================*/
	ulint		id,		/*!< in: space id */
	buf_remove_t	buf_remove)	/*!< in: specify the action to take
					on the tables pages in the buffer
					pool */
{
	char*		path = 0;
	fil_space_t*	space = 0;

	ut_a(!is_system_tablespace(id));

	dberr_t err = fil_check_pending_operations(
		id, FIL_OPERATION_DELETE, &space, &path);

	if (err != DB_SUCCESS) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot delete tablespace %lu because it is not"
			" found in the tablespace memory cache.",
			(ulong) id);

		return(err);
	}

	ut_a(space);
	ut_a(path != 0);

	/* Important: We rely on the data dictionary mutex to ensure
	that a race is not possible here. It should serialize the tablespace
	drop/free. We acquire an X latch only to avoid a race condition
	when accessing the tablespace instance via:

	  fsp_get_available_space_in_free_extents().

	There our main motivation is to reduce the contention on the
	dictionary mutex. */

	rw_lock_x_lock(&space->latch);

#ifndef UNIV_HOTBACKUP
	/* IMPORTANT: Because we have set space::stop_new_ops there
	can't be any new ibuf merges, reads or flushes. We are here
	because node::n_pending was zero above. However, it is still
	possible to have pending read and write requests:

	A read request can happen because the reader thread has
	gone through the ::stop_new_ops check in buf_page_init_for_read()
	before the flag was set and has not yet incremented ::n_pending
	when we checked it above.

	A write request can be issued any time because we don't check
	the ::stop_new_ops flag when queueing a block for write.

	We deal with pending write requests in the following function
	where we'd minimally evict all dirty pages belonging to this
	space from the flush_list. Not that if a block is IO-fixed
	we'll wait for IO to complete.

	To deal with potential read requests by checking the
	::stop_new_ops flag in fil_io() */

	buf_LRU_flush_or_remove_pages(id, buf_remove, 0);

#endif /* !UNIV_HOTBACKUP */

	/* If it is a delete then also delete any generated files, otherwise
	when we drop the database the remove directory will fail. */
	{
		char*	cfg_name = fil_make_filepath(path, NULL, CFG, false);
		if (cfg_name != NULL) {
			os_file_delete_if_exists(innodb_data_file_key, cfg_name, NULL);
			::ut_free(cfg_name);
		}
	}

	/* Delete the link file pointing to the ibd file we are deleting. */
	if (FSP_FLAGS_HAS_DATA_DIR(space->flags)) {
		RemoteDatafile::delete_link_file(space->name);
	}

	mutex_enter(&fil_system->mutex);

	/* Double check the sanity of pending ops after reacquiring
	the fil_system::mutex. */
	if (fil_space_get_by_id(id)) {
		ut_a(space->n_pending_ops == 0);
		ut_a(UT_LIST_GET_LEN(space->chain) == 1);
		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		ut_a(node->n_pending == 0);
	}

	if (!fil_space_free(id, true)) {
		err = DB_TABLESPACE_NOT_FOUND;
	}

	mutex_exit(&fil_system->mutex);

	if (err != DB_SUCCESS) {
		rw_lock_x_unlock(&space->latch);
	} else if (!os_file_delete(innodb_data_file_key, path)
		   && !os_file_delete_if_exists(
			innodb_data_file_key, path, NULL)) {

		/* Note: This is because we have removed the
		tablespace instance from the cache. */

		err = DB_IO_ERROR;
	}

	if (err == DB_SUCCESS) {
#ifndef UNIV_HOTBACKUP
		/* Write a log record about the deletion of the .ibd
		file, so that ibbackup can replay it in the
		--apply-log phase. We use a dummy mtr and the familiar
		log write mechanism. */
		mtr_t		mtr;

		/* When replaying the operation in ibbackup, do not try
		to write any log record */
		mtr_start(&mtr);

		fil_op_write_log(MLOG_FILE_DELETE, id, 0, 0, path, NULL, &mtr);
		mtr_commit(&mtr);
#endif /* UNIV_HOTBACKUP */

		err = DB_SUCCESS;
	}

	::ut_free(path);

	return(err);
}

/** Check if an index tree is freed by checking a descriptor bit of
index root page.
@param[in]	space_id	space id
@param[in]	root_page_no	root page no of an index tree
@param[in]	page_size	page size
@return true if the index tree is freed */
bool
fil_index_tree_is_freed(
	ulint			space_id,
	ulint			root_page_no,
	const page_size_t&	page_size)
{
	rw_lock_t*	latch;
	mtr_t		mtr;
	xdes_t*		descr;

	latch = fil_space_get_latch(space_id, NULL);

	mtr_start(&mtr);
	mtr_x_lock(latch, &mtr);

	descr = xdes_get_descriptor(space_id, root_page_no, page_size, &mtr);

	if (descr == NULL
	    || (descr != NULL
		&& xdes_get_bit(descr, XDES_FREE_BIT,
			root_page_no % FSP_EXTENT_SIZE)) == TRUE) {

		mtr_commit(&mtr);
		/* The tree has already been freed */
		return(true);
	}
	mtr_commit(&mtr);

	return(false);
}

/*******************************************************************//**
Prepare for truncating a single-table tablespace.
1) Check pending operations on a tablespace;
2) Remove all insert buffer entries for the tablespace;
@return DB_SUCCESS or error */

dberr_t
fil_prepare_for_truncate(
/*=====================*/
	ulint	id)		/*!< in: space id */
{
	char*		path = 0;
	fil_space_t*	space = 0;

	ut_a(!is_system_tablespace(id));

	dberr_t	err = fil_check_pending_operations(
		id, FIL_OPERATION_TRUNCATE, &space, &path);

	::ut_free(path);

	if (err == DB_TABLESPACE_NOT_FOUND) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot truncate tablespace %lu because it is"
			" not found in the tablespace memory cache.",
			(ulong) id);
	}

	return(err);
}

/**********************************************************************//**
Reinitialize the original tablespace header with the same space id
for single tablespace */

void
fil_reinit_space_header(
/*====================*/
	ulint		id,	/*!< in: space id */
	ulint		size)	/*!< in: size in blocks */
{
	ut_a(!is_system_tablespace(id));

	/* Invalidate in the buffer pool all pages belonging
	to the tablespace */
	buf_LRU_flush_or_remove_pages(id, BUF_REMOVE_ALL_NO_WRITE, 0);

	/* Remove all insert buffer entries for the tablespace */
	ibuf_delete_for_discarded_space(id);

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(id);

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	space->size = node->size = size;

	mutex_exit(&fil_system->mutex);

	mtr_t	mtr;

	mtr_start(&mtr);

	fsp_header_init(id, size, &mtr);

	mtr_commit(&mtr);
}

/*******************************************************************//**
Returns true if a single-table tablespace is being deleted.
@return true if being deleted */

bool
fil_tablespace_is_being_deleted(
/*============================*/
	ulint		id)	/*!< in: space id */
{
	fil_space_t*	space;
	bool		is_being_deleted;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space != NULL);

	is_being_deleted = (space->stop_new_ops && !space->is_being_truncated);

	mutex_exit(&fil_system->mutex);

	return(is_being_deleted);
}

#ifndef UNIV_HOTBACKUP
/*******************************************************************//**
Discards a single-table tablespace. The tablespace must be cached in the
memory cache. Discarding is like deleting a tablespace, but

 1. We do not drop the table from the data dictionary;

 2. We remove all insert buffer entries for the tablespace immediately;
    in DROP TABLE they are only removed gradually in the background;

 3. Free all the pages in use by the tablespace.
@return DB_SUCCESS or error */

dberr_t
fil_discard_tablespace(
/*===================*/
	ulint	id)	/*!< in: space id */
{
	dberr_t	err;

	switch (err = fil_delete_tablespace(id, BUF_REMOVE_ALL_NO_WRITE)) {
	case DB_SUCCESS:
		break;

	case DB_IO_ERROR:
		ib_logf(IB_LOG_LEVEL_WARN,
			"While deleting tablespace %lu in DISCARD TABLESPACE."
			" File rename/delete failed: %s",
			(ulong) id, ut_strerr(err));
		break;

	case DB_TABLESPACE_NOT_FOUND:
		ib_logf(IB_LOG_LEVEL_WARN,
			"Cannot delete tablespace %lu in DISCARD"
			" TABLESPACE. %s",
			(ulong) id, ut_strerr(err));
		break;

	default:
		ut_error;
	}

	/* Remove all insert buffer entries for the tablespace */

	ibuf_delete_for_discarded_space(id);

	return(err);
}
#endif /* !UNIV_HOTBACKUP */

/*******************************************************************//**
Renames the memory cache structures of a single-table tablespace.
@return true if success */
static
bool
fil_rename_tablespace_in_mem(
/*=========================*/
	fil_space_t*	space,	/*!< in: tablespace memory object */
	fil_node_t*	node,	/*!< in: file node of that tablespace */
	const char*	new_name,	/*!< in: new name */
	const char*	new_path)	/*!< in: new file path */
{
	fil_space_t*	space2;
	const char*	old_name	= space->name;

	ut_ad(mutex_own(&fil_system->mutex));

	space2 = fil_space_get_by_name(old_name);
	if (space != space2) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot find %s in tablespace memory cache",
			old_name);

		return(false);
	}

	space2 = fil_space_get_by_name(new_name);
	if (space2 != NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"%s is already in tablespace memory cache",
			new_name);

		return(false);
	}

	HASH_DELETE(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(space->name), space);
	::ut_free(space->name);
	::ut_free(node->name);

	space->name = mem_strdup(new_name);
	node->name = mem_strdup(new_path);

	HASH_INSERT(fil_space_t, name_hash, fil_system->name_hash,
		    ut_fold_string(new_name), space);
	return(true);
}

/*******************************************************************//**
Allocates and builds a file name from a path, a table or tablespace name
and a suffix. The string must be freed by caller with ut_free().
@param[in] path NULL or the direcory path or the full path and filename.
@param[in] name NULL if path is full, or Table/Tablespace name
@param[in] suffix NULL or the file extention to use.
@param[in] trim_name true if the last name on the path should be trimmed.
@return own: file name */

char*
fil_make_filepath(
	const char*	path,
	const char*	name,
	ib_extention	ext,
	bool		trim_name)
{
	/* The path may contain the basename of the file, if so we do not
	need the name.  If the path is NULL, we can use the default path,
	but there needs to be a name. */
	ut_ad(path != NULL || name != NULL);

	/* If we are going to strip a name off the path, there better be a
	path and a new name to put back on. */
	ut_ad(!trim_name || (path != NULL && name != NULL));

	ulint	len		= 0;	/* current length */
	ulint	path_len	= (path ? ::strlen(path)
				  : ::strlen(fil_path_to_mysql_datadir));
	ulint	name_len	= (name ? ::strlen(name) : 0);
	const char* suffix	= dot_ext[ext];
	ulint	suffix_len	= ::strlen(suffix);
	ulint	full_len	= path_len + 1 + name_len + suffix_len + 1;

	char*	full_name = static_cast<char*>(ut_malloc(full_len));
	if (full_name == NULL) {
		return NULL;
	}

	::memcpy(full_name, (path ? path : fil_path_to_mysql_datadir),
		 path_len);
	len = path_len;
	full_name[len] = '\0';

	os_normalize_path_for_win(full_name);

	if (trim_name) {
		/* Find the offset of the last DIR separator and set it to
		null in order to strip off the old basename from this path. */
		char* last_dir_sep = strrchr(full_name, OS_PATH_SEPARATOR);
		if (last_dir_sep) {
			last_dir_sep[0] = '\0';
			len = ::strlen(full_name);
		}
	}

	if (name != NULL) {
		if (len && full_name[len - 1] != OS_PATH_SEPARATOR) {
			/* Add a DIR separator */
			full_name[len] = OS_PATH_SEPARATOR;
			full_name[len + 1] = '\0';
			len++;
		}

		::memcpy(&full_name[len], name, name_len);
		len += name_len;
		full_name[len] = '\0';

		/* The name might be like "dbname/tablename".
		So we have to do this again. */
		os_normalize_path_for_win(full_name);
	}

	/* Make sure that the specified suffix is at the end of the filepath
	string provided. This assumes that the suffix starts with '.'.
	If the first char of the suffix is found in the filepath at the same
	length as the suffix from the end, then we will assume that there is
	a previous suffix that needs to be replaced. */
	if (suffix != NULL) {
		ut_ad(len < full_len);  /* Need room for the trailing null byte. */

		if ((len > suffix_len)
		   && (full_name[len - suffix_len] == suffix[0])) {
			/* Another suffix exists, make it the one requested. */
			memcpy(&full_name[len - suffix_len], suffix, suffix_len);

		} else {
			/* No previous suffix, add it. */
			ut_ad(len + suffix_len < full_len);
			memcpy(&full_name[len], suffix, suffix_len);
			full_name[len + suffix_len] = '\0';
		}
	}

	return(full_name);
}

/*******************************************************************//**
Renames a single-table tablespace. The tablespace must be cached in the
tablespace memory cache.
@return true if success */

bool
fil_rename_tablespace(
/*==================*/
	const char*	old_name_in,	/*!< in: old table name in the
					standard databasename/tablename
					format of InnoDB, or NULL if we
					do the rename based on the space
					id only */
	ulint		id,		/*!< in: space id */
	const char*	new_name,	/*!< in: new table name in the
					standard databasename/tablename
					format of InnoDB */
	const char*	new_path_in)	/*!< in: new full datafile path
					if the tablespace is remotely
					located, or NULL if it is located
					in the normal data directory. */
{
	bool		success = false;
	bool		sleep = false;
	bool		flush = false;
	fil_space_t*	space;
	fil_node_t*	node;
	ulint		count		= 0;
	char*		new_path = NULL;
	char*		old_name = NULL;
	char*		old_path = NULL;
	const char*	not_given	= "(name not specified)";

	ut_a(id != 0);

retry:
	count++;

	if (!(count % 1000)) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Problems renaming %s to %s, %lu iterations",
			old_name_in ? old_name_in : not_given,
			new_name, (ulong) count);
	}

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	DBUG_EXECUTE_IF("fil_rename_tablespace_failure_1", space = NULL; );

	if (space == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot find space id %lu in the tablespace"
			" memory cache, though the table '%s' in a"
			" rename operation should have that id.",
			(ulong) id, old_name_in ? old_name_in : not_given);
		goto func_exit;
	}

	if (count > 25000) {
		space->stop_ios = false;
		goto func_exit;
	}

	/* We temporarily close the .ibd file because we do not trust that
	operating systems can rename an open file. For the closing we have to
	wait until there are no pending i/o's or flushes on the file. */

	space->stop_ios = true;

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);
	node = UT_LIST_GET_FIRST(space->chain);

	if (node->n_pending > 0
	    || node->n_pending_flushes > 0
	    || node->being_extended) {
		/* There are pending i/o's or flushes or the file is
		currently being extended, sleep for a while and
		retry */
		sleep = true;

	} else if (node->modification_counter > node->flush_counter) {
		/* Flush the space */
		sleep = flush = true;

	} else if (node->is_open) {
		/* Close the file */

		fil_node_close_file(node, fil_system);
	}

	if (sleep) {

		mutex_exit(&fil_system->mutex);

		os_thread_sleep(20000);

		if (flush) {
			fil_flush(id);
		}

		sleep = flush = false;
		goto retry;
	}

	/* Check that the old name in the space is right */

	if (old_name_in) {
		old_name = mem_strdup(old_name_in);
		ut_a(strcmp(space->name, old_name) == 0);
	} else {
		old_name = mem_strdup(space->name);
	}
	old_path = mem_strdup(node->name);

	/* Rename the tablespace and the node in the memory cache */
	new_path = new_path_in
		? mem_strdup(new_path_in)
		: fil_make_filepath(NULL, new_name, IBD, false);

	if (old_name != NULL && old_path != NULL && new_path != NULL) {
		success = fil_rename_tablespace_in_mem(
			space, node, new_name, new_path);
	}

	if (success) {

		DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
			goto skip_second_rename; );

		success = os_file_rename(
			innodb_data_file_key, old_path, new_path);

		DBUG_EXECUTE_IF("fil_rename_tablespace_failure_2",
skip_second_rename:
			success = false; );

		if (!success) {
			/* We have to revert the changes we made
			to the tablespace memory cache */

			bool	reverted = fil_rename_tablespace_in_mem(
				space, node, old_name, old_path);

			ut_a(reverted);
		}
	}

	space->stop_ios = false;

func_exit:
	mutex_exit(&fil_system->mutex);

#ifndef UNIV_HOTBACKUP
	if (success && !recv_recovery_on) {
		mtr_t		mtr;

		mtr_start(&mtr);

		fil_op_write_log(MLOG_FILE_RENAME, id, 0, 0, old_name, new_name,
				 &mtr);
		mtr_commit(&mtr);
	}
#endif /* !UNIV_HOTBACKUP */

	if (new_path) {
		::ut_free(new_path);
	}
	if (old_path) {
		::ut_free(old_path);
	}
	if (old_name) {
		::ut_free(old_name);
	}

	return(success);
}

/*******************************************************************//**
Creates a new single-table tablespace to a database directory of MySQL.
Database directories are under the 'datadir' of MySQL. The datadir is the
directory of a running mysqld program. We can refer to it by simply the
path '.'. Tables created with CREATE TEMPORARY TABLE we place in the temp
dir of the mysqld server.

@return DB_SUCCESS or error code */

dberr_t
fil_create_new_single_table_tablespace(
/*===================================*/
	ulint		space_id,	/*!< in: space id */
	const char*	tablename,	/*!< in: the table name in the usual
					databasename/tablename format
					of InnoDB */
	const char*	dir_path,	/*!< in: NULL or a dir path */
	ulint		flags,		/*!< in: tablespace flags */
	ulint		flags2,		/*!< in: table flags2 */
	ulint		size)		/*!< in: the initial size of the
					tablespace file in pages,
					must be >= FIL_IBD_FILE_INITIAL_SIZE */
{
	os_file_t	file;
	dberr_t		err;
	byte*		buf2;
	byte*		page;
	char*		path;
	bool		success;
	bool		is_temp = !!(flags2 & DICT_TF2_TEMPORARY);
	bool		has_data_dir = FSP_FLAGS_HAS_DATA_DIR(flags);
	fil_space_t*	space = NULL;

	ut_ad(!is_system_tablespace(space_id));
	ut_ad(!srv_read_only_mode);
	ut_a(space_id < SRV_LOG_SPACE_FIRST_ID);
	ut_a(size >= FIL_IBD_FILE_INITIAL_SIZE);
	ut_a(fsp_flags_is_valid(flags));

	if (is_temp) {
		/* Temporary table filepath */
		ut_ad(dir_path);
		path = fil_make_filepath(dir_path, NULL, IBD, false);
	} else if (has_data_dir) {
		ut_ad(dir_path);
		path = fil_make_filepath(dir_path, tablename, IBD, true);

		/* Since this tablespace file will be created in a
		remote directory, let's create the subdirectories
		in the path, if they are not there already. */
		success = os_file_create_subdirs_if_needed(path);
		if (!success) {
			ut_free(path);
			return(DB_ERROR);
		}
	} else {
		path = fil_make_filepath(NULL, tablename, IBD, false);
	}

	if (path == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	file = os_file_create(
		innodb_data_file_key, path,
		OS_FILE_CREATE | OS_FILE_ON_ERROR_NO_EXIT,
		OS_FILE_NORMAL,
		OS_DATA_FILE,
		&success);

	if (!success) {
		/* The following call will print an error message */
		ulint	error = os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot create file '%s'", path);

		if (error == OS_FILE_ALREADY_EXISTS) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The file '%s' already exists though the"
				" corresponding table did not exist"
				" in the InnoDB data dictionary."
				" Have you moved InnoDB .ibd files"
				" around without using the SQL commands"
				" DISCARD TABLESPACE and IMPORT TABLESPACE,"
				" or did mysqld crash in the middle of"
				" CREATE TABLE?"
				" You can resolve the problem by removing"
				" the file '%s' under the 'datadir' of MySQL.",
				path, path);

			ut_free(path);
			return(DB_TABLESPACE_EXISTS);
		}

		if (error == OS_FILE_DISK_FULL) {
			ut_free(path);
			return(DB_OUT_OF_FILE_SPACE);
		}

		ut_free(path);
		return(DB_ERROR);
	}

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
	if (fil_fusionio_enable_atomic_write(file)) {

		/* This is required by FusionIO HW/Firmware */
		if (posix_fallocate(file, 0, size * UNIV_PAGE_SIZE) == -1) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"posix_fallocate(): Failed to preallocate"
				" data for file %s, desired size %lu."
				" Operating system error number %lu. Check"
				" that the disk is not full or a disk quota"
				" exceeded. Some operating system error"
				" numbers are described at " REFMAN
				" operating-system-error-codes.html",
				path,
				(ulong) size * UNIV_PAGE_SIZE,
				(ulong) errno);

			success = false;
		} else {
			success = true;
		}
	} else {

		success = os_file_set_size(path, file, size * UNIV_PAGE_SIZE);
	}
#else
	success = os_file_set_size(path, file, size * UNIV_PAGE_SIZE);
#endif /* !NO_FALLOCATE && UNIV_LINUX */

	if (!success) {
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		ut_free(path);
		return(DB_OUT_OF_FILE_SPACE);
	}

	/* printf("Creating tablespace %s id %lu\n", path, space_id); */

	/* We have to write the space id to the file immediately and flush the
	file to disk. This is because in crash recovery we must be aware what
	tablespaces exist and what are their space id's, so that we can apply
	the log records to the right file. It may take quite a while until
	buffer pool flush algorithms write anything to the file and flush it to
	disk. If we would not write here anything, the file would be filled
	with zeros from the call of os_file_set_size(), until a buffer pool
	flush would write to it. */

	buf2 = static_cast<byte*>(ut_malloc(3 * UNIV_PAGE_SIZE));
	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	memset(page, '\0', UNIV_PAGE_SIZE);

	/* Add the UNIV_PAGE_SIZE to the table flags and write them to the
	tablespace header. */
	flags = fsp_flags_set_page_size(flags, UNIV_PAGE_SIZE);
	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	const page_size_t	page_size(flags);

	if (!page_size.is_compressed()) {
		buf_flush_init_for_writing(page, NULL, 0);
		success = os_file_write(path, file, page, 0,
					page_size.physical());
	} else {
		page_zip_des_t	page_zip;

		page_zip_set_size(&page_zip, page_size.physical());
		page_zip.data = page + UNIV_PAGE_SIZE;
#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
			page_zip.m_end = page_zip.m_nonempty =
			page_zip.n_blobs = 0;
		buf_flush_init_for_writing(page, &page_zip, 0);
		success = os_file_write(path, file, page_zip.data, 0,
					page_size.physical());
	}

	::ut_free(buf2);

	if (!success) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Could not write the first page to tablespace '%s'",
			path);
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		ut_free(path);
		return(DB_ERROR);
	}

	success = os_file_flush(file);

	if (!success) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"File flush of tablespace '%s' failed", path);
		os_file_close(file);
		os_file_delete(innodb_data_file_key, path);
		ut_free(path);
		return(DB_ERROR);
	}

	if (has_data_dir) {
		/* Now that the IBD file is created, make the ISL file. */
		err = RemoteDatafile::create_link_file(tablename, path);
		if (err != DB_SUCCESS) {
			os_file_close(file);
			os_file_delete(innodb_data_file_key, path);
			ut_free(path);
			return(err);
		}
	}

	space = fil_space_create(tablename, space_id, flags, is_temp
				 ? FIL_TYPE_TEMPORARY : FIL_TYPE_TABLESPACE);

	if (!fil_node_create(path, size, space, false)) {
		err = DB_ERROR;
		goto error_exit_1;
	}

#ifndef UNIV_HOTBACKUP
	{
		mtr_t		mtr;
		ulint		mlog_file_flag = 0;

		if (is_temp) {
			mlog_file_flag |= MLOG_FILE_FLAG_TEMP;
		}

		mtr_start(&mtr);

		fil_op_write_log(flags
				 ? MLOG_FILE_CREATE2
				 : MLOG_FILE_CREATE,
				 space_id, mlog_file_flag, flags,
				 tablename, NULL, &mtr);

		mtr_commit(&mtr);
	}
#endif
	err = DB_SUCCESS;

	/* Error code is set.  Cleanup the various variables used.
	These labels reflect the order in which variables are assigned or
	actions are done. */
error_exit_1:
	if (has_data_dir && err != DB_SUCCESS) {
		RemoteDatafile::delete_link_file(tablename);
	}

	os_file_close(file);
	if (err != DB_SUCCESS) {
		os_file_delete(innodb_data_file_key, path);
	}

	::ut_free(path);

	return(err);
}

#ifndef UNIV_HOTBACKUP
/********************************************************************//**
Tries to open a single-table tablespace and optionally checks that the
space id in it is correct. If this does not succeed, print an error message
to the .err log. This function is used to open a tablespace when we start
mysqld after the dictionary has been booted, and also in IMPORT TABLESPACE.

NOTE that we assume this operation is used either at the database startup
or under the protection of the dictionary mutex, so that two users cannot
race here. This operation does not leave the file associated with the
tablespace open, but closes it after we have looked at the space id in it.

If the validate boolean is set, we read the first page of the file and
check that the space id in the file is what we expect. We assume that
this function runs much faster if no check is made, since accessing the
file inode probably is much faster (the OS caches them) than accessing
the first page of the file.  This boolean may be initially false, but if
a remote tablespace is found it will be changed to true.

If the fix_dict boolean is set, then it is safe to use an internal SQL
statement to update the dictionary tables if they are incorrect.

@param[in] validate True if we should validate the tablespace.
@param[in] fix_dict True if the dictionary is available to be fixed.
@param[in] purpose FIL_TYPE_TABLESPACE or FIL_TYPE_TEMPORARY
@param[in] id Tablespace ID
@param[in] flags Tablespace flags
@param[in] tablename Table name in the databasename/tablename format.
@param[in] path_in Tablespace filepath if found in SYS_DATAFILES
@return DB_SUCCESS or error code */

dberr_t
fil_open_single_table_tablespace(
	bool		validate,
	bool		fix_dict,
	fil_type_t	purpose,
	ulint		id,
	ulint		flags,
	const char*	tablename,
	const char*	path_in)
{
	dberr_t		err = DB_SUCCESS;
	bool		dict_filepath_same_as_default = false;
	bool		link_file_found = false;
	bool		link_file_is_bad = false;
	Datafile	df_default;	/* default location */
	Datafile	df_dict;	/* dictionary location */
	RemoteDatafile	df_remote;	/* remote location */
	ulint		tablespaces_found = 0;
	ulint		valid_tablespaces_found = 0;

#ifdef UNIV_SYNC_DEBUG
	ut_ad(!fix_dict || rw_lock_own(&dict_operation_lock, RW_LOCK_X));
#endif /* UNIV_SYNC_DEBUG */

	ut_ad(!fix_dict || mutex_own(&(dict_sys->mutex)));
	ut_ad(purpose == FIL_TYPE_TABLESPACE || purpose == FIL_TYPE_TEMPORARY);

	if (!fsp_flags_is_valid(flags)) {
		return(DB_CORRUPTION);
	}

	df_default.init(tablename, 0, 0);
	df_dict.init(tablename, 0, 0);
	df_remote.init(tablename, 0, 0);

	/* Discover the correct filepath.  We will always look for an ibd
	in the default location. If it is remote, it should not be here. */
	df_default.make_filepath(NULL);

	/* The path_in was read from SYS_DATAFILES. */
	if (path_in) {
		if (df_default.same_filepath_as(path_in)) {
			dict_filepath_same_as_default = true;
		} else {
			df_dict.set_filepath(path_in);
			/* possibility of multiple files. */
			validate = true;
		}
	}

	if (df_remote.open_read_only() == DB_SUCCESS) {
		ut_ad(df_remote.is_open());

		/* A link file was found. MySQL does not allow a DATA
		DIRECTORY to be the same as the default filepath.
		This could happen if the link file was edited directly.*/
		if (df_default.same_filepath_as(df_remote.filepath())) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Link files should not refer to files in"
				" the default location. Please delete %s"
				" or change the remote file it refers to.",
				df_remote.link_filepath());
			return(DB_CORRUPTION);
		}

		validate = true;	/* possibility of multiple files. */
		tablespaces_found++;
		link_file_found = true;

		/* If there was a filepath found in SYS_DATAFILES,
		we hope it was the same as this remote.filepath found
		in the ISL file. */
		if (df_dict.filepath()
		    && 0 == strcmp(df_dict.filepath(), df_remote.filepath())) {
			df_remote.close();
			tablespaces_found--;
		}
	}

	/* Attempt to open the tablespace at the dictionary filepath. */
	if (df_dict.open_read_only() == DB_SUCCESS) {
		ut_ad(df_dict.is_open());
		validate = true;	/* possibility of multiple files. */
		tablespaces_found++;
	}

	/* Always look for a file at the default location. */
	ut_a(df_default.filepath());
	if (df_default.open_read_only() == DB_SUCCESS) {
		ut_ad(df_default.is_open());
		tablespaces_found++;
	}

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
	if (!srv_use_doublewrite_buf) {
		fil_fusionio_enable_atomic_write(df_default.handle());

	}
#endif /* !NO_FALLOCATE && UNIV_LINUX */

	/*  We have now checked all possible tablespace locations and
	have a count of how many we found.  If things are normal, we
	only found 1. */
	if (!validate && tablespaces_found == 1) {
		goto skip_validate;
	}

	/* Read and validate the first page of these three tablespace
	locations, if found. */
	valid_tablespaces_found +=
		(df_remote.validate_to_dd(id, flags) == DB_SUCCESS) ? 1 : 0;

	valid_tablespaces_found +=
		(df_default.validate_to_dd(id, flags) == DB_SUCCESS) ? 1 : 0;

	valid_tablespaces_found +=
		(df_dict.validate_to_dd(id, flags) == DB_SUCCESS) ? 1 : 0;

	/* Make sense of these three possible locations.
	First, bail out if no tablespace files were found. */
	if (valid_tablespaces_found == 0) {
		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Could not find a valid tablespace file for '%s'. %s",
			tablename, TROUBLESHOOT_DATADICT_MSG);

		return(DB_CORRUPTION);
	}

	/* Do not open any tablespaces if more than one tablespace with
	the correct space ID and flags were found. */
	if (tablespaces_found > 1) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"A tablespace for %s has been found in"
			" multiple places;", tablename);
		if (df_default.is_open()) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Default location; %s, LSN=" LSN_PF
				", Space ID=%lu, Flags=%lu",
				df_default.filepath(),
				df_default.flushed_lsn(),
				(ulong) df_default.space_id(),
				(ulong) df_default.flags());
		}
		if (df_remote.is_open()) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Remote location; %s, LSN=" LSN_PF
				", Space ID=%lu, Flags=%lu",
				df_remote.filepath(), df_remote.flushed_lsn(),
				(ulong) df_remote.space_id(),
				(ulong) df_remote.flags());
		}
		if (df_dict.is_open()) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Dictionary location; %s, LSN=" LSN_PF
				", Space ID=%lu, Flags=%lu",
				df_dict.filepath(), df_dict.flushed_lsn(),
				(ulong) df_dict.space_id(),
				(ulong) df_dict.flags());
		}

		/* Force-recovery will allow some tablespaces to be
		skipped by REDO if there was more than one file found.
		Unlike during the REDO phase of recovery, we now know
		if the tablespace is valid according to the dictionary,
		which was not available then. So if we did not force
		recovery and there is only one good tablespace, ignore
		any bad tablespaces. */
		if (valid_tablespaces_found > 1 || srv_force_recovery > 0) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Will not open the tablespace for '%s'",
				tablename);

			/* If the file is not open it cannot be valid. */
			ut_ad(df_default.is_open() || !df_default.is_valid());
			ut_ad(df_dict.is_open()    || !df_dict.is_valid());
			ut_ad(df_remote.is_open()  || !df_remote.is_valid());

			/* Having established that, this is an easy way to
			look for corrupted data files. */
			if (df_default.is_open() != df_default.is_valid()
			    || df_dict.is_open() != df_dict.is_valid()
			    || df_remote.is_open() != df_remote.is_valid()) {
				return(DB_CORRUPTION);
			}
			return(DB_ERROR);
		}

		/* There is only one valid tablespace found and we did
		not use srv_force_recovery during REDO.  Use this one
		tablespace and clean up invalid tablespace pointers */
		if (df_default.is_open() && !df_default.is_valid()) {
			df_default.close();
			tablespaces_found--;
		}
		if (df_dict.is_open() && !df_dict.is_valid()) {
			df_dict.close();
			/* Leave dict.filepath so that SYS_DATAFILES
			can be corrected below. */
			tablespaces_found--;
		}
		if (df_remote.is_open() && !df_remote.is_valid()) {
			df_remote.close();
			tablespaces_found--;
			link_file_is_bad = true;
		}
	}

	/* At this point, there should be only one filepath. */
	ut_a(tablespaces_found == 1);
	ut_a(valid_tablespaces_found == 1);

	/* Only fix the dictionary at startup when there is only one thread.
	Calls to dict_load_table() can be done while holding other latches. */
	if (!fix_dict) {
		goto skip_validate;
	}

	/* We may need to change what is stored in SYS_DATAFILES or
	SYS_TABLESPACES or adjust the link file.
	Since a failure to update SYS_TABLESPACES or SYS_DATAFILES does
	not prevent opening and using the single_table_tablespace either
	this time or the next, we do not check the return code or fail
	to open the tablespace. But dict_update_filepath() will issue a
	warning to the log. */
	if (df_dict.filepath()) {
		if (df_remote.is_open()) {
			dict_update_filepath(id, df_remote.filepath());

		} else if (df_default.is_open()) {
			dict_update_filepath(id, df_default.filepath());
			if (link_file_is_bad) {
				RemoteDatafile::delete_link_file(tablename);
			}

		} else if (!link_file_found || link_file_is_bad) {
			ut_ad(df_dict.is_open());
			/* Fix the link file if we got our filepath
			from the dictionary but a link file did not
			exist or it did not point to a valid file. */
			RemoteDatafile::delete_link_file(tablename);
			RemoteDatafile::create_link_file(
				tablename, df_dict.filepath());
		}

	} else if (df_remote.is_open()) {
		if (dict_filepath_same_as_default) {
			dict_update_filepath(id, df_remote.filepath());

		} else if (path_in == NULL) {
			/* SYS_DATAFILES record for this space ID
			was not found. */
			dict_insert_tablespace_and_filepath(
				id, tablename, df_remote.filepath(), flags);
		}
	}

skip_validate:
	if (err == DB_SUCCESS) {
		fil_space_t*	space	= fil_space_create(
			tablename, id, flags, purpose);
		/* We do not measure the size of the file, that is why
		we pass the 0 below */

		if (!fil_node_create(
			    df_remote.is_open() ? df_remote.filepath() :
			    df_dict.is_open() ? df_dict.filepath() :
			    df_default.filepath(), 0, space, false)) {
			err = DB_ERROR;
		}
	}

	return(err);
}
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_HOTBACKUP
/*******************************************************************//**
Allocates a file name for an old version of a single-table tablespace.
The string must be freed by caller with ut_free()!
@return own: file name */
static
char*
fil_make_ibbackup_old_name(
/*=======================*/
	const char*	name)		/*!< in: original file name */
{
	static const char suffix[] = "_ibbackup_old_vers_";
	char*	path;
	ulint	len	= ::strlen(name);

	path = static_cast<char*>(ut_malloc(len + (15 + sizeof suffix)));

	memcpy(path, name, len);
	memcpy(path + len, suffix, (sizeof suffix) - 1);
	ut_sprintf_timestamp_without_extra_chars(
		path + len + ((sizeof suffix) - 1));
	return(path);
}
#endif /* UNIV_HOTBACKUP */

/** Looks for a pre-existing fil_space_t with the given tablespace ID
and, if found, returns the name and filepath in newly allocated buffers
that the caller must free.
@param[in]	space_id	The tablespace ID to search for.
@param[out]	name		Name of the tablespace found.
@param[out]	filepath	The filepath of the first datafile for the
tablespace.
@return true if tablespace is found, false if not. */

bool
fil_space_read_name_and_filepath(
	ulint	space_id,
	char**	name,
	char**	filepath)
{
	bool	success = false;
	*name = NULL;
	*filepath = NULL;

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space != NULL) {
		*name = mem_strdup(space->name);

		fil_node_t* node = UT_LIST_GET_FIRST(space->chain);
		*filepath = mem_strdup(node->name);

		success = true;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}


/********************************************************************//**
Opens an .ibd file and adds the associated single-table tablespace to the
InnoDB fil0fil.cc data structures. */
static
void
fil_load_single_table_tablespace(
/*=============================*/
	const char*	dbname,		/*!< in: database name */
	const char*	filename)	/*!< in: file name (not a path),
					including the .ibd or .isl extension */
{
	char*		name;
	ulint		name_len;
	ulint		dbname_len = ::strlen(dbname);
	ulint		filename_len = ::strlen(filename);
	Datafile	df_default;
	RemoteDatafile	df_remote;

	/* The caller assured that the extension is ".ibd" or ".isl". */
	ut_ad(0 == memcmp(filename + filename_len - 4, DOT_IBD, 4)
	      || 0 == memcmp(filename + filename_len - 4, DOT_ISL, 4));

	/* Build up the tablename in the standard form database/table. */
	name_len = dbname_len + filename_len + 2;
	name = static_cast<char*>(ut_malloc(name_len));
	ut_snprintf(name, name_len, "%s/%s", dbname, filename);
	name_len = ::strlen(name) - ::strlen(DOT_IBD);
	name[name_len] = '\0';

	df_default.init(name, 0, 0);
	df_remote.init(name, 0, 0);

	/* There may be both .ibd and .isl file in the directory.
	And it is possible that the .isl file refers to a different
	.ibd file.  If so, we open and compare them the first time
	one of them is sent to this function.  So if this table has
	already been loaded, there is nothing to do.*/
	mutex_enter(&fil_system->mutex);
	if (fil_space_get_by_name(name)) {
		::ut_free(name);
		mutex_exit(&fil_system->mutex);
		return;
	}
	mutex_exit(&fil_system->mutex);

	/* Check for a link file which locates a remote tablespace. */
	if (df_remote.open_read_only() == DB_SUCCESS) {
		/* Read and validate the first page of the remote tablespace */
		if (df_remote.validate_for_recovery() != DB_SUCCESS) {
			df_remote.close();
		}
	}

	/* Try to open the tablespace in the datadir. */
	df_default.make_filepath(NULL);
	if (df_default.open_read_only() == DB_SUCCESS) {
		/* Read and validate the first page of the default tablespace */
		if (df_default.validate_for_recovery() != DB_SUCCESS) {
			df_default.close();
		}
	}

	if (!df_default.is_open() && !df_remote.is_open()) {
		/* The following call prints an error message */
		os_file_get_last_error(true);
		ib_logf(IB_LOG_LEVEL_WARN,
			"Could not open single-table tablespace file %s",
			df_default.filepath());

		if (!strncmp(filename,
			     TEMP_FILE_PREFIX, TEMP_FILE_PREFIX_LENGTH)) {
			/* Ignore errors for #sql tablespaces. */
			::ut_free(name);
			return;
		}
no_good_file:
		ib_logf(IB_LOG_LEVEL_WARN,
			"We do not continue the crash recovery, because"
			" the table may become corrupt if we cannot apply"
			" the log records in the InnoDB log to it."
			" To fix the problem and start mysqld:");
		ib_logf(IB_LOG_LEVEL_INFO,
			"1) If there is a permission problem in the file"
			" and mysqld cannot open the file, you should"
			" modify the permissions.");
		ib_logf(IB_LOG_LEVEL_INFO,
			"2) If the table is not needed, or you can restore"
			" it from a backup, then you can remove the .ibd"
			" file, and InnoDB will do a normal crash recovery"
			" and ignore that table.");
		ib_logf(IB_LOG_LEVEL_INFO,
			"3) If the file system or the disk is broken, and"
			" you cannot remove the .ibd file, you can set"
			" innodb_force_recovery > 0 in my.cnf and force"
			" InnoDB to continue crash recovery here.");

will_not_choose:
		::ut_free(name);

		if (srv_force_recovery > 0) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"innodb_force_recovery was set to %lu."
				" Continuing crash recovery even though we"
				" cannot access the .ibd file of this table.",
				srv_force_recovery);
			return;
		}

		exit(1);
	}

	if (df_default.is_valid() && df_remote.is_valid()) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Tablespaces for %s have been found in two places;"
			" Location 1: SpaceID: %lu  LSN: %lu  File: %s;"
			" Location 2: SpaceID: %lu  LSN: %lu  File: %s;"
			" You must delete one of them.",
			name,
			ulong(df_default.space_id()),
			ulong(df_default.flushed_lsn()),
			df_default.filepath(),
			ulong(df_remote.space_id()),
			ulong(df_remote.flushed_lsn()),
			df_remote.filepath());

		df_default.close();
		df_remote.close();
		goto will_not_choose;
	}

	/* At this point, only one tablespace is open */
	ut_a(df_default.is_open() == !df_remote.is_open());

	Datafile*	df = df_default.is_open() ? &df_default : &df_remote;

	/* Get and test the file size. */
	os_offset_t	size = os_file_get_size(df->handle());

	if (size == (os_offset_t) -1) {
		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Could not measure the size of single-table"
			" tablespace file %s", df->filepath());

		goto no_good_file;
	}

	/* Every .ibd file is created >= 4 pages in size. Smaller files
	cannot be ok. */
	ulong	minimum_size = FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE;

	if (size < minimum_size) {
#ifndef UNIV_HOTBACKUP
		ib_logf(IB_LOG_LEVEL_ERROR,
			"The size of single-table tablespace file %s"
			" is only " UINT64PF ", should be at least %lu!",
			df->filepath(), size, minimum_size);
		goto no_good_file;
#else
		df->set_space_id(ULINT_UNDEFINED);
		fsf->set_flags(0);
#endif /* !UNIV_HOTBACKUP */
	}

	fil_space_t*	space = NULL;

#ifdef UNIV_HOTBACKUP
	if (df->space_id() == ULINT_UNDEFINED || df->space_id() == 0) {
		char*	new_path;

		ib_logf(IB_LOG_LEVEL_INFO,
			"Renaming tablespace %s of id %lu, to"
			" %s_ibbackup_old_vers_<timestamp> because its size"
			INT64PF " is too small (< 4 pages 16 kB each), or the"
			" space id in the file header is not sensible. This can"
			" happen in an ibbackup run, and is not dangerous.",
			df->name(), df->space_id(), df->name(), size);
		df->close();

		new_path = fil_make_ibbackup_old_name(df->filepath());

		bool	success = os_file_rename(
			innodb_data_file_key, df->filepath(), new_path);

		ut_a(success);

		::ut_free(new_path);

		goto func_exit;
	}

	/* A backup may contain the same space several times, if the space got
	renamed at a sensitive time. Since it is enough to have one version of
	the space, we rename the file if a space with the same space id
	already exists in the tablespace memory cache. We rather rename the
	file than delete it, because if there is a bug, we do not want to
	destroy valuable data. */

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(df->space_id());

	if (space != NULL) {
		char*	new_path;

		ib_logf(IB_LOG_LEVEL_INFO,
			"Renaming data file %s with space ID %lu, to"
			" %s_ibbackup_old_vers_<timestamp> because space %s"
			" with the same id was scanned earlier. This can happen"
			" if you have renamed tables during an ibbackup run.",
			df->name(), df->space_id(), df->name(), space->name);
		df->close();

		new_path = fil_make_ibbackup_old_name(df->filepath());

		mutex_exit(&fil_system->mutex);

		bool	success = os_file_rename(
			innodb_data_file_key, df->filepath(), new_path);

		ut_a(success);

		::ut_free(new_path);

		goto func_exit;
	}
	mutex_exit(&fil_system->mutex);
#endif /* UNIV_HOTBACKUP */

	space = fil_space_create(
		name, df->space_id(), df->flags(), FIL_TYPE_TABLESPACE);

	if (space == NULL && srv_force_recovery > 0) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"innodb_force_recovery was set to %lu."
			" Continuing crash recovery even though"
			" the tablespace creation of this table"
			" failed.",
			srv_force_recovery);
		goto func_exit;
	}

	/* We do not use the size information we have about the file, because
	the rounding formula for extents and pages is somewhat complex; we
	let fil_node_open() do that task. */

	if (!fil_node_create(df->filepath(), 0, space, false)) {
		ut_a(space != NULL);
		ut_error;
	}

func_exit:

#ifndef UNIV_HOTBACKUP
	ut_ad(!mutex_own(&fil_system->mutex));
#endif
	::ut_free(name);
}

/***********************************************************************//**
A fault-tolerant function that tries to read the next file name in the
directory. We retry 100 times if os_file_readdir_next_file() returns -1. The
idea is to read as much good data as we can and jump over bad data.
@return 0 if ok, -1 if error even after the retries, 1 if at the end
of the directory */

int
fil_file_readdir_next_file(
/*=======================*/
	dberr_t*	err,	/*!< out: this is set to DB_ERROR if an error
				was encountered, otherwise not changed */
	const char*	dirname,/*!< in: directory name or path */
	os_file_dir_t	dir,	/*!< in: directory stream */
	os_file_stat_t*	info)	/*!< in/out: buffer where the
				info is returned */
{
	for (ulint i = 0; i < 100; i++) {
		int	ret = os_file_readdir_next_file(dirname, dir, info);

		if (ret != -1) {

			return(ret);
		}

		ib_logf(IB_LOG_LEVEL_ERROR,
			"os_file_readdir_next_file() returned -1 in"
			" directory %s, crash recovery may have failed"
			" for some .ibd files!", dirname);

		*err = DB_ERROR;
	}

	return(-1);
}

/********************************************************************//**
At the server startup, if we need crash recovery, scans the database
directories under the MySQL datadir, looking for .ibd files. Those files are
single-table tablespaces. We need to know the space id in each of them so that
we know into which file we should look to check the contents of a page stored
in the doublewrite buffer, also to know where to apply log records where the
space id is != 0.
@return DB_SUCCESS or error number */

dberr_t
fil_load_single_table_tablespaces(void)
/*===================================*/
{
	int		ret;
	char*		dbpath		= NULL;
	ulint		dbpath_len	= 100;
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	dberr_t		err		= DB_SUCCESS;

	/* The datadir of MySQL is always the default directory of mysqld */

	dir = os_file_opendir(fil_path_to_mysql_datadir, true);

	if (dir == NULL) {

		return(DB_ERROR);
	}

	dbpath = static_cast<char*>(ut_malloc(dbpath_len));

	/* Scan all directories under the datadir. They are the database
	directories of MySQL. */

	ret = fil_file_readdir_next_file(&err, fil_path_to_mysql_datadir, dir,
					 &dbinfo);
	while (ret == 0) {
		ulint len;
		/* printf("Looking at %s in datadir\n", dbinfo.name); */

		if (dbinfo.type == OS_FILE_TYPE_FILE
		    || dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

			goto next_datadir_item;
		}

		/* We found a symlink or a directory; try opening it to see
		if a symlink is a directory */

		len = ::strlen(fil_path_to_mysql_datadir)
			+ ::strlen (dbinfo.name) + 2;
		if (len > dbpath_len) {
			dbpath_len = len;
			::ut_free(dbpath);
			dbpath = static_cast<char*>(ut_malloc(dbpath_len));
		}
		ut_snprintf(dbpath, dbpath_len,
			    "%s/%s", fil_path_to_mysql_datadir, dbinfo.name);
		os_normalize_path_for_win(dbpath);

		dbdir = os_file_opendir(dbpath, false);

		if (dbdir != NULL) {

			/* We found a database directory; loop through it,
			looking for possible .ibd files in it */

			ret = fil_file_readdir_next_file(&err, dbpath, dbdir,
							 &fileinfo);
			while (ret == 0) {

				if (fileinfo.type == OS_FILE_TYPE_DIR) {

					goto next_file_item;
				}

				/* We found a symlink or a file */
				if (::strlen(fileinfo.name) > 4
				    && (0 == strcmp(fileinfo.name
						   + ::strlen(fileinfo.name) - 4,
						   DOT_IBD)
					|| 0 == strcmp(fileinfo.name
						   + ::strlen(fileinfo.name) - 4,
						   DOT_ISL))) {
					/* The name ends in .ibd or .isl;
					try opening the file */
					fil_load_single_table_tablespace(
						dbinfo.name, fileinfo.name);
				}
next_file_item:
				ret = fil_file_readdir_next_file(&err,
								 dbpath, dbdir,
								 &fileinfo);
			}

			if (0 != os_file_closedir(dbdir)) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"Could not close database directory %s",
					dbpath);

				err = DB_ERROR;
			}
		}

next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						 fil_path_to_mysql_datadir,
						 dir, &dbinfo);
	}

	::ut_free(dbpath);

	if (0 != os_file_closedir(dir)) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Could not close MySQL datadir");

		return(DB_ERROR);
	}

	return(err);
}

/*******************************************************************//**
Returns true if a single-table tablespace does not exist in the memory cache,
or is being deleted there.
@return true if does not exist or is being deleted */

bool
fil_tablespace_deleted_or_being_deleted_in_mem(
/*===========================================*/
	ulint		id,	/*!< in: space id */
	ib_int64_t	version)/*!< in: tablespace_version should be this; if
				you pass -1 as the value of this, then this
				parameter is ignored */
{
	bool already_deleted = false;
	bool being_deleted = false;

	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	already_deleted = (space == NULL
			   || (space->stop_new_ops
			       && !space->is_being_truncated));

	if (!already_deleted) {
		being_deleted = (version != ib_int64_t(-1)
				 && space->tablespace_version != version);
	}

	mutex_exit(&fil_system->mutex);

	return(already_deleted || being_deleted);
}

/*******************************************************************//**
Returns true if a single-table tablespace exists in the memory cache.
@return true if exists */

bool
fil_tablespace_exists_in_mem(
/*=========================*/
	ulint	id)	/*!< in: space id */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	mutex_exit(&fil_system->mutex);

	return(space != NULL);
}

/*******************************************************************//**
Report that a tablespace for a table was not found. */
static
void
fil_report_missing_tablespace(
/*===========================*/
	const char*	name,			/*!< in: table name */
	ulint		space_id)		/*!< in: table's space id */
{
	char	index_name[MAX_FULL_NAME_LEN + 1];

	innobase_format_name(index_name, sizeof(index_name), name, TRUE);

	ib_logf(IB_LOG_LEVEL_ERROR,
		"Table %s in the InnoDB data dictionary has tablespace id %lu,"
		" but tablespace with that id or name does not exist. Have"
		" you deleted or moved .ibd files? This may also be a table"
		" created with CREATE TEMPORARY TABLE whose .ibd and .frm"
		" files MySQL automatically removed, but the table still"
		" exists in the InnoDB internal data dictionary.",
		name, space_id);
}

/*******************************************************************//**
Returns true if a matching tablespace exists in the InnoDB tablespace memory
cache. Note that if we have not done a crash recovery at the database startup,
there may be many tablespaces which are not yet in the memory cache.
@return true if a matching tablespace exists in the memory cache */

bool
fil_space_for_table_exists_in_mem(
/*==============================*/
	ulint		id,		/*!< in: space id */
	const char*	name,		/*!< in: table name used in
					fil_space_create().  Either the
					standard 'dbname/tablename' format
					or table->dir_path_of_temp_table */
	bool		print_error_if_does_not_exist,
					/*!< in: print detailed error
					information to the .err log if a
					matching tablespace is not found from
					memory */
	bool		adjust_space,	/*!< in: whether to adjust space id
					when find table space mismatch */
	mem_heap_t*	heap,		/*!< in: heap memory */
	table_id_t	table_id)	/*!< in: table id */
{
	fil_space_t*	fnamespace;
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	/* Look if there is a space with the same id */

	space = fil_space_get_by_id(id);

	/* Look if there is a space with the same name; the name is the
	directory path from the datadir to the file */

	fnamespace = fil_space_get_by_name(name);
	if (space && space == fnamespace) {
		/* Found */

		mutex_exit(&fil_system->mutex);

		return(true);
	}

	/* Info from "fnamespace" comes from the ibd file itself, it can
	be different from data obtained from System tables since it is
	not transactional. If adjust_space is set, and the mismatching
	space are between a user table and its temp table, we shall
	adjust the ibd file name according to system table info */
	if (adjust_space
	    && space != NULL
	    && row_is_mysql_tmp_table_name(space->name)
	    && !row_is_mysql_tmp_table_name(name)) {

		mutex_exit(&fil_system->mutex);

		DBUG_EXECUTE_IF("ib_crash_before_adjust_fil_space",
				DBUG_SUICIDE(););

		if (fnamespace) {
			const char*	tmp_name;

			tmp_name = dict_mem_create_temporary_tablename(
				heap, name, table_id);

			fil_rename_tablespace(fnamespace->name, fnamespace->id,
					      tmp_name, NULL);
		}

		DBUG_EXECUTE_IF("ib_crash_after_adjust_one_fil_space",
				DBUG_SUICIDE(););

		fil_rename_tablespace(space->name, id, name, NULL);

		DBUG_EXECUTE_IF("ib_crash_after_adjust_fil_space",
				DBUG_SUICIDE(););

		mutex_enter(&fil_system->mutex);
		fnamespace = fil_space_get_by_name(name);
		ut_ad(space == fnamespace);
		mutex_exit(&fil_system->mutex);

		return(true);
	}

	if (!print_error_if_does_not_exist) {

		mutex_exit(&fil_system->mutex);

		return(false);
	}

	if (space == NULL) {
		if (fnamespace == NULL) {
			if (print_error_if_does_not_exist) {
				fil_report_missing_tablespace(name, id);
			}
		} else {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Table %s in InnoDB data dictionary has"
				" tablespace id %lu, but a tablespace with"
				" that id does not exist. There is a tablespace"
				" of name %s and id %lu, though. Have you"
				" deleted or moved .ibd files?",
				name, (ulong) id, fnamespace->name,
				(ulong) fnamespace->id);
		}
error_exit:
		ib_logf(IB_LOG_LEVEL_WARN, "%s", TROUBLESHOOT_DATADICT_MSG);

		mutex_exit(&fil_system->mutex);

		return(false);
	}

	if (0 != strcmp(space->name, name)) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Table %s in InnoDB data dictionary has tablespace id"
			" %lu, but the tablespace with that id has name %s."
			" Have you deleted or moved .ibd files?",
			name, (ulong) id, space->name);

		if (fnamespace != NULL) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"There is a tablespace with the right name:"
				" %s, but its id is %lu.",
				fnamespace->name, (ulong) fnamespace->id);
		}

		goto error_exit;
	}

	mutex_exit(&fil_system->mutex);

	return(false);
}

/*******************************************************************//**
Checks if a single-table tablespace for a given table name exists in the
tablespace memory cache.
@return space id, ULINT_UNDEFINED if not found */

ulint
fil_get_space_id_for_table(
/*=======================*/
	const char*	tablename)	/*!< in: table name in the standard
				'databasename/tablename' format */
{
	fil_space_t*	fnamespace;
	ulint		id		= ULINT_UNDEFINED;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	/* Look if there is a space with the same name. */

	fnamespace = fil_space_get_by_name(tablename);

	if (fnamespace) {
		id = fnamespace->id;
	}

	mutex_exit(&fil_system->mutex);

	return(id);
}

/**********************************************************************//**
Tries to extend a data file so that it would accommodate the number of pages
given. The tablespace must be cached in the memory cache. If the space is big
enough already, does nothing.
@return true if success */

bool
fil_extend_space_to_desired_size(
/*=============================*/
	ulint*	actual_size,	/*!< out: size of the space after extension;
				if we ran out of disk space this may be lower
				than the desired size */
	ulint	space_id,	/*!< in: space id */
	ulint	size_after_extend)/*!< in: desired size in pages after the
				extension; if the current space size is bigger
				than this already, the function does nothing */
{
	ut_ad(!srv_read_only_mode);

retry:
	bool		success = true;

	fil_mutex_enter_and_prepare_for_io(space_id);
	fil_space_t*	space = fil_space_get_by_id(space_id);

	if (space->size >= size_after_extend) {
		/* Space already big enough */

		*actual_size = space->size;

		mutex_exit(&fil_system->mutex);

		return(true);
	}

	const ulint	page_size = page_size_t(space->flags).physical();
	fil_node_t*	node = UT_LIST_GET_LAST(space->chain);

	if (!node->being_extended) {
		/* Mark this node as undergoing extension. This flag
		is used by other threads to wait for the extension
		opereation to finish. */
		node->being_extended = true;
	} else {
		/* Another thread is currently extending the file. Wait
		for it to finish.  It'd have been better to use an event
		driven mechanism but the entire module is peppered with
		polling code. */

		mutex_exit(&fil_system->mutex);
		os_thread_sleep(100000);
		goto retry;
	}

	if (!fil_node_prepare_for_io(node, fil_system, space)) {
		/* The tablespace data file, such as .ibd file, is missing */
		node->being_extended = false;
		mutex_exit(&fil_system->mutex);

		return(false);
	}

	/* At this point it is safe to release fil_system mutex. No
	other thread can rename, delete or close the file because
	we have set the node->being_extended flag. */
	mutex_exit(&fil_system->mutex);

	ulint		pages_added = 0;
	os_offset_t	start_page_no = space->size;

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
	/* Note: This code is going to be execute independent of FusionIO HW
	if the OS supports posix_fallocate() */
	os_offset_t	start = start_page_no * page_size;
	os_offset_t	end = (size_after_extend - start_page_no) * page_size;

	DBUG_EXECUTE_IF("ib_crash_during_tablespace_extension",
			DBUG_SUICIDE(););

	/* This is also required by FusionIO HW/Firmware */
	if (posix_fallocate(node->handle, start, end) == -1) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"posix_fallocate(): Failed to preallocate"
			" data for file %s, desired size %lu."
			" Operating system error number %lu. Check"
			" that the disk is not full or a disk quota"
			" exceeded. Some operating system error"
			" numbers are described at " REFMAN ""
			" operating-system-error-codes.html",
			node->name, (ulong) end, (ulong) errno);

		success = false;
	} else {
		success = true;
	}

	if (success) {
		pages_added = size_after_extend - start_page_no;
		os_has_said_disk_full = FALSE;
	}
#else
	byte*		buf;
	byte*		ptr;
	ulint		buf_size;
	os_offset_t	file_start_page_no = start_page_no - node->size;

	/* Extend at most 64 pages at a time */
	buf_size = ut_min(64, size_after_extend - start_page_no) * page_size;

	ptr = static_cast<byte*>(ut_malloc(buf_size + page_size));
	buf = static_cast<byte*>(ut_align(ptr, (ulint) page_size));

	::memset(buf, 0, buf_size);

	while (start_page_no < size_after_extend) {

		ulint	n_pages;

		n_pages = ut_min(
			buf_size / (ulint) page_size,
			size_after_extend - start_page_no);

		os_offset_t	offset;

		offset = os_offset_t(start_page_no - file_start_page_no);
		offset *= page_size;

#ifdef UNIV_HOTBACKUP
		success = os_file_write(node->name, node->handle, buf,
					offset, page_size * n_pages);
#else
		success = os_aio(OS_FILE_WRITE, OS_AIO_SYNC,
				 node->name, node->handle, buf,
				 offset, page_size * n_pages,
				 NULL, NULL);
#endif /* UNIV_HOTBACKUP */
		if (success) {
			os_has_said_disk_full = false;
		} else {
			/* Let us measure the size of the file to determine
			how much we were able to extend it */
			os_offset_t	size;

			size = os_file_get_size(node->handle);
			ut_a(size != (os_offset_t) -1);

			n_pages = ((ulint) (size / page_size))
				- node->size - pages_added;

			pages_added += n_pages;
			break;
		}

		start_page_no += n_pages;
		pages_added += n_pages;

		DBUG_EXECUTE_IF("ib_crash_during_tablespace_extension",
				DBUG_SUICIDE(););
	}

	::ut_free(ptr);
#endif /* NO_FALLOCATE || !UNIV_LINUX */

	mutex_enter(&fil_system->mutex);

	ut_a(node->being_extended);

	space->size += pages_added;
	node->size += pages_added;
	node->being_extended = false;

	fil_node_complete_io(node, fil_system, OS_FILE_WRITE);

	*actual_size = space->size;

#ifndef UNIV_HOTBACKUP
	/* Keep the last data file size info up to date, rounded to
	full megabytes */
	ulint	pages_per_mb = (1024 * 1024) / page_size;
	ulint	size_in_pages = ((node->size / pages_per_mb) * pages_per_mb);

	if (space_id == srv_sys_space.space_id()) {
		srv_sys_space.set_last_file_size(size_in_pages);
	} else if (space_id == srv_tmp_space.space_id()) {
		srv_tmp_space.set_last_file_size(size_in_pages);
	}
#endif /* !UNIV_HOTBACKUP */

	mutex_exit(&fil_system->mutex);

	fil_flush(space_id);

	return(success);
}

#ifdef UNIV_HOTBACKUP
/********************************************************************//**
Extends all tablespaces to the size stored in the space header. During the
ibbackup --apply-log phase we extended the spaces on-demand so that log records
could be applied, but that may have left spaces still too small compared to
the size stored in the space header. */

void
fil_extend_tablespaces_to_stored_len(void)
/*======================================*/
{
	byte*		buf;
	ulint		actual_size;
	ulint		size_in_header;
	dberr_t		error;
	bool		success;

	buf = ut_malloc(UNIV_PAGE_SIZE);

	mutex_enter(&fil_system->mutex);

	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {

		ut_a(space->purpose == FIL_TYPE_TABLESPACE);

		mutex_exit(&fil_system->mutex); /* no need to protect with a
					      mutex, because this is a
					      single-threaded operation */
		error = fil_read(
			page_id_t(space->id, 0),
			page_size_t(space->flags),
			0, univ_page_size.physical(), buf);

		ut_a(error == DB_SUCCESS);

		size_in_header = fsp_get_size_low(buf);

		success = fil_extend_space_to_desired_size(
			&actual_size, space->id, size_in_header);
		if (!success) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Could not extend the tablespace of %s"
				" to the size stored in header, %lu pages;"
				" size after extension %lu pages. Check that"
				" you have free disk space and retry!",
				space->name, size_in_header, actual_size);
			ut_a(success);
		}

		mutex_enter(&fil_system->mutex);
	}

	mutex_exit(&fil_system->mutex);

	::ut_free(buf);
}
#endif

/*========== RESERVE FREE EXTENTS (for a B-tree split, for example) ===*/

/*******************************************************************//**
Tries to reserve free extents in a file space.
@return true if succeed */

bool
fil_space_reserve_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_free_now,	/*!< in: number of free extents now */
	ulint	n_to_reserve)	/*!< in: how many one wants to reserve */
{
	fil_space_t*	space;
	bool		success;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	if (space->n_reserved_extents + n_to_reserve > n_free_now) {
		success = false;
	} else {
		space->n_reserved_extents += n_to_reserve;
		success = true;
	}

	mutex_exit(&fil_system->mutex);

	return(success);
}

/*******************************************************************//**
Releases free extents in a file space. */

void
fil_space_release_free_extents(
/*===========================*/
	ulint	id,		/*!< in: space id */
	ulint	n_reserved)	/*!< in: how many one reserved */
{
	fil_space_t*	space;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);
	ut_a(space->n_reserved_extents >= n_reserved);

	space->n_reserved_extents -= n_reserved;

	mutex_exit(&fil_system->mutex);
}

/*******************************************************************//**
Gets the number of reserved extents. If the database is silent, this number
should be zero. */

ulint
fil_space_get_n_reserved_extents(
/*=============================*/
	ulint	id)		/*!< in: space id */
{
	fil_space_t*	space;
	ulint		n;

	ut_ad(fil_system);

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(id);

	ut_a(space);

	n = space->n_reserved_extents;

	mutex_exit(&fil_system->mutex);

	return(n);
}

/*============================ FILE I/O ================================*/

/********************************************************************//**
NOTE: you must call fil_mutex_enter_and_prepare_for_io() first!

Prepares a file node for i/o. Opens the file if it is closed. Updates the
pending i/o's field in the node and the system appropriately. Takes the node
off the LRU list if it is in the LRU list. The caller must hold the fil_sys
mutex.
@return false if the file can't be opened, otherwise true */
static
bool
fil_node_prepare_for_io(
/*====================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	fil_space_t*	space)	/*!< in: space */
{
	ut_ad(node && system && space);
	ut_ad(mutex_own(&(system->mutex)));

	if (system->n_open > system->max_n_open + 5) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Open files %lu exceeds the limit %lu",
			ulong(system->n_open),
			ulong(system->max_n_open));
	}

	if (!node->is_open) {
		/* File is closed: open it */
		ut_a(node->n_pending == 0);

		if (!fil_node_open_file(node, system, space)) {
			return(false);
		}
	}

	if (node->n_pending == 0 && fil_space_belongs_in_lru(space)) {
		/* The node is in the LRU list, remove it */

		ut_a(UT_LIST_GET_LEN(system->LRU) > 0);

		UT_LIST_REMOVE(system->LRU, node);
	}

	node->n_pending++;

	return(true);
}

/********************************************************************//**
Updates the data structures when an i/o operation finishes. Updates the
pending i/o's field in the node appropriately. */
static
void
fil_node_complete_io(
/*=================*/
	fil_node_t*	node,	/*!< in: file node */
	fil_system_t*	system,	/*!< in: tablespace memory cache */
	ulint		type)	/*!< in: OS_FILE_WRITE or OS_FILE_READ; marks
				the node as modified if
				type == OS_FILE_WRITE */
{
	ut_ad(mutex_own(&system->mutex));
	ut_a(node->n_pending > 0);

	--node->n_pending;

	if (type == OS_FILE_WRITE) {
		ut_ad(!srv_read_only_mode);
		system->modification_counter++;
		node->modification_counter = system->modification_counter;

		if (fil_buffering_disabled(node->space)) {

			/* We don't need to keep track of unflushed
			changes as user has explicitly disabled
			buffering. */
			ut_ad(!node->space->is_in_unflushed_spaces);
			node->flush_counter = node->modification_counter;

		} else if (!node->space->is_in_unflushed_spaces) {

			node->space->is_in_unflushed_spaces = true;

			UT_LIST_ADD_FIRST(
				system->unflushed_spaces, node->space);
		}
	}

	if (node->n_pending == 0 && fil_space_belongs_in_lru(node->space)) {

		/* The node must be put back to the LRU list */
		UT_LIST_ADD_FIRST(system->LRU, node);
	}
}

/********************************************************************//**
Report information about an invalid page access. */
static
void
fil_report_invalid_page_access(
/*===========================*/
	ulint		block_offset,	/*!< in: block offset */
	ulint		space_id,	/*!< in: space id */
	const char*	space_name,	/*!< in: space name */
	ulint		byte_offset,	/*!< in: byte offset */
	ulint		len,		/*!< in: I/O length */
	ulint		type)		/*!< in: I/O type */
{
	ib_logf(IB_LOG_LEVEL_ERROR,
		"Trying to access page number %lu in space %lu, space name %s,"
		" which is outside the tablespace bounds. Byte offset %lu,"
		" len %lu, i/o type %lu. If you get this error at mysqld"
		" startup, please check that your my.cnf matches the ibdata"
		" files that you have in the MySQL server.",
		(ulong) block_offset, (ulong) space_id, space_name,
		(ulong) byte_offset, (ulong) len, (ulong) type);
}

/** Reads or writes data. This operation could be asynchronous (aio).
@param[in]	type		OS_FILE_READ or OS_FILE_WRITE, ORed to
OS_FILE_LOG, if a log i/o and ORed to OS_AIO_SIMULATED_WAKE_LATER if
simulated aio and we want to post a batch of IOs; NOTE that a simulated
batch may introduce hidden chances of deadlocks, because IOs are not
actually handled until all have been posted: use with great caution!
@param[in]	sync		true if synchronous aio is desired
@param[in]	page_id		page id
@param[in]	page_size	page size
@param[in]	byte_offset	remainder of offset in bytes; in aio this
must be divisible by the OS block size
@param[in]	len		how many bytes to read or write; this must
not cross a file boundary; in aio this must be a block size multiple
@param[in,out]	buf		buffer where to store read data or from where
to write; in aio this must be appropriately aligned
@param[in]	message		message for aio handler if non-sync aio used,
else ignored
@return DB_SUCCESS, DB_TABLESPACE_DELETED or DB_TABLESPACE_TRUNCATED
if we are trying to do i/o on a tablespace which does not exist */
dberr_t
fil_io(
	ulint			type,
	bool			sync,
	const page_id_t&	page_id,
	const page_size_t&	page_size,
	ulint			byte_offset,
	ulint			len,
	void*			buf,
	void*			message)
{
	ulint		mode;
	fil_space_t*	space;
	fil_node_t*	node;
	bool		ret;
	ulint		is_log;
	ulint		wake_later;
	os_offset_t	offset;
	ulint		ignore_nonexistent_pages;

	is_log = type & OS_FILE_LOG;
	type = type & ~OS_FILE_LOG;

	wake_later = type & OS_AIO_SIMULATED_WAKE_LATER;
	type = type & ~OS_AIO_SIMULATED_WAKE_LATER;

	ignore_nonexistent_pages = type & BUF_READ_IGNORE_NONEXISTENT_PAGES;
	type &= ~BUF_READ_IGNORE_NONEXISTENT_PAGES;

	ut_ad(byte_offset < UNIV_PAGE_SIZE);
	ut_ad(!page_size.is_compressed() || byte_offset == 0);
	ut_ad(buf);
	ut_ad(len > 0);
	ut_ad(UNIV_PAGE_SIZE == (ulong)(1 << UNIV_PAGE_SIZE_SHIFT));
#if (1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MAX) != UNIV_PAGE_SIZE_MAX"
#endif
#if (1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN
# error "(1 << UNIV_PAGE_SIZE_SHIFT_MIN) != UNIV_PAGE_SIZE_MIN"
#endif
	ut_ad(fil_validate_skip());
#ifndef UNIV_HOTBACKUP
	/* ibuf bitmap pages must be read in the sync aio mode: */
	ut_ad(recv_no_ibuf_operations
	      || type == OS_FILE_WRITE
	      || !ibuf_bitmap_page(page_id, page_size)
	      || sync
	      || is_log);
	if (sync) {
		mode = OS_AIO_SYNC;
	} else if (is_log) {
		mode = OS_AIO_LOG;
	} else if (type == OS_FILE_READ
		   && !recv_no_ibuf_operations
		   && ibuf_page(page_id, page_size, NULL)) {
		mode = OS_AIO_IBUF;
	} else {
		mode = OS_AIO_NORMAL;
	}
#else /* !UNIV_HOTBACKUP */
	ut_a(sync);
	mode = OS_AIO_SYNC;
#endif /* !UNIV_HOTBACKUP */

	if (type == OS_FILE_READ) {
		srv_stats.data_read.add(len);
	} else if (type == OS_FILE_WRITE) {
		ut_ad(!srv_read_only_mode);
		srv_stats.data_written.add(len);
	}

	/* Reserve the fil_system mutex and make sure that we can open at
	least one file while holding it, if the file is not already open */

	fil_mutex_enter_and_prepare_for_io(page_id.space());

	space = fil_space_get_by_id(page_id.space());

	/* If we are deleting a tablespace we don't allow any read
	operations on that. However, we do allow write operations. */
	if (space == NULL
	    || (type == OS_FILE_READ
		&& space->stop_new_ops && !space->is_being_truncated)) {
		mutex_exit(&fil_system->mutex);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to do i/o to a tablespace which does "
			"not exist. i/o type %lu, space id " UINT32PF ", "
			"page no. " UINT32PF ", i/o length %lu bytes",
			(ulong) type, page_id.space(), page_id.page_no(),
			(ulong) len);

		return(DB_TABLESPACE_DELETED);
	}

	ut_ad(mode != OS_AIO_IBUF
	      || space->purpose == FIL_TYPE_TABLESPACE
	      || space->purpose == FIL_TYPE_TEMPORARY);

	node = UT_LIST_GET_FIRST(space->chain);

	ulint	cur_page_no = page_id.page_no();

	for (;;) {
		if (node == NULL) {
			if (ignore_nonexistent_pages != 0) {
				mutex_exit(&fil_system->mutex);
				return(DB_ERROR);
			}

			fil_report_invalid_page_access(
				cur_page_no, page_id.space(),
				space->name, byte_offset, len, type);

			ut_error;

		} else if (fil_is_user_tablespace_id(space->id)
			   && node->size == 0) {

			/* We do not know the size of a single-table tablespace
			before we open the file */
			break;
		} else if (node->size > cur_page_no) {
			/* Found! */
			break;
		} else {
			if (space->id != srv_sys_space.space_id()
			    && UT_LIST_GET_LEN(space->chain) == 1
			    && (srv_is_tablespace_truncated(space->id)
				|| space->is_being_truncated)
			    && type == OS_FILE_READ) {
				/* Handle page which is outside the truncated
				tablespace bounds when recovering from a crash
				happened during a truncation */
				mutex_exit(&fil_system->mutex);
				return(DB_TABLESPACE_TRUNCATED);
			}

			cur_page_no -= node->size;

			node = UT_LIST_GET_NEXT(chain, node);
		}
	}

	/* Open file if closed */
	if (!fil_node_prepare_for_io(node, fil_system, space)) {
		if ((space->purpose == FIL_TYPE_TABLESPACE
		     || space->purpose == FIL_TYPE_TEMPORARY)
		    && fil_is_user_tablespace_id(space->id)) {
			mutex_exit(&fil_system->mutex);

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Trying to do i/o to a tablespace which "
				"exists without .ibd data file. "
				"i/o type %lu, space id " UINT32PF
				", page no " ULINTPF ", "
				"i/o length %lu bytes",
				(ulint) type,
				page_id.space(),
				cur_page_no,
				(ulint) len);

			return(DB_TABLESPACE_DELETED);
		}

		/* The tablespace is for log. Currently, we just assert here
		to prevent handling errors along the way fil_io returns.
		Also, if the log files are missing, it would be hard to
		promise the server can continue running. */
		ut_a(0);
	}

	/* Check that at least the start offset is within the bounds of a
	single-table tablespace, including rollback tablespaces. */
	if (node->size <= cur_page_no
	    && space->id != 0
	    && (space->purpose == FIL_TYPE_TABLESPACE
		|| space->purpose == FIL_TYPE_TEMPORARY)) {

		fil_report_invalid_page_access(
			cur_page_no, page_id.space(),
			space->name, byte_offset, len, type);

		ut_error;
	}

	/* Now we have made the changes in the data structures of fil_system */
	mutex_exit(&fil_system->mutex);

	/* Calculate the low 32 bits and the high 32 bits of the file offset */

	if (!page_size.is_compressed()) {
		offset = ((os_offset_t) cur_page_no
			  << UNIV_PAGE_SIZE_SHIFT) + byte_offset;

		ut_a(node->size - cur_page_no
		     >= ((byte_offset + len + (UNIV_PAGE_SIZE - 1))
			 / UNIV_PAGE_SIZE));
	} else {
		ulint	size_shift;

		switch (page_size.physical()) {
		case 1024: size_shift = 10; break;
		case 2048: size_shift = 11; break;
		case 4096: size_shift = 12; break;
		case 8192: size_shift = 13; break;
		case 16384: size_shift = 14; break;
		default: ut_error;
		}

		offset = ((os_offset_t) cur_page_no << size_shift)
			+ byte_offset;

		ut_a(node->size - cur_page_no
		     >= (len + (page_size.physical() - 1))
		     / page_size.physical());
	}

	/* Do aio */

	ut_a(byte_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a((len % OS_FILE_LOG_BLOCK_SIZE) == 0);

#ifdef UNIV_HOTBACKUP
	/* In ibbackup do normal i/o, not aio */
	if (type == OS_FILE_READ) {
		ret = os_file_read(node->handle, buf, offset, len);
	} else {
		ut_ad(!srv_read_only_mode);
		ret = os_file_write(node->name, node->handle, buf,
				    offset, len);
	}
#else
	/* Queue the aio request */
	ret = os_aio(type, mode | wake_later, node->name, node->handle, buf,
		     offset, len, node, message);
#endif /* UNIV_HOTBACKUP */
	ut_a(ret);

	if (mode == OS_AIO_SYNC) {
		/* The i/o operation is already completed when we return from
		os_aio: */

		mutex_enter(&fil_system->mutex);

		fil_node_complete_io(node, fil_system, type);

		mutex_exit(&fil_system->mutex);

		ut_ad(fil_validate_skip());
	}

	return(DB_SUCCESS);
}

#ifndef UNIV_HOTBACKUP
/**********************************************************************//**
Waits for an aio operation to complete. This function is used to write the
handler for completed requests. The aio array of pending requests is divided
into segments (see os0file.cc for more info). The thread specifies which
segment it wants to wait for. */

void
fil_aio_wait(
/*=========*/
	ulint	segment)	/*!< in: the number of the segment in the aio
				array to wait for */
{
	bool		ret;
	fil_node_t*	fil_node;
	void*		message;
	ulint		type;

	ut_ad(fil_validate_skip());

	if (srv_use_native_aio) {
		srv_set_io_thread_op_info(segment, "native aio handle");
#ifdef WIN_ASYNC_IO
		ret = os_aio_windows_handle(
			segment, 0, &fil_node, &message, &type);
#elif defined(LINUX_NATIVE_AIO)
		ret = os_aio_linux_handle(
			segment, &fil_node, &message, &type);
#else
		ut_error;
		ret = 0; /* Eliminate compiler warning */
#endif /* WIN_ASYNC_IO */
	} else {
		srv_set_io_thread_op_info(segment, "simulated aio handle");

		ret = os_aio_simulated_handle(
			segment, &fil_node, &message, &type);
	}

	ut_a(ret);
	if (fil_node == NULL) {
		ut_ad(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
		return;
	}

	srv_set_io_thread_op_info(segment, "complete io for fil node");

	mutex_enter(&fil_system->mutex);

	fil_node_complete_io(fil_node, fil_system, type);

	mutex_exit(&fil_system->mutex);

	ut_ad(fil_validate_skip());

	/* Do the i/o handling */
	/* IMPORTANT: since i/o handling for reads will read also the insert
	buffer in tablespace 0, you have to be very careful not to introduce
	deadlocks in the i/o system. We keep tablespace 0 data files always
	open, and use a special i/o thread to serve insert buffer requests. */

	switch (fil_node->space->purpose) {
	case FIL_TYPE_TABLESPACE:
	case FIL_TYPE_TEMPORARY:
		srv_set_io_thread_op_info(segment, "complete io for buf page");
		buf_page_io_complete(static_cast<buf_page_t*>(message));
		return;
	case FIL_TYPE_LOG:
		srv_set_io_thread_op_info(segment, "complete io for log");
		log_io_complete(static_cast<log_group_t*>(message));
		return;
	}

	ut_ad(0);
}
#endif /* UNIV_HOTBACKUP */

/**********************************************************************//**
Flushes to disk possible writes cached by the OS. If the space does not exist
or is being dropped, does not do anything. */

void
fil_flush(
/*======*/
	ulint	space_id)	/*!< in: file space id (this can be a group of
				log files or a tablespace of the database) */
{
	fil_space_t*	space;
	fil_node_t*	node;
	os_file_t	file;

	mutex_enter(&fil_system->mutex);

	space = fil_space_get_by_id(space_id);

	if (!space
	    || space->purpose == FIL_TYPE_TEMPORARY
	    || space->stop_new_ops
	    || space->is_being_truncated) {
		mutex_exit(&fil_system->mutex);

		return;
	}

	if (fil_buffering_disabled(space)) {

		/* No need to flush. User has explicitly disabled
		buffering. */
		ut_ad(!space->is_in_unflushed_spaces);
		ut_ad(fil_space_is_flushed(space));
		ut_ad(space->n_pending_flushes == 0);

#ifdef UNIV_DEBUG
		for (node = UT_LIST_GET_FIRST(space->chain);
		     node != NULL;
		     node = UT_LIST_GET_NEXT(chain, node)) {
			ut_ad(node->modification_counter
			      == node->flush_counter);
			ut_ad(node->n_pending_flushes == 0);
		}
#endif /* UNIV_DEBUG */

		mutex_exit(&fil_system->mutex);
		return;
	}

	space->n_pending_flushes++;	/*!< prevent dropping of the space while
					we are flushing */
	for (node = UT_LIST_GET_FIRST(space->chain);
	     node != NULL;
	     node = UT_LIST_GET_NEXT(chain, node)) {

		ib_int64_t old_mod_counter = node->modification_counter;;

		if (old_mod_counter <= node->flush_counter) {
			continue;
		}

		ut_a(node->is_open);

		switch (space->purpose) {
		case FIL_TYPE_TEMPORARY:
			ut_ad(0); // we already checked for this
		case FIL_TYPE_TABLESPACE:
			fil_n_pending_tablespace_flushes++;
			break;
		case FIL_TYPE_LOG:
			fil_n_pending_log_flushes++;
			fil_n_log_flushes++;
			break;
		}
#ifdef _WIN32
		if (node->is_raw_disk) {

			goto skip_flush;
		}
#endif /* _WIN32 */
retry:
		if (node->n_pending_flushes > 0) {
			/* We want to avoid calling os_file_flush() on
			the file twice at the same time, because we do
			not know what bugs OS's may contain in file
			i/o */

			ib_int64_t sig_count =
				os_event_reset(node->sync_event);

			mutex_exit(&fil_system->mutex);

			os_event_wait_low(node->sync_event, sig_count);

			mutex_enter(&fil_system->mutex);

			if (node->flush_counter >= old_mod_counter) {

				goto skip_flush;
			}

			goto retry;
		}

		ut_a(node->is_open);
		file = node->handle;
		node->n_pending_flushes++;

		mutex_exit(&fil_system->mutex);

		os_file_flush(file);

		mutex_enter(&fil_system->mutex);

		os_event_set(node->sync_event);

		node->n_pending_flushes--;
skip_flush:
		if (node->flush_counter < old_mod_counter) {
			node->flush_counter = old_mod_counter;

			if (space->is_in_unflushed_spaces
			    && fil_space_is_flushed(space)) {

				space->is_in_unflushed_spaces = false;

				UT_LIST_REMOVE(
					fil_system->unflushed_spaces,
					space);
			}
		}

		switch (space->purpose) {
		case FIL_TYPE_TEMPORARY:
			ut_ad(0); // we already checked for this
		case FIL_TYPE_TABLESPACE:
			fil_n_pending_tablespace_flushes--;
			continue;
		case FIL_TYPE_LOG:
			fil_n_pending_log_flushes--;
			continue;
		}

		ut_ad(0);
	}

	space->n_pending_flushes--;

	mutex_exit(&fil_system->mutex);
}

/** Flush to disk the writes in file spaces of the given type
possibly cached by the OS.
@param[in]	purpose	FIL_TYPE_TABLESPACE or FIL_TYPE_LOG */

void
fil_flush_file_spaces(
	fil_type_t	purpose)
{
	fil_space_t*	space;
	ulint*		space_ids;
	ulint		n_space_ids;

	ut_ad(purpose == FIL_TYPE_TABLESPACE || purpose == FIL_TYPE_LOG);

	mutex_enter(&fil_system->mutex);

	n_space_ids = UT_LIST_GET_LEN(fil_system->unflushed_spaces);
	if (n_space_ids == 0) {

		mutex_exit(&fil_system->mutex);
		return;
	}

	/* Assemble a list of space ids to flush.  Previously, we
	traversed fil_system->unflushed_spaces and called UT_LIST_GET_NEXT()
	on a space that was just removed from the list by fil_flush().
	Thus, the space could be dropped and the memory overwritten. */
	space_ids = static_cast<ulint*>(
		ut_malloc(n_space_ids * sizeof *space_ids));

	n_space_ids = 0;

	for (space = UT_LIST_GET_FIRST(fil_system->unflushed_spaces);
	     space;
	     space = UT_LIST_GET_NEXT(unflushed_spaces, space)) {

		if (space->purpose == purpose
		    && !space->stop_new_ops
		    && !space->is_being_truncated) {

			space_ids[n_space_ids++] = space->id;
		}
	}

	mutex_exit(&fil_system->mutex);

	/* Flush the spaces.  It will not hurt to call fil_flush() on
	a non-existing space id. */
	for (ulint i = 0; i < n_space_ids; i++) {

		fil_flush(space_ids[i]);
	}

	::ut_free(space_ids);
}

/** Functor to validate the space list. */
struct	Check {
	void	operator()(const fil_node_t* elem)
	{
		ut_a(elem->is_open || !elem->n_pending);
	}
};

/******************************************************************//**
Checks the consistency of the tablespace cache.
@return true if ok */

bool
fil_validate(void)
/*==============*/
{
	fil_space_t*	space;
	fil_node_t*	fil_node;
	ulint		n_open		= 0;

	mutex_enter(&fil_system->mutex);

	/* Look for spaces in the hash table */

	for (ulint i = 0; i < hash_get_n_cells(fil_system->spaces); i++) {

		for (space = static_cast<fil_space_t*>(
				HASH_GET_FIRST(fil_system->spaces, i));
		     space != 0;
		     space = static_cast<fil_space_t*>(
				HASH_GET_NEXT(hash, space))) {

			UT_LIST_VALIDATE(space->chain, Check());

			for (fil_node = UT_LIST_GET_FIRST(space->chain);
			     fil_node != 0;
			     fil_node = UT_LIST_GET_NEXT(chain, fil_node)) {

				if (fil_node->n_pending > 0) {
					ut_a(fil_node->is_open);
				}

				if (fil_node->is_open) {
					n_open++;
				}
			}
		}
	}

	ut_a(fil_system->n_open == n_open);

	UT_LIST_CHECK(fil_system->LRU);

	for (fil_node = UT_LIST_GET_FIRST(fil_system->LRU);
	     fil_node != 0;
	     fil_node = UT_LIST_GET_NEXT(LRU, fil_node)) {

		ut_a(fil_node->n_pending == 0);
		ut_a(!fil_node->being_extended);
		ut_a(fil_node->is_open);
		ut_a(fil_space_belongs_in_lru(fil_node->space));
	}

	mutex_exit(&fil_system->mutex);

	return(true);
}

/********************************************************************//**
Returns true if file address is undefined.
@return true if undefined */

bool
fil_addr_is_null(
/*=============*/
	fil_addr_t	addr)	/*!< in: address */
{
	return(addr.page == FIL_NULL);
}

/********************************************************************//**
Get the predecessor of a file page.
@return FIL_PAGE_PREV */

ulint
fil_page_get_prev(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	return(mach_read_from_4(page + FIL_PAGE_PREV));
}

/********************************************************************//**
Get the successor of a file page.
@return FIL_PAGE_NEXT */

ulint
fil_page_get_next(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	return(mach_read_from_4(page + FIL_PAGE_NEXT));
}

/*********************************************************************//**
Sets the file page type. */

void
fil_page_set_type(
/*==============*/
	byte*	page,	/*!< in/out: file page */
	ulint	type)	/*!< in: type */
{
	ut_ad(page);

	mach_write_to_2(page + FIL_PAGE_TYPE, type);
}

/*********************************************************************//**
Gets the file page type.
@return type; NOTE that if the type has not been written to page, the
return value not defined */

ulint
fil_page_get_type(
/*==============*/
	const byte*	page)	/*!< in: file page */
{
	ut_ad(page);

	return(mach_read_from_2(page + FIL_PAGE_TYPE));
}

/****************************************************************//**
Closes the tablespace memory cache. */

void
fil_close(void)
/*===========*/
{
	hash_table_free(fil_system->spaces);

	hash_table_free(fil_system->name_hash);

	ut_a(UT_LIST_GET_LEN(fil_system->LRU) == 0);
	ut_a(UT_LIST_GET_LEN(fil_system->unflushed_spaces) == 0);
	ut_a(UT_LIST_GET_LEN(fil_system->space_list) == 0);

	mutex_free(&fil_system->mutex);

	::ut_free(fil_system);

	fil_system = NULL;
}

/********************************************************************//**
Initializes a buffer control block when the buf_pool is created. */
static
void
fil_buf_block_init(
/*===============*/
	buf_block_t*	block,		/*!< in: pointer to control block */
	byte*		frame)		/*!< in: pointer to buffer frame */
{
	UNIV_MEM_DESC(frame, UNIV_PAGE_SIZE);

	block->frame = frame;

	block->page.io_fix = BUF_IO_NONE;
	/* There are assertions that check for this. */
	block->page.buf_fix_count = 1;
	block->page.state = BUF_BLOCK_READY_FOR_USE;

	page_zip_des_init(&block->page.zip);
}

struct fil_iterator_t {
	os_file_t	file;			/*!< File handle */
	const char*	filepath;		/*!< File path name */
	os_offset_t	start;			/*!< From where to start */
	os_offset_t	end;			/*!< Where to stop */
	os_offset_t	file_size;		/*!< File size in bytes */
	ulint		page_size;		/*!< Page size */
	ulint		n_io_buffers;		/*!< Number of pages to use
						for IO */
	byte*		io_buffer;		/*!< Buffer to use for IO */
};

/********************************************************************//**
TODO: This can be made parallel trivially by chunking up the file and creating
a callback per thread. . Main benefit will be to use multiple CPUs for
checksums and compressed tables. We have to do compressed tables block by
block right now. Secondly we need to decompress/compress and copy too much
of data. These are CPU intensive.

Iterate over all the pages in the tablespace.
@param iter Tablespace iterator
@param block block to use for IO
@param callback Callback to inspect and update page contents
@retval DB_SUCCESS or error code */
static
dberr_t
fil_iterate(
/*========*/
	const fil_iterator_t&	iter,
	buf_block_t*		block,
	PageCallback&		callback)
{
	os_offset_t		offset;
	ulint			page_no = 0;
	ulint			space_id = callback.get_space_id();
	ulint			n_bytes = iter.n_io_buffers * iter.page_size;

	ut_ad(!srv_read_only_mode);

	/* TODO: For compressed tables we do a lot of useless
	copying for non-index pages. Unfortunately, it is
	required by buf_zip_decompress() */

	for (offset = iter.start; offset < iter.end; offset += n_bytes) {

		byte*		io_buffer = iter.io_buffer;

		block->frame = io_buffer;

		if (callback.get_page_size().is_compressed()) {
			page_zip_des_init(&block->page.zip);
			page_zip_set_size(&block->page.zip, iter.page_size);

			block->page.size.copy_from(
				page_size_t(iter.page_size,
					    univ_page_size.logical(),
					    true));

			block->page.zip.data = block->frame + UNIV_PAGE_SIZE;
			ut_d(block->page.zip.m_external = true);
			ut_ad(iter.page_size
			      == callback.get_page_size().physical());

			/* Zip IO is done in the compressed page buffer. */
			io_buffer = block->page.zip.data;
		} else {
			io_buffer = iter.io_buffer;
		}

		/* We have to read the exact number of bytes. Otherwise the
		InnoDB IO functions croak on failed reads. */

		n_bytes = static_cast<ulint>(
			ut_min(static_cast<os_offset_t>(n_bytes),
			       iter.end - offset));

		ut_ad(n_bytes > 0);
		ut_ad(!(n_bytes % iter.page_size));

		if (!os_file_read(iter.file, io_buffer, offset,
				  (ulint) n_bytes)) {

			ib_logf(IB_LOG_LEVEL_ERROR, "os_file_read() failed");

			return(DB_IO_ERROR);
		}

		bool		updated = false;
		os_offset_t	page_off = offset;
		ulint		n_pages_read = (ulint) n_bytes / iter.page_size;

		for (ulint i = 0; i < n_pages_read; ++i) {

			buf_block_set_file_page(
				block, page_id_t(space_id, page_no++));

			dberr_t	err;

			if ((err = callback(page_off, block)) != DB_SUCCESS) {

				return(err);

			} else if (!updated) {
				updated = buf_block_get_state(block)
					== BUF_BLOCK_FILE_PAGE;
			}

			buf_block_set_state(block, BUF_BLOCK_NOT_USED);
			buf_block_set_state(block, BUF_BLOCK_READY_FOR_USE);

			page_off += iter.page_size;
			block->frame += iter.page_size;
		}

		/* A page was updated in the set, write back to disk. */
		if (updated
		    && !os_file_write(
				iter.filepath, iter.file, io_buffer,
				offset, (ulint) n_bytes)) {

			ib_logf(IB_LOG_LEVEL_ERROR, "os_file_write() failed");

			return(DB_IO_ERROR);
		}
	}

	return(DB_SUCCESS);
}

/********************************************************************//**
Iterate over all the pages in the tablespace.
@param table the table definiton in the server
@param n_io_buffers number of blocks to read and write together
@param callback functor that will do the page updates
@return DB_SUCCESS or error code */

dberr_t
fil_tablespace_iterate(
/*===================*/
	dict_table_t*	table,
	ulint		n_io_buffers,
	PageCallback&	callback)
{
	dberr_t		err;
	os_file_t	file;
	char*		filepath;
	bool		success;

	ut_a(n_io_buffers > 0);
	ut_ad(!srv_read_only_mode);

	DBUG_EXECUTE_IF("ib_import_trigger_corruption_1",
			return(DB_CORRUPTION););

	/* Make sure the data_dir_path is set. */
	dict_get_and_save_data_dir_path(table, false);

	if (DICT_TF_HAS_DATA_DIR(table->flags)) {
		ut_a(table->data_dir_path);

		filepath = fil_make_filepath(
			table->data_dir_path, table->name, IBD, true);
	} else {
		filepath = fil_make_filepath(
			NULL, table->name, IBD, false);
	}

	if (filepath == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	file = os_file_create_simple_no_error_handling(
		innodb_data_file_key, filepath,
		OS_FILE_OPEN, OS_FILE_READ_WRITE, &success);

	DBUG_EXECUTE_IF("fil_tablespace_iterate_failure",
	{
		static bool once;

		if (!once || ut_rnd_interval(0, 10) == 5) {
			once = true;
			success = false;
			os_file_close(file);
		}
	});

	if (!success) {
		/* The following call prints an error message */
		os_file_get_last_error(true);

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Trying to import a tablespace, but could not"
			" open the tablespace file %s", filepath);

		::ut_free(filepath);

		return(DB_TABLESPACE_NOT_FOUND);

	} else {
		err = DB_SUCCESS;
	}

	callback.set_file(filepath, file);

	os_offset_t	file_size = os_file_get_size(file);
	ut_a(file_size != (os_offset_t) -1);

	/* The block we will use for every physical page */
	buf_block_t*	block;

	block = reinterpret_cast<buf_block_t*>(ut_zalloc(sizeof(*block)));

	mutex_create("buf_block_mutex", &block->mutex);

	/* Allocate a page to read in the tablespace header, so that we
	can determine the page size and zip size (if it is compressed).
	We allocate an extra page in case it is a compressed table. One
	page is to ensure alignement. */

	void*	page_ptr = ut_malloc(3 * UNIV_PAGE_SIZE);
	byte*	page = static_cast<byte*>(ut_align(page_ptr, UNIV_PAGE_SIZE));

	fil_buf_block_init(block, page);

	/* Read the first page and determine the page and zip size. */

	if (!os_file_read(file, page, 0, UNIV_PAGE_SIZE)) {

		err = DB_IO_ERROR;

	} else if ((err = callback.init(file_size, block)) == DB_SUCCESS) {
		fil_iterator_t	iter;

		iter.file = file;
		iter.start = 0;
		iter.end = file_size;
		iter.filepath = filepath;
		iter.file_size = file_size;
		iter.n_io_buffers = n_io_buffers;
		iter.page_size = callback.get_page_size().physical();

		/* Compressed pages can't be optimised for block IO for now.
		We do the IMPORT page by page. */

		if (callback.get_page_size().is_compressed()) {
			iter.n_io_buffers = 1;
			ut_a(iter.page_size
			     == callback.get_page_size().physical());
		}

		/** Add an extra page for compressed page scratch area. */

		void*	io_buffer = ut_malloc(
			(2 + iter.n_io_buffers) * UNIV_PAGE_SIZE);

		iter.io_buffer = static_cast<byte*>(
			ut_align(io_buffer, UNIV_PAGE_SIZE));

		err = fil_iterate(iter, block, callback);

		::ut_free(io_buffer);
	}

	if (err == DB_SUCCESS) {

		ib_logf(IB_LOG_LEVEL_INFO, "Sync to disk");

		if (!os_file_flush(file)) {
			ib_logf(IB_LOG_LEVEL_INFO, "os_file_flush() failed!");
			err = DB_IO_ERROR;
		} else {
			ib_logf(IB_LOG_LEVEL_INFO, "Sync to disk - done!");
		}
	}

	os_file_close(file);

	::ut_free(page_ptr);
	::ut_free(filepath);

	mutex_free(&block->mutex);

	::ut_free(block);

	return(err);
}

/** Set the tablespace table size.
@param[in]	page	a page belonging to the tablespace */
void
PageCallback::set_page_size(
	const buf_frame_t*	page) UNIV_NOTHROW
{
	m_page_size.copy_from(fsp_header_get_page_size(page));
}

/********************************************************************//**
Delete the tablespace file and any related files like .cfg.
This should not be called for temporary tables.
@param[in] ibd_filepath File path of the IBD tablespace */

void
fil_delete_file(
/*============*/
	const char*	ibd_filepath)
{
	/* Force a delete of any stale .ibd files that are lying around. */

	ib_logf(IB_LOG_LEVEL_INFO, "Deleting %s", ibd_filepath);

	os_file_delete_if_exists(innodb_data_file_key, ibd_filepath, NULL);

	char*	cfg_filepath = fil_make_filepath(
		ibd_filepath, NULL, CFG, false);
	if (cfg_filepath != NULL) {
		os_file_delete_if_exists(
			innodb_data_file_key, cfg_filepath, NULL);
		::ut_free(cfg_filepath);
	}
}

/**
Iterate over all the spaces in the space list and fetch the
tablespace names. It will return a copy of the name that must be
freed by the caller using: delete[].
@return DB_SUCCESS if all OK. */

dberr_t
fil_get_space_names(
/*================*/
	space_name_list_t&	space_name_list)
				/*!< in/out: List to append to */
{
	fil_space_t*	space;
	dberr_t		err = DB_SUCCESS;

	mutex_enter(&fil_system->mutex);

	for (space = UT_LIST_GET_FIRST(fil_system->space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space)) {

		if (space->purpose == FIL_TYPE_TABLESPACE) {
			ulint	len;
			char*	name;

			len = ::strlen(space->name);
			name = new(std::nothrow) char[len + 1];

			if (name == 0) {
				/* Caller to free elements allocated so far. */
				err = DB_OUT_OF_MEMORY;
				break;
			}

			memcpy(name, space->name, len);
			name[len] = 0;

			space_name_list.push_back(name);
		}
	}

	mutex_exit(&fil_system->mutex);

	return(err);
}

/****************************************************************//**
Generate redo logs for swapping two .ibd files */

void
fil_mtr_rename_log(
/*===============*/
	ulint		old_space_id,	/*!< in: tablespace id of the old
					table. */
	const char*	old_name,	/*!< in: old table name */
	ulint		new_space_id,	/*!< in: tablespace id of the new
					table */
	const char*	new_name,	/*!< in: new table name */
	const char*	tmp_name,	/*!< in: temp table name used while
					swapping */
	mtr_t*		mtr)		/*!< in/out: mini-transaction */
{
	if (!is_system_tablespace(old_space_id)) {
		fil_op_write_log(MLOG_FILE_RENAME, old_space_id,
				 0, 0, old_name, tmp_name, mtr);
	}

	if (!is_system_tablespace(new_space_id)) {
		fil_op_write_log(MLOG_FILE_RENAME, new_space_id,
				 0, 0, new_name, old_name, mtr);
	}
}

/**
Truncate a single-table tablespace. The tablespace must be cached
in the memory cache.
@param space_id			space id
@param dir_path			directory path
@param tablename		the table name in the usual
				databasename/tablename format of InnoDB
@param flags			tablespace flags
@param trunc_to_default		truncate to default size if tablespace
				is being newly re-initialized.
@return DB_SUCCESS or error */
dberr_t
truncate_t::truncate(
/*=================*/
	ulint		space_id,
	const char*	dir_path,
	const char*	tablename,
	ulint		flags,
	bool		trunc_to_default)
{
	dberr_t		err = DB_SUCCESS;
	char*		path;
	bool		has_data_dir = FSP_FLAGS_HAS_DATA_DIR(flags);

	ut_a(!is_system_tablespace(space_id));

	if (has_data_dir) {
		ut_ad(dir_path != NULL);

		path = fil_make_filepath(dir_path, tablename, IBD, true);

	} else {
		path = fil_make_filepath(NULL, tablename, IBD, false);
	}

	if (path == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	mutex_enter(&fil_system->mutex);

	fil_space_t*	space = fil_space_get_by_id(space_id);

	/* The following code must change when InnoDB supports
	multiple datafiles per tablespace. */
	ut_a(UT_LIST_GET_LEN(space->chain) == 1);

	fil_node_t*	node = UT_LIST_GET_FIRST(space->chain);

	if (trunc_to_default) {
		space->size = node->size = FIL_IBD_FILE_INITIAL_SIZE;
	}

	const bool already_open = node->is_open;

	if (!already_open) {

		bool	ret;

		node->handle = os_file_create_simple_no_error_handling(
			innodb_data_file_key, path, OS_FILE_OPEN,
			OS_FILE_READ_WRITE, &ret);

		if (!ret) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Failed to open tablespace file %s.", path);

			::ut_free(path);

			return(DB_ERROR);
		}

		node->is_open = true;
	}

	os_offset_t	trunc_size = trunc_to_default
		? FIL_IBD_FILE_INITIAL_SIZE
		: space->size;

	const bool success = os_file_truncate(
		path, node->handle, trunc_size * UNIV_PAGE_SIZE);

	if (!success) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot truncate file %s in TRUNCATE TABLESPACE.",
			path);

		err = DB_ERROR;
	}

	space->stop_new_ops = false;
	space->is_being_truncated = false;

	mutex_exit(&fil_system->mutex);

	/* If we opened the file in this function, close it. */
	if (!already_open) {
		bool	closed = os_file_close(node->handle);

		if (!closed) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Failed to close tablespace file %s.", path);

			err = DB_ERROR;
		} else {
			node->is_open = false;
		}
	}

	::ut_free(path);

	return(err);
}

/* Unit Tests */
#ifdef UNIV_COMPILE_TEST_FUNCS
#define MF  fil_make_filepath
#define DISPLAY ib_logf(IB_LOG_LEVEL_INFO, "%s", path)
void
test_make_filepath()
{
	char* path;
	const char* long_path =
		"this/is/a/very/long/path/including/a/very/"
		"looooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooooo"
		"oooooooooooooooooooooooooooooooooooooooooooooooong"
		"/folder/name";
	path = MF("/this/is/a/path/with/a/filename", NULL, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", NULL, ISL, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename", NULL, CFG, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.ibd", NULL, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.ibd", NULL, IBD, false); DISPLAY;
	path = MF("/this/is/a/path/with/a/filename.dat", NULL, IBD, false); DISPLAY;
	path = MF(NULL, "tablespacename", NO_EXT, false); DISPLAY;
	path = MF(NULL, "tablespacename", IBD, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", NO_EXT, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", IBD, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", ISL, false); DISPLAY;
	path = MF(NULL, "dbname/tablespacename", CFG, false); DISPLAY;
	path = MF(NULL, "dbname\\tablespacename", NO_EXT, false); DISPLAY;
	path = MF(NULL, "dbname\\tablespacename", IBD, false); DISPLAY;
	path = MF("/this/is/a/path", "dbname/tablespacename", IBD, false); DISPLAY;
	path = MF("/this/is/a/path", "dbname/tablespacename", IBD, true); DISPLAY;
	path = MF("./this/is/a/path", "dbname/tablespacename.ibd", IBD, true); DISPLAY;
	path = MF("this\\is\\a\\path", "dbname/tablespacename", IBD, true); DISPLAY;
	path = MF("/this/is/a/path", "dbname\\tablespacename", IBD, true); DISPLAY;
	path = MF(long_path, NULL, IBD, false); DISPLAY;
	path = MF(long_path, "tablespacename", IBD, false); DISPLAY;
	path = MF(long_path, "tablespacename", IBD, true); DISPLAY;
}
#endif /* UNIV_COMPILE_TEST_FUNCS */
/* @} */
