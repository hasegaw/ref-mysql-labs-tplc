/***********************************************************************

Copyright (c) 1995, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Percona Inc.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

***********************************************************************/

/**************************************************//**
@file include/os0file.h
The interface to the operating system file io

Created 10/21/1995 Heikki Tuuri
*******************************************************/

#ifndef os0file_h
#define os0file_h

#include "univ.i"

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#endif /* !_WIN32 */

/** File node of a tablespace or the log data space */
struct fil_node_t;

extern bool	os_has_said_disk_full;

/** Number of pending read operations */
extern ulint	os_n_pending_reads;
/** Number of pending write operations */
extern ulint	os_n_pending_writes;

#ifdef _WIN32

/** We define always WIN_ASYNC_IO, and check at run-time whether
   the OS actually supports it: Win 95 does not, NT does. */
#define WIN_ASYNC_IO

/** Use unbuffered I/O */
#define UNIV_NON_BUFFERED_IO

#endif

/** File offset in bytes */
typedef ib_uint64_t os_offset_t;

#ifdef _WIN32
/** File handle */
# define os_file_t	HANDLE

/** Convert a C file descriptor to a native file handle
@param fd file descriptor
@return native file handle */
# define OS_FILE_FROM_FD(fd) (HANDLE) _get_osfhandle(fd)

#else
/** File handle */
typedef int	os_file_t;

/** Convert a C file descriptor to a native file handle
@param fd file descriptor
@return native file handle */
# define OS_FILE_FROM_FD(fd) fd

#endif /* _WIN32 */

static const os_file_t OS_FILE_CLOSED = os_file_t(~0);

/** The next value should be smaller or equal to the smallest sector size used
on any disk. A log block is required to be a portion of disk which is written
so that if the start and the end of a block get written to disk, then the
whole block gets written. This should be true even in most cases of a crash:
if this fails for a log block, then it is equivalent to a media failure in the
log. */

#define OS_FILE_LOG_BLOCK_SIZE		512

/** Options for os_file_create_func @{ */
enum os_file_create_t {
	OS_FILE_OPEN = 51,		/*!< to open an existing file (if
					doesn't exist, error) */
	OS_FILE_CREATE,			/*!< to create new file (if
					exists, error) */
	OS_FILE_OVERWRITE,		/*!< to create a new file, if exists
					the overwrite old file */
	OS_FILE_OPEN_RAW,		/*!< to open a raw device or disk
					partition */
	OS_FILE_CREATE_PATH,		/*!< to create the directories */
	OS_FILE_OPEN_RETRY,		/*!< open with retry */

	/** Flags that can be combined with the above values. Please ensure
	that the above values stay below 128. */

	OS_FILE_ON_ERROR_NO_EXIT = 128,	/*!< do not exit on unknown errors */
	OS_FILE_ON_ERROR_SILENT = 256	/*!< don't print diagnostic messages to
					the log unless it is a fatal error,
					this flag is only used if
					ON_ERROR_NO_EXIT is set */
};

static const ulint OS_FILE_READ_ONLY = 333;
static const ulint OS_FILE_READ_WRITE = 444;
static const ulint OS_FILE_READ_ALLOW_DELETE = 555;/* for ibbackup */

/* Options for file_create */
static const ulint OS_FILE_AIO = 61;
static const ulint OS_FILE_NORMAL = 62;
/* @} */

/** Types for file create @{ */
static const ulint OS_DATA_FILE = 100;
static const ulint OS_LOG_FILE = 101;
static const ulint OS_DATA_TEMP_FILE = 102;
/* @} */

/** Error codes from os_file_get_last_error @{ */
static const ulint OS_FILE_NOT_FOUND = 71;
static const ulint OS_FILE_DISK_FULL = 72;
static const ulint OS_FILE_ALREADY_EXISTS = 73;
static const ulint OS_FILE_PATH_ERROR = 74;

/** wait for OS aio resources to become available again */
static const ulint OS_FILE_AIO_RESOURCES_RESERVED = 75;

static const ulint OS_FILE_SHARING_VIOLATION = 76;
static const ulint OS_FILE_ERROR_NOT_SPECIFIED = 77;
static const ulint OS_FILE_INSUFFICIENT_RESOURCE = 78;
static const ulint OS_FILE_AIO_INTERRUPTED = 79;
static const ulint OS_FILE_OPERATION_ABORTED = 80;
static const ulint OS_FILE_ACCESS_VIOLATION = 81;
static const ulint OS_FILE_ERROR_MAX = 100;
/* @} */

/** Types for aio operations @{ */

/** Redo log read/write */
#define IORequestReadLog	IORequest(IORequest::READ | IORequest::LOG)
#define IORequestWriteLog	IORequest(IORequest::WRITE | IORequest::LOG)

/** Transform pages during read/write */
#define IORequestReadCompress	IORequest(IORequest::READ | IORequest::COMPRESS)
#define IORequestWriteCompress	IORequest(IORequest::WRITE | IORequest::COMPRESS)

/** No transformations during read/write, write as is. */
#define IORequestRead		IORequest(IORequest::READ)
#define IORequestWrite		IORequest(IORequest::WRITE)

/** Deals with InnoDB native Zip tables */
#define IORequestReadZip	IORequestRead
#define IORequestWriteZip	IORequestWrite

class IORequest {
public:
	enum {
		READ = 1,
		WRITE = 2,

		/** Enumearations below can be ORed to READ/WRITE above*/

		/** Compress the page before a write and decompress
		after a read if it was a compressed page */
		COMPRESS = 4,

		/** Data file */
		DATA_FILE = 8,

		/** Log file request*/
		LOG = 16,

		/**
		Do not to wake i/o-handler threads, but the caller will do
		the waking explicitly later, in this way the caller can post
		several requests in a batch; NOTE that the batch must not be
		so big that it exhausts the slots in AIO arrays! NOTE that
		a simulated batch may introduce hidden chances of deadlocks,
		because I/Os are not actually handled until all
		have been posted: use with great caution! */
		DO_NOT_WAKE = 32,

		/** Ignore failed reads of non-existent pages */
		IGNORE_MISSING = 64,

		/** Use punch hole if available, only makes sense with
		COMPRESS. Ignored if COMPRESS is not set */
		PUNCH_HOLE = 128
	};

	IORequest() : m_type(READ), m_block_size(UNIV_SECTOR_SIZE) { }
	explicit IORequest(ulint type)
		:
		m_type(type),
		m_block_size(UNIV_SECTOR_SIZE)
	{
		if (!have_punch_hole()) {
			clear_punch_hole();
		}
	}

	~IORequest() { }

	static bool ignore_missing(ulint type)
	{
		return((type & IGNORE_MISSING) == IGNORE_MISSING);
	}

	bool is_read() const
	{
		return((m_type & READ) == READ);
	}

	bool is_write() const
	{
		return((m_type & WRITE) == WRITE);
	}

	bool is_compressed() const
	{
		return((m_type & COMPRESS) == COMPRESS);
	}

	bool is_log() const
	{
		return((m_type & LOG) == LOG);
	}

	bool is_wake() const
	{
		return((m_type & DO_NOT_WAKE) == 0);
	}

	bool ignore_missing() const
	{
		return(ignore_missing(m_type));
	}

	bool punch_hole() const
	{
		return((m_type & PUNCH_HOLE) == PUNCH_HOLE);
	}

	bool validate() const
	{
		ut_a(is_read() ^ is_write());

		return(!is_read() || !punch_hole());
	}

	void set_punch_hole()
	{
		if (have_punch_hole()) {
			m_type |= PUNCH_HOLE;
		}
	}

	void clear_do_not_wake()
	{
		m_type &= ~DO_NOT_WAKE;
	}

	void clear_punch_hole()
	{
		m_type &= ~PUNCH_HOLE;
	}

	ulint block_size() const
	{
		return(m_block_size);
	}

	void block_size(ulint block_size)
	{
		m_block_size = block_size;
	}

	void clear_compressed()
	{
		m_type &= ~COMPRESS;
		clear_punch_hole();
	}

	bool operator==(const IORequest& rhs) const
	{
		return(m_type == rhs.m_type);
	}

	static bool have_punch_hole()
	{
#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
		extern my_bool	srv_punch_hole;

		return(srv_punch_hole);
#else
		return(false);
#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */
	}

private:
	/** Request type bit flags */
	ulint			m_type;

	/* File system best block size */
	ulint			m_block_size;
};

/* @} */

struct os_file_size_t {
	/** Total size of file in bytes */
	os_offset_t	m_total_size;

	/** If it is a sparse file then this is the number of bytes
	actually allocated for the file. */
	os_offset_t	m_alloc_size;
};

/** Win NT does not allow more than 64 */
static const ulint OS_AIO_N_PENDING_IOS_PER_THREAD = 32;

/** Modes for aio operations @{ */
/** Normal asynchronous i/o not for ibuf pages or ibuf bitmap pages */
static const ulint OS_AIO_NORMAL = 21;

/**  Asynchronous i/o for ibuf pages or ibuf bitmap pages */
static const ulint OS_AIO_IBUF = 22;

/** Asynchronous i/o for the log */
static const ulint OS_AIO_LOG = 23;

/** Asynchronous i/o where the calling thread will itself wait for
the i/o to complete, doing also the job of the i/o-handler thread;
can be used for any pages, ibuf or non-ibuf.  This is used to save
CPU time, as we can do with fewer thread switches. Plain synchronous
I/O is not as good, because it must serialize the file seek and read
or write, causing a bottleneck for parallelism. */
static const ulint OS_AIO_SYNC = 24;
/* @} */

enum {
	/** Microsoft Windows 3.x */
	OS_WIN31 = 1,		

	/** Microsoft Windows 95 */
	OS_WIN95,		

	/** Microsoft Windows NT 3.x */
	OS_WINNT,		

	/** Microsoft Windows 2000 */
	OS_WIN2000,		

	/** Microsoft Windows XP or Windows Server 2003 */
	OS_WINXP,		

	/** Microsoft Windows Vista or Windows Server 2008 */
	OS_WINVISTA,		

	/** Microsoft Windows 7 or Windows Server 2008 R2 */
	OS_WIN7			
};

extern ulint	os_n_file_reads;
extern ulint	os_n_file_writes;
extern ulint	os_n_fsyncs;

#ifdef UNIV_PFS_IO
/* Keys to register InnoDB I/O with performance schema */
extern mysql_pfs_key_t	innodb_data_file_key;
extern mysql_pfs_key_t	innodb_log_file_key;
extern mysql_pfs_key_t	innodb_temp_file_key;

/* Following four macros are instumentations to register
various file I/O operations with performance schema.
1) register_pfs_file_open_begin() and register_pfs_file_open_end() are
used to register file creation, opening, closing and renaming.
2) register_pfs_file_io_begin() and register_pfs_file_io_end() are
used to register actual file read, write and flush
3) register_pfs_file_close_begin() and register_pfs_file_close_end()
are used to register file deletion operations*/
# define register_pfs_file_open_begin(state, locker, key, op, name,	\
				      src_file, src_line)		\
do {									\
	locker = PSI_FILE_CALL(get_thread_file_name_locker)(		\
		state, key, op, name, &locker);				\
	if (locker != NULL) {						\
		PSI_FILE_CALL(start_file_open_wait)(			\
			locker, src_file, src_line);			\
	}								\
} while (0)

# define register_pfs_file_open_end(locker, file)			\
do {									\
	if (locker != NULL) {						\
		PSI_FILE_CALL(end_file_open_wait_and_bind_to_descriptor)(\
			locker, file);					\
	}								\
} while (0)

# define register_pfs_file_close_begin(state, locker, key, op, name,	\
				      src_file, src_line)		\
do {									\
	locker = PSI_FILE_CALL(get_thread_file_name_locker)(		\
		state, key, op, name, &locker);				\
	if (locker != NULL) {						\
		PSI_FILE_CALL(start_file_close_wait)(			\
			locker, src_file, src_line);			\
	}								\
} while (0)

# define register_pfs_file_close_end(locker, result)			\
do {									\
	if (locker != NULL) {						\
		PSI_FILE_CALL(end_file_close_wait)(			\
			locker, result);				\
	}								\
} while (0)

# define register_pfs_file_io_begin(state, locker, file, count, op,	\
				    src_file, src_line)			\
do {									\
	locker = PSI_FILE_CALL(get_thread_file_descriptor_locker)(	\
		state, file, op);					\
	if (locker != NULL) {						\
		PSI_FILE_CALL(start_file_wait)(				\
			locker, count, src_file, src_line);		\
	}								\
} while (0)

# define register_pfs_file_io_end(locker, count)			\
do {									\
	if (locker != NULL) {						\
		PSI_FILE_CALL(end_file_wait)(locker, count);		\
	}								\
} while (0)
#endif /* UNIV_PFS_IO  */

/* Following macros/functions are file I/O APIs that would be performance
schema instrumented if "UNIV_PFS_IO" is defined. They would point to
wrapper functions with performance schema instrumentation in such case.

os_file_create
os_file_create_simple
os_file_create_simple_no_error_handling
os_file_close
os_file_rename
os_aio
os_file_read
os_file_read_no_error_handling
os_file_write

The wrapper functions have the prefix of "innodb_". */

#ifdef UNIV_PFS_IO
# define os_file_create(key, name, create, purpose, type, success)	\
	pfs_os_file_create_func(key, name, create, purpose,	type,	\
				success, __FILE__, __LINE__)

# define os_file_create_simple(key, name, create, access, success)	\
	pfs_os_file_create_simple_func(key, name, create, access,	\
				       success, __FILE__, __LINE__)

# define os_file_create_simple_no_error_handling(			\
		key, name, create_mode, access, success)		\
	pfs_os_file_create_simple_no_error_handling_func(		\
		key, name, create_mode, access, success, __FILE__, __LINE__)

# define os_file_close(file)						\
	pfs_os_file_close_func(file, __FILE__, __LINE__)

# define os_aio(type, mode, name, file, buf, offset,			\
		n, message1, message2)					\
	pfs_os_aio_func(type, mode, name, file, buf, offset,		\
			n, message1, message2, __FILE__, __LINE__)

# define os_file_read(type, file, buf, offset, n)			\
	pfs_os_file_read_func(type, file, buf, offset, n, __FILE__, __LINE__)

# define os_file_read_no_error_handling(type, file, buf, offset, n)	\
	pfs_os_file_read_no_error_handling_func(type, file, buf, offset, \
						n, __FILE__, __LINE__)

# define os_file_write(type, name, file, buf, offset, n)	\
	pfs_os_file_write_func(type, name, file, buf, offset,	\
			       n, __FILE__, __LINE__)

# define os_file_flush(file)						\
	pfs_os_file_flush_func(file, __FILE__, __LINE__)

# define os_file_rename(key, oldpath, newpath)				\
	pfs_os_file_rename_func(key, oldpath, newpath, __FILE__, __LINE__)

# define os_file_delete(key, name)					\
	pfs_os_file_delete_func(key, name, __FILE__, __LINE__)

# define os_file_delete_if_exists(key, name, exist)			\
	pfs_os_file_delete_if_exists_func(key, name, exist, __FILE__, __LINE__)
#else /* UNIV_PFS_IO */

/* If UNIV_PFS_IO is not defined, these I/O APIs point
to original un-instrumented file I/O APIs */
# define os_file_create(key, name, create, purpose, type, success)	\
	os_file_create_func(name, create, purpose, type, success)

# define os_file_create_simple(key, name, create_mode, access, success)	\
	os_file_create_simple_func(name, create_mode, access, success)

# define os_file_create_simple_no_error_handling(			\
		key, name, create_mode, access, success)		\
	os_file_create_simple_no_error_handling_func(			\
		name, create_mode, access, success)

# define os_file_close(file)	os_file_close_func(file)

# define os_aio(type, mode, name, file, buf, offset, n, message1, message2) \
	os_aio_func(type, mode, name, file, buf, offset, n,		\
		    message1, message2)

# define os_file_read(type, file, buf, offset, n)			\
	os_file_read_func(type, file, buf, offset, n)

# define os_file_read_no_error_handling(type, file, buf, offset, n)	\
	os_file_read_no_error_handling_func(type, file, buf, offset, n)

# define os_file_write(type, name, file, buf, offset, n)		\
	os_file_write_func(type, name, file, buf, offset, n)

# define os_file_flush(file)	os_file_flush_func(file)

# define os_file_rename(key, oldpath, newpath)				\
	os_file_rename_func(oldpath, newpath)

# define os_file_delete(key, name)	os_file_delete_func(name)

# define os_file_delete_if_exists(key, name, exist)			\
	os_file_delete_if_exists_func(name, exist)

#endif /* UNIV_PFS_IO */

/* File types for directory entry data type */

enum os_file_type_t {
	OS_FILE_TYPE_UNKNOWN = 0,
	OS_FILE_TYPE_FILE,			/* regular file */
	OS_FILE_TYPE_DIR,			/* directory */
	OS_FILE_TYPE_LINK,			/* symbolic link */
	OS_FILE_TYPE_BLOCK			/* block device */
};

/* Maximum path string length in bytes when referring to tables with in the
'./databasename/tablename.ibd' path format; we can allocate at least 2 buffers
of this size from the thread stack; that is why this should not be made much
bigger than 4000 bytes */
#define OS_FILE_MAX_PATH	4000

/** Struct used in fetching information of a file in a directory */
struct os_file_stat_t {
	char		name[OS_FILE_MAX_PATH];	/*!< path to a file */
	os_file_type_t	type;			/*!< file type */
	ib_int64_t	size;			/*!< file size in bytes */
	ib_int64_t	alloc_size;		/*!< Allocated size for
						sparse files in bytes */
	size_t		block_size;		/*!< Block size to use for IO
						in bytes*/
	time_t		ctime;			/*!< creation time */
	time_t		mtime;			/*!< modification time */
	time_t		atime;			/*!< access time */
	bool		rw_perm;		/*!< true if can be opened
						in read-write mode. Only valid
						if type == OS_FILE_TYPE_FILE */
};

#ifdef _WIN32
typedef HANDLE	os_file_dir_t;	/*!< directory stream */
#else
typedef DIR*	os_file_dir_t;	/*!< directory stream */
#endif

#ifdef _WIN32
/**
Gets the operating system version. Currently works only on Windows.
@return OS_WIN95, OS_WIN31, OS_WINNT, OS_WIN2000, OS_WINXP, OS_WINVISTA,
OS_WIN7. */

ulint
os_get_os_version();

#endif /* _WIN32 */
#ifndef UNIV_HOTBACKUP
/**
Creates the seek mutexes used in positioned reads and writes. */

void
os_io_init_simple();

/**
Creates a temporary file.  This function is like tmpfile(3), but
the temporary file is created in the MySQL temporary directory.
@return temporary file handle, or NULL on error */

FILE*
os_file_create_tmpfile();
#endif /* !UNIV_HOTBACKUP */

/**
The os_file_opendir() function opens a directory stream corresponding to the
directory named by the dirname argument. The directory stream is positioned
at the first entry. In both Unix and Windows we automatically skip the '.'
and '..' items at the start of the directory listing.

@param[in] dirname	directory name; it must not contain a trailing
			'\' or '/'
@param[in] is_fatal	true if we should treat an error as a fatal error;
			if we try to open symlinks then we do not wish a
			fatal error if it happens not to be a directory
@return directory stream, NULL if error */

os_file_dir_t
os_file_opendir(
	const char*	dirname,
	bool		is_fatal);

/**
Closes a directory stream.
@param[in] dir	directory stream
@return 0 if success, -1 if failure */

int
os_file_closedir(
	os_file_dir_t	dir);

/**
This function returns information of the next file in the directory. We jump
over the '.' and '..' entries in the directory.
@param[in] dirname	directory name or path
@param[in] dir		directory stream
@param[out] info	buffer where the info is returned
@return 0 if ok, -1 if error, 1 if at the end of the directory */

int
os_file_readdir_next_file(
	const char*	dirname,
	os_file_dir_t	dir,
	os_file_stat_t*	info);

/**
This function attempts to create a directory named pathname. The new directory
gets default permissions. On Unix, the permissions are (0770 & ~umask). If the
directory exists already, nothing is done and the call succeeds, unless the
fail_if_exists arguments is true.

@param[in] pathname	directory name as null-terminated string
@param[in] fail_if_exists if true, pre-existing directory is treated as an error.
@return true if call succeeds, false on error */

bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists);

/**
NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[out] success	true if succeed, false if error
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_simple_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool*		success);

/**
NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
			OS_FILE_READ_ALLOW_DELETE; the last option is used
			by a backup program reading the file
@param[out] success	true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_simple_no_error_handling_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool*		success)
	__attribute__((warn_unused_result));

/**
Tries to disable OS caching on an opened file descriptor.
@param[in] fd		file descriptor to alter
@param[in] file_name	file name, used in the diagnostic message
@param[in] name		"open" or "create"; used in the diagnostic message */

void
os_file_set_nocache(
	int		fd,
	const char*	file_name,
	const char*	operation_name);

/**
NOTE! Use the corresponding macro os_file_create(), not directly
this function!
Opens an existing file or creates a new.
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] purpose	OS_FILE_AIO, if asynchronous, non-buffered i/o is
			desired, OS_FILE_NORMAL, if any normal file;
			NOTE that it also depends on type, os_aio_..
			and srv_.. variables whether we really use async
			i/o or unbuffered i/o: look in the function source
			code for the exact rules
@param[in] type		OS_DATA_FILE or OS_LOG_FILE
@param[in] success	true if succeeded
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool*		success)
	__attribute__((warn_unused_result));

/**
Deletes a file. The file has to be closed before calling this.
@param[in] name		file path as a null-terminated string
@return true if success */

bool
os_file_delete_func(const char* name);

/**
Deletes a file if it exists. The file has to be closed before calling this.
@param[in] name		file path as a null-terminated string
@param[out] exist	indicate if file pre-exist
@return true if success */

bool
os_file_delete_if_exists_func(const char* name, bool* exist);

/**
NOTE! Use the corresponding macro os_file_rename(), not directly
this function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in] oldpath	old file path as a null-terminated string
@param[in] newpath	new file path
@return true if success */

bool
os_file_rename_func(const char* oldpath, const char* newpath);

/**
NOTE! Use the corresponding macro os_file_close(), not directly this function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in] file		own: handle to a file
@return true if success */

bool
os_file_close_func(os_file_t file);

#ifdef UNIV_PFS_IO
/**
NOTE! Please use the corresponding macro os_file_create_simple(),
not directly this function!
A performance schema instrumented wrapper function for
os_file_create_simple() which opens or creates a file.
@param[in] key		Performance Schema Key
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[out] success	true if succeeded
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
UNIV_INLINE
os_file_t
pfs_os_file_create_simple_func(
	mysql_pfs_key_t key,
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool*		success,
	const char*	src_file,
	ulint		src_line)
	__attribute__((warn_unused_result));

/**
NOTE! Please use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A performance schema instrumented wrapper function for
os_file_create_simple_no_error_handling(). Add instrumentation to
monitor file creation/open.
@param[in] key		Performance Schema Key
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
			OS_FILE_READ_ALLOW_DELETE; the last option is used
			by a backup program reading the file
@param[out] success	true if succeeded
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
UNIV_INLINE
os_file_t
pfs_os_file_create_simple_no_error_handling_func(
	mysql_pfs_key_t key,
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool*		success,
	const char*	src_file,
	ulint		src_line)
	__attribute__((warn_unused_result));

/**
NOTE! Please use the corresponding macro os_file_create(), not directly
this function!
A performance schema wrapper function for os_file_create().
Add instrumentation to monitor file creation/open.
@param[in] key		Performance Schema Key
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] purpose	OS_FILE_AIO, if asynchronous, non-buffered i/o is
			desired, OS_FILE_NORMAL, if any normal file; NOTE
			that it also depends on type, os_aio_..  and
			srv_.. variables whether we really use async
			i/o or unbuffered i/o: look in the function source
			code for the exact rules
@param[out] success	true if succeeded
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */
UNIV_INLINE
os_file_t
pfs_os_file_create_func(
	mysql_pfs_key_t key,
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool*		success,
	const char*	src_file,
	ulint		src_line)
	__attribute__((warn_unused_result));

/**
NOTE! Please use the corresponding macro os_file_close(), not directly
this function!
A performance schema instrumented wrapper function for os_file_close().
@param[in] file		handle to a file
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_close_func(
	os_file_t	file,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_read(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_read() which requests a synchronous read operation.
@param[in, out] type	IO request context
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_read_func(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
This is the performance schema instrumented wrapper function for
os_file_read_no_error_handling_func() which requests a synchronous
read operation.
@param[in, out] type	IO request context
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_read_no_error_handling_func(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_aio(), not directly this
function!
Performance schema wrapper function of os_aio() which requests
an asynchronous i/o operation.
@param[in] type		IO request context
@param[in] mode		IO mode
@param[in] name		Name of the file or path as NUL terminated string
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@param[in,out] m1	Message for the AIO handler, (can be used to identify
			a completed AIO operation); ignored if mode is
			OS_AIO_SYNC
@param[in,out] m2	message for the aio handler (can be used to identify
			a completed AIO operation); ignored if mode is
			OS_AIO_SYNC
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return DB_SUCCESS if request was queued successfully, FALSE if fail */
UNIV_INLINE
dberr_t
pfs_os_aio_func(
	IORequest&	type,
	ulint		mode,
	const char*	name,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	fil_node_t*	m1,
	void*		m2,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_write(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_write() which requests a synchronous write operation.
@param[in, out] type	IO request context
@param[in] name		Name of the file or path as NUL terminated string
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_write_func(
	IORequest&	type,
	const char*	name,
	os_file_t	file,
	const void*	buf,
	os_offset_t	offset,
	ulint		n,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_flush(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_flush() which flushes the write buffers of a given file to the disk.
Flushes the write buffers of a given file to the disk.
@param[in] file		Open file handle
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return TRUE if success */
UNIV_INLINE
bool
pfs_os_file_flush_func(
	os_file_t	file,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_rename(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_rename()
@param[in] key		Performance Schema Key
@param[in] oldpath	old file path as a null-terminated string
@param[in] newpath	new file path
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_rename_func(
	mysql_pfs_key_t	key,
	const char*	oldpath,
	const char*	newpath,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_delete(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_delete()
@param[in] key		Performance Schema Key
@param[in] name		old file path as a null-terminated string
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_delete_func(
	mysql_pfs_key_t	key,
	const char*	name,
	const char*	src_file,
	ulint		src_line);

/**
NOTE! Please use the corresponding macro os_file_delete_if_exists(), not
directly this function!
This is the performance schema instrumented wrapper function for
os_file_delete_if_exists()
@param[in] key		Performance Schema Key
@param[in] name		old file path as a null-terminated string
@param[in] exist	indicate if file pre-exist
@param[in] src_file	file name where func invoked
@param[in] src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_delete_if_exists_func(
	mysql_pfs_key_t	key,
	const char*	name,
	bool*		exist,
	const char*	src_file,
	ulint		src_line);
#endif	/* UNIV_PFS_IO */

#ifdef UNIV_HOTBACKUP
/**
Closes a file handle.
@param[in] file		handle to a file
@return true if success */

bool
os_file_close_no_error_handling(os_file_t file);
#endif /* UNIV_HOTBACKUP */

/**
Gets a file size.
@param[in] file		handle to a file
@return file size if OK, else set m_total_size to ~0 and m_alloc_size to errno */

os_file_size_t
os_file_get_size(
	const char*	filename)
	__attribute__((warn_unused_result));

/**
Gets a file size.
@param[in] file		handle to a file
@return file size, or (os_offset_t) -1 on failure */

os_offset_t
os_file_get_size(
	os_file_t	file)
	__attribute__((warn_unused_result));
/**
Write the specified number of zeros to a newly created file.
@param[in] name		name of the file or path as a null-terminated string
@param[in] file		handle to a file
@param[in] size		file size
@return true if success */

bool
os_file_set_size(
	const char*	name,
	os_file_t	file,
	os_offset_t	size)
	__attribute__((warn_unused_result));

/**
Truncates a file at its current position.
@param[in/out] file	file to be truncated
@return true if success */

bool
os_file_set_eof(
	FILE*		file);	/*!< in: file to be truncated */

/**
Truncates a file to a specified size in bytes. Do nothing if the size
preserved is smaller or equal than current size of file.
@param[in] pathname	file path
@param[in] file		file to be truncated
@param[in] size		size preserved in bytes
@return true if success */

bool
os_file_truncate(
	const char*	pathname,
	os_file_t	file,
	os_offset_t	size);

/**
NOTE! Use the corresponding macro os_file_flush(), not directly this function!
Flushes the write buffers of a given file to the disk.
@param[in] file		handle to a file
@return true if success */

bool
os_file_flush_func(
	os_file_t	file);

/**
Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in] report	true if we want an error message printed for all errors
@return error number, or OS error number + 100 */

ulint
os_file_get_last_error(
	bool		report);

/**
NOTE! Use the corresponding macro os_file_read(), not directly this function!
Requests a synchronous read operation.
@param[in] type		IO request context
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@return DB_SUCCESS if request was successful */

dberr_t
os_file_read_func(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n);

/**
Rewind file to its start, read at most size - 1 bytes from it to str, and
NUL-terminate str. All errors are silently ignored. This function is
mostly meant to be used with temporary files.
@param[in,out] file	file to read from
@param[in,out] str	buffer where to read
@param[in] size		size of buffer */

void
os_file_read_string(
	FILE*		file,
	char*		str,
	ulint		size);

/**
NOTE! Use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
Requests a synchronous positioned read operation. This function does not do
any error handling. In case of error it returns FALSE.
@param[in] type		IO request context
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@return DB_SUCCESS or error code */

dberr_t
os_file_read_no_error_handling_func(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n);

/**
NOTE! Use the corresponding macro os_file_write(), not directly this
function!
Requests a synchronous write operation.
@param[in,out] type	IO request context
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@return DB_SUCCESS if request was successful */

dberr_t
os_file_write_func(
	IORequest&	type,
	const char*	name,
	os_file_t	file,
	const void*	buf,
	os_offset_t	offset,
	ulint		n);

/**
Check the existence and type of the given file.
@param[in] path		pathname of the file
@param[out] exists	true if file exists
@param[out] type	type of the file (if it exists)
@return true if call succeeded */

bool
os_file_status(
	const char*	path,
	bool*		exists,
	os_file_type_t* type);

/**
The function os_file_dirname returns a directory component of a
null-terminated pathname string.  In the usual case, dirname returns
the string up to, but not including, the final '/', and basename
is the component following the final '/'.  Trailing '/' characters
are not counted as part of the pathname.

If path does not contain a slash, dirname returns the string ".".

Concatenating the string returned by dirname, a "/", and the basename
yields a complete pathname.

The return value is  a copy of the directory component of the pathname.
The copy is allocated from heap. It is the caller responsibility
to free it after it is no longer needed.

The following list of examples (taken from SUSv2) shows the strings
returned by dirname and basename for different paths:

       path	      dirname	     basename
       "/usr/lib"     "/usr"	     "lib"
       "/usr/"	      "/"	     "usr"
       "usr"	      "."	     "usr"
       "/"	      "/"	     "/"
       "."	      "."	     "."
       ".."	      "."	     ".."

@param[in] path		pathname
@return own: directory component of the pathname */

char*
os_file_dirname(
	const char*	path);

/**
This function returns a new path name after replacing the basename
in an old path with a new basename.  The old_path is a full path
name including the extension.  The tablename is in the normal
form "databasename/tablename".  The new base name is found after
the forward slash.  Both input strings are null terminated.

This function allocates memory to be returned.  It is the callers
responsibility to free the return value after it is no longer needed.

@param[in] old_path		pathname
@param[in] new_name		new file name
@return own: new full pathname */

char*
os_file_make_new_pathname(
	const char*	old_path,
	const char*	new_name);

/**
This function reduces a null-terminated full remote path name into
the path that is sent by MySQL for DATA DIRECTORY clause.  It replaces
the 'databasename/tablename.ibd' found at the end of the path with just
'tablename'.

Since the result is always smaller than the path sent in, no new memory
is allocated. The caller should allocate memory for the path sent in.
This function manipulates that path in place.

If the path format is not as expected, just return.  The result is used
to inform a SHOW CREATE TABLE command.

@praam[out] data_dir_path	full path/data_dir_path */

void
os_file_make_data_dir_path(
	char*	data_dir_path);

/**
Creates all missing subdirectories along the given path.
@param[in] path		path name
@return true if call succeeded FALSE otherwise */
bool
os_file_create_subdirs_if_needed(
	const char*	path);

/**
Initializes the asynchronous io system. Creates one array each for ibuf
and log i/o. Also creates one array each for read and write where each
array is divided logically into n_read_segs and n_write_segs
respectively. The caller must create an i/o handler thread for each
segment in these arrays. This function also creates the sync array.
No i/o handler thread needs to be created for that
@param[in] n_per_seg		maximum number of pending aio
				operations allowed per segment
@param[in] n_read_segs		number of reader threads
@param[in] n_write_segs		number of writer threads
@param[in] n_slots_sync		number of slots in the sync aio array */

bool
os_aio_init(
	ulint		n_per_seg,
	ulint		n_read_segs,
	ulint		n_write_segs,
	ulint		n_slots_sync);

/**
Frees the asynchronous io system. */

void
os_aio_free();

/**
NOTE! Use the corresponding macro os_aio(), not directly this function!
Requests an asynchronous i/o operation.
@param[in] type		IO request context
@param[in] mode		IO mode
@param[in] name		Name of the file or path as NUL terminated string
@param[in] file		Open file handle
@param[out] buf		buffer where to read
@param[in] offset	file offset where to read
@param[in] n		number of bytes to read
@param[in,out] m1	Message for the AIO handler, (can be used to identify
			a completed AIO operation); ignored if mode is
			OS_AIO_SYNC
@param[in,out] m2	message for the aio handler (can be used to identify
			a completed AIO operation); ignored if mode is
			OS_AIO_SYNC
@return DB_SUCCESS or error code */

dberr_t
os_aio_func(
	IORequest&	type,
	ulint		mode,
	const char*	name,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	fil_node_t*	m1,
	void*		m2);

/**
Wakes up all async i/o threads so that they know to exit themselves in
shutdown. */

void
os_aio_wake_all_threads_at_shutdown();

/**
Waits until there are no pending writes in os_aio_write_array. There can
be other, synchronous, pending writes. */

void
os_aio_wait_until_no_pending_writes();

/**
Wakes up simulated aio i/o-handler threads if they have something to do. */

void
os_aio_simulated_wake_handler_threads();

/**
This function can be called if one wants to post a batch of reads and
prefers an i/o-handler thread to handle them all at once later. You must
call os_aio_simulated_wake_handler_threads later to ensure the threads
are not left sleeping! */

void
os_aio_simulated_put_read_threads_to_sleep();

#ifdef WIN_ASYNC_IO
/**
This function is only used in Windows asynchronous i/o.
Waits for an aio operation to complete. This function is used to wait the
for completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!
@param[in] segment	the number of the segment in the aio arrays to
			wait for; segment 0 is the ibuf i/o thread, segment
			1 the log i/o thread, then follow the non-ibuf read
			threads, and as the last are the non-ibuf write
			threads; if this is ULINT_UNDEFINED, then it means
			that sync aio is used, and this parameter is ignored
@param[in] pos		this parameter is used only in sync aio: wait for the
			aio slot at this position
@param[out] m1		the messages passed with the aio request; note that
			also in the case where the aio operation failed, these
			output parameters are valid and can be used to restart
			the operation, for example
@param[out] m2		callback message
@param[out] type	OS_FILE_WRITE or ..._READ
@return true if the aio operation succeeded */

bool
os_aio_windows_handle(
	ulint		segment,
	ulint		pos,
	fil_node_t**	m1,
	void**		m2,
	ulint*		type);
#endif /* WIN_ASYNC_IO */

/**
Does simulated aio. This function should be called by an i/o-handler
thread.

@param[in] segment	the number of the segment in the aio arrays to wait
			for; segment 0 is the ibuf i/o thread, segment 1 the
			log i/o thread, then follow the non-ibuf read threads,
			and as the last are the non-ibuf write threads
@param[out] m1		the messages passed with the aio request; note that
			also in the case where the aio operation failed, these
			output parameters are valid and can be used to restart
			the operation, for example
@param[out] m2		Callback argument
@param[in] type		IO context
@return true if the aio operation succeeded */

bool
os_aio_simulated_handle(
	ulint		segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type);

/**
Validates the consistency of the aio system.
@return true if ok */

bool
os_aio_validate();

/**
Prints info of the aio arrays.
@param[in/out] file	file where to print */

void
os_aio_print(FILE* file);

/**
Refreshes the statistics used to print per-second averages. */

void
os_aio_refresh_stats();

/**
This function returns information about the specified file
@param[in] path		pathname of the file
@param[in] stat_info	information of a file in a directory
@param[in] check_rw_perm for testing whether the file can be opened in RW mode
@return DB_SUCCESS if all OK */

dberr_t
os_file_get_status(
	const char*	path,
	os_file_stat_t* stat_info,
	bool		check_rw_perm);

#if !defined(UNIV_HOTBACKUP)
/**
Creates a temporary file that will be deleted on close.
This function is defined in ha_innodb.cc.
@return temporary file descriptor, or < 0 on error */

int
innobase_mysql_tmpfile();
#endif /* !UNIV_HOTBACKUP */


#if defined(LINUX_NATIVE_AIO)
/**
This function is only used in Linux native asynchronous i/o.
Waits for an aio operation to complete. This function is used to wait the
for completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!
@param[in] global_seg	segment number in the aio array to wait for; segment 0
			is the ibuf i/o thread, segment 1 is log i/o thread,
			then follow the non-ibuf read threads, and the last are
			the non-ibuf write threads.
@param[out] m1		the messages passed with the
@param[out] m2		AIO request; note that in case the aio operation
			failed, these output parameters are valid and can be
			used to restart the operation.
@param[in] type		IO context
@return true if the IO was successful */

bool
os_aio_linux_handle(
	ulint		global_seg,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type);

#endif /* LINUX_NATIVE_AIO */

/**
If it is a compressed page return the compressed page data + footer size
@param[in] buf		Buffer to check, must include header + 10 bytes
@return ULINT_UNDEFINED if the page is not a compressed page or length
	of the compressed data (including footer) if it is a compressed page */

ulint
os_file_compressed_page_size(const byte* buf);

/**
If it is a compressed page return the original page data + footer size
@param[in] buf		Buffer to check, must include header + 10 bytes
@return ULINT_UNDEFINED if the page is not a compressed page or length
	of the original data + footer if it is a compressed page */

ulint
os_file_original_page_size(const byte* buf);

/**
Normalizes a directory path for Windows: converts slashes to backslashes.
@param[in,out] str A null-terminated Windows directory and file path */
#ifdef _WIN32
void os_normalize_path_for_win(char* str);
#else
#define os_normalize_path_for_win(str)
#endif /* _WIN32 */

/**
Set the file create umask
@param[in] umask	The umask to use for file creation. */
void
os_file_set_umask(ulint umask);

/**
Free storage space associated with a section of the file.
@param[in] fh		Open file handle
@param[in] page_size	Tablespace page size
@param[in] block_size	File system block size
@param[in] off		Starting offset (SEEK_SET)
@param[in] len		Number of bytes to free at the end of th page, must
			be a multiple of UNIV_SECTOR_SIZE
@return DB_SUCCESS or error code */

dberr_t
os_file_punch_hole(
	os_file_t	fh,
	os_offset_t	page_size,
	ulint		block_size,
	os_offset_t	off,
	os_offset_t	len);

#ifndef UNIV_NONINL
#include "os0file.ic"
#endif

#endif
