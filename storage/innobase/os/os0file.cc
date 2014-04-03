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
@file os/os0file.cc
The interface to the operating system file i/o primitives

Created 10/21/1995 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "os0file.h"

#ifdef UNIV_NONINL
#include "os0file.ic"
#endif

#include "srv0srv.h"
#include "srv0start.h"
#include "fil0fil.h"
#ifndef UNIV_HOTBACKUP
# include "os0event.h"
# include "os0thread.h"
#else /* !UNIV_HOTBACKUP */
# ifdef _WIN32
/* Add includes for the _stat() call to compile on Windows */
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <errno.h>
# endif /* _WIN32 */
#endif /* !UNIV_HOTBACKUP */

#ifdef LINUX_NATIVE_AIO
#include <libaio.h>
#endif /* LINUX_NATIVE_AIO */

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
# include <fcntl.h>
# include <linux/falloc.h>
#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

#include <zlib.h>

#ifdef HAVE_LZ4
#include <lz4.h>
#include <lz4hc.h>
#endif /* HAVE_LZ4 */

#ifdef HAVE_LZMA
#include <lzma.h>
#endif /* HAVE_LZMA */

#ifdef HAVE_LZO1X
#include <lzo/lzo1x.h>
bool srv_lzo_disabled = false;
#endif /* HAVE_LZO1X */

/** Global compression algorithm configuration:
	--innodb-compression-algorithm 0 - None, 1 ZLib and 2 LZ4 */
ulong	srv_compression_algorithm = 0;

/** Global punch hole configuration:
	--innodb-compression-punch-hole := bool */
my_bool	srv_punch_hole = true;

/** Global AIO compressed table read configuration:
	--innodb-read-block-size := bool */
my_bool srv_read_block_size = false;

/** Insert buffer segment id */
static const ulint IO_IBUF_SEGMENT = 0;

/** Log segment id */
static const ulint IO_LOG_SEGMENT = 1;

/** Number of retries for partial I/O's */
static const ulint NUM_RETRIES_ON_PARTIAL_IO = 10;

/* This specifies the file permissions InnoDB uses when it creates files in
Unix; the value of os_innodb_umask is initialized in ha_innodb.cc to
my_umask */

#ifndef _WIN32
/** Umask for creating files */
static ulint	os_innodb_umask = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
#else
/** Umask for creating files */
static ulint	os_innodb_umask	= 0;
#endif /* _WIN32 */

#ifndef UNIV_HOTBACKUP

/** We use these mutexes to protect lseek + file i/o operation, if the
OS does not provide an atomic pread or pwrite, or similar */
const static ulint	OS_FILE_N_SEEK_MUTEXES = 16;

static SysMutex*	os_file_seek_mutexes[OS_FILE_N_SEEK_MUTEXES];

/** In simulated aio, merge at most this many consecutive i/os */
static const ulint	OS_AIO_MERGE_N_CONSECUTIVE = 64;

/**********************************************************************

InnoDB AIO Implementation:
=========================

We support native AIO for windows and linux. For rest of the platforms
we simulate AIO by special io-threads servicing the IO-requests.

Simulated AIO:
==============

In platforms where we 'simulate' AIO following is a rough explanation
of the high level design.
There are four io-threads (for ibuf, log, read, write).
All synchronous IO requests are serviced by the calling thread using
os_file_write/os_file_read. The Asynchronous requests are queued up
in an array (there are four such arrays) by the calling thread.
Later these requests are picked up by the io-thread and are serviced
synchronously.

Windows native AIO:
==================

If srv_use_native_aio is not set then windows follow the same
code as simulated AIO. If the flag is set then native AIO interface
is used. On windows, one of the limitation is that if a file is opened
for AIO no synchronous IO can be done on it. Therefore we have an
extra fifth array to queue up synchronous IO requests.
There are innodb_file_io_threads helper threads. These threads work
on the four arrays mentioned above in Simulated AIO. No thread is
required for the sync array.
If a synchronous IO request is made, it is first queued in the sync
array. Then the calling thread itself waits on the request, thus
making the call synchronous.
If an AIO request is made the calling thread not only queues it in the
array but also submits the requests. The helper thread then collects
the completed IO request and calls completion routine on it.

Linux native AIO:
=================

If we have libaio installed on the system and innodb_use_native_aio
is set to true we follow the code path of native AIO, otherwise we
do simulated AIO.
There are innodb_file_io_threads helper threads. These threads work
on the four arrays mentioned above in Simulated AIO.
If a synchronous IO request is made, it is handled by calling
os_file_write/os_file_read.
If an AIO request is made the calling thread not only queues it in the
array but also submits the requests. The helper thread then collects
the completed IO request and calls completion routine on it.

**********************************************************************/


#ifdef UNIV_PFS_IO
/* Keys to register InnoDB I/O with performance schema */
mysql_pfs_key_t  innodb_data_file_key;
mysql_pfs_key_t  innodb_log_file_key;
mysql_pfs_key_t  innodb_temp_file_key;
#endif /* UNIV_PFS_IO */

/** The asynchronous i/o array slot structure */
struct Slot {
	/** index of the slot in the aio array */
	ulint			pos;

	/** true if this slot is reserved */
	bool			is_reserved;

	/** time when reserved */
	time_t			reservation_time;

	/** length of the block to read or write */
	ulint			len;

	/** buffer used in i/o */
	byte*			buf;

	/** Buffer pointer used for actual IO. We advance this
	when partial IO is required and not buf */
	byte*			ptr;

	/** OS_FILE_READ or OS_FILE_WRITE */
	IORequest		type;

	/** file offset in bytes */
	os_offset_t		offset;

	/** file where to read or write */
	os_file_t		file;

	/** file name or path */
	const char*		name;

	/** used only in simulated aio: true if the physical i/o
	already made and only the slot message needs to be passed
	to the caller of os_aio_simulated_handle */
	bool			io_already_done;

	/** message which is given by the */
	fil_node_t*		m1;

	/** the requester of an aio operation and which can be used
	to identify which pending aio operation was completed */
	void*			m2;

	/** AIO completion status */
	dberr_t			err;

#ifdef WIN_ASYNC_IO
	/** handle object we need in the OVERLAPPED struct */
	HANDLE			handle;

	/** Windows control block for the aio request */
	OVERLAPPED		control;

#elif defined(LINUX_NATIVE_AIO)
	/** Linux control block for aio */
	struct iocb		control;

	/** bytes written/read. */
	int			n_bytes;

	/** AIO return code */
	int			ret;

#endif /* WIN_ASYNC_IO */

	/** Length of the block before it was compressed */
	ulint			original_len;

	/** Unaligned buffer for compressed pages */
	byte*			compressed_ptr;

	/** Compressed data page, aligned and derinved from compressed_ptr */
	byte*			compressed_page;

	/** true if the fil_page_compress() was successful */
	bool			compress_ok;
#ifdef HAVE_LZO1X
	byte			mem[LZO1X_1_15_MEM_COMPRESS];
#else
	byte*			mem;
#endif /* HAVE_LZO1X */
};

/** The asynchronous i/o array structure */
class AIO {
public:
	/**
	@param[in] n_slots	Number of slots to configure
	@param[in] segments	Number of segments to configure */
	AIO(ulint n_slots, ulint segments);

	/** Destructor */
	~AIO();

	/** Initialize the instance
	@return DB_SUCCESS or error code */
	dberr_t init();

	/**
	Requests for a slot in the aio array. If no slot is available, waits
	until not_full-event becomes signaled.

	@param[in,out] type	IO context
	@param[in,out] m1	message to be passed along with the aio operation
	@param[in,out] m2	message to be passed along with the aio operation
	@param[in] file 	file handle
	@param[in] name		name of the file or path as a null-terminated
				string
	@param[in,out] buf	buffer where to read or from which to write
	@param[in] offset	file offset, where to read from or start writing
	@param[in] len		length of the block to read or write
	@return pointer to slot */

	Slot* reserve_slot(
		IORequest&	type,
		fil_node_t*	m1,
		void*		m2,
		os_file_t	file,
		const char*	name,
		void*		buf,
		os_offset_t	offset,
		ulint		len);

	/**
	@return true on success */
	bool validate() const;

	/**
	Returns a pointer to the nth slot in the aio array.
	@param[in] index	Index of the slot in the array
	@return pointer to slot */
	const Slot* at(ulint i) const
	{
		ut_a(i < n_slots);

		return(slots + i);
	}

	/** Non const version */
	Slot* at(ulint i)
	{
		ut_a(i < n_slots);

		return(slots + i);
	}

	/**
	Frees a slot in the aio array. 
	@param[in,out] slot	Slot to release */
	void release_low(Slot* slot);

	/**
	Frees a slot in the aio array. 
	@param[in,out] slot	Slot to release */
	void release(Slot* slot);

	/**
	Prints info about the aio array.
	@param[in,out] file	Where to print */
	void print(FILE* file);

#ifdef LINUX_NATIVE_AIO
	/**
	Dispatch an AIO request to the kernel.
	@param[in,out] slot	an already reserved slot
	@return true on success. */
	bool linux_dispatch(Slot* slot);

	/**
	Creates an io_context for native linux AIO.
	@param[in] max_events		number of events
	@param[out] io_ctx		io_ctx to initialize.
	@return true on success. */
	static bool linux_create_io_ctx(ulint max_events, io_context_t* io_ctx);

	/**
	Checks if the system supports native linux aio. On some kernel
	versions where native aio is supported it won't work on tmpfs. In such
	cases we can't use native aio as it is not possible to mix simulated
	and native aio.
	@return: true if supported, false otherwise. */
	static bool is_linux_native_aio_supported();
#endif /* LINUX_NATIVE_AIO */

#ifdef WIN_ASYNC_IO
	/**
	Wakes up all async i/o threads in the array in Windows async i/o at
	shutdown. */
	void wake_at_shutdown()
	{
		Slot*	slot = slots;

		for (ulint i = 0; i < n_slots; ++i, ++slot) {
			SetEvent(slot->handle);
		}
	}

	/**
	Wake up all ai/o threads in Windows native aio */
	static void wake_at_shutdown()
	{
		s_reads->wake_at_shutdown();

		if (s_writes != NULL) {
			s_writes->wake_at_shutdown();
		}

		if (s_ibuf != NULL) {
			s_ibuf->wake_at_shutdown();
		}

		if (s_log != NULL) {
			s_log->wake_at_shutdown();
		}
	}
#endif /* WIN_ASYNC_IO */

	/**
	@return the number of slots per segment */
	ulint get_n_slots() const
	{
		return(n_slots / n_segments);
	}

	/**
	Create an instance using new(std::nothrow) */
	static AIO* create(ulint n_slots, ulint segments);

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

	static bool start(
		ulint		n_per_seg,
		ulint		n_read_segs,
		ulint		n_write_segs,
		ulint		n_slots_sync);

	/**
	Free the AIO arrays */
	static void shutdown();

	/**
	Print all the AIO segments
	@param[in,out] file	Where to print */
	static void print_all(FILE* file);

	/**
	Calculates local segment number and aio array from global segment number.
	@param[out] array		aio wait array
	@param[in] segment		global segment number
	@return local segment number within the aio array */
	static ulint get_array_and_local_segment(
		AIO**		array,
		ulint		segment);

	/**
	Select the IO slot array
	@param[in] type		Type of IO, READ or WRITE
	@param[in] read_only	true if running in read-only mode
	@param[in] mode		IO mode
	@return slot array or NULL if invalid mode specified */
	static AIO* select_slot_array(
		IORequest&	type,
		bool		read_only,
		ulint		mode);

	/**
	Calculates segment number for a slot.
	@param[in] array	aio wait array
	@param[int] slot	slot in this array
	@return segment number (which is the number used by, for example,
		i/o-handler threads) */
	static ulint get_segment_no_from_slot(
		const AIO*	array,
		const Slot*	slot);
	/**
	Wakes up a simulated aio i/o-handler thread if it has something to do.
	@param[in] global_segment	the number of the segment in the
					AIO arrays */
	static void wake_simulated_handler_thread(ulint global_segment);

private:
	/**
	Initialise the slots
	@return DB_SUCCESS or error code */
	dberr_t init_slots();

	/**
	Wakes up a simulated AIO I/O-handler thread if it has something to do
	for a local segment in the AIO array.
	@param[in] global_segment	the number of the segment in the
					AIO arrays 
	@param[in] segment		the local segment in the AIO array */
	void wake_simulated_handler_thread(ulint global_segment, ulint segment);

	/**
	Prints pending IO requests per segment of an aio array.
	We probably don't need per segment statistics but they can help us
	during development phase to see if the IO requests are being
	distributed as expected.
	@param[in,out file	file where to print
	@param[in] segments	pending IO array */
	void print_segment_info(
		FILE*		file,
		const ulint*	segments);

#ifdef LINUX_NATIVE_AIO
	/**
	Initialise the Linux native AIO data structures
	@return DB_SUCCESS or error code */
	dberr_t init_linux_native_aio();
#endif /* LINUX_NATIVE_AIO */

	// FIXME: Eventually this must be private
public:
	/** the mutex protecting the aio array */
	mutable SysMutex	mutex;

	/** Pointer to the slots in the array */
	Slot*			slots;

	/** Total number of slots in the aio array.  This must be
	divisible by n_threads. */
	ulint			n_slots;

	/** Number of segments in the aio array of pending aio requests.
	A thread can wait separately for any one of the segments. */
	ulint			n_segments;

	/** The event which is set to the signaled state when
	there is space in the aio outside the ibuf segment */
	os_event_t		not_full;

	/** The event which is set to the signaled state when
	there are no pending i/os in this array */
	os_event_t		is_empty;

	/** Number of reserved slots in the aio array outside the ibuf segment */
	ulint			n_reserved;

#ifdef _WIN32
	/** Pointer to an array of OS native event handles where
	we copied the handles from slots, in the same order. This
	can be used in WaitForMultipleObjects; used only in Windows */
	HANDLE*			handles;
#endif /* _WIN32 */

#if defined(LINUX_NATIVE_AIO)
	/** completion queue for IO. There is one such queue per
	segment. Each thread will work on one ctx exclusively. */
	io_context_t*		aio_ctx;

	/** The array to collect completed IOs. There is one such
	event for each possible pending IO. The size of the array
	is equal to n_slots. */
	struct io_event*	events;
#endif /* LINUX_NATIV_AIO */

	/** The aio arrays for non-ibuf i/o and ibuf i/o, as well as
	sync AIO. These are NULL when the module has not yet been initialized. */

	/** Reads */
	static AIO*		s_reads;

	/** Writes */
	static AIO*		s_writes;

	/** Insert buffer */
	static AIO*		s_ibuf;

	/** Reado log */
	static AIO*		s_log;

	/** Synchronous I/O */
	static AIO*		s_sync;
};

/** Static declarations */
AIO*	AIO::s_reads;
AIO*	AIO::s_writes;
AIO*	AIO::s_ibuf;
AIO*	AIO::s_log;
AIO*	AIO::s_sync;

#if defined(LINUX_NATIVE_AIO)
/** timeout for each io_getevents() call = 500ms. */
static const ulint OS_AIO_REAP_TIMEOUT = 500000000UL;

/** time to sleep, in microseconds if io_setup() returns EAGAIN. */
static const ulint OS_AIO_IO_SETUP_RETRY_SLEEP = 500000UL;

/** number of attempts before giving up on io_setup(). */
static const int OS_AIO_IO_SETUP_RETRY_ATTEMPTS = 5;
#endif /* LINUX_NATIVE_AIO */

/** Array of events used in simulated aio */
static os_event_t*	os_aio_segment_wait_events = NULL;

/** Number of asynchronous I/O segments.  Set by os_aio_init(). */
static ulint	os_aio_n_segments	= ULINT_UNDEFINED;

/** If the following is true, read i/o handler threads try to
wait until a batch of new read requests have been posted */
static bool	os_aio_recommend_sleep_for_read_threads = false;
#endif /* !UNIV_HOTBACKUP */

ulint	os_n_file_reads		= 0;
ulint	os_bytes_read_since_printout = 0;
ulint	os_n_file_writes	= 0;
ulint	os_n_fsyncs		= 0;
ulint	os_n_file_reads_old	= 0;
ulint	os_n_file_writes_old	= 0;
ulint	os_n_fsyncs_old		= 0;
/** Number of pending write operations */
ulint	os_n_pending_writes = 0;
/** Number of pending read operations */
ulint	os_n_pending_reads = 0;

time_t	os_last_printout;
bool	os_has_said_disk_full	= false;

#ifndef HAVE_ATOMIC_BUILTINS
/** The mutex protecting the following counts of pending I/O operations */
static SysMutex		os_file_count_mutex;
#endif /*!HAVE_ATOMIC_BUILTINS */

/** Default Zip compression level */
static const ulint	COMPRESSION_NONE = 0;
static const ulint      ZLIB_ALGORITHM = 1;
static const ulint      LZ4_ALGORITHM = 2;
static const ulint 	LZO1X_ALGORITHM = 3;
static const ulint 	LZMA_ALGORITHM = 4;

extern uint page_zip_level;

#if DATA_TRX_ID_LEN > 6
#error "COMPRESSION_ALGORITHM will not fit"
#endif /* DATA_TRX_ID_LEN */

/**
Does error handling when a file operation fails.
@param[in] name		File name or NULL
@param[in] operation	Name of operation e.g., "read", "write"
@return true if we should retry the operation */
static
bool
os_file_handle_error(
	const char*	name,
	const char*	operation);

/**
Does error handling when a file operation fails.
@param[in] name		File name or NULL
@param[in] operation	Name of operation e.g., "read", "write"
@param[in] silent	if true then don't print any message to the log.
@return true if we should retry the operation */
static
bool
os_file_handle_error_no_exit(
	const char*	name,
	const char*	operation,
	bool		silent);

/**
If it is a compressed page return the compressed page data + footer size
@param[in] buf		Buffer to check, must include header + 10 bytes
@return ULINT_UNDEFINED if the page is not a compressed page or length
	of the compressed data (including footer) if it is a compressed page */

ulint
os_file_compressed_page_size(const byte* buf)
{
	ulint   type = mach_read_from_2(buf + FIL_PAGE_TYPE);

	if (type == FIL_PAGE_COMPRESSED) {
	        ulint   version = mach_read_from_1(buf + FIL_PAGE_VERSION);
		ut_a(version == 1);
		return(mach_read_from_2(buf + FIL_PAGE_COMPRESS_SIZE_V1));
	}

	return(ULINT_UNDEFINED);
}

/**
If it is a compressed page return the original page data + footer size
@param[in] buf		Buffer to check, must include header + 10 bytes
@return ULINT_UNDEFINED if the page is not a compressed page or length
	of the original data + footer if it is a compressed page */

ulint
os_file_original_page_size(const byte* buf)
{
	ulint   type = mach_read_from_2(buf + FIL_PAGE_TYPE);

	if (type == FIL_PAGE_COMPRESSED) {

	        ulint   version = mach_read_from_1(buf + FIL_PAGE_VERSION);
		ut_a(version == 1);

		return(mach_read_from_2(buf + FIL_PAGE_ORIGINAL_SIZE_V1));
	}

	return(ULINT_UNDEFINED);
}

/**
Validates the consistency of an aio array.
@return true if ok */

bool
AIO::validate() const
{
	ulint	count = 0;

	mutex_enter(&mutex);

	ut_a(n_slots > 0);
	ut_a(n_segments > 0);

	Slot*	slot = slots;

	for (ulint i = 0; i < n_slots; ++i, ++slot) {

		if (slot->is_reserved) {
			++count;
			ut_a(slot->len > 0);
		}
	}

	ut_a(n_reserved == count);

	mutex_exit(&mutex);

	return(true);
}

// FIXME: Do overlapping compress/decompress where possible

/**
Compress a data page
@param[in] page_size	The configured page size for the tablespace
#param[in] block_size	File system block size
@param[in] src		Source contents to compress
@param[in] src_len	Length in bytes of the source
@param[out] dst		Compressed page contents
@param[out] dst_len	Lengh in bytes of dst contents
@return buffer data, dst_len will have the length of the data */
static
byte*
os_file_page_compress(
	ulint		page_size,
	ulint		block_size,
	byte*		src,
	ulint		src_len,
	byte*		dst,
	ulint*		dst_len,
	byte*		mem)
{
	ulint		len;
	ulint		compression_level = page_zip_level;
	ulint           algorithm = srv_compression_algorithm;
	ulint		page_type = mach_read_from_2(src + FIL_PAGE_TYPE);

	ut_ad(page_type != FIL_PAGE_COMPRESSED);

	/* Leave the header alone when compressing. */
	ulint		data_len = src_len - FIL_PAGE_DATA;

	switch (algorithm) {
	case COMPRESSION_NONE:
		*dst_len = src_len;
		return(src);

	case ZLIB_ALGORITHM:

		len = data_len;

		/* Only compress the data + trailer, leave the header alone */
		if (compress2(
			dst + FIL_PAGE_DATA, &len,
			src + FIL_PAGE_DATA, data_len,
			compression_level) != Z_OK) {

			*dst_len = src_len;

			return(src);
		}

		break;
#ifdef HAVE_LZ4
	case LZ4_ALGORITHM:

#ifdef HAVE_LZ4_COMPRESSHC2_LIMITEDOUTPUT
		len = LZ4_compressHC2_limitedOutput(
			(const char*) src + FIL_PAGE_DATA,
			(char*) dst + FIL_PAGE_DATA,
			data_len,
			data_len,
			compression_level);
#else
		len = LZ4_compressHC_limitedOutput(
			(const char*) src + FIL_PAGE_DATA,
			(char*) dst + FIL_PAGE_DATA,
			data_len,
			data_len);
#endif /* HAVE_LZ4_COMPRESSHC2_LIMITEDOUTPUT */

		ut_a(len <= data_len);

		if (len == 0 || len > data_len) {

			*dst_len = src_len;

			return(src);
		}
		break;
#endif /* HAVE_LZ4 */

#ifdef HAVE_LZMA
	case LZMA_ALGORITHM:

		len = 0;

		if (lzma_easy_buffer_encode(
			compression_level,
			LZMA_CHECK_NONE,
			NULL, 	/* No custom allocator, use malloc/free */
			reinterpret_cast<uint8_t*>(src + FIL_PAGE_DATA),
			data_len,
			reinterpret_cast<uint8_t*>(dst + FIL_PAGE_DATA),
			&len,
			data_len) != LZMA_OK) {

			*dst_len = src_len;            

			return(src);
		}

		break;
#endif /* HAVE_LZMA */

#ifdef HAVE_LZO1X
	case LZO1X_ALGORITHM: {

		if (srv_lzo_disabled) {
			*dst_len = src_len;
			return(src);
		}

		len = data_len;

		if (lzo1x_1_15_compress(
			src + FIL_PAGE_DATA,
			data_len,
			dst + FIL_PAGE_DATA, &len, mem) != LZO_E_OK
		    || len >= data_len) {

			*dst_len = src_len;

			return(src);
		}
		break;
	}
#endif /* HAVE_LZO 1X*/

	default:
		*dst_len = src_len;
		return(src);
	}

	ut_a(len <= data_len);

	ut_ad(memcmp(src + FIL_PAGE_LSN + 4,
		     src + page_size - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4) == 0);

	/* Copy the header as is. */
	memmove(dst, src, FIL_PAGE_DATA);

	/* Add compression control information. Required for decompressing. */
	mach_write_to_2(dst + FIL_PAGE_TYPE, FIL_PAGE_COMPRESSED);

	mach_write_to_1(dst + FIL_PAGE_VERSION, 1);

	mach_write_to_1(dst + FIL_PAGE_ALGORITHM_V1, algorithm);

	mach_write_to_2(dst + FIL_PAGE_ORIGINAL_TYPE_V1, page_type);

	mach_write_to_2(dst + FIL_PAGE_ORIGINAL_SIZE_V1, data_len);

	mach_write_to_2(dst + FIL_PAGE_COMPRESS_SIZE_V1, len);

	/* Round to the next full sector size */
	*dst_len = ut_calc_align(len + FIL_PAGE_DATA, block_size);

	return(dst);
}

/**
Decompress the page data contents. Page type must be FIL_PAGE_COMPRESSED, if
not then the source contents are left unchanged and DB_SUCCESS is returned.
@param[in,out] src              Data read from disk, decompressed data will be
				copied to this page
@param[in,out] dst              Scratch area to use for decompression
@param[in] dst_len              Size of the scratch area in bytes
@return DB_SUCCESS or error code */
static
dberr_t
os_file_page_decompress(byte* src, byte* dst, ulint dst_len)
{
	ulint	page_type = mach_read_from_2(src + FIL_PAGE_TYPE);

	if (page_type != FIL_PAGE_COMPRESSED) {
		/* It is not a compressed page */
		return(DB_SUCCESS);
	}

	byte*   ptr = src + FIL_PAGE_DATA;
	ulint   version = mach_read_from_1(src + FIL_PAGE_VERSION);

	ut_a(version == 1);

	/* Read the original page type, before we compressed the data. */
	page_type = mach_read_from_2(src + FIL_PAGE_ORIGINAL_TYPE_V1);

	ulint   size = mach_read_from_2(src + FIL_PAGE_COMPRESS_SIZE_V1);
	ulint   original_len = mach_read_from_2(src + FIL_PAGE_ORIGINAL_SIZE_V1);

	// FIXME: Add more checks

	if (original_len < UNIV_PAGE_SIZE_MIN - (FIL_PAGE_DATA + 8)
	     || original_len > UNIV_PAGE_SIZE_MAX - FIL_PAGE_DATA
	     || dst_len < original_len + FIL_PAGE_DATA) {

		/* FIXME: The last check should return DB_OVERFLOW,
		the caller should be able to retry with a larger buffer. */
		return(DB_CORRUPTION);
	}

	ulint	len = original_len;
	ulint   algorithm = mach_read_from_1(src + FIL_PAGE_ALGORITHM_V1);

	switch(algorithm) {
	case ZLIB_ALGORITHM:

		if (uncompress(dst, &len, ptr, size) != Z_OK) {
			return(DB_IO_DECOMPRESS_FAIL);
		}

		break;

#ifdef HAVE_LZ4
	case LZ4_ALGORITHM:
		if (LZ4_decompress_fast(
			(const char*) ptr, (char*) dst, len) < 0) {

			return(DB_IO_DECOMPRESS_FAIL);
		}
		break;
#endif /* HAVE_LZ4 */

#ifdef HAVE_LZMA
	case LZMA_ALGORITHM: {

		lzma_ret	ret;
		size_t		src_pos = 0;
		size_t		dst_pos = 0;
		uint64_t 	memlimit = UINT64_MAX;

		ret = lzma_stream_buffer_decode(
			&memlimit, 0, NULL,
			ptr, &src_pos, size,
			dst, &dst_pos, len);


		if (ret != LZMA_OK) {
			return(DB_IO_DECOMPRESS_FAIL);
		}

		break;
	}
#endif /* HAVE_LZMA */

#ifdef HAVE_LZO1X
	case LZO1X_ALGORITHM:
		if (srv_lzo_disabled
		    || lzo1x_decompress(ptr, size, dst, &len, NULL)
		    != LZO_E_OK) {

			return(DB_IO_DECOMPRESS_FAIL);
		}
		break;
#endif /* HAVE_LZO1X */

	default:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown compression algorithm: %lu\n",
			(ulint) algorithm);

		return(DB_UNSUPPORTED);
	}

	/* Leave the header alone */
	memmove(src + FIL_PAGE_DATA, dst, len);

	mach_write_to_2(src + FIL_PAGE_TYPE, page_type);

	ut_ad(memcmp(src + FIL_PAGE_LSN + 4,
		     src + (original_len + FIL_PAGE_DATA)
		     - FIL_PAGE_END_LSN_OLD_CHKSUM + 4, 4) == 0);

	return(DB_SUCCESS);
}

#ifdef UNIV_DEBUG
# ifndef UNIV_HOTBACKUP
/**
Validates the consistency the aio system some of the time.
@return true if ok or the check was skipped */

bool
os_aio_validate_skip()
{
/** Try os_aio_validate() every this many times */
# define OS_AIO_VALIDATE_SKIP	13

	/** The os_aio_validate() call skip counter.
	Use a signed type because of the race condition below. */
	static int os_aio_validate_count = OS_AIO_VALIDATE_SKIP;

	/* There is a race condition below, but it does not matter,
	because this call is only for heuristic purposes. We want to
	reduce the call frequency of the costly os_aio_validate()
	check in debug builds. */
	--os_aio_validate_count;

	if (os_aio_validate_count > 0) {
		return(true);
	}

	os_aio_validate_count = OS_AIO_VALIDATE_SKIP;
	return(os_aio_validate());
}
# endif /* !UNIV_HOTBACKUP */
#endif /* UNIV_DEBUG */

#undef USE_FILE_LOCK
#define USE_FILE_LOCK
#if defined(UNIV_HOTBACKUP) || defined(_WIN32)
/* InnoDB Hot Backup does not lock the data files.
 * On Windows, mandatory locking is used.
 */
# undef USE_FILE_LOCK
#endif
#ifdef USE_FILE_LOCK
/**
Obtain an exclusive lock on a file.
@param[in] fd		file descriptor
@param[in] name		file name
@return 0 on success */
static
int
os_file_lock(
	int		fd,
	const char*	name)
{
	struct flock lk;

	ut_ad(!srv_read_only_mode);

	lk.l_type = F_WRLCK;
	lk.l_whence = SEEK_SET;
	lk.l_start = lk.l_len = 0;

	if (fcntl(fd, F_SETLK, &lk) == -1) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unable to lock %s, error: %d", name, errno);

		if (errno == EAGAIN || errno == EACCES) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Check that you do not already have"
				" another mysqld process using the"
				" same InnoDB data or log files.");
		}

		return(-1);
	}

	return(0);
}
#endif /* USE_FILE_LOCK */

#ifndef UNIV_HOTBACKUP

/**
Calculates local segment number and aio array from global segment number.
@param[out] array		aio wait array
@param[in] segment		global segment number
@return local segment number within the aio array */

ulint
AIO::get_array_and_local_segment(
	AIO**		array,
	ulint		segment)
{
	ulint		local_segment;

	ut_a(segment < os_aio_n_segments);

	if (srv_read_only_mode) {
		*array = s_reads;

		return(segment);
	} else if (segment == IO_IBUF_SEGMENT) {
		*array = s_ibuf;
		local_segment = 0;

	} else if (segment == IO_LOG_SEGMENT) {
		*array = s_log;
		local_segment = 0;

	} else if (segment < s_reads->n_segments + 2) {
		*array = s_reads;

		local_segment = segment - 2;
	} else {
		*array = s_writes;

		local_segment = segment - (s_reads->n_segments + 2);
	}

	return(local_segment);
}

/**
Frees a slot in the aio array. 
@param[in,out] slot	Slot to release */
void
AIO::release_low(Slot* slot)
{
	ut_ad(mutex_own(&mutex));

	ut_ad(slot->is_reserved);

	slot->is_reserved = false;

	--n_reserved;

	if (n_reserved == n_slots - 1) {
		os_event_set(not_full);
	}

	if (n_reserved == 0) {
		os_event_set(is_empty);
	}

#ifdef WIN_ASYNC_IO

	ResetEvent(slot->handle);

#elif defined(LINUX_NATIVE_AIO)

	if (srv_use_native_aio) {
		memset(&slot->control, 0x0, sizeof(slot->control));
		slot->ret = 0;
		slot->n_bytes = 0;
	} else {
		/* These fields should not be used if we are not
		using native AIO. */
		ut_ad(slot->n_bytes == 0);
		ut_ad(slot->ret == 0);
	}

#endif /* WIN_ASYNC_IO */
}

/**
Frees a slot in the aio array. 
@param[in,out] slot	Slot to release */

void
AIO::release(Slot* slot)
{
	mutex_enter(&mutex);

	release_low(slot);

	mutex_exit(&mutex);
}

/**
Creates the seek mutexes used in positioned reads and writes. */

void
os_io_init_simple()
{
#ifndef HAVE_ATOMIC_BUILTINS
	mutex_create("os_file_count_mutex", &os_file_count_mutex);
#endif /* !HAVE_ATOMIC_BUILTINS */

	for (ulint i = 0; i < OS_FILE_N_SEEK_MUTEXES; i++) {
		os_file_seek_mutexes[i] = new(std::nothrow) SysMutex();
		mutex_create("os_file_seek_mutex", os_file_seek_mutexes[i]);
	}
}

/**
Creates a temporary file.  This function is like tmpfile(3), but
the temporary file is created in the MySQL temporary directory.
@return temporary file handle, or NULL on error */

FILE*
os_file_create_tmpfile()
{
	FILE*	file	= NULL;
	int	fd	= innobase_mysql_tmpfile();

	ut_ad(!srv_read_only_mode);

	if (fd >= 0) {
		file = fdopen(fd, "w+b");
	}

	if (!file) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unable to create temporary file; errno: %d", errno);
		if (fd >= 0) {
			close(fd);
		}
	}

	return(file);
}

/**
Rewind file to its start, read at most size - 1 bytes from it to str, and
NUL-terminate str. All errors are silently ignored. This function is
mostly meant to be used with temporary files. */

void
os_file_read_string(
	FILE*	file,	/*!< in: file to read from */
	char*	str,	/*!< in: buffer where to read */
	ulint	size)	/*!< in: size of buffer */
{
	if (size != 0) {
		rewind(file);

		size_t	flen = fread(str, 1, size - 1, file);

		str[flen] = '\0';
	}
}

/**
Decompress after a read and punch a hole in the file if it was a write
@param[in] type		IO context
@param[in] fh		Open file handle
@param[in,out] buf	Buffer to transform
@param[in,out] scratch	Scratch area for read decompression
@param[in] src_len	Length of the buffer before compression
@param[in] len		Used buffer length for write and output buf len for read
@return DB_SUCCESS or error code */
static
dberr_t
os_file_io_complete(
	const IORequest&type,
	os_file_t	fh,
	byte*		buf,
	byte*		scratch,
	ulint		src_len,
	ulint		offset,
	ulint		len)
{
	/* We never compress/decompress the first page */
	ut_a(offset > 0);
	ut_ad(type.validate());
	ut_ad(type.is_compressed());

	if (type.is_read()) {

		return(os_file_page_decompress(buf, scratch, len));

	} else if (type.punch_hole()) {

		ulint	block_size = type.block_size();

		offset += len;

		return(os_file_punch_hole(fh, src_len, block_size, offset, len));
	}

	return(DB_SUCCESS);
}

#endif /* !UNIV_HOTBACKUP */

/**
This function returns a new path name after replacing the basename
in an old path with a new basename.  The old_path is a full path
name including the extension.  The tablename is in the normal
form "databasename/tablename".  The new base name is found after
the forward slash.  Both input strings are null terminated.

This function allocates memory to be returned.  It is the callers
responsibility to free the return value after it is no longer needed.

@return own: new full pathname */

char*
os_file_make_new_pathname(
	const char*	old_path,	/*!< in: pathname */
	const char*	tablename)	/*!< in: contains new base name */
{
	ulint		dir_len;
	char*		last_slash;
	char*		base_name;
	char*		new_path;
	ulint		new_path_len;

	/* Split the tablename into its database and table name components.
	They are separated by a '/'. */
	last_slash = strrchr((char*) tablename, '/');
	base_name = last_slash ? last_slash + 1 : (char*) tablename;

	/* Find the offset of the last slash. We will strip off the
	old basename.ibd which starts after that slash. */
	last_slash = strrchr((char*) old_path, OS_PATH_SEPARATOR);
	dir_len = last_slash ? last_slash - old_path : strlen(old_path);

	/* allocate a new path and move the old directory path to it. */
	new_path_len = dir_len + strlen(base_name) + sizeof "/.ibd";
	new_path = static_cast<char*>(ut_malloc(new_path_len));
	memcpy(new_path, old_path, dir_len);

	ut_snprintf(new_path + dir_len,
		    new_path_len - dir_len,
		    "%c%s.ibd",
		    OS_PATH_SEPARATOR,
		    base_name);

	return(new_path);
}

/**
This function reduces a null-terminated full remote path name into
the path that is sent by MySQL for DATA DIRECTORY clause.  It replaces
the 'databasename/tablename.ibd' found at the end of the path with just
'tablename'.

Since the result is always smaller than the path sent in, no new memory
is allocated. The caller should allocate memory for the path sent in.
This function manipulates that path in place.

If the path format is not as expected, just return.  The result is used
to inform a SHOW CREATE TABLE command. */

void
os_file_make_data_dir_path(
	char*	data_dir_path)	/*!< in/out: full path/data_dir_path */
{
	char*	ptr;
	char*	tablename;
	ulint	tablename_len;

	/* Replace the period before the extension with a null byte. */
	ptr = strrchr((char*) data_dir_path, '.');
	if (!ptr) {
		return;
	}
	ptr[0] = '\0';

	/* The tablename starts after the last slash. */
	ptr = strrchr((char*) data_dir_path, OS_PATH_SEPARATOR);
	if (!ptr) {
		return;
	}
	ptr[0] = '\0';
	tablename = ptr + 1;

	/* The databasename starts after the next to last slash. */
	ptr = strrchr((char*) data_dir_path, OS_PATH_SEPARATOR);
	if (!ptr) {
		return;
	}
	tablename_len = ut_strlen(tablename);

	ut_memmove(++ptr, tablename, tablename_len);

	ptr[tablename_len] = '\0';
}

/**
The function os_file_dirname returns a directory component of a
null-terminated pathname string. In the usual case, dirname returns
the string up to, but not including, the final '/', and basename
is the component following the final '/'. Trailing '/' characters
are not counted as part of the pathname.

If path does not contain a slash, dirname returns the string ".".

Concatenating the string returned by dirname, a "/", and the basename
yields a complete pathname.

The return value is a copy of the directory component of the pathname.
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

@return own: directory component of the pathname */

char*
os_file_dirname(
	const char*	path)	/*!< in: pathname */
{
	/* Find the offset of the last slash */
	const char* last_slash = strrchr(path, OS_PATH_SEPARATOR);
	if (!last_slash) {
		/* No slash in the path, return "." */

		return(mem_strdup("."));
	}

	/* Ok, there is a slash */

	if (last_slash == path) {
		/* last slash is the first char of the path */

		return(mem_strdup("/"));
	}

	/* Non-trivial directory component */

	return(mem_strdupl(path, last_slash - path));
}

/**
Creates all missing subdirectories along the given path.
@return true if call succeeded, false otherwise */

bool
os_file_create_subdirs_if_needed(
	const char*	path)	/*!< in: path name */
{
	if (srv_read_only_mode) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"read only mode set. Can't create subdirectories '%s'",
			path);

		return(false);

	}

	char*	subdir = os_file_dirname(path);

	if (strlen(subdir) == 1
	    && (*subdir == OS_PATH_SEPARATOR || *subdir == '.')) {
		/* subdir is root or cwd, nothing to do */
		ut_free(subdir);

		return(true);
	}

	/* Test if subdir exists */
	os_file_type_t	type;
	bool	subdir_exists;
	bool	success = os_file_status(subdir, &subdir_exists, &type);

	if (success && !subdir_exists) {

		/* subdir does not exist, create it */
		success = os_file_create_subdirs_if_needed(subdir);

		if (!success) {
			ut_free(subdir);

			return(false);
		}

		success = os_file_create_directory(subdir, false);
	}

	ut_free(subdir);

	return(success);
}

#ifndef _WIN32

/**
Free storage space associated with a section of the file.
@param[in] fh		Open file handle
@param[in] page_size	Tablespace page size
@param[in] block_size	File system block size
@param[in] off		Starting offset (SEEK_SET)
@param[in] len		Number of bytes to free at the end of the page, must
			be a multiple of UNIV_SECTOR_SIZE
@return DB_SUCCESS or error code */

dberr_t
os_file_punch_hole(
	os_file_t	fh,
	os_offset_t	page_size,
	ulint		block_size,
	os_offset_t	off,
	os_offset_t	len)
{
#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
	ut_a(page_size >= len);

	os_offset_t	n_bytes;

	n_bytes = ut_calc_align(page_size - len, block_size);

	if (n_bytes == 0) {
		return(DB_SUCCESS);
	}

	int	ret = fallocate(
		fh, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, off, n_bytes);

	if (ret == 0) {
		return(DB_SUCCESS);
	}

	ut_a(ret == -1);

	if (errno == ENOTSUP) {
		return(DB_IO_NO_PUNCH_HOLE);
	}

	ib_logf(IB_LOG_LEVEL_WARN,
		"fallocate(%lu, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,"
		"%lu, %lu) returned errno: %lu",
		(ulint) fh, (ulint) off, (ulint) n_bytes, (ulint) errno);

	return(DB_IO_ERROR);

#elif defined(UNIV_SOLARIS)

	// Use F_FREESP

#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */

	return(DB_IO_NO_PUNCH_HOLE);
}

/**
Decompress after a read and punch a hole in the file if it was a write */
static
dberr_t
os_file_io_complete(const Slot* slot)
{
	ut_a(slot->offset > 0);
	ut_a(slot->type.is_read() || slot->compress_ok);

	return(os_file_io_complete(
			slot->type, slot->file, slot->buf,
			slot->compressed_page, slot->original_len,
			slot->offset, slot->len));
}

#if defined(LINUX_NATIVE_AIO)

/** Linux native AIO handler */
class LinuxAIOHandler {
public:
	/**
	@param[in] global_segment	The global segment*/
	LinuxAIOHandler(ulint global_segment)
		:
		m_global_segment(global_segment)
	{
		/* Should never be doing Sync IO here. */
		ut_a(m_global_segment != ULINT_UNDEFINED);

		/* Find the array and the local segment. */

		m_segment = AIO::get_array_and_local_segment(
			&m_array, m_global_segment);

		m_n_slots = m_array->get_n_slots();
	}

	~LinuxAIOHandler()
	{
		// No op
	}

	/**
	Process a Linux AIO request
	@param[out] m1			the messages passed with the
	@param[out] m2			AIO request; note that in case the
					AIO operation failed, these output
					parameters are valid and can be used to
					restart the operation.
	@param[out] request		IO context
	@return DB_SUCCESS or error code */
	dberr_t poll(fil_node_t** m1, void** m2, IORequest* request);

private:
	/**
	Resubmit an IO request that was only partially successful
	@param[in,out] slot	Request to resubmit
	@return DB_SUCCESS or DB_FAIL if the IO resubmit request failed */
	dberr_t	resubmit(Slot* slot);

	/*
	Check if the AIO succeeded
	@param[in,out] slot		The slot to check
	@return DB_SUCCESS, DB_FAIL if the operation should be retried or
		DB_IO_ERROR on all other errors */
	dberr_t	check_state(Slot* slot);

	/*
	@return true if a shutdown was detected */
	bool is_shutdown() const
	{
		return(srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS);
	}

	/**
	If no slot was found then the m_array->mutex will be released.
	@param[out] n_pending		The number of pending IOs
	@return NULL or a slot that has completed IO */
	Slot* find_completed_slot(ulint* n_pending);

	/**
	This function is only used in Linux native asynchronous i/o. This is
	called from within the io-thread. If there are no completed IO requests
	in the slot array, the thread calls this function to collect more
	requests from the kernel.
	The io-thread waits on io_getevents(), which is a blocking call, with
	a timeout value. Unless the system is very heavy loaded, keeping the
	io-thread very busy, the io-thread will spend most of its time waiting
	in this function.
	The io-thread also exits in this function. It checks server status at
	each wakeup and that is why we use timed wait in io_getevents(). */
	void collect();

	/**
	@param[in] slot		The slot that contains the IO request
	@return true if it was a compressed page */
	bool is_compressed_page(const Slot* slot) const
	{
		const byte*	src = slot->buf;

		ulint	page_type = mach_read_from_2(src + FIL_PAGE_TYPE);

		return(page_type == FIL_PAGE_COMPRESSED);
	}

	/**
	@param[in] slot		The slot that contains the IO request
	@return number of bytes to read for a successful decompress */
	ulint page_size(const Slot* slot) const
	{
		ut_ad(slot->type.is_read());
		ut_ad(is_compressed_page(slot));

		ulint		size;
		const byte*	src = slot->buf;

		size = mach_read_from_2(src + FIL_PAGE_COMPRESS_SIZE_V1);

		return(size + FIL_PAGE_DATA);
	}

	/**
	@param[in] slot		The slot that contains the IO request
	@return true if the data read has all the compressed data */
	bool can_decompress(const Slot* slot) const
	{
		ut_ad(slot->type.is_read());
		ut_ad(is_compressed_page(slot));

		int		version;
		const byte*	src = slot->buf;

		version = mach_read_from_1(src + FIL_PAGE_VERSION);

		ut_a(version == 1);

		/* Includes the page header size too */
		int	size = page_size(slot);

		return(size <= (slot->ptr - slot->buf) + slot->n_bytes);
	}

	/**
	@param[in] slot		The slot that contains the IO request
	@param[in] n_bytes	Total bytes read so far
	@return DB_SUCCESS or error code */
	dberr_t check_read(Slot* slot, ulint n_bytes) const;

private:
	/** Slot array */
	AIO*			m_array;

	/** Number of slots inthe local segment */
	ulint			m_n_slots;

	/** The local segment to check */
	ulint			m_segment;

	/** The global segment */
	ulint			m_global_segment;
};

/**
Resubmit an IO request that was only partially successful
@param[in,out] slot	Request to resubmit
@return DB_SUCCESS or DB_FAIL if the IO resubmit request failed */
dberr_t
LinuxAIOHandler::resubmit(Slot* slot)
{
#ifdef UNIV_DEBUG
	/* Bytes already read/written out */
	ulint	n_bytes = slot->ptr - slot->buf;

	ut_ad(mutex_own(&m_array->mutex));
	ut_ad(n_bytes < slot->original_len);
	ut_ad(static_cast<ulint>(slot->n_bytes) < slot->original_len - n_bytes);
	/* Partial read or write scenario */
	ut_ad(slot->len >= static_cast<ulint>(slot->n_bytes));
#endif /* UNIV_DEBUG */

	slot->len -= slot->n_bytes;
	slot->ptr += slot->n_bytes;
	slot->offset += slot->n_bytes;

	/* Resetting the bytes read/written */
	slot->n_bytes = 0;
	slot->io_already_done = false;

	struct iocb*    iocb = &slot->control;

	if (slot->type.is_read()) {

		io_prep_pread(
			iocb,
			slot->file,
			slot->ptr,
			slot->len,
			static_cast<off_t>(slot->offset));
	} else {

		ut_a(slot->type.is_write());

		io_prep_pwrite(
			iocb,
			slot->file,
			slot->ptr,
			slot->len,
			static_cast<off_t>(slot->offset));
	}

	iocb->data = slot;

	/* Resubmit an I/O request */
	int	ret = io_submit(m_array->aio_ctx[m_segment], 1, &iocb);

	if (ret < -1)  {
		errno = -ret;
	}

	return(ret < 0 ? DB_IO_PARTIAL_FAILED : DB_SUCCESS);
}

/**
@param[in] slot		The slot that contains the IO request
@param[in] n_bytes	Total bytes read so far
@return DB_SUCCESS or error code */
dberr_t
LinuxAIOHandler::check_read(Slot* slot, ulint n_bytes) const
{
	dberr_t	err;

	ut_ad(slot->type.is_read());
	ut_ad(slot->original_len > slot->len);

	if (slot->type.is_compressed() && is_compressed_page(slot)) {

		if (can_decompress(slot)) {

			ut_a(slot->offset > 0);

			slot->len = slot->original_len;
			slot->n_bytes = n_bytes;

			err = os_file_io_complete(slot);
			ut_a(err == DB_SUCCESS);

		} else {
			/* Read the next block in */
			ut_ad(page_size(slot) >= n_bytes);

			lint	remain = page_size(slot) - n_bytes;
			ulint	block_size = slot->type.block_size();

			ut_a(remain > 0);

			slot->len += ut_calc_align(remain, block_size);

			err = DB_FAIL;
		}
	} else {
		/* Try and read the remaining bytes in as is */
		slot->len += slot->original_len - slot->len;

		err = DB_FAIL;
	}

	return(err);
}

/*
Check if the AIO succeeded
@param[in,out] slot		The slot to check
@return DB_SUCCESS, DB_FAIL if the operation should be retried or
	DB_IO_ERROR on all other errors */
dberr_t
LinuxAIOHandler::check_state(Slot* slot)
{
	ut_ad(mutex_own(&m_array->mutex));

	/* Note that it may be that there are more then one completed
	IO requests. We process them one at a time. We may have a case
	here to improve the performance slightly by dealing with all
	requests in one sweep. */

	srv_set_io_thread_op_info(
		m_global_segment, "processing completed aio requests");

	ut_ad(slot->is_reserved);
	ut_ad(slot->io_already_done);

	dberr_t	err;

	if (slot->ret == 0) {

		/* Total bytes read so far */
		ulint	n_bytes = (slot->ptr - slot->buf) + slot->n_bytes;

		/* Compressed writes can be smaller than the original length.
		Therefore they can be processed without further IO. */
	       	if (n_bytes == slot->original_len
		    || (slot->type.is_compressed()
			&& slot->type.is_write()
			&& slot->len == static_cast<ulint>(slot->n_bytes))) {

			if (slot->type.is_compressed()
			    && is_compressed_page(slot)) {

				ut_a(slot->offset > 0);

				if (slot->type.is_read()) {
					slot->len = slot->original_len;
				}

				err = os_file_io_complete(slot);

				// FIXME: Improve error handling
				ut_ad(err == DB_SUCCESS
				      || err == DB_IO_NO_PUNCH_HOLE);
			} else {
				err = DB_SUCCESS;
			}

		} else if (slot->n_bytes == (int) slot->len) {
			/* Has to be a read request, if it is less than
			the original length. */
			ut_ad(slot->type.is_read());
			err = check_read(slot, n_bytes);
		} else {
			err = DB_FAIL;
		}

	} else {
		errno = -slot->ret;

		/* os_file_handle_error does tell us if we should retry
		this IO. As it stands now, we don't do this retry when
		reaping requests from a different context than
		the dispatcher. This non-retry logic is the same for
		windows and linux native AIO.
		We should probably look into this to transparently
		re-submit the IO. */
		os_file_handle_error(slot->name, "Linux aio");

		err = DB_IO_ERROR;
	}

	return(err);
}

/**
If no slot was found then the m_array->mutex will be released.
@param[out] n_pending		The number of pending IOs
@return NULL or a slot that has completed IO */
Slot*
LinuxAIOHandler::find_completed_slot(ulint* n_pending)
{
	ulint	offset = m_n_slots * m_segment;

	*n_pending = 0;

	mutex_enter(&m_array->mutex);

	Slot*	slot = m_array->at(offset);

	for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

		if (slot->is_reserved) {

			++*n_pending;

			if (slot->io_already_done) {

				/* Something for us to work on.
				Note: We don't release the mutex. */
				return(slot);
			}
		}
	}

	mutex_exit(&m_array->mutex);

	return(NULL);
}

/**
This function is only used in Linux native asynchronous i/o. This is
called from within the io-thread. If there are no completed IO requests
in the slot array, the thread calls this function to collect more
requests from the kernel.
The io-thread waits on io_getevents(), which is a blocking call, with
a timeout value. Unless the system is very heavy loaded, keeping the
io-thread very busy, the io-thread will spend most of its time waiting
in this function.
The io-thread also exits in this function. It checks server status at
each wakeup and that is why we use timed wait in io_getevents(). */
void
LinuxAIOHandler::collect()
{
	ut_ad(m_n_slots > 0);
	ut_ad(m_array != NULL);
	ut_ad(m_segment < m_array->n_segments);

	/* Which io_context we are going to use. */
	struct io_context*	io_ctx = m_array->aio_ctx[m_segment];

	/* Starting point of the m_segment we will be working on. */
	ulint	start_pos = m_segment * m_n_slots;

	/* End point. */
	ulint	end_pos = start_pos + m_n_slots;

	for (;;) {
		struct io_event*	events;

		/* Which part of event array we are going to work on. */
		events = &m_array->events[m_segment * m_n_slots];

		/* Initialize the events. */
		memset(events, 0, sizeof(*events) * m_n_slots);

		/* The timeout value is arbitrary. We probably need
		to experiment with it a little. */
		struct timespec		timeout;

		timeout.tv_sec = 0;
		timeout.tv_nsec = OS_AIO_REAP_TIMEOUT;

		int	ret;

		ret = io_getevents(io_ctx, 1, m_n_slots, events, &timeout);

		for (int i = 0; i < ret; ++i) {

			struct iocb*	iocb;

			iocb = reinterpret_cast<struct iocb*>(events[i].obj);
			ut_a(iocb != NULL);

			Slot*	slot = reinterpret_cast<Slot*>(iocb->data);

			/* Some sanity checks. */
			ut_a(slot != NULL);
			ut_a(slot->is_reserved);

			/* We are not scribbling previous segment. */
			ut_a(slot->pos >= start_pos);

			/* We have not overstepped to next segment. */
			ut_a(slot->pos < end_pos);

			/* Mark this request as completed. The error handling
			will be done in the calling function. */
			mutex_enter(&m_array->mutex);

			slot->ret = events[i].res2;
			slot->io_already_done = true;
			slot->n_bytes = events[i].res;
			slot->err = DB_SUCCESS;

			mutex_exit(&m_array->mutex);
		}

		if (srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS || ret > 0) {

			break;
		}

		/* This error handling is for any error in collecting the
		IO requests. The errors, if any, for any particular IO
		request are simply passed on to the calling routine. */

		switch (ret) {
		case -EAGAIN:
			/* Not enough resources! Try again. */

		case -EINTR:
			/* Interrupted! The behaviour in case of an interrupt.
			If we have some completed IOs available then the
			return code will be the number of IOs. We get EINTR
			only if there are no completed IOs and we have been
			interrupted. */

		case 0:
			/* No pending request! Go back and check again. */

			continue;
		}

		/* All other errors should cause a trap for now. */
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unexpected ret_code[%d] from io_getevents()!",
			ret);

		break;
	}
}
/**
Process a Linux AIO request
@param[out] m1			the messages passed with the
@param[out] m2			AIO request; note that in case the
				AIO operation failed, these output
				parameters are valid and can be used to
				restart the operation.
@param[out] request		IO context
@return DB_SUCCESS or error code */
dberr_t
LinuxAIOHandler::poll(fil_node_t** m1, void** m2, IORequest* request)
{
	dberr_t		err;
	Slot*		slot;

	/* Loop until we have found a completed request. */
	for (;;) {

		ulint	n_pending;

		slot = find_completed_slot(&n_pending);

		if (slot != NULL) {

			ut_ad(mutex_own(&m_array->mutex));

			err = check_state(slot);

			/* DB_FAIL is not a hard error, we should retry */
			if (err != DB_FAIL) {
				break;
			}

			/* Partial IO, resubmit request for
			remaining bytes to read/write */
			err = resubmit(slot);

			if (err != DB_SUCCESS) {
				break;
			}

			mutex_exit(&m_array->mutex);

		} else if (is_shutdown() && n_pending == 0) {

			/* There is no completed request. If there is
			no pending request at all, and the system is
			being shut down, exit. */

			*m1 = NULL;
			*m2 = NULL;

			return(DB_SUCCESS);

		} else {

			/* Wait for some request. Note that we return
			from wait iff we have found a request. */

			srv_set_io_thread_op_info(
				m_global_segment,
				"waiting for completed aio requests");

			collect();
		}
	}

	if (err == DB_IO_PARTIAL_FAILED) {
		/* Aborting in case of submit failure */
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Native Linux AIO interface. "
			"io_submit() call failed when "
			"resubmitting a partial I/O "
			"request on the file %s.",
			slot->name);
	}

	*m1 = slot->m1;
	*m2 = slot->m2;

	*request = slot->type;

	m_array->release_low(slot);

	mutex_exit(&m_array->mutex);

	return(err);
}

/**
This function is only used in Linux native asynchronous i/o.
Waits for an aio operation to complete. This function is used to wait for
the completed requests. The aio array of pending requests is divided
into segments. The thread specifies which segment or slot it wants to wait
for. NOTE: this function will also take care of freeing the aio slot,
therefore no other thread is allowed to do the freeing!

@param[in] global_seg		segment number in the aio array
				to wait for; segment 0 is the ibuf
				i/o thread, segment 1 is log i/o thread,
				then follow the non-ibuf read threads,
				and the last are the non-ibuf write
				threads.
@param[out] m1			the messages passed with the
@param[out] m2			AIO request; note that in case the
				AIO operation failed, these output
				parameters are valid and can be used to
				restart the operation.
@param[out] request		IO context
@return true if the IO was successful */

bool
os_aio_linux_handle(
	ulint		global_segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	request)
{
	LinuxAIOHandler	handler(global_segment);

	dberr_t	err = handler.poll(m1, m2, request);

	if (err == DB_IO_NO_PUNCH_HOLE) {
		fil_no_punch_hole(*m1);
		err = DB_SUCCESS;
	}

	return(err == DB_SUCCESS);
}

/**
Dispatch an AIO request to the kernel.
@param[in,out] slot	an already reserved slot
@return true on success. */
bool
AIO::linux_dispatch(Slot* slot)
{
	ut_a(slot->is_reserved);
	ut_ad(slot->type.validate());

	/* Find out what we are going to work with.
	The iocb struct is directly in the slot.
	The io_context is one per segment. */

	ulint		io_ctx_index;
	struct iocb*	iocb = &slot->control;

	io_ctx_index = (slot->pos * n_segments) / n_slots;

	int	ret = io_submit(aio_ctx[io_ctx_index], 1, &iocb);

	/* io_submit() returns number of successfully queued requests
	or -errno. */

	if (ret != 1) {
		errno = -ret;
	}

	return(ret == 1);
}

/**
Creates an io_context for native linux AIO.
@param[in] max_events		number of events
@param[out] io_ctx		io_ctx to initialize.
@return true on success. */
bool
AIO::linux_create_io_ctx(
	ulint		max_events,
	io_context_t*	io_ctx)
{
	ssize_t		n_retries = 0;

	for (;;) {

		memset(io_ctx, 0x0, sizeof(*io_ctx));

		/* Initialize the io_ctx. Tell it how many pending
		IO requests this context will handle. */

		int	ret = io_setup(max_events, io_ctx);

		if (ret == 0) {
			/* Success. Return now. */
			return(true);
		}

		/* If we hit EAGAIN we'll make a few attempts before failing. */

		switch (ret) {
		case -EAGAIN:
			if (n_retries == 0) {
				/* First time around. */
				ib_logf(IB_LOG_LEVEL_WARN,
					"io_setup() failed with EAGAIN."
					" Will make %d attempts before "
					"giving up.",
					OS_AIO_IO_SETUP_RETRY_ATTEMPTS);
			}

			if (n_retries < OS_AIO_IO_SETUP_RETRY_ATTEMPTS) {

				++n_retries;

				ib_logf(IB_LOG_LEVEL_WARN,
					"io_setup() attempt %lu failed.",
					(ulint) n_retries);

				os_thread_sleep(OS_AIO_IO_SETUP_RETRY_SLEEP);

				continue;
			}

			/* Have tried enough. Better call it a day. */
			ib_logf(IB_LOG_LEVEL_ERROR,
				"io_setup() failed with EAGAIN after %d "
				"attempts.",
				OS_AIO_IO_SETUP_RETRY_ATTEMPTS);
			break;

		case -ENOSYS:
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Linux Native AIO interface"
				" is not supported on this platform. Please"
				" check your OS documentation and install"
				" appropriate binary of InnoDB.");

			break;

		default:
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Linux Native AIO setup"
				" returned following error[%d]", -ret);
			break;
		}

		ib_logf(IB_LOG_LEVEL_INFO,
			"You can disable Linux Native AIO by"
			" setting innodb_use_native_aio = 0 in my.cnf");

		break;
	}

	return(false);
}

/**
Checks if the system supports native linux aio. On some kernel
versions where native aio is supported it won't work on tmpfs. In such
cases we can't use native aio as it is not possible to mix simulated
and native aio.
@return: true if supported, false otherwise. */

bool
AIO::is_linux_native_aio_supported()
{
	int		fd;
	io_context_t	io_ctx;
	char		name[1000];

	if (!linux_create_io_ctx(1, &io_ctx)) {
		/* The platform does not support native aio. */
		return(false);
	} else if (!srv_read_only_mode) {
		/* Now check if tmpdir supports native aio ops. */
		fd = innobase_mysql_tmpfile();

		if (fd < 0) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Unable to create temp file to check"
				" native AIO support.");

			return(false);
		}
	} else {

		os_normalize_path_for_win(srv_log_group_home_dir);

		ulint	dirnamelen = strlen(srv_log_group_home_dir);

		ut_a(dirnamelen < (sizeof name) - 10 - sizeof "ib_logfile");
		memcpy(name, srv_log_group_home_dir, dirnamelen);

		/* Add a path separator if needed. */
		if (dirnamelen && name[dirnamelen - 1] != OS_PATH_SEPARATOR) {
			name[dirnamelen++] = OS_PATH_SEPARATOR;
		}

		strcpy(name + dirnamelen, "ib_logfile0");

		fd = ::open(name, O_RDONLY);

		if (fd == -1) {

			ib_logf(IB_LOG_LEVEL_WARN,
				"Unable to open \"%s\" to check"
				" native AIO read support.", name);

			return(false);
		}
	}

	struct io_event	io_event;

	memset(&io_event, 0x0, sizeof(io_event));

	byte*	buf = static_cast<byte*>(ut_malloc(UNIV_PAGE_SIZE * 2));
	byte*	ptr = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

	struct iocb	iocb;

	/* Suppress valgrind warning. */
	memset(buf, 0x00, UNIV_PAGE_SIZE * 2);
	memset(&iocb, 0x0, sizeof(iocb));

	struct iocb*	p_iocb = &iocb;

	if (!srv_read_only_mode) {
		io_prep_pwrite(p_iocb, fd, ptr, UNIV_PAGE_SIZE, 0);
	} else {
		ut_a(UNIV_PAGE_SIZE >= 512);
		io_prep_pread(p_iocb, fd, ptr, 512, 0);
	}

	int	err = io_submit(io_ctx, 1, &p_iocb);

	if (err >= 1) {
		/* Now collect the submitted IO request. */
		err = io_getevents(io_ctx, 1, 1, &io_event, NULL);
	}

	ut_free(buf);
	close(fd);

	switch (err) {
	case 1:
		return(true);

	case -EINVAL:
	case -ENOSYS:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Linux Native AIO not supported. You can either"
			" move %s to a file system that supports native"
			" AIO or you can set innodb_use_native_aio to"
			" FALSE to avoid this message.",
			srv_read_only_mode ? name : "tmpdir");

		/* fall through. */
	default:
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Linux Native AIO check on %s returned error[%d]",
			srv_read_only_mode ? name : "tmpdir", -err);
	}

	return(false);
}

#endif /* LINUX_NATIVE_AIO */

/**
Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in] report_all_errors	true if we want an error message printed of
				all errors
@param[in] on_error_silent	true then don't print any diagnostic to the log
@return error number, or OS error number + 100 */
static
ulint
os_file_get_last_error_low(
	bool	report_all_errors,
	bool	on_error_silent)
{
	int	err = errno;

	if (err == 0) {
		return(0);
	}

	if (report_all_errors
	    || (err != ENOSPC && err != EEXIST && !on_error_silent)) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Operating system error number %d"
			" in a file operation.", err);

		if (err == ENOENT) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means the system"
				" cannot find the path specified.");

			if (srv_is_being_started) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"If you are installing InnoDB,"
					" remember that you must create"
					" directories yourself, InnoDB"
					" does not create them.");
			}
		} else if (err == EACCES) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means mysqld does not have"
				" the access rights to the directory.");
		} else {
			if (strerror(err) != NULL) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Error number %d means '%s'.",
					err, strerror(err));
			}

			ib_logf(IB_LOG_LEVEL_INFO, "%s",
				OPERATING_SYSTEM_ERROR_MSG);
		}
	}

	switch (err) {
	case ENOSPC:
		return(OS_FILE_DISK_FULL);
	case ENOENT:
		return(OS_FILE_NOT_FOUND);
	case EEXIST:
		return(OS_FILE_ALREADY_EXISTS);
	case EXDEV:
	case ENOTDIR:
	case EISDIR:
		return(OS_FILE_PATH_ERROR);
	case EAGAIN:
		if (srv_use_native_aio) {
			return(OS_FILE_AIO_RESOURCES_RESERVED);
		}
		break;
	case EINTR:
		if (srv_use_native_aio) {
			return(OS_FILE_AIO_INTERRUPTED);
		}
		break;
	case EACCES:
		return(OS_FILE_ACCESS_VIOLATION);
	}
	return(OS_FILE_ERROR_MAX + err);
}

/**
Wrapper to fsync(2) that retries the call on some errors.
Returns the value 0 if successful; otherwise the value -1 is returned and
the global variable errno is set to indicate the error.
@param[in] file		open file handle
@return 0 if success, -1 otherwise */

static
int
os_file_fsync_posix(
	os_file_t	file)
{
	ulint		failures = 0;

	for (;;) {

		++os_n_fsyncs;

		int	ret = fsync(file);

		if (ret == -1 && errno == ENOLCK) {

			++failures;

			if (!(failures % 100)) {

				ib_logf(IB_LOG_LEVEL_WARN,
					"fsync(): No locks available;"
					" retrying");
			}

			/* 0.2 sec */
			os_thread_sleep(200000);

		} else {
			return(ret);
		}
	}

	ut_error;

	return(-1);
}

/**
Does a syncronous read or write depending upon the type specified
In case of partial reads/writes the function tries
NUM_RETRIES_ON_PARTIAL_IO times to read/write the complete data.
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer where to read
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@param[out] err		DB_SUCCESS or error code
@return number of bytes read/written, -1 if error */
static __attribute__((warn_unused_result))
ssize_t
os_file_io_posix(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	ulint		n,
	off_t		offset,
	dberr_t*	err)
{
	byte*		ptr;
	ssize_t		n_bytes;
	ulint		n_io_bytes = n;
	byte*		compressed_page;
	ssize_t		bytes_returned = 0;

	// FIXME: Move to a separate function that is called from 
	// the Windows code too.
	if (type.is_compressed() && offset > 0) {

		// FIXME: This is going to be expensive.

		ptr = reinterpret_cast<byte*>(ut_malloc(n * 2));

		if (ptr == NULL) {
			*err = DB_OUT_OF_MEMORY;
			return(-1);
		}

		compressed_page = static_cast<byte*>(
			ut_align(ptr, UNIV_SECTOR_SIZE));

		if (type.is_write() && srv_compression_algorithm > 0) {

			byte*	buf_ptr;
			ulint	compressed_len = n;
			ulint	old_compressed_len;

			old_compressed_len = mach_read_from_2(
				reinterpret_cast<byte*>(buf)
		        	+ FIL_PAGE_COMPRESS_SIZE_V1);

			old_compressed_len = ut_calc_align(
				old_compressed_len + FIL_PAGE_DATA,
				type.block_size());

#ifdef HAVE_LZO1X
			byte	mem[LZO1X_1_15_MEM_COMPRESS];
#else
			byte*	mem = NULL;
#endif /* HAVE_LZO1X */

			buf_ptr = os_file_page_compress(
				n_io_bytes,
				type.block_size(),
				reinterpret_cast<byte*>(buf),
				n,
				compressed_page,
				&compressed_len,
				mem);

			bool	compressed = buf_ptr != buf;

			if (compressed) {

				buf = buf_ptr;
				n = compressed_len;

				if (type.punch_hole()
				    && compressed_len == old_compressed_len) {

					// FIXME:
					//type.clear_punch_hole();
				}
			}
		}

	} else {
		ptr = compressed_page = NULL;
	}

	for (ulint i = 0; i < NUM_RETRIES_ON_PARTIAL_IO; ++i) {

		if (type.is_read()) {
			n_bytes = pread(file, buf, n, offset);
		} else {
			n_bytes = pwrite(file, buf, n, offset);
		}
 
		if ((ulint) n_bytes == n) {

			bytes_returned += n_bytes;

			if (offset > 0 && type.is_compressed()) {

				*err = os_file_io_complete(
					type, file,
					reinterpret_cast<byte*>(buf),
					compressed_page, n_io_bytes, offset, n);

				if (ptr != NULL) {
					ut_free(ptr);
				}
			} else {

				*err = DB_SUCCESS;
			}

			return(n_io_bytes);

		} else if (n_bytes > 0 && (ulint) n_bytes < n) {

			/* For partial read/write scenario */
			if (type.is_read()) {
				ib_logf(IB_LOG_LEVEL_WARN,
					"%lu bytes should have"
					" been read. Only %lu bytes"
					" read. Retrying again to read"
					" the remaining bytes.",
					(ulong) n, (ulong) n_bytes);
			} else {
				ib_logf(IB_LOG_LEVEL_WARN,
				"%lu bytes should have"
				" been written. Only %lu bytes"
				" written. Retrying again to"
				" write the remaining bytes.",
				(ulong) n, (ulong) n_bytes);
			}

			offset += n_bytes;
			n -=  (ulint) n_bytes;
			bytes_returned += (ulint) n_bytes;
			buf = reinterpret_cast<uchar*>(buf) + (ulint) n_bytes;

		} else {
			break;
		}
	}

	if (ptr != NULL) {
		ut_free(ptr);
	}

	*err = DB_IO_ERROR;

	ib_logf(IB_LOG_LEVEL_WARN,
		"Retry attempts for %s partial data failed.",
		type.is_read() ? "reading" : "writing");

	return(bytes_returned);
}

/**
Validate the type, offset and number of bytes to read *
@param[in] type		IO flags
@param[in] offset	Offset from start of the file
@param[in] n		Number of bytes to read from offset */
static
void
os_file_check_args(const IORequest& type, os_offset_t offset, ulint n)
{
	ut_ad(type.validate());

	ut_ad(n > 0);

	/* If off_t is > 4 bytes in size, then we assume we can pass a
	64-bit address */
	off_t		offs = (off_t) offset;

	if (sizeof(off_t) <= 4 && offset != (os_offset_t) offs) {
		ib_logf(IB_LOG_LEVEL_ERROR, "file write at offset > 4 GB.");
	}
}

/**
Does a synchronous read operation in Posix.
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer where to read
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@param[out] err		DB_SUCCESS or error code
@return number of bytes read, -1 if error */
static __attribute__((warn_unused_result))
ssize_t
os_file_pread(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	ulint		n,
	os_offset_t	offset,
	dberr_t*	err)
{
	++os_n_file_reads;

# if defined(HAVE_ATOMIC_BUILTINS)
	(void) os_atomic_increment_ulint(&os_n_pending_reads, 1);
	MONITOR_ATOMIC_INC(MONITOR_OS_PENDING_READS);
# else
	mutex_enter(&os_file_count_mutex);
	++os_n_pending_reads;
	MONITOR_INC(MONITOR_OS_PENDING_READS);
	mutex_exit(&os_file_count_mutex);
# endif /* HAVE_ATOMIC_BUILTINS */

	ssize_t	n_bytes = os_file_io_posix(type, file, buf, n, offset, err);

# ifdef HAVE_ATOMIC_BUILTINS
	(void) os_atomic_decrement_ulint(&os_n_pending_reads, 1);
	MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_READS);
# else
	mutex_enter(&os_file_count_mutex);
	--os_n_pending_reads;
	MONITOR_DEC(MONITOR_OS_PENDING_READS);
	mutex_exit(&os_file_count_mutex);
# endif /* HAVE_ATOMIC_BUILTINS */

	return(n_bytes);
}

/**
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, false if fail
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer where to read
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@param[in] exit_on_err	if true then exit on error
@return DB_SUCCESS or error code */
static
dberr_t
os_file_read_posix(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	bool		exit_on_err)
{
	dberr_t		err;

	os_bytes_read_since_printout += n;


	os_file_check_args(type, offset, n);

	for (;;) {
		ssize_t	n_bytes;
	       
		n_bytes = os_file_pread(type, file, buf, n, offset, &err);

		if (err != DB_SUCCESS) {

			return(err);

		} else if ((ulint) n_bytes == n) {

			return(DB_SUCCESS);
		}

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Tried to read " ULINTPF " bytes at offset"
			" " UINT64PF " was only able to read %ld.",
			n, offset, (lint) n_bytes);

		if (exit_on_err) {

			if (!os_file_handle_error(NULL, "read")) {
				/* Hard error */
				break;
			}

		} else if (!os_file_handle_error_no_exit(NULL, "read", false)) {
			/* Hard error */
			break;
		}

		if (n_bytes > 0 && (ulint) n_bytes < n) {
			n -= (ulint) n_bytes;
			offset += (ulint) n_bytes;
			buf = reinterpret_cast<uchar*>(buf) + (ulint) n_bytes;
		}
	}

	ib_logf(IB_LOG_LEVEL_FATAL,
		"Cannot read from file. OS error number %lu.", (ulong) errno);

	return(err);
}

/**
Does a synchronous write operation in Posix.
@param[in] type,	IO context
@param[in] file		handle to an open file
@param[out] buf		buffer from which to write
@param[in] n		number of bytes to read, starting from offset
@param[in] offset	file offset from the start where to read
@param[out] err		DB_SUCCESS or error code
@return number of bytes written, -1 if error */
static __attribute__((warn_unused_result))
ssize_t
os_file_pwrite(
	IORequest&	type,
	os_file_t	file,
	const byte*	buf,
	ulint		n,
	os_offset_t	offset,
	dberr_t*	err)
{
	ut_ad(!srv_read_only_mode);
	ut_ad(type.validate());

	++os_n_file_writes;

#ifdef HAVE_ATOMIC_BUILTINS
	(void) os_atomic_increment_ulint(&os_n_pending_writes, 1);
	MONITOR_ATOMIC_INC(MONITOR_OS_PENDING_WRITES);
#else
	mutex_enter(&os_file_count_mutex);
	++os_n_pending_writes;
	MONITOR_INC(MONITOR_OS_PENDING_WRITES);
	mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */

	ssize_t	n_bytes = os_file_io_posix(
		type, file, (void*) buf, n, offset, err);

#ifdef HAVE_ATOMIC_BUILTINS
	(void) os_atomic_decrement_ulint(&os_n_pending_writes, 1);
	MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_WRITES);
#else
	mutex_enter(&os_file_count_mutex);
	--os_n_pending_writes;
	MONITOR_DEC(MONITOR_OS_PENDING_WRITES);
	mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */

	return(n_bytes);
}

/**
Requests a synchronous write operation.
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer from which to write
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@return DB_SUCCESS if request was successful, false if fail */
static
dberr_t
os_file_write_posix(
	IORequest&	type,
	const char*	name,
	os_file_t	file,
	const byte*	buf,
	os_offset_t	offset,
	ulint		n)
{
	dberr_t		err;

	os_file_check_args(type, offset, n);

	ssize_t	n_bytes = os_file_pwrite(type, file, buf, n, offset, &err);

	if ((ulint) n_bytes != n && !os_has_said_disk_full) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Write to file %s failed at offset " UINT64PF "."
			" %lu bytes should have been written,"
			" only %ld were written."
			" Operating system error number %lu."
			" Check that your OS and file system"
			" support files of this size."
			" Check also that the disk is not full"
			" or a disk quota exceeded.",
			name, offset, n, (lint) n_bytes,
			(ulint) errno);

		if (strerror(errno) != NULL) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Error number %d means '%s'.",
				errno, strerror(errno));
		}

		ib_logf(IB_LOG_LEVEL_INFO, "%s", OPERATING_SYSTEM_ERROR_MSG);

		os_has_said_disk_full = true;
	}

	return(err);
}

/**
Check the existence and type of the given file.
@param[in] path		path name of file
@param[out] exists	true if the file exists
@param[out] type	Type of the file, if it exists
@return true if call succeeded */

bool
os_file_status_posix(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
	struct stat	statinfo;

	int	ret = stat(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */

	} else if (errno == ENOENT || errno == ENOTDIR) {
		/* file does not exist */
		return(true);

	} else {
		/* file exists, but stat call failed */
		os_file_handle_error_no_exit(path, "stat", false);
		return(false);
	}

	if (S_ISDIR(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_DIR;

	} else if (S_ISLNK(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_LINK;

	} else if (S_ISREG(statinfo.st_mode)) {
		*type = OS_FILE_TYPE_FILE;

	} else {
		*type = OS_FILE_TYPE_UNKNOWN;
	}

	return(true);
}

/**
NOTE! Use the corresponding macro os_file_flush(), not directly this function!
Flushes the write buffers of a given file to the disk.
@param[in] file		handle to a file
@return true if success */

bool
os_file_flush_func(
	os_file_t	file)
{
	int	ret;

	ret = os_file_fsync_posix(file);

	if (ret == 0) {
		return(true);
	}

	/* Since Linux returns EINVAL if the 'file' is actually a raw device,
	we choose to ignore that error if we are using raw disks */

	if (srv_start_raw_disk_in_use && errno == EINVAL) {

		return(true);
	}

	ib_logf(IB_LOG_LEVEL_ERROR, "The OS said file flush did not succeed");

	os_file_handle_error(NULL, "flush");

	/* It is a fatal error if a file flush does not succeed, because then
	the database can get corrupt on disk */
	ut_error;

	return(false);
}

/**
NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[out] success	true if succeed, false if error
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_simple_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool*		success)
{
	os_file_t	file;

	*success = false;

	int		create_flag;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {

		if (access_type == OS_FILE_READ_ONLY) {
			create_flag = O_RDONLY;
		} else if (srv_read_only_mode) {
			create_flag = O_RDONLY;
		} else {
			create_flag = O_RDWR;
		}

	} else if (srv_read_only_mode) {

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else if (create_mode == OS_FILE_CREATE_PATH) {

		/* Create subdirs along the path if needed  */

		*success = os_file_create_subdirs_if_needed(name);

		if (!*success) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Unable to create subdirectories '%s'", name);

			return(OS_FILE_CLOSED);
		}

		create_flag = O_RDWR | O_CREAT | O_EXCL;
		create_mode = OS_FILE_CREATE;
	} else {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file create mode (%lu) for file '%s'",
			create_mode, name);

		return(OS_FILE_CLOSED);
	}

	bool		retry;

	do {
		file = ::open(name, create_flag, os_innodb_umask);

		if (file == -1) {
			*success = false;

			retry = os_file_handle_error(
				name,
				create_mode == OS_FILE_OPEN
				?  "open" : "create");
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

#ifdef USE_FILE_LOCK
	if (!srv_read_only_mode
	    && *success
	    && access_type == OS_FILE_READ_WRITE
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;
	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/**
This function attempts to create a directory named pathname. The new
directory gets default permissions. On Unix the permissions are
(0770 & ~umask). If the directory exists already, nothing is done and
the call succeeds, unless the fail_if_exists arguments is true.
If another error occurs, such as a permission error, this does not crash,
but reports the error and returns false.
@param[in] pathname	directory name as null-terminated string
@param[in] fail_if_exists if true, pre-existing directory is treated as an error.
@return true if call succeeds, false on error */

bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists)
{
	int	rcode = mkdir(pathname, 0770);

	if (!(rcode == 0 || (errno == EEXIST && !fail_if_exists))) {
		/* failure */
		os_file_handle_error_no_exit(pathname, "mkdir", false);

		return(false);
	}

	return (true);
}

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
	bool		error_is_fatal)
{
	os_file_dir_t		dir;
	dir = opendir(dirname);

	if (dir == NULL && error_is_fatal) {
		os_file_handle_error(dirname, "opendir");
	}

	return(dir);
}

/**
Closes a directory stream.
@param[in] dir	directory stream
@return 0 if success, -1 if failure */

int
os_file_closedir(
	os_file_dir_t	dir)
{
	int	ret = closedir(dir);

	if (ret != 0) {
		os_file_handle_error_no_exit(NULL, "closedir", false);
	}

	return(ret);
}

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
	os_file_stat_t*	info)
{
	struct dirent*	ent;
	char*		full_path;
	int		ret;
	struct stat	statinfo;

#ifdef HAVE_READDIR_R
	char		dirent_buf[sizeof(struct dirent)
				   + _POSIX_PATH_MAX + 100];
	/* In /mysys/my_lib.c, _POSIX_PATH_MAX + 1 is used as
	the max file name len; but in most standards, the
	length is NAME_MAX; we add 100 to be even safer */
#endif /* HAVE_READDIR_R */

next_file:

#ifdef HAVE_READDIR_R
	ret = readdir_r(dir, (struct dirent*) dirent_buf, &ent);

	if (ret != 0) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Cannot read directory %s, error %lu",
			dirname, (ulong) ret);

		return(-1);
	}

	if (ent == NULL) {
		/* End of directory */

		return(1);
	}

	ut_a(strlen(ent->d_name) < _POSIX_PATH_MAX + 100 - 1);
#else
	ent = readdir(dir);

	if (ent == NULL) {

		return(1);
	}
#endif /* HAVE_READDIR_R */
	ut_a(strlen(ent->d_name) < OS_FILE_MAX_PATH);

	if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {

		goto next_file;
	}

	strcpy(info->name, ent->d_name);

	full_path = static_cast<char*>(
		ut_malloc(strlen(dirname) + strlen(ent->d_name) + 10));

	sprintf(full_path, "%s/%s", dirname, ent->d_name);

	ret = stat(full_path, &statinfo);

	if (ret) {

		if (errno == ENOENT) {
			/* readdir() returned a file that does not exist,
			it must have been deleted in the meantime. Do what
			would have happened if the file was deleted before
			readdir() - ignore and go to the next entry.
			If this is the last entry then info->name will still
			contain the name of the deleted file when this
			function returns, but this is not an issue since the
			caller shouldn't be looking at info when end of
			directory is returned. */

			ut_free(full_path);

			goto next_file;
		}

		os_file_handle_error_no_exit(full_path, "stat", false);

		ut_free(full_path);

		return(-1);
	}

	info->size = (ib_int64_t) statinfo.st_size;

	if (S_ISDIR(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_DIR;
	} else if (S_ISLNK(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_LINK;
	} else if (S_ISREG(statinfo.st_mode)) {
		info->type = OS_FILE_TYPE_FILE;
	} else {
		info->type = OS_FILE_TYPE_UNKNOWN;
	}

	ut_free(full_path);

	return(0);
}

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
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool*		success)
{
	os_file_t	file;
	bool		retry;
	bool		on_error_no_exit;
	bool		on_error_silent;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		errno = ENOSPC;
		return(OS_FILE_CLOSED);
	);

	int		create_flag;
	const char*	mode_str	= NULL;

	on_error_no_exit = create_mode & OS_FILE_ON_ERROR_NO_EXIT
		? true : false;
	on_error_silent = create_mode & OS_FILE_ON_ERROR_SILENT
		? true : false;

	create_mode &= ~OS_FILE_ON_ERROR_NO_EXIT;
	create_mode &= ~OS_FILE_ON_ERROR_SILENT;

	if (create_mode == OS_FILE_OPEN
	    || create_mode == OS_FILE_OPEN_RAW
	    || create_mode == OS_FILE_OPEN_RETRY) {

		mode_str = "OPEN";

		create_flag = srv_read_only_mode ? O_RDONLY : O_RDWR;

	} else if (srv_read_only_mode) {

		mode_str = "OPEN";

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		mode_str = "CREATE";
		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else if (create_mode == OS_FILE_OVERWRITE) {

		mode_str = "OVERWRITE";
		create_flag = O_RDWR | O_CREAT | O_TRUNC;

	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file create mode (%lu) for file '%s'",
			create_mode, name);

		return(OS_FILE_CLOSED);
	}

	ut_a(type == OS_LOG_FILE
	     || type == OS_DATA_FILE
	     || type == OS_DATA_TEMP_FILE);
	ut_a(purpose == OS_FILE_AIO || purpose == OS_FILE_NORMAL);

#ifdef O_SYNC
	/* We let O_SYNC only affect log files; note that we map O_DSYNC to
	O_SYNC because the datasync options seemed to corrupt files in 2001
	in both Linux and Solaris */

	if (!srv_read_only_mode
	    && type == OS_LOG_FILE
	    && srv_unix_file_flush_method == SRV_UNIX_O_DSYNC) {

		create_flag |= O_SYNC;
	}
#endif /* O_SYNC */

	do {
		file = ::open(name, create_flag, os_innodb_umask);

		if (file == -1) {
			const char*	operation;

			operation = (create_mode == OS_FILE_CREATE
				     && !srv_read_only_mode)
				? "create" : "open";

			*success = false;

			if (on_error_no_exit) {
				retry = os_file_handle_error_no_exit(
					name, operation, on_error_silent);
			} else {
				retry = os_file_handle_error(name, operation);
			}
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	/* We disable OS caching (O_DIRECT) only on data files */

	if (!srv_read_only_mode
	    && *success
	    && (type != OS_LOG_FILE && type != OS_DATA_TEMP_FILE)
	    && (srv_unix_file_flush_method == SRV_UNIX_O_DIRECT
		|| srv_unix_file_flush_method == SRV_UNIX_O_DIRECT_NO_FSYNC)) {

		os_file_set_nocache(file, name, mode_str);
	}

#ifdef USE_FILE_LOCK
	if (!srv_read_only_mode
	    && *success
	    && create_mode != OS_FILE_OPEN_RAW
	    && os_file_lock(file, name)) {

		if (create_mode == OS_FILE_OPEN_RETRY) {

			ut_a(!srv_read_only_mode);

			ib_logf(IB_LOG_LEVEL_INFO,
				"Retrying to lock the first data file");

			for (int i = 0; i < 100; i++) {
				os_thread_sleep(1000000);

				if (!os_file_lock(file, name)) {
					*success = true;
					return(file);
				}
			}

			ib_logf(IB_LOG_LEVEL_INFO,
				"Unable to open the first data file");
		}

		*success = false;
		close(file);
		file = -1;
	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/**
NOTE! Use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A simple function to open or create a file.
@return own: handle to the file, not defined if error, error number
can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_simple_no_error_handling_func(
	const char*	name,	/*!< in: name of the file or path as a
				null-terminated string */
	ulint		create_mode,/*!< in: create mode */
	ulint		access_type,/*!< in: OS_FILE_READ_ONLY,
				OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option is
				used by a backup program reading the file */
	bool*		success)/*!< out: true if succeed, false if error */
{
	os_file_t	file;
	int		create_flag;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	*success = false;

	if (create_mode == OS_FILE_OPEN) {

		if (access_type == OS_FILE_READ_ONLY) {

			create_flag = O_RDONLY;

		} else if (srv_read_only_mode) {

			create_flag = O_RDONLY;

		} else {

			ut_a(access_type == OS_FILE_READ_WRITE
			     || access_type == OS_FILE_READ_ALLOW_DELETE);

			create_flag = O_RDWR;
		}

	} else if (srv_read_only_mode) {

		create_flag = O_RDONLY;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = O_RDWR | O_CREAT | O_EXCL;

	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file create mode (%lu) for file '%s'",
			create_mode, name);

		return(OS_FILE_CLOSED);
	}

	file = ::open(name, create_flag, os_innodb_umask);

	*success = (file != -1);

#ifdef USE_FILE_LOCK
	if (!srv_read_only_mode
	    && *success
	    && access_type == OS_FILE_READ_WRITE
	    && os_file_lock(file, name)) {

		*success = false;
		close(file);
		file = -1;

	}
#endif /* USE_FILE_LOCK */

	return(file);
}

/**
Deletes a file if it exists. The file has to be closed before calling this.
@param[in] name		file path as a null-terminated string
@param[out] exist	indicate if file pre-exist
@return true if success */

bool
os_file_delete_if_exists_func(
	const char*	name,
	bool*		exist)
{
	if (exist) {
		*exist = true;
	}

	int	ret = unlink(name);

	if (ret != 0 && errno == ENOENT) {
		if (exist) {
			*exist = false;
		}
	} else if (ret != 0 && errno != ENOENT) {
		os_file_handle_error_no_exit(name, "delete", false);

		return(false);
	}

	return(true);
}

/**
Deletes a file. The file has to be closed before calling this.
@param[in] name		file path as a null-terminated string
@return true if success */

bool
os_file_delete_func(
	const char*	name)
{
	int	ret = unlink(name);

	if (ret != 0) {
		os_file_handle_error_no_exit(name, "delete", false);

		return(false);
	}

	return(true);
}

/**
NOTE! Use the corresponding macro os_file_rename(), not directly this function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in] oldpath	old file path as a null-terminated string
@param[in] newpath	new file path
@return true if success */

bool
os_file_rename_func(
	const char*	oldpath,
	const char*	newpath)
{
#ifdef UNIV_DEBUG
	os_file_type_t	type;
	bool		exists;

	/* New path must not exist. */
	ut_ad(os_file_status(newpath, &exists, &type));
	ut_ad(!exists);

	/* Old path must exist. */
	ut_ad(os_file_status(oldpath, &exists, &type));
	ut_ad(exists);
#endif /* UNIV_DEBUG */

	int	ret = rename(oldpath, newpath);

	if (ret != 0) {
		os_file_handle_error_no_exit(oldpath, "rename", false);

		return(false);
	}

	return(true);
}

/**
NOTE! Use the corresponding macro os_file_close(), not directly this function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@param[in] file		Handle to close
@return true if success */

bool
os_file_close_func(
	os_file_t	file)
{
	int	ret = close(file);

	if (ret == -1) {
		os_file_handle_error(NULL, "close");

		return(false);
	}

	return(true);
}

/**
Gets a file size.
@param[in] file		handle to an open file
@return file size, or (os_offset_t) -1 on failure */

os_offset_t
os_file_get_size(
	os_file_t	file)
{
	/* Store current position */
	os_offset_t	pos = lseek(file, 0, SEEK_CUR);
	os_offset_t	file_size = lseek(file, 0, SEEK_END);

	/* Restore current position as the function should not change it */
	lseek(file, pos, SEEK_SET);

	return(file_size);
}

/**
Gets a file size.
@param[in] filename	Full path to the filename to check
@return file size if OK, else set m_total_size to ~0 and m_alloc_size to errno */

os_file_size_t
os_file_get_size(
	const char*	filename)
{
	struct stat	s;
	os_file_size_t	file_size;

	int	ret = stat(filename, &s);

	if (ret == 0) {
		file_size.m_total_size = s.st_size;
		/* st_blocks is in 512 byte sized blocks */
		file_size.m_alloc_size = s.st_blocks * 512;
	} else {
		file_size.m_total_size = ~0;
		file_size.m_alloc_size = (os_offset_t) errno;
	}

	return(file_size);
}

/**
This function returns information about the specified file
@param[in] path		pathname of the file
@param[out] stat_info	information of a file in a directory
@param[in,out] statinfo	information of a file in a directory
@param[in] check_rw_perm for testing whether the file can be opened in RW mode
@return DB_SUCCESS if all OK */

dberr_t
os_file_get_status_posix(
	const char*	path,
	os_file_stat_t* stat_info,
	struct stat*	statinfo,
	bool		check_rw_perm)
{
	int	ret = stat(path, statinfo);

	if (ret && (errno == ENOENT || errno == ENOTDIR)) {
		/* file does not exist */

		return(DB_NOT_FOUND);

	} else if (ret) {
		/* file exists, but stat call failed */

		os_file_handle_error_no_exit(path, "stat", false);

		return(DB_FAIL);
	}

	switch (statinfo->st_mode & S_IFMT) {
	case S_IFDIR:
		stat_info->type = OS_FILE_TYPE_DIR;
		break;
	case S_IFLNK:
		stat_info->type = OS_FILE_TYPE_LINK;
		break;
	case S_IFBLK:
		stat_info->type = OS_FILE_TYPE_BLOCK;
		break;
	case S_IFREG:
		stat_info->type = OS_FILE_TYPE_FILE;
		break;
	default:
		stat_info->type = OS_FILE_TYPE_UNKNOWN;
	}

	stat_info->size = statinfo->st_size;
	stat_info->block_size = statinfo->st_blksize;
	stat_info->alloc_size = statinfo->st_blocks * 512;

	if (check_rw_perm
	    && (stat_info->type == OS_FILE_TYPE_FILE
		|| stat_info->type == OS_FILE_TYPE_BLOCK)) {

		int	access = !srv_read_only_mode ? O_RDWR : O_RDONLY;
		int	fh = ::open(path, access, os_innodb_umask);

		if (fh == -1) {
			stat_info->rw_perm = false;
		} else {
			stat_info->rw_perm = true;
			close(fh);
		}
	}

	return(DB_SUCCESS);
}

/** Truncates a file to a specified size in bytes.
Do nothing if the size to preserve is greater or equal to the current
size of the file.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size to preserve in bytes
@return true if success */
static
bool
os_file_truncate_posix(
	const char*     pathname,
	os_file_t       file,
	os_offset_t	size)
{
	int	res = ftruncate(file, size);

	if (res == -1) {
		os_file_handle_error_no_exit(pathname, "truncate", false);
	}

	return(res == 0);
}

/**
Truncates a file at its current position.
@return true if success */

bool
os_file_set_eof(
	FILE*		file)	/*!< in: file to be truncated */
{
	return(!ftruncate(fileno(file), ftell(file)));
}

#ifdef UNIV_HOTBACKUP
/**
Closes a file handle.
@return true if success */

bool
os_file_close_no_error_handling(
	os_file_t	file)	/*!< in, own: handle to a file */
{
	return(close(file) != -1);
}
#endif /* UNIV_HOTBACKUP */

/**
This function can be called if one wants to post a batch of reads and
prefers an i/o-handler thread to handle them all at once later. You must
call os_aio_simulated_wake_handler_threads later to ensure the threads
are not left sleeping! */

void
os_aio_simulated_put_read_threads_to_sleep()
{
	/* No op on non Windows */
}

#else /* !_WIN32 */

/**
Free storage space associated with a section of the file.
@param[in] fh		Open file handle
@param[in] page_size	Tablespace page size
@param[in] block_size	File system block size
@param[in] off		Starting offset (SEEK_SET)
@param[in] len		Number of bytes to free at the end of the page, must
			be a multiple of UNIV_SECTOR_SIZE
@return 0 on success or errno */

dberr_t
os_file_punch_hole(
	os_file_t	fh,
	os_offset_t	page_size,
	ulint		block_size,
	os_offset_t	off,
	os_offset_t	len)
{
	os_offset_t	n_bytes;

	n_bytes = ut_calc_align(page_size - len, block_size);

	FILE_LEVEL_TRIM	trim;

	trim.Key = 0;
	trim.NumRanges = 1;
	trim.Ranges[0].Offset = off;
	trim.Ranges[0].Length = n_bytes;

	BOOL		ret;

	ret = DeviceIoControl(
		fh,
		FSCTL_FILE_LEVEL_TRIM,
		&trim,
		sizeof(trim),
		NULL, NULL, NULL, NULL);

	if (!ret) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"DeviceControl(%lu, trim={off=%lu, len=%lu}) failed."
			fh,
			trim.Ranges[0].Offset,
			trim.Ranges[0].Length);

		return(DB_UNSUPPORTED);
	}

	return(DB_SUCCESS);
}

/**
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, false if fail
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer where to read
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@param[in] exit_on_err	if true then exit on error
@return DB_SUCCESS or error code */
static
dberr_t
os_file_read_win32(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n,
	bool		exit_on_err)
{
	ut_ad(n > 0);

	/* On 64-bit Windows, ulint is 64 bits. But offset and n should be
	no more than 32 bits. */
	ut_a((n & 0xFFFFFFFFUL) == n);

	++os_n_file_reads;
	os_bytes_read_since_printout += n;

	for (;;) {

#ifdef HAVE_ATOMIC_BUILTINS
		(void) os_atomic_increment_ulint(&os_n_pending_reads, 1);
		MONITOR_ATOMIC_INC(MONITOR_OS_PENDING_READS);
#else
		mutex_enter(&os_file_count_mutex);
		++os_n_pending_reads;
		MONITOR_INC(MONITOR_OS_PENDING_READS);
		mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */
 
		DWORD	high = (DWORD) (offset >> 32);
		DWORD	low = (DWORD) offset & 0xFFFFFFFF;

		DWORD	ret2 = SetFilePointer(
			file, low, reinterpret_cast<PLONG>(&high), FILE_BEGIN);

		if (ret2 == 0xFFFFFFFF && GetLastError() != NO_ERROR) {

#ifdef HAVE_ATOMIC_BUILTINS
			(void) os_atomic_decrement_ulint(
				&os_n_pending_reads, 1);

			MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_READS);
#else
			mutex_enter(&os_file_count_mutex);
			--os_n_pending_reads;
			MONITOR_DEC(MONITOR_OS_PENDING_READS);
			mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */
 
			if (exit_on_err) {

				if (!os_file_handle_error(NULL, "read")) {
					/* Hard error */
					break;
				}

			} else if (!os_file_handle_error_no_exit(
					NULL, "read", false)) {
				/* Hard error */
				break;
			}

			continue;
		}

		DWORD	len;
		BOOL	ret = ReadFile(file, buf, (DWORD) n, &len, NULL);
 
#ifndef UNIV_HOTBACKUP
		mutex_exit(os_file_seek_mutexes[i]);
#endif /* !UNIV_HOTBACKUP */

#ifdef HAVE_ATOMIC_BUILTINS
		(void) os_atomic_decrement_ulint(&os_n_pending_reads, 1);
		MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_READS);
#else
		mutex_enter(&os_file_count_mutex);
		--os_n_pending_reads;
		MONITOR_DEC(MONITOR_OS_PENDING_READS);
		mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */

		if (ret && len == n) {
			return(DB_SUCCESS);
		}
	}

	ib_logf(IB_LOG_LEVEL_FATAL,
		"Cannot read from file. OS error number %lu.",
		(ulong) GetLastError());
 
	return(DB_IO_ERROR);
}
 
/**
Requests a synchronous write operation.
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer from which to write
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@return DB_SUCCESS if request was successful, false if fail */
static
dberr_t
os_file_write_win32(
	IORequest&	type,
	const char*	name,
	os_file_t	file,
	const byte*	buf,
	os_offset_t	offset,
	ulint		n)
{
	BOOL		ret;
	DWORD		len;
	DWORD		ret2;
	DWORD		low;
	DWORD		high;
	ulint		n_retries = 0;

#ifndef UNIV_HOTBACKUP
	ulint		i;
#endif /* !UNIV_HOTBACKUP */

	/* On 64-bit Windows, ulint is 64 bits. But offset and n should be
	no more than 32 bits. */
	ut_a((n & 0xFFFFFFFFUL) == n);

	++os_n_file_writes;

	ut_ad(file);
	ut_ad(n > 0);

retry:
	low = (DWORD) offset & 0xFFFFFFFF;
	high = (DWORD) (offset >> 32);

#ifdef HAVE_ATOMIC_BUILTINS
	(void) os_atomic_increment_ulint(&os_n_pending_writes, 1);
	MONITOR_ATOMIC_INC(MONITOR_OS_PENDING_WRITES);
#else
	mutex_enter(&os_file_count_mutex);
	++os_n_pending_writes;
	MONITOR_INC(MONITOR_OS_PENDING_WRITES);
	mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */

#ifndef UNIV_HOTBACKUP
	/* Protect the seek / write operation with a mutex */
	i = ((ulint) file) % OS_FILE_N_SEEK_MUTEXES;

	mutex_enter(os_file_seek_mutexes[i]);
#endif /* !UNIV_HOTBACKUP */

	ret2 = SetFilePointer(
		file, low, reinterpret_cast<PLONG>(&high), FILE_BEGIN);

	if (ret2 == 0xFFFFFFFF && GetLastError() != NO_ERROR) {

#ifndef UNIV_HOTBACKUP
		mutex_exit(os_file_seek_mutexes[i]);
#endif /* !UNIV_HOTBACKUP */

#ifdef HAVE_ATOMIC_BUILTINS
		(void) os_atomic_decrement_ulint(&os_n_pending_writes, 1);
		MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_WRITES);
#else
		mutex_enter(&os_file_count_mutex);
		--os_n_pending_writes;
		MONITOR_DEC(MONITOR_OS_PENDING_WRITES);
		mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */

		ib_logf(IB_LOG_LEVEL_ERROR,
			"File pointer positioning to"
			" file %s failed at offset %llu."
			" Operating system error number %lu. %s",
			name, offset, (ulong) GetLastError(),
			OPERATING_SYSTEM_ERROR_MSG);

		return(DB_IO_ERROR);
	}

	ret = WriteFile(file, buf, (DWORD) n, &len, NULL);

#ifndef UNIV_HOTBACKUP
	mutex_exit(os_file_seek_mutexes[i]);
#endif /* !UNIV_HOTBACKUP */

#ifdef HAVE_ATOMIC_BUILTINS
	(void) os_atomic_decrement_ulint(&os_n_pending_writes, 1);
	MONITOR_ATOMIC_DEC(MONITOR_OS_PENDING_WRITES);
#else
	mutex_enter(&os_file_count_mutex);
	--os_n_pending_writes;
	MONITOR_DEC(MONITOR_OS_PENDING_WRITES);
	mutex_exit(&os_file_count_mutex);
#endif /* HAVE_ATOMIC_BUILTINS */

	if (ret && len == n) {

		return(DB_SUCCESS);
	}

	/* If some background file system backup tool is running, then, at
	least in Windows 2000, we may get here a specific error. Let us
	retry the operation 100 times, with 1 second waits. */

	if (GetLastError() == ERROR_LOCK_VIOLATION && n_retries < 100) {

		os_thread_sleep(1000000);

		++n_retries;

		goto retry;
	}

	if (!os_has_said_disk_full) {
		ulint	err_code;

		err_code = (ulint) GetLastError();

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Write to file %s failed at offset %llu."
			" %lu bytes should have been written,"
			" only %lu were written."
			" Operating system error number %lu."
			" Check that your OS and file system"
			" support files of this size."
			" Check also that the disk is not full"
			" or a disk quota exceeded.",
			name, offset,
			(ulong) n, (ulong) len, (ulong) err_code);

		if (strerror((int) err_code) != NULL) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Error number %lu means '%s'.",
				(ulong) err, strerror((int) err_code));
		}

		ib_logf(IB_LOG_LEVEL_INFO, "%s",
			OPERATING_SYSTEM_ERROR_MSG);

		os_has_said_disk_full = true;
	}

	return(DB_IO_ERROR);
}

/**
Check the existence and type of the given file.
@param[in] path		path name of file
@param[out] exists	true if the file exists
@param[out] type	Type of the file, if it exists
@return true if call succeeded */

bool
os_file_status_win32(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
	int		ret;
	struct _stat64	statinfo;

	ret = _stat64(path, &statinfo);

	*exists = !ret;

	if (!ret) {
		/* file exists, everything OK */

	} else if (errno == ENOENT || errno == ENOTDIR) {
		/* file does not exist */
		return(true);

	} else {
		/* file exists, but stat call failed */
		os_file_handle_error_no_exit(path, "stat", false);
		return(false);
	}

	if (_S_IFDIR & statinfo.st_mode) {
		*type = OS_FILE_TYPE_DIR;

	} else if (_S_IFREG & statinfo.st_mode) {
		*type = OS_FILE_TYPE_FILE;

	} else {
		*type = OS_FILE_TYPE_UNKNOWN;
	}

	return(true);
}

/**
NOTE! Use the corresponding macro os_file_flush(), not directly this function!
Flushes the write buffers of a given file to the disk.
@param[in] file		handle to a file
@return true if success */

bool
os_file_flush_func(
	os_file_t	file)
{
	BOOL	ret;

	++os_n_fsyncs;

	ret = FlushFileBuffers(file);

	if (ret) {
		return(true);
	}

	/* Since Windows returns ERROR_INVALID_FUNCTION if the 'file' is
	actually a raw device, we choose to ignore that error if we are using
	raw disks */

	if (srv_start_raw_disk_in_use && GetLastError()
	    == ERROR_INVALID_FUNCTION) {
		return(true);
	}

	os_file_handle_error(NULL, "flush");

	/* It is a fatal error if a file flush does not succeed, because then
	the database can get corrupt on disk */
	ut_error;

	return(false);
}

/**
Gets the operating system version. Currently works only on Windows.
@return OS_WIN95, OS_WIN31, OS_WINNT, OS_WIN2000, OS_WINXP, OS_WINVISTA,
OS_WIN7. */

ulint
os_get_os_version()
{
	OSVERSIONINFO	os_info;

	os_info.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);

	ut_a(GetVersionEx(&os_info));

	if (os_info.dwPlatformId == VER_PLATFORM_WIN32s) {
		return(OS_WIN31);
	} else if (os_info.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS) {
		return(OS_WIN95);
	} else if (os_info.dwPlatformId == VER_PLATFORM_WIN32_NT) {
		switch (os_info.dwMajorVersion) {
		case 3:
		case 4:
			return(OS_WINNT);
		case 5:
			return (os_info.dwMinorVersion == 0)
				? OS_WIN2000 : OS_WINXP;
		case 6:
			return (os_info.dwMinorVersion == 0)
				? OS_WINVISTA : OS_WIN7;
		default:
			return(OS_WIN7);
		}
	} else {
		ut_error;
		return(0);
	}
}

/**
Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in] report_all_errors	true if we want an error message printed
				of all errors
@param[in] on_error_silent)	true then don't print any diagnostic to the log
@return error number, or OS error number + 100 */
static
ulint
os_file_get_last_error_low(
	bool	report_all_errors,
	bool	on_error_silent)
{
	ulint	err = (ulint) GetLastError();

	if (err == ERROR_SUCCESS) {
		return(0);
	}

	if (report_all_errors
	    || (!on_error_silent
		&& err != ERROR_DISK_FULL
		&& err != ERROR_FILE_EXISTS)) {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Operating system error number %lu"
			" in a file operation.", (ulong) err);

		if (err == ERROR_PATH_NOT_FOUND) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means the system"
				" cannot find the path specified.");

			if (srv_is_being_started) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"If you are installing InnoDB,"
					" remember that you must create"
					" directories yourself, InnoDB"
					" does not create them.");
			}
		} else if (err == ERROR_ACCESS_DENIED) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means mysqld does not have"
				" the access rights to"
				" the directory. It may also be"
				" you have created a subdirectory"
				" of the same name as a data file.");
		} else if (err == ERROR_SHARING_VIOLATION
			   || err == ERROR_LOCK_VIOLATION) {
				ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means that another program"
				" is using InnoDB's files."
				" This might be a backup or antivirus"
				" software or another instance"
				" of MySQL."
				" Please close it to get rid of this error.");
		} else if (err == ERROR_WORKING_SET_QUOTA
			   || err == ERROR_NO_SYSTEM_RESOURCES) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means that there are no"
				" sufficient system resources or quota to"
				" complete the operation.");
		} else if (err == ERROR_OPERATION_ABORTED) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"The error means that the I/O"
				" operation has been aborted"
				" because of either a thread exit"
				" or an application request."
				" Retry attempt is made.");
		} else {
			ib_logf(IB_LOG_LEVEL_INFO, "%s",
				OPERATING_SYSTEM_ERROR_MSG);
		}
	}

	if (err == ERROR_FILE_NOT_FOUND) {
		return(OS_FILE_NOT_FOUND);
	} else if (err == ERROR_DISK_FULL) {
		return(OS_FILE_DISK_FULL);
	} else if (err == ERROR_FILE_EXISTS) {
		return(OS_FILE_ALREADY_EXISTS);
	} else if (err == ERROR_SHARING_VIOLATION
		   || err == ERROR_LOCK_VIOLATION) {
		return(OS_FILE_SHARING_VIOLATION);
	} else if (err == ERROR_WORKING_SET_QUOTA
		   || err == ERROR_NO_SYSTEM_RESOURCES) {
		return(OS_FILE_INSUFFICIENT_RESOURCE);
	} else if (err == ERROR_OPERATION_ABORTED) {
		return(OS_FILE_OPERATION_ABORTED);
	} else if (err == ERROR_ACCESS_DENIED) {
		return(OS_FILE_ACCESS_VIOLATION);
	}

	return(OS_FILE_ERROR_MAX + err);
}

/**
NOTE! Use the corresponding macro os_file_create_simple(), not directly
this function!
A simple function to open or create a file.
@param[in] name		name of the file or path as a null-terminated string
@param[in] create_mode	create mode
@param[in] access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[out] success	true if succeed, false if error
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_simple_func(
	const char*	name,
	ulint		create_mode,
	ulint		access_type,
	bool*		success)
{
	os_file_t	file;
	bool		retry;

	*success = false;

	DWORD		access;
	DWORD		create_flag;
	DWORD		attributes = 0;

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {

		create_flag = OPEN_EXISTING;

	} else if (srv_read_only_mode) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else if (create_mode == OS_FILE_CREATE_PATH) {

		ut_a(!srv_read_only_mode);

		/* Create subdirs along the path if needed  */
		*success = os_file_create_subdirs_if_needed(name);

		if (!*success) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Unable to create subdirectories '%s'", name);

			return(OS_FILE_CLOSED);
		}

		create_flag = CREATE_NEW;
		create_mode = OS_FILE_CREATE;

	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file create mode (%lu) for file '%s'",
			create_mode, name);

		return(OS_FILE_CLOSED);
	}

	if (access_type == OS_FILE_READ_ONLY) {
		access = GENERIC_READ;
	} else if (srv_read_only_mode) {

		ib_logf(IB_LOG_LEVEL_INFO,
			"Read only mode set. Unable to"
			" open file '%s' in RW mode, trying RO mode", name);

		access = GENERIC_READ;

	} else if (access_type == OS_FILE_READ_WRITE) {
		access = GENERIC_READ | GENERIC_WRITE;
	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file access type (%lu) for file '%s'",
			access_type, name);

		return(OS_FILE_CLOSED);
	}

	do {
		/* Use default security attributes and no template file. */

		file = CreateFile(
			(LPCTSTR) name, access, FILE_SHARE_READ, NULL,
			create_flag, attributes, NULL);

		if (file == INVALID_HANDLE_VALUE) {

			*success = false;

			retry = os_file_handle_error(
				name, create_mode == OS_FILE_OPEN ?
				"open" : "create");

		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	return(file);
}

/**
This function attempts to create a directory named pathname. The new
directory gets default permissions. On Unix the permissions are
(0770 & ~umask). If the directory exists already, nothing is done and
the call succeeds, unless the fail_if_exists arguments is true.
If another error occurs, such as a permission error, this does not crash,
but reports the error and returns false.
@param[in] pathname	directory name as null-terminated string
@param[in] fail_if_exists if true, pre-existing directory is treated as an error.
@return true if call succeeds, false on error */

bool
os_file_create_directory(
	const char*	pathname,
	bool		fail_if_exists)
{
	BOOL	rcode;

	rcode = CreateDirectory((LPCTSTR) pathname, NULL);
	if (!(rcode != 0
	      || (GetLastError() == ERROR_ALREADY_EXISTS
		  && !fail_if_exists))) {

		os_file_handle_error_no_exit(
			pathname, "CreateDirectory", false);

		return(false);
	}

	return(true);
}

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
	bool		error_is_fatal)
{
	os_file_dir_t		dir;
	LPWIN32_FIND_DATA	lpFindFileData;
	char			path[OS_FILE_MAX_PATH + 3];

	ut_a(strlen(dirname) < OS_FILE_MAX_PATH);

	strcpy(path, dirname);
	strcpy(path + strlen(path), "\\*");

	/* Note that in Windows opening the 'directory stream' also retrieves
	the first entry in the directory. Since it is '.', that is no problem,
	as we will skip over the '.' and '..' entries anyway. */

	lpFindFileData = static_cast<LPWIN32_FIND_DATA>(
		ut_malloc(sizeof(WIN32_FIND_DATA)));

	dir = FindFirstFile((LPCTSTR) path, lpFindFileData);

	ut_free(lpFindFileData);

	if (dir == INVALID_HANDLE_VALUE) {

		if (error_is_fatal) {
			os_file_handle_error(dirname, "opendir");
		}

		return(NULL);
	}

	return(dir);
}

/**
Closes a directory stream.
@param[in] dir	directory stream
@return 0 if success, -1 if failure */

int
os_file_closedir(
	os_file_dir_t	dir)
{
	BOOL		ret;

	ret = FindClose(dir);

	if (!ret) {
		os_file_handle_error_no_exit(NULL, "closedir", false);

		return(-1);
	}

	return(0);
}

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
	os_file_stat_t*	info)
{
	LPWIN32_FIND_DATA	lpFindFileData;
	BOOL			ret;

	lpFindFileData = static_cast<LPWIN32_FIND_DATA>(
		ut_malloc(sizeof(WIN32_FIND_DATA)));
next_file:
	ret = FindNextFile(dir, lpFindFileData);

	if (ret) {
		ut_a(strlen((char*) lpFindFileData->cFileName)
		     < OS_FILE_MAX_PATH);

		if (strcmp((char*) lpFindFileData->cFileName, ".") == 0
		    || strcmp((char*) lpFindFileData->cFileName, "..") == 0) {

			goto next_file;
		}

		strcpy(info->name, (char*) lpFindFileData->cFileName);

		info->size = (ib_int64_t)(lpFindFileData->nFileSizeLow)
			+ (((ib_int64_t)(lpFindFileData->nFileSizeHigh))
			   << 32);

		if (lpFindFileData->dwFileAttributes
		    & FILE_ATTRIBUTE_REPARSE_POINT) {
			/* TODO: test Windows symlinks */
			/* TODO: MySQL has apparently its own symlink
			implementation in Windows, dbname.sym can
			redirect a database directory:
			REFMAN "windows-symbolic-links.html" */
			info->type = OS_FILE_TYPE_LINK;
		} else if (lpFindFileData->dwFileAttributes
			   & FILE_ATTRIBUTE_DIRECTORY) {
			info->type = OS_FILE_TYPE_DIR;
		} else {
			/* It is probably safest to assume that all other
			file types are normal. Better to check them rather
			than blindly skip them. */

			info->type = OS_FILE_TYPE_FILE;
		}
	}

	ut_free(lpFindFileData);

	if (ret) {
		return(0);
	} else if (GetLastError() == ERROR_NO_MORE_FILES) {

		return(1);
	} 

	os_file_handle_error_no_exit(NULL, "readdir_next_file", false);
	return(-1);
}

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
@return handle to the file, not defined if error, error number
	can be retrieved with os_file_get_last_error */

os_file_t
os_file_create_func(
	const char*	name,
	ulint		create_mode,
	ulint		purpose,
	ulint		type,
	bool*		success)
{
	os_file_t	file;
	bool		retry;
	bool		on_error_no_exit;
	bool		on_error_silent;

	*success = false;

	DBUG_EXECUTE_IF(
		"ib_create_table_fail_disk_full",
		*success = false;
		SetLastError(ERROR_DISK_FULL);
		return(OS_FILE_CLOSED);
	);

	DWORD		create_flag;
	DWORD		share_mode	= FILE_SHARE_READ;

	on_error_no_exit = create_mode & OS_FILE_ON_ERROR_NO_EXIT
		? true : false;

	on_error_silent = create_mode & OS_FILE_ON_ERROR_SILENT
		? true : false;

	create_mode &= ~OS_FILE_ON_ERROR_NO_EXIT;
	create_mode &= ~OS_FILE_ON_ERROR_SILENT;

	if (create_mode == OS_FILE_OPEN_RAW) {

		ut_a(!srv_read_only_mode);

		create_flag = OPEN_EXISTING;

		/* On Windows Physical devices require admin privileges and
		have to have the write-share mode set. See the remarks
		section for the CreateFile() function documentation in MSDN. */

		share_mode |= FILE_SHARE_WRITE;

	} else if (create_mode == OS_FILE_OPEN
		   || create_mode == OS_FILE_OPEN_RETRY) {

		create_flag = OPEN_EXISTING;

	} else if (srv_read_only_mode) {

		create_flag = OPEN_EXISTING;

	} else if (create_mode == OS_FILE_CREATE) {

		create_flag = CREATE_NEW;

	} else if (create_mode == OS_FILE_OVERWRITE) {

		create_flag = CREATE_ALWAYS;

	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file create mode (%lu) for file '%s'",
			create_mode, name);

		return(OS_FILE_CLOSED);
	}

	DWORD		attributes = 0;

#ifdef UNIV_HOTBACKUP
	attributes |= FILE_FLAG_NO_BUFFERING;
#else
	if (purpose == OS_FILE_AIO) {

#ifdef WIN_ASYNC_IO
		/* If specified, use asynchronous (overlapped) io and no
		buffering of writes in the OS */

		if (srv_use_native_aio) {
			attributes |= FILE_FLAG_OVERLAPPED;
		}
#endif /* WIN_ASYNC_IO */

	} else if (purpose == OS_FILE_NORMAL) {
		/* Use default setting. */
	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown purpose flag (%lu) while opening file '%s'",
			purpose, name);

		return(OS_FILE_CLOSED);
	}

#ifdef UNIV_NON_BUFFERED_IO
	// TODO: Create a bug, this looks wrong. The flush log
	// parameter is dynamic.
	if (type == OS_FILE_LOG && srv_flush_log_at_trx_commit == 2) {

		/* Do not use unbuffered i/o for the log files because
		value 2 denotes that we do not flush the log at every
		commit, but only once per second */

	} else if (srv_win_file_flush_method == SRV_WIN_IO_UNBUFFERED) {

		attributes |= FILE_FLAG_NO_BUFFERING;
	}
#endif /* UNIV_NON_BUFFERED_IO */

#endif /* UNIV_HOTBACKUP */
	DWORD	access = GENERIC_READ;

	if (!srv_read_only_mode) {
		access |= GENERIC_WRITE;
	}

	do {
		/* Use default security attributes and no template file. */
		file = CreateFile(
			(LPCTSTR) name, access, share_mode, NULL,
			create_flag, attributes, NULL);

		if (file == INVALID_HANDLE_VALUE) {
			const char*	operation;

			operation = (create_mode == OS_FILE_CREATE
				     && !srv_read_only_mode)
				? "create" : "open";

			*success = false;

			if (on_error_no_exit) {
				retry = os_file_handle_error_no_exit(
					name, operation, on_error_silent);
			} else {
				retry = os_file_handle_error(name, operation);
			}
		} else {
			*success = true;
			retry = false;
		}

	} while (retry);

	return(file);
}

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
{
	os_file_t	file;

	*success = false;

	DWORD		access;
	DWORD		create_flag;
	DWORD		attributes	= 0;
	DWORD		share_mode	= FILE_SHARE_READ;

	ut_a(name);

	ut_a(!(create_mode & OS_FILE_ON_ERROR_SILENT));
	ut_a(!(create_mode & OS_FILE_ON_ERROR_NO_EXIT));

	if (create_mode == OS_FILE_OPEN) {
		create_flag = OPEN_EXISTING;
	} else if (srv_read_only_mode) {
		create_flag = OPEN_EXISTING;
	} else if (create_mode == OS_FILE_CREATE) {
		create_flag = CREATE_NEW;
	} else {

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file create mode (%lu) for file '%s'",
			create_mode, name);

		return(OS_FILE_CLOSED);
	}

	if (access_type == OS_FILE_READ_ONLY) {
		access = GENERIC_READ;
	} else if (srv_read_only_mode) {
		access = GENERIC_READ;
	} else if (access_type == OS_FILE_READ_WRITE) {
		access = GENERIC_READ | GENERIC_WRITE;
	} else if (access_type == OS_FILE_READ_ALLOW_DELETE) {

		ut_a(!srv_read_only_mode);

		access = GENERIC_READ;

		/*!< A backup program has to give mysqld the maximum
		freedom to do what it likes with the file */

		share_mode |= FILE_SHARE_DELETE | FILE_SHARE_WRITE;
	} else {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Unknown file access type (%lu) for file '%s'",
			access_type, name);

		return(OS_FILE_CLOSED);
	}

	file = CreateFile((LPCTSTR) name,
			  access,
			  share_mode,
			  NULL,			// Security attributes
			  create_flag,
			  attributes,
			  NULL);		// No template file

	*success = (file != INVALID_HANDLE_VALUE);

	return(file);
}

/**
Deletes a file if it exists. The file has to be closed before calling this.
@param[in] name		file path as a null-terminated string
@param[out] exist	indicate if file pre-exist
@return true if success */

bool
os_file_delete_if_exists_func(
	const char*	name,
	bool*		exist)
{
	bool	ret;
	ulint	count	= 0;

	if (exist) {
		*exist = true;
	}
loop:
	/* In Windows, deleting an .ibd file may fail if ibbackup is copying
	it */

	ret = DeleteFile((LPCTSTR) name);

	if (ret) {
		return(true);
	}

	DWORD lasterr = GetLastError();
	if (lasterr == ERROR_FILE_NOT_FOUND
	    || lasterr == ERROR_PATH_NOT_FOUND) {
		/* the file does not exist, this not an error */
		if (exist) {
			*exist = false;
		}
		return(true);
	}

	++count;

	if (count > 100 && 0 == (count % 10)) {
		os_file_get_last_error(true); /* print error information */

		ib_logf(IB_LOG_LEVEL_WARN, "Delete of file %s failed.", name);
	}

	os_thread_sleep(1000000);	/* sleep for a second */

	if (count > 2000) {

		return(false);
	}

	goto loop;
}

/**
Deletes a file. The file has to be closed before calling this.
@return true if success */

bool
os_file_delete_func(
	const char*	name)	/*!< in: file path as a null-terminated
				string */
{
	ulint	count	= 0;

	for (;;) {
		/* In Windows, deleting an .ibd file may fail if ibbackup
		is copying it */

		BOOL	ret = DeleteFile((LPCTSTR) name);

		if (ret) {
			return(true);
		}

		if (GetLastError() == ERROR_FILE_NOT_FOUND) {
			/* If the file does not exist, we classify this as
			a 'mild' error and return */

			return(false);
		}

		++count;

		if (count > 100 && 0 == (count % 10)) {
			/* print error information */
			os_file_get_last_error(true);

			ib_logf(IB_LOG_LEVEL_WARN,
				"Cannot delete file %s. Are you running"
				" ibbackup to back up the file?", name);
		}

		/* sleep for a second */
		os_thread_sleep(1000000);

		if (count > 2000) {

			return(false);
		}
	}

	ut_error;
	return(false);
}

/**
NOTE! Use the corresponding macro os_file_rename(), not directly this function!
Renames a file (can also move it to another directory). It is safest that the
file is closed before calling this function.
@param[in] oldpath	old file path as a null-terminated string
@param[in] newpath	new file path
@return true if success */

bool
os_file_rename_func(
	const char*	oldpath,
	const char*	newpath)
{
#ifdef UNIV_DEBUG
	os_file_type_t	type;
	bool		exists;

	/* New path must not exist. */
	ut_ad(os_file_status(newpath, &exists, &type));
	ut_ad(!exists);

	/* Old path must exist. */
	ut_ad(os_file_status(oldpath, &exists, &type));
	ut_ad(exists);
#endif /* UNIV_DEBUG */

	BOOL	ret;

	ret = MoveFile((LPCTSTR) oldpath, (LPCTSTR) newpath);

	if (ret) {
		return(true);
	}

	os_file_handle_error_no_exit(oldpath, "rename", false);

	return(false);
}

/**
NOTE! Use the corresponding macro os_file_close(), not directly this function!
Closes a file handle. In case of error, error number can be retrieved with
os_file_get_last_error.
@return true if success */

bool
os_file_close_func(
	os_file_t	file)	/*!< in, own: handle to a file */
{
	ut_a(file);

	BOOL	ret = CloseHandle(file);

	if (ret) {
		return(true);
	}

	os_file_handle_error(NULL, "close");

	return(false);
}

/**
Gets a file size.
@return file size, or (os_offset_t) -1 on failure */

os_offset_t
os_file_get_size(
	os_file_t	file)	/*!< in: handle to a file */
{
	DWORD		high;
	DWORD		low = GetFileSize(file, &high);

	if (low == 0xFFFFFFFF && GetLastError() != NO_ERROR) {
		return((os_offset_t) -1);
	}

	return(os_offset_t(low | (os_offset_t(high) << 32)));
}

/**
This function returns information about the specified file
@param[in] path		pathname of the file
@param[out] stat_info	information of a file in a directory
@param[in,out] statinfo	information of a file in a directory
@param[in] check_rw_perm for testing whether the file can be opened in RW mode
@return DB_SUCCESS if all OK */
static
dberr_t
os_file_get_status_win32(
	const char*	path,
	os_file_stat_t* stat_info,
	struct _stat64	statinfo,
	bool		check_rw_perm)
{
	int	ret = _stat64(path, statinfo);

	if (ret && (errno == ENOENT || errno == ENOTDIR)) {
		/* file does not exist */

		return(DB_NOT_FOUND);

	} else if (ret) {
		/* file exists, but stat call failed */

		os_file_handle_error_no_exit(path, "stat", false);

		return(DB_FAIL);

	} else if (_S_IFDIR & statinfo->st_mode) {
		stat_info->type = OS_FILE_TYPE_DIR;
	} else if (_S_IFREG & statinfo->st_mode) {

		DWORD	access = GENERIC_READ;

		if (!srv_read_only_mode) {
			access |= GENERIC_WRITE;
		}

		stat_info->type = OS_FILE_TYPE_FILE;

		/* Check if we can open it in read-only mode. */

		if (check_rw_perm) {
			HANDLE	fh;

			fh = CreateFile(
				(LPCTSTR) path,		// File to open
				access,
				0,			// No sharing
				NULL,			// Default security
				OPEN_EXISTING,		// Existing file only
				FILE_ATTRIBUTE_NORMAL,	// Normal file
				NULL);			// No attr. template

			if (fh == INVALID_HANDLE_VALUE) {
				stat_info->rw_perm = false;
			} else {
				stat_info->rw_perm = true;
				CloseHandle(fh);
			}
		}
	} else {
		stat_info->type = OS_FILE_TYPE_UNKNOWN;
	}

	return(DB_SUCCESS);
}

/** Truncates a file to a specified size in bytes.
Do nothing if the size to preserve is greater or equal to the current
size of the file.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size to preserve in bytes
@return true if success */
static
bool
os_file_truncate_win32(
	const char*     pathname,
	os_file_t       file,
	os_offset_t	size)
{
	LARGE_INTEGER    length;

	length.QuadPart = size;

	BOOL	success = SetFilePointerEx(file, length, NULL, FILE_BEGIN);

	if (!success) {
		os_file_handle_error_no_exit(
			pathname, "SetFilePointerEx", false);
	} else {
		success = SetEndOfFile(file);
		if (!success) {
			os_file_handle_error_no_exit(
				pathname, "SetEndOfFile", false);
		}
	}
	return(success);
}

/**
Truncates a file at its current position.
@param[in] file		Handle to be truncated
@return true if success */

bool
os_file_set_eof(
	FILE*		file)
{
	HANDLE	h = (HANDLE) _get_osfhandle(fileno(file));

	return(SetEndOfFile(h));
}

#ifdef UNIV_HOTBACKUP
/**
Closes a file handle.
@param[in] file		Handle to close
@return true if success */

bool
os_file_close_no_error_handling(
	os_file_t	file)
{
	return(CloseHandle(file) ? true : false);
}
#endif /* UNIV_HOTBACKUP */

/**
Normalizes a directory path for Windows: converts slashes to backslashes.
@param[in,out] str A null-terminated Windows directory and file path */

void
os_normalize_path_for_win(char* str)
{
	for (; *str; str++) {
		if (*str == '/') {
			*str = '\\';
		}
	}
}

/**
This function can be called if one wants to post a batch of reads and
prefers an i/o-handler thread to handle them all at once later. You must
call os_aio_simulated_wake_handler_threads later to ensure the threads
are not left sleeping! */

void
os_aio_simulated_put_read_threads_to_sleep()
{
	/* The idea of putting background IO threads to sleep is only for
	Windows when using simulated AIO. Windows XP seems to schedule
	background threads too eagerly to allow for coalescing during
	readahead requests. */

	AIO*	array;

	if (srv_use_native_aio) {
		/* We do not use simulated aio: do nothing */

		return;
	}

	os_aio_recommend_sleep_for_read_threads	= true;

	for (ulint i = 0; i < os_aio_n_segments; i++) {
		AIO::get_array_and_local_segment(&array, i);

		if (array == AIO::s_reads) {

			os_event_reset(os_aio_segment_wait_events[i]);
		}
	}
}
#endif /* !_WIN32*/

/**
Retrieves the last error number if an error occurs in a file io function.
The number should be retrieved before any other OS calls (because they may
overwrite the error number). If the number is not known to this program,
the OS error number + 100 is returned.
@param[in] report_all_errors	true if we want an error printed for all errors
@return error number, or OS error number + 100 */

ulint
os_file_get_last_error(
	bool	report_all_errors)
{
	return(os_file_get_last_error_low(report_all_errors, false));
}

/**
Does error handling when a file operation fails.
Conditionally exits (calling exit(3)) based on should_exit value and the
error type, if should_exit is true then on_error_silent is ignored.
@param[in] name		name of a file or NULL
@param[in] operation	operation
@param[in] should_exit	call exit(3) if unknown error and this parameter is true
@param[in] on_error_silent if true then don't print any message to the log
			iff it is an unknown non-fatal error
@return true if we should retry the operation */
static
bool
os_file_handle_error_cond_exit(
	const char*	name,
	const char*	operation,
	bool		should_exit,
	bool		on_error_silent)
{
	ulint	err;

	err = os_file_get_last_error_low(false, on_error_silent);

	switch (err) {
	case OS_FILE_DISK_FULL:
		/* We only print a warning about disk full once */

		if (os_has_said_disk_full) {

			return(false);
		}

		/* Disk full error is reported irrespective of the
		on_error_silent setting. */

		if (name) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Encountered a problem with file %s", name);
		}

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Disk is full. Try to clean the disk to free space.");

		os_has_said_disk_full = true;

		return(false);

	case OS_FILE_AIO_RESOURCES_RESERVED:
	case OS_FILE_AIO_INTERRUPTED:

		return(true);

	case OS_FILE_PATH_ERROR:
	case OS_FILE_ALREADY_EXISTS:
	case OS_FILE_ACCESS_VIOLATION:

		return(false);

	case OS_FILE_SHARING_VIOLATION:

		os_thread_sleep(10000000);	/* 10 sec */
		return(true);

	case OS_FILE_OPERATION_ABORTED:
	case OS_FILE_INSUFFICIENT_RESOURCE:

		os_thread_sleep(100000);	/* 100 ms */
		return(true);

	default:

		/* If it is an operation that can crash on error then it
		is better to ignore on_error_silent and print an error message
		to the log. */

		if (should_exit || !on_error_silent) {
			ib_logf(IB_LOG_LEVEL_ERROR, "File %s: '%s' returned OS"
				" error " ULINTPF ".%s",
				name ? name : "(unknown)",
				operation, err,
				should_exit
				? " Cannot continue operation" : "");
		}

		if (should_exit) {
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Cannot continue operation.");
			fflush(stderr);
			exit(3);
		}
	}

	return(false);
}

/**
Does error handling when a file operation fails.
@param[in] name,		name of a file or NULL
@param[in] operation		operation
@return true if we should retry the operation */
static
bool
os_file_handle_error(
	const char*	name,
	const char*	operation)
{
	/* Exit in case of unknown error */
	return(os_file_handle_error_cond_exit(name, operation, true, false));
}

/**
Does error handling when a file operation fails.
@param[in] name,		name of a file or NULL
@param[in] operation		operation
@param[in] on_error_silent	if true then don't print any message to the log.
@return true if we should retry the operation */

static
bool
os_file_handle_error_no_exit(
	const char*	name,
	const char*	operation,
	bool		on_error_silent)
{
	/* Don't exit in case of unknown error */
	return(os_file_handle_error_cond_exit(
			name, operation, false, on_error_silent));
}

/**
Tries to disable OS caching on an opened file descriptor.
@param[in] fd		file descriptor to alter
@param[in] file_name	file name, used in the diagnostic message
@param[in] name		"open" or "create"; used in the diagnostic message */

void
os_file_set_nocache(
	int		fd 		__attribute__((unused)),
	const char*	file_name 	__attribute__((unused)),
	const char*	operation_name	__attribute__((unused)))
{
	/* some versions of Solaris may not have DIRECTIO_ON */
#if defined(UNIV_SOLARIS) && defined(DIRECTIO_ON)
	if (directio(fd, DIRECTIO_ON) == -1) {
		int	errno_save = errno;

		ib_logf(IB_LOG_LEVEL_ERROR,
			"Failed to set DIRECTIO_ON on file %s: %s: %s,"
			" continuing anyway.",
			file_name, operation_name, strerror(errno_save));
	}
#elif defined(O_DIRECT)
	if (fcntl(fd, F_SETFL, O_DIRECT) == -1) {
		int		errno_save = errno;
		static bool	warning_message_printed = false;
		if (errno_save == EINVAL) {
			if (!warning_message_printed) {
				warning_message_printed = true;
# ifdef UNIV_LINUX
				ib_logf(IB_LOG_LEVEL_WARN,
					"Failed to set O_DIRECT on file"
					" %s: %s: %s, continuing anyway."
					" O_DIRECT is known to result"
					" in 'Invalid argument' on Linux on"
					" tmpfs, see MySQL Bug#26662.",
					file_name, operation_name,
					strerror(errno_save));
# else /* UNIV_LINUX */
				goto short_warning;
# endif /* UNIV_LINUX */
			}
		} else {
# ifndef UNIV_LINUX
short_warning:
# endif
			ib_logf(IB_LOG_LEVEL_WARN,
				"Failed to set O_DIRECT on file %s: %s: %s,"
				" continuing anyway.",
				file_name, operation_name, strerror(errno_save));
		}
	}
#endif /* defined(UNIV_SOLARIS) && defined(DIRECTIO_ON) */
}

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
{
	ulint		buf_size;
	os_offset_t	current_size = 0;

	/* Write up to 1 megabyte at a time. */
	buf_size = ut_min(64, (ulint) (size / UNIV_PAGE_SIZE)) * UNIV_PAGE_SIZE;

	/* Align the buffer for possible raw i/o */
	byte*	buf2 = static_cast<byte*>(ut_malloc(buf_size + UNIV_PAGE_SIZE));
	byte*	buf = static_cast<byte*>(ut_align(buf2, UNIV_PAGE_SIZE));

	/* Write buffer full of zeros */
	memset(buf, 0, buf_size);

	if (size >= (os_offset_t) 100 << 20) {

		ib_logf(IB_LOG_LEVEL_INFO, "Progress in MB:");
	}

	while (current_size < size) {
		ulint	n_bytes;

		if (size - current_size < (os_offset_t) buf_size) {
			n_bytes = (ulint) (size - current_size);
		} else {
			n_bytes = buf_size;
		}

		dberr_t		err;
		IORequest	request(IORequest::WRITE);

		err = os_file_write(
			request, name, file, buf, current_size, n_bytes);

		if (err != DB_SUCCESS) {
			ut_free(buf2);
			return(false);
		}

		/* Print about progress for each 100 MB written */
		if ((current_size + n_bytes) / (100 << 20)
		    != current_size / (100 << 20)) {

			fprintf(stderr, " %lu00",
				(ulong) ((current_size + n_bytes)
					 / (100 << 20)));
		}

		current_size += n_bytes;
	}
	
	if (size >= (os_offset_t) 100 << 20) {

		fprintf(stderr, "\n");
	}

	ut_free(buf2);

	return(os_file_flush(file));
}

/** Truncates a file to a specified size in bytes.
Do nothing if the size to preserve is greater or equal to the current
size of the file.
@param[in]	pathname	file path
@param[in]	file		file to be truncated
@param[in]	size		size to preserve in bytes
@return true if success */

bool
os_file_truncate(
	const char*     pathname,
	os_file_t       file,
	os_offset_t	size)
{
	/* Do nothing if the size preserved is larger than or equal to the
	current size of file */
	os_offset_t	size_bytes = os_file_get_size(file);

	if (size >= size_bytes) {
		return(true);
	}

#ifdef _WIN32
	return(os_file_truncate_win32(pathname, file, size));
#else /* _WIN32 */
	return(os_file_truncate_posix(pathname, file, size));
#endif /* _WIN32 */
}

/**
NOTE! Use the corresponding macro os_file_read(), not directly this function!
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, false if fail
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer where to read
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@param[in] exit_on_err	if true then exit on error
@return DB_SUCCESS or error code */

dberr_t
os_file_read_func(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n)
{
	ut_ad(type.is_read());

#ifdef _WIN32
	return(os_file_read_win32(type, file, buf, offset, n, true));
#else /* _WIN32 */
	return(os_file_read_posix(type, file, buf, offset, n, true));
#endif /* _WIN32 */
}

/**
NOTE! Use the corresponding macro os_file_read_no_error_handling(),
not directly this function!
Requests a synchronous positioned read operation.
@return DB_SUCCESS if request was successful, false if fail
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer where to read
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@param[in] exit_on_err	if true then exit on error
@return DB_SUCCESS or error code */

dberr_t
os_file_read_no_error_handling_func(
	IORequest&	type,
	os_file_t	file,
	void*		buf,
	os_offset_t	offset,
	ulint		n)
{
	ut_ad(type.is_read());

#ifdef _WIN32
	return(os_file_read_win32(type, file, buf, offset, n, false));
#else /* _WIN32 */
	return(os_file_read_posix(type, file, buf, offset, n, false));
#endif /* _WIN32 */
}

/**
NOTE! Use the corresponding macro os_file_write(), not directly
Requests a synchronous write operation.
@param[in] type,	IO flags
@param[in] file		handle to an open file
@param[out] buf		buffer from which to write
@param[in] offset	file offset from the start where to read
@param[in] n		number of bytes to read, starting from offset
@return DB_SUCCESS if request was successful, false if fail */

dberr_t
os_file_write_func(
	IORequest&	type,
	const char*	name,
	os_file_t	file,
	const void*	buf,
	os_offset_t	offset,
	ulint		n)
{
	ut_ad(!srv_read_only_mode);
	ut_ad(type.validate());
	ut_ad(type.is_write());

	const byte*	ptr = reinterpret_cast<const byte*>(buf);

#ifdef _WIN32
	return(os_file_write_win32(type, name, file, ptr, offset, n));
#else
	return(os_file_write_posix(type, name, file, ptr, offset, n));
#endif /* _WIN32 */
}

/**
Check the existence and type of the given file.
@param[in] path		path name of file
@param[out] exists	true if the file exists
@param[out] type	Type of the file, if it exists
@return true if call succeeded */

bool
os_file_status(
	const char*	path,
	bool*		exists,
	os_file_type_t* type)
{
#ifdef _WIN32
	return(os_file_status_win32(path, exists, type));
#else
	return(os_file_status_posix(path, exists, type));
#endif /* _WIN32 */
}

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
	bool		check_rw_perm)
{
	dberr_t	ret;

#ifdef _WIN32
	struct _stat64	info;
	ret = os_file_get_status_win32(path, stat_info, &info, check_rw_perm);
#else
	struct stat	info;
	ret = os_file_get_status_posix(path, stat_info, &info, check_rw_perm);
#endif /* _WIN32 */

	if (ret == DB_SUCCESS) {
		stat_info->ctime = info.st_ctime;
		stat_info->atime = info.st_atime;
		stat_info->mtime = info.st_mtime;
		stat_info->size  = info.st_size;
	}

	return(ret);
}

#ifndef UNIV_HOTBACKUP

AIO::AIO(ulint n, ulint segments)
	:
	slots(),
	n_slots(n),
	n_segments(segments),
	n_reserved()
# ifdef LINUX_NATIVE_AIO
	,aio_ctx(),
	events()
# elif defined(_WIN32)
	,handles()
# endif /* LINUX_NATIVE_AIO */
{
	ut_a(n > 0);
	ut_a(n_segments > 0);

	mutex_create("os_aio_mutex", &mutex);

	not_full = os_event_create("aio_not_full");
	is_empty = os_event_create("aio_is_empty");

	os_event_set(is_empty);
}

/**
Initialise the slots */
dberr_t
AIO::init_slots()
{
	slots = static_cast<Slot*>(ut_zalloc(n_slots * sizeof(*slots)));

	if (slots == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	Slot*	slot = slots;

	for (ulint i = 0; i < n_slots; ++i, ++slot) {
		slot->pos = i;

		slot->is_reserved = false;

#ifdef WIN_ASYNC_IO
		OVERLAPPED*     over;

		slot->handle = CreateEvent(NULL, TRUE, FALSE, NULL);

		over = &slot->control;

		over->hEvent = slot->handle;

		array->handles[i] = over->hEvent;

#elif defined(LINUX_NATIVE_AIO)

		slot->ret = 0;

		slot->n_bytes = 0;

		memset(&slot->control, 0x0, sizeof(slot->control));

#endif /* WIN_ASYNC_IO */

		slot->compressed_ptr = reinterpret_cast<byte*>(
			ut_zalloc(UNIV_PAGE_SIZE_MAX * 2));

		if (slot->compressed_ptr == NULL) {
			return(DB_OUT_OF_MEMORY);
		}

		slot->compressed_page = static_cast<byte *>(
			ut_align(slot->compressed_ptr, UNIV_PAGE_SIZE));
	}

	return(DB_SUCCESS);
}

#ifdef LINUX_NATIVE_AIO
dberr_t
AIO::init_linux_native_aio()
{
	/* If we are not using native AIO interface then skip this
	part of initialization. */

	/* Initialize the io_context array. One io_context
	per segment in the array. */

	ut_a(aio_ctx == NULL);

	aio_ctx = static_cast<io_context**>(
		ut_zalloc(n_segments * sizeof(*aio_ctx)));

	if (aio_ctx == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	io_context**	ctx = aio_ctx;
	ulint		max_events = get_n_slots();

	for (ulint i = 0; i < n_segments; ++i, ++ctx) {

		if (!linux_create_io_ctx(max_events, ctx)) {
			/* If something bad happened during aio setup
			we should call it a day and return right away.
			We don't care about any leaks because a failure
			to initialize the io subsystem means that the
			server (or atleast the innodb storage engine)
			is not going to startup. */
			return(DB_IO_ERROR);
		}
	}

	struct io_event*        io_event;

	/* Initialize the event array. One event per slot. */
	io_event = static_cast<struct io_event*>(
		ut_zalloc(n_slots * sizeof(*io_event)));

	if (io_event == NULL) {
		return(DB_OUT_OF_MEMORY);
	}

	ut_a(events == NULL);
	events = io_event;

	return(DB_SUCCESS);
}
#endif /* LINUX_NATIVE_AIO */

/**
Initialise the array */
dberr_t
AIO::init()
{
	ut_a(slots == NULL);

#ifdef _WIN32
	ut_a(handles == NULL);
	handles = static_cast<HANDLE*>(ut_zalloc(n_slots * sizeof(*handles)));
#endif /* _WIN32 */

	if (srv_use_native_aio) {
#ifdef LINUX_NATIVE_AIO
		dberr_t	err = init_linux_native_aio();

		if (err != DB_SUCCESS) {
			return(err);
		}

#endif /* LINUX_NATIVE_AIO */
	}

	return(init_slots());
}

/**
Creates an aio wait array. Note that we return NULL in case of failure.
We don't care about freeing memory here because we assume that a
failure will result in server refusing to start up.
@param[in] n		maximum number of pending aio operations allowed;
			n must be divisible by n_segments
@param[in] n_segments	number of segments in the aio array
@return own: aio array, NULL on failure */

AIO*
AIO::create(ulint n, ulint n_segments)
{
	AIO*	array = new(std::nothrow) AIO(n, n_segments);
	ut_a(array != NULL);

	if (array->init() != DB_SUCCESS) {
		delete array;
		array = NULL;
	}

	return(array);
}

/**
AIO destructor */
AIO::~AIO()
{
	Slot*	slot = slots;

#ifdef WIN_ASYNC_IO
	for (ulint i = 0; i < n_slots; ++i, ++slot) {
		CloseHandle(slot->handle);
	}
#endif /* WIN_ASYNC_IO */

#ifdef _WIN32
	ut_free(handles);
#endif /* _WIN32 */

	mutex_destroy(&mutex);

	os_event_destroy(not_full);
	os_event_destroy(is_empty);

#if defined(LINUX_NATIVE_AIO)
	if (srv_use_native_aio) {
		ut_free(events);
		ut_free(aio_ctx);
	}
#endif /* LINUX_NATIVE_AIO */

	slot = slots;

	for (ulint i = 0; i < n_slots; ++i, ++slot) {

		if (slot->compressed_ptr != NULL) {
			ut_free(slot->compressed_ptr);
			slot->compressed_ptr = NULL;
			slot->compressed_page = NULL;
		}
	}

	ut_free(slots);
	slots = NULL;
}

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
AIO::start(
	ulint		n_per_seg,
	ulint		n_read_segs,
	ulint		n_write_segs,
	ulint		n_slots_sync)
{
#if defined(LINUX_NATIVE_AIO)
	/* Check if native aio is supported on this system and tmpfs */
	if (srv_use_native_aio && !is_linux_native_aio_supported()) {

		ib_logf(IB_LOG_LEVEL_WARN, "Linux Native AIO disabled.");

		srv_use_native_aio = FALSE;
	}
#endif /* LINUX_NATIVE_AIO */

	srv_reset_io_thread_op_info();

	s_reads = create(n_read_segs * n_per_seg, n_read_segs);

	if (s_reads == NULL) {
		return(false);
	}

	ulint	start = (srv_read_only_mode) ? 0 : 2;
	ulint	n_segs = n_read_segs + start;

	/* 0 is the ibuf segment and 1 is the insert buffer segment. */
	for (ulint i = start; i < n_segs; ++i) {
		ut_a(i < SRV_MAX_N_IO_THREADS);
		srv_io_thread_function[i] = "read thread";
	}

	ulint	n_segments = n_read_segs;

	if (!srv_read_only_mode) {

		s_log = create(n_per_seg, 1);

		if (s_log == NULL) {
			return(false);
		}

		++n_segments;

		srv_io_thread_function[1] = "log thread";

		s_ibuf = create(n_per_seg, 1);

		if (s_ibuf == NULL) {
			return(false);
		}

		++n_segments;

		srv_io_thread_function[0] = "insert buffer thread";

		s_writes = create(n_write_segs * n_per_seg, n_write_segs);

		if (s_writes == NULL) {
			return(false);
		}

		n_segments += n_write_segs;

		for (ulint i = start + n_read_segs; i < n_segments; ++i) {
			ut_a(i < SRV_MAX_N_IO_THREADS);
			srv_io_thread_function[i] = "write thread";
		}

		ut_ad(n_segments >= 4);
	} else {
		ut_ad(n_segments > 0);
	}

	s_sync = create(n_slots_sync, 1);

	if (s_sync == NULL) {
		return(false);
	}

	os_aio_n_segments = n_segments;

	os_aio_validate();

	os_aio_segment_wait_events = static_cast<os_event_t*>(
		ut_zalloc(n_segments * sizeof *os_aio_segment_wait_events));

	if (os_aio_segment_wait_events == NULL) {
		return(false);
	}

	for (ulint i = 0; i < n_segments; ++i) {
		os_aio_segment_wait_events[i] = os_event_create(0);
	}

	os_last_printout = ut_time();

	return(true);
}

/**
Free the AIO arrays */
void
AIO::shutdown()
{
	if (s_ibuf != NULL) {
		delete s_ibuf;
		s_ibuf = NULL;
	}

	if (s_log != NULL) {
		delete s_log;
		s_log = NULL;
	}

	if (s_writes != NULL) {
		delete s_writes;
		s_writes = NULL;
	}

	if (s_sync != NULL) {
		delete s_sync;
		s_sync = NULL;
	}

	delete s_reads;
	s_reads = NULL;
}

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
	ulint		n_slots_sync)
{
	os_io_init_simple();

	return(AIO::start(n_per_seg, n_read_segs, n_write_segs, n_slots_sync));
}

/**
Frees the asynchronous io system. */

void
os_aio_free()
{
	AIO::shutdown();

#ifndef UNIV_HOTBACKUP
	for (ulint i = 0; i < OS_FILE_N_SEEK_MUTEXES; i++) {
		mutex_free(os_file_seek_mutexes[i]);
		delete os_file_seek_mutexes[i];
	}
#endif /* !UNIV_HOTBACKUP */

	for (ulint i = 0; i < os_aio_n_segments; i++) {
		os_event_destroy(os_aio_segment_wait_events[i]);
	}

#if !defined(HAVE_ATOMIC_BUILTINS)
	mutex_free(&os_file_count_mutex);
#endif /* !HAVE_ATOMIC_BUILTINS */

	ut_free(os_aio_segment_wait_events);
	os_aio_segment_wait_events = 0;
	os_aio_n_segments = 0;
}

/**
Wakes up all async i/o threads so that they know to exit themselves in
shutdown. */

void
os_aio_wake_all_threads_at_shutdown()
{
#ifdef WIN_ASYNC_IO

	AIO::wake_at_shutdown();

#elif defined(LINUX_NATIVE_AIO)

	/* When using native AIO interface the io helper threads
	wait on io_getevents with a timeout value of 500ms. At
	each wake up these threads check the server status.
	No need to do anything to wake them up. */

	if (srv_use_native_aio) {
		return;
	}

#endif /* !WIN_ASYNC_AIO */

	/* Fall through to simulated AIO handler wakeup if we are
	not using native AIO. */

	/* This loop wakes up all simulated ai/o threads */

	for (ulint i = 0; i < os_aio_n_segments; ++i) {

		os_event_set(os_aio_segment_wait_events[i]);
	}
}

/**
Waits until there are no pending writes in AIO::s_writes. There can
be other, synchronous, pending writes. */

void
os_aio_wait_until_no_pending_writes()
{
	ut_ad(!srv_read_only_mode);
	os_event_wait(AIO::s_writes->is_empty);
}

/**
Calculates segment number for a slot.
@param[in] array	aio wait array
@param[int] slot	slot in this array
@return segment number (which is the number used by, for example,
	i/o-handler threads) */
ulint
AIO::get_segment_no_from_slot(
	const AIO*	array,
	const Slot*	slot)
{
	ulint	segment;
	ulint	seg_len;

	if (array == s_ibuf) {
		ut_ad(!srv_read_only_mode);

		segment = IO_IBUF_SEGMENT;

	} else if (array == s_log) {
		ut_ad(!srv_read_only_mode);

		segment = IO_LOG_SEGMENT;

	} else if (array == s_reads) {
		seg_len = s_reads->get_n_slots();

		segment = (srv_read_only_mode ? 0 : 2) + slot->pos / seg_len;
	} else {
		ut_ad(!srv_read_only_mode);
		ut_a(array == s_writes);

		seg_len = s_writes->get_n_slots();

		segment = s_reads->n_segments + 2 + slot->pos / seg_len;
	}

	return(segment);
}

/**
Requests for a slot in the aio array. If no slot is available, waits until
not_full-event becomes signaled.

@param[in,out] type	IO context
@param[in,out] m1	message to be passed along with the aio operation
@param[in,out] m2	message to be passed along with the aio operation
@param[in] file 	file handle
@param[in] name		name of the file or path as a null-terminated string
@param[in,out] buf	buffer where to read or from which to write
@param[in] offset	file offset, where to read from or start writing
@param[in] len		length of the block to read or write
@return pointer to slot */

Slot*
AIO::reserve_slot(
	IORequest&	type,
	fil_node_t*	m1,
	void*		m2,
	os_file_t	file,
	const char*	name,
	void*		buf,
	os_offset_t	offset,
	ulint		len)
{
#ifdef WIN_ASYNC_IO
	ut_a((len & 0xFFFFFFFFUL) == len);
#endif /* WIN_ASYNC_IO */

	/* No need of a mutex. Only reading constant fields */
	ulint		slots_per_seg;

	ut_ad(type.validate());

	slots_per_seg = get_n_slots();

	/* We attempt to keep adjacent blocks in the same local
	segment. This can help in merging IO requests when we are
	doing simulated AIO */
	ulint		local_seg;

	local_seg = (offset >> (UNIV_PAGE_SIZE_SHIFT + 6)) % n_segments;

	for (;;) {

		mutex_enter(&mutex);

		if (n_reserved != n_slots) {
			break;
		}

		mutex_exit(&mutex);

		if (!srv_use_native_aio) {
			/* If the handler threads are suspended,
			wake them so that we get more slots */

			os_aio_simulated_wake_handler_threads();
		}

		os_event_wait(not_full);
	}

	ulint		counter = 0;
	Slot*	slot = NULL;

	/* We start our search for an available slot from our preferred
	local segment and do a full scan of the array. We are
	guaranteed to find a slot in full scan. */
	for (ulint i = local_seg * slots_per_seg;
	     counter < n_slots;
	     ++i, ++counter) {

		i %= n_slots;

		slot = at(i);

		if (slot->is_reserved == false) {
			break;
		}
	}

	/* We MUST always be able to get hold of a reserved slot. */
	ut_a(counter < n_slots);

	ut_a(slot->is_reserved == false);

	++n_reserved;

	if (n_reserved == 1) {
		os_event_reset(is_empty);
	}

	if (n_reserved == n_slots) {
		os_event_reset(not_full);
	}

	slot->is_reserved = true;
	slot->reservation_time = ut_time();
	slot->m1 = m1;
	slot->m2 = m2;
	slot->file     = file;
	slot->name     = name;
	slot->len      = len;
	slot->type     = type;
	slot->buf      = static_cast<byte*>(buf);
	slot->ptr      = slot->buf;
	slot->offset   = offset;
	slot->err      = DB_SUCCESS;
	slot->compress_ok = false;
	slot->original_len = len;
	slot->io_already_done = false;

	if (srv_use_native_aio
	    && srv_compression_algorithm > 0
	    && offset > 0
	    && type.is_compressed()
	    && type.is_write()) {

		ut_ad(!type.is_log());

		mutex_exit(&mutex);

		byte*	ptr;
		ulint	compressed_len = len;

		ulint	old_compressed_len;

		old_compressed_len = mach_read_from_2(
			reinterpret_cast<byte*>(buf)
		        + FIL_PAGE_COMPRESS_SIZE_V1);

		old_compressed_len = ut_calc_align(
			old_compressed_len + FIL_PAGE_DATA, type.block_size());

		ptr = os_file_page_compress(
			slot->len,
			slot->type.block_size(),
			reinterpret_cast<byte*>(buf),
			slot->len,
			slot->compressed_page,
			&compressed_len, slot->mem);

		if (ptr != buf) {
			len = compressed_len;
			slot->len = compressed_len;
			slot->buf = slot->compressed_page;
			slot->ptr = slot->buf;
			slot->compress_ok = true;

			if (slot->type.punch_hole()
			    && compressed_len == old_compressed_len) {

				// FIXME:
				//slot->type.clear_punch_hole();
			}
		} else {
			slot->compress_ok = false;
		}

		mutex_enter(&mutex);
	}

#ifdef WIN_ASYNC_IO
	{
		OVERLAPPED*	control;

		control = &slot->control;
		control->Offset = (DWORD) offset & 0xFFFFFFFF;
		control->OffsetHigh = (DWORD) (offset >> 32);

		ResetEvent(slot->handle);
	}
#elif defined(LINUX_NATIVE_AIO)

	/* If we are not using native AIO skip this part. */
	if (srv_use_native_aio) {

		off_t		aio_offset;

		/* Check if we are dealing with 64 bit arch.
		If not then make sure that offset fits in 32 bits. */
		aio_offset = (off_t) offset;

		ut_a(sizeof(aio_offset) >= sizeof(offset)
		     || ((os_offset_t) aio_offset) == offset);

		struct iocb*	iocb = &slot->control;

		if (type.is_read()) {

			if (type.is_compressed() && srv_read_block_size) {

				ulint	n_bytes;

				/* The first page is never compressed */
				ut_ad(slot->offset > 0);

				n_bytes = ut_min(
					type.block_size() * 2, slot->len);

				slot->len = n_bytes;
			}

			io_prep_pread(
				iocb, file, slot->ptr, slot->len, aio_offset);
		} else {
			ut_ad(type.is_write());

			io_prep_pwrite(
				iocb, file, slot->ptr, slot->len, aio_offset);
		}

		iocb->data = slot;

		slot->n_bytes = 0;
		slot->ret = 0;
	}
#endif /* LINUX_NATIVE_AIO */

	mutex_exit(&mutex);

	return(slot);
}

/**
Wakes up a simulated aio i/o-handler thread if it has something to do.
@param[in] global_segment	the number of the segment in the AIO arrays */
void
AIO::wake_simulated_handler_thread(ulint global_segment)
{
	ut_ad(!srv_use_native_aio);

	AIO*	array;
	ulint	segment = get_array_and_local_segment(&array, global_segment);

	array->wake_simulated_handler_thread(global_segment, segment);
}

/**
Wakes up a simulated AIO I/O-handler thread if it has something to do
for a local segment in the AIO array.
@param[in] global_segment	the number of the segment in the AIO arrays 
@param[in] segment		the local segment in the AIO array */
void
AIO::wake_simulated_handler_thread(ulint global_segment, ulint segment)
{
	ut_ad(!srv_use_native_aio);

	ulint	n = get_n_slots();
	ulint	offset = segment * n;

	/* Look through n slots after the segment * n'th slot */

	mutex_enter(&mutex);

	const Slot*	slot = at(offset);

	for (ulint i = 0; i < n; ++i, ++slot) {

		if (slot->is_reserved) {

			/* Found an i/o request */

			mutex_exit(&mutex);

			os_event_t	event;

			event = os_aio_segment_wait_events[global_segment];

			os_event_set(event);

			return;
		}
	}

	mutex_exit(&mutex);
}

/**
Wakes up simulated aio i/o-handler threads if they have something to do. */

void
os_aio_simulated_wake_handler_threads()
{
	if (srv_use_native_aio) {
		/* We do not use simulated aio: do nothing */

		return;
	}

	os_aio_recommend_sleep_for_read_threads	= false;

	for (ulint i = 0; i < os_aio_n_segments; i++) {
		AIO::wake_simulated_handler_thread(i);
	}
}

/**
Select the IO slot array
@param[in] type		Type of IO, READ or WRITE
@param[in] read_only	true if running in read-only mode
@param[in] mode		IO mode
@return slot array or NULL if invalid mode specified */

AIO*
AIO::select_slot_array(IORequest& type, bool read_only, ulint mode)
{
	AIO*	array;

	ut_ad(type.validate());
	ut_ad(type.is_read() || type.is_write() == !read_only);


	switch (mode) {
	case OS_AIO_NORMAL:

		array = type.is_read() ? AIO::s_reads : AIO::s_writes;
		break;

	case OS_AIO_IBUF:
		ut_ad(type.is_read());

		/* Reduce probability of deadlock bugs in connection with ibuf:
		do not let the ibuf i/o handler sleep */

		type.clear_do_not_wake();

		array = read_only ? AIO::s_reads : AIO::s_ibuf;
		break;

	case OS_AIO_LOG:

		array = read_only ? AIO::s_reads : AIO::s_log;
		break;

	case OS_AIO_SYNC:

		array = AIO::s_sync;
#if defined(LINUX_NATIVE_AIO)
		/* In Linux native AIO we don't use sync IO array. */
		ut_a(!srv_use_native_aio);
#endif /* LINUX_NATIVE_AIO */
		break;

	default:
		ut_error;
		array = NULL; /* Eliminate compiler warning */
	}

	return(array);
}

/**
NOTE! Use the corresponding macro os_aio(), not directly this function!
Requests an asynchronous i/o operation.
@return DB_SUCCESS if request was queued successfully */

dberr_t
os_aio_func(
	IORequest&	type,
	ulint		mode,	/*!< in: OS_AIO_NORMAL, ... */
	const char*	name,	/*!< in: name of the file or path as a
				null-terminated string */
	os_file_t	file,	/*!< in: handle to a file */
	void*		buf,	/*!< in: buffer where to read or from which
				to write */
	os_offset_t	offset,	/*!< in: file offset where to read or write */
	ulint		n,	/*!< in: number of bytes to read or write */
	fil_node_t*	m1,/*!< in: message for the aio handler
				(can be used to identify a completed
				aio operation); ignored if mode is
				OS_AIO_SYNC */
	void*		m2)/*!< in: message for the aio handler
				(can be used to identify a completed
				aio operation); ignored if mode is
				OS_AIO_SYNC */
{
#ifdef WIN_ASYNC_IO
	BOOL		ret = TRUE;
	DWORD		len = (DWORD) n;
#endif /* WIN_ASYNC_IO */

	ut_ad(n > 0);
	ut_ad(n % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(os_aio_validate_skip());

#ifdef WIN_ASYNC_IO
	ut_ad((n & 0xFFFFFFFFUL) == n);
#endif /* WIN_ASYNC_IO */

	if (mode == OS_AIO_SYNC
#ifdef WIN_ASYNC_IO
	    && !srv_use_native_aio
#endif /* WIN_ASYNC_IO */
	    ) {
		/* This is actually an ordinary synchronous read or write:
		no need to use an i/o-handler thread. NOTE that if we use
		Windows async i/o, Windows does not allow us to use
		ordinary synchronous os_file_read etc. on the same file,
		therefore we have built a special mechanism for synchronous
		wait in the Windows case.
		Also note that the Performance Schema instrumentation has
		been performed by current os_aio_func()'s wrapper function
		pfs_os_aio_func(). So we would no longer need to call
		Performance Schema instrumented os_file_read() and
		os_file_write(). Instead, we should use os_file_read_func()
		and os_file_write_func() */

		if (type.is_read()) {
			return(os_file_read_func(type, file, buf, offset, n));
		}

		ut_ad(!srv_read_only_mode);
		ut_ad(type.is_write());

		return(os_file_write_func(type, name, file, buf, offset, n));
	}

try_again:

	AIO*	array;

	array = AIO::select_slot_array(type, srv_read_only_mode, mode);

	Slot*	slot;

	slot = array->reserve_slot(type, m1, m2, file, name, buf, offset, n);

	if (type.is_read()) {
		if (srv_use_native_aio) {

			++os_n_file_reads;

			os_bytes_read_since_printout += n;
#ifdef WIN_ASYNC_IO
			ret = ReadFile(
				file, buf, (DWORD) n, &len, &slot->control);

#elif defined(LINUX_NATIVE_AIO)
			if (!array->linux_dispatch(slot)) {
				goto err_exit;
			}
#endif /* WIN_ASYNC_IO */
		} else if (type.is_wake()) {
			AIO::wake_simulated_handler_thread(
				AIO::get_segment_no_from_slot(array, slot));
		}
	} else if (type.is_write()) {

		ut_ad(!srv_read_only_mode);

		if (srv_use_native_aio) {
			++os_n_file_writes;

#ifdef WIN_ASYNC_IO
			ret = WriteFile(
				file, buf, (DWORD) n, &len, &slot->control);

#elif defined(LINUX_NATIVE_AIO)
			if (!array->linux_dispatch(slot)) {
				goto err_exit;
			}
#endif /* WIN_ASYNC_IO */

		} else if (type.is_wake()) {
			AIO::wake_simulated_handler_thread(
				AIO::get_segment_no_from_slot(array, slot));
		}
	} else {
		ut_error;
	}

#ifdef WIN_ASYNC_IO
	if (srv_use_native_aio) {
		if ((ret && len == n)
		    || (!ret && GetLastError() == ERROR_IO_PENDING)) {
			/* aio was queued successfully! */

			if (mode == OS_AIO_SYNC && !type.is_log()) {
				bool		retval;
				IORequest	dummy_type;
				void*		dummy_mess2;
				struct fil_node_t* dummy_mess1;

				/* We want a synchronous i/o operation on a
				file where we also use async i/o: in Windows
				we must use the same wait mechanism as for
				async i/o */

				retval = os_aio_windows_handle(
					ULINT_UNDEFINED, slot->pos,
					&dummy_mess1, &dummy_mess2,
					&dummy_type);

				return(retval ? DB_SUCCESS : DB_IO_ERROR);
			}

			return(DB_SUCCESS);
		}

		goto err_exit;
	}
#endif /* WIN_ASYNC_IO */

	/* AIO request was queued successfully! */
	return(DB_SUCCESS);

#if defined LINUX_NATIVE_AIO || defined WIN_ASYNC_IO
err_exit:
#endif /* LINUX_NATIVE_AIO || WIN_ASYNC_IO */

	array->release(slot);

	if (os_file_handle_error(
		name, type.is_read() ? "aio read" : "aio write")) {

		goto try_again;
	}

	return(DB_IO_ERROR);
}

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
	ulint*		type)
{
	ulint		orig_seg	= segment;
	AIO*	array;
	Slot*	slot;
	ulint		n;
	ulint		i;
	bool		ret_val;
	BOOL		ret;
	DWORD		len;
	BOOL		retry		= FALSE;

	if (segment == ULINT_UNDEFINED) {
		segment = 0;
		array = AIO::s_sync;
	} else {
		segment = AIO::get_array_and_local_segment(&array, segment);
	}

	/* NOTE! We only access constant fields in os_aio_array. Therefore
	we do not have to acquire the protecting mutex yet */

	ut_ad(os_aio_validate_skip());
	ut_ad(segment < array->n_segments);

	n = array->get_n_slots();

	if (array == AIO::s_sync) {

		WaitForSingleObject(array->at(pos)->handle, INFINITE);

		i = pos;

	} else {
		if (orig_seg != ULINT_UNDEFINED) {
			srv_set_io_thread_op_info(orig_seg, "wait Windows aio");
		}

		i = WaitForMultipleObjects(
			(DWORD) n, array->handles + segment * n,
			FALSE, INFINITE);
	}

	mutex_enter(&array->mutex);

	if (srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS
	    && array->n_reserved == 0) {
		*m1 = NULL;
		*m2 = NULL;
		mutex_exit(&array->mutex);
		return(true);
	}

	ut_a(i >= WAIT_OBJECT_0 && i <= WAIT_OBJECT_0 + n);

	slot = array->at(i + segment * n);

	ut_a(slot->is_reserved);

	if (orig_seg != ULINT_UNDEFINED) {
		srv_set_io_thread_op_info(
			orig_seg, "get windows aio return value");
	}

	ret = GetOverlappedResult(slot->file, &(slot->control), &len, TRUE);

	*m1 = slot->m1;
	*m2 = slot->m2;

	*type = slot->type;

	if (ret && len == slot->len) {

		ret_val = true;
	} else if (os_file_handle_error(slot->name, "Windows aio")) {

		retry = true;
	} else {

		ret_val = false;
	}

	mutex_exit(&array->mutex);

	if (retry) {
		/* retry failed read/write operation synchronously.
		No need to hold array->mutex. */

#ifdef UNIV_PFS_IO
		/* This read/write does not go through os_file_read
		and os_file_write APIs, need to register with
		performance schema explicitly here. */
		struct PSI_file_locker* locker = NULL;
		register_pfs_file_io_begin(
			locker, slot->file, slot->len,
			slot->type.is_write()
			? PSI_FILE_WRITE : PSI_FILE_READ, __FILE__, __LINE__);
#endif /* UNIV_PFS_IO */

		ut_a((slot->len & 0xFFFFFFFFUL) == slot->len);

		if (slot->type.is_write()) {
			ret = WriteFile(
				slot->file, slot->ptr, (DWORD) slot->len,
				&len, &slot->control);

		} else {
			ut_ad(slot->type.is_read());

			ret = ReadFile(
				slot->file, slot->ptr, (DWORD) slot->len,
				&len, &slot->control);

			break;
		default:
			ut_error;
		}

#ifdef UNIV_PFS_IO
		register_pfs_file_io_end(locker, len);
#endif /* UNIV_PFS_IO */

		if (!ret && GetLastError() == ERROR_IO_PENDING) {
			/* aio was queued successfully!
			We want a synchronous i/o operation on a
			file where we also use async i/o: in Windows
			we must use the same wait mechanism as for
			async i/o */

			ret = GetOverlappedResult(
				slot->file, &slot->control, &len, TRUE);
		}

		ret_val = ret && len == slot->len;
	}

	array->release(slot);

	return(ret_val);
}
#endif

/** Simulated AIO handler for reaping IO requests */
class SimulatedAIOHandler {

public:

	/**
	Constructor
	@param[in,out] array	The AIO array
	@param[in] segment	Local segment in the array */
	SimulatedAIOHandler(AIO* array, ulint segment)
		:
		m_oldest(),
		m_n_elems(),
		m_lowest_offset(IB_UINT64_MAX),
		m_array(array),
		m_n_slots(),
		m_segment(segment),
		m_ptr(),
		m_buf()
	{
		m_slots.reserve(OS_AIO_MERGE_N_CONSECUTIVE);
	}


	/**
	Destructor */
	~SimulatedAIOHandler()
	{
		if (m_ptr != NULL) {
			ut_free(m_ptr);
		}
	}

	/**
	Reset the state of the handler */
	void init(ulint n_slots)
	{
		m_oldest = 0;
		m_n_elems = 0;
		m_n_slots = n_slots;
		m_lowest_offset = IB_UINT64_MAX;

		if (m_ptr != NULL) {
			ut_free(m_ptr);
			m_ptr = m_buf = NULL;
		}

		m_slots[0] = NULL;
	}

	/**
	Check if there is a slot for which the i/o has already been done
	@param[out] n_reserved	Number of reserved slots
	@return the first completed slot that is found. */
	Slot* check_completed(ulint* n_reserved)
	{
		ulint	offset = m_segment * m_n_slots;

		*n_reserved = 0;

		Slot*	slot;

		slot = m_array->at(offset);

		for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

			if (slot->is_reserved) {
		       
				if (slot->io_already_done) {

					ut_a(slot->is_reserved);

					return(slot);
				}

				++*n_reserved;
			}
		}

		return(NULL);
	}

	/** If there are at least 2 seconds old requests, then pick the
	oldest one to prevent starvation.  If several requests have the
	same age, then pick the one at the lowest offset.
	@return true if request was selected */
	bool select()
	{
		if (!select_oldest()) {

			return(select_lowest_offset());
		}

		return(true);
	}

	/** Check if there are several consecutive blocks
	to read or write. Merge them if found. */
	void merge()
	{
		/* if m_n_elems != 0, then we have assigned
		something valid to consecutive_ios[0] */
		ut_ad(m_n_elems != 0);
		ut_ad(first_slot() != NULL);

		Slot*	slot = first_slot();

		while (!merge_adjacent(slot)) {
			/* No op */
		}
	}

	/** We have now collected n_consecutive i/o requests
	in the array; allocate a single buffer which can hold
	all data, and perform the I/O
	@return the length of the buffer */
	ulint allocate_buffer()
	{
		ulint		len = 0;
		Slot*	slot = first_slot();

		ut_ad(m_ptr == NULL);

		if (slot->type.is_read() && m_n_elems > 1) {

			for (ulint i = 0; i < m_n_elems; ++i) {
				len += m_slots[i]->len;
			}

			m_ptr = static_cast<byte*>(
				ut_malloc(len + UNIV_PAGE_SIZE));

			m_buf = static_cast<byte*>(
				ut_align(m_ptr, UNIV_PAGE_SIZE));

		} else {
			len = first_slot()->len;
			m_buf = first_slot()->buf;
		}

		return(len);
	}

	/** We have to compress the individual pages and punch
	holes in them on a page by page basis when writing to
	tables that can be compresed at the IO level.
	@param[in] len		Value returned by allocate_buffer */
	void copy_to_buffer(ulint len)
	{
		Slot*	slot = first_slot();

		if (len > slot->len && slot->type.is_write()) {

			byte*	ptr = m_buf;

			ut_ad(ptr != slot->buf);

			/* Copy the buffers to the combined buffer */
			for (ulint i = 0; i < m_n_elems; ++i) {

				slot = m_slots[i];

				memmove(ptr, slot->buf, slot->len);

				ptr += slot->len;
			}
		}
	}

	/** Do the I/O with ordinary, synchronous i/o functions:
	@param[in] len		Length of buffer for IO */
	void io()
	{
		if (first_slot()->type.is_write()) {

			for (ulint i = 0; i < m_n_elems; ++i) {
				write(m_slots[i]);
			}

		} else {

			for (ulint i = 0; i < m_n_elems; ++i) {
				read(m_slots[i]);
			}
		}
	}

	/**
	Do the decompression of the pages read in */
	void io_complete()
	{
		// FIXME: For non-compressed tables. Not required
		// for correctness.
	}

	/** Mark the i/os done in slots */

	void done()
	{
		for (ulint i = 0; i < m_n_elems; ++i) {
			m_slots[i]->io_already_done = true;
		}
	}

	/**
	@return the first slot in the consecutive array */
	Slot* first_slot()
	{
		ut_a(m_n_elems > 0);

		return(m_slots[0]);
	}

	/**
	Wait for I/O requests
	@param[in] global_segment The global segment
	@param[in,out] event	Wait on event if no active requests
	@return the number of slots */
	ulint check_pending(
		ulint		global_segment,
		os_event_t	event);
private:

	/**
	Do the file read
	@param[in,out] slot	Slot that has the IO context */
	void read(Slot* slot)
	{
		dberr_t	err = os_file_read(
			slot->type,
			slot->file,
			slot->ptr,
			slot->offset,
			slot->len);

		ut_a(err == DB_SUCCESS);
	}

	/**
	Do the file read
	@param[in,out] slot	Slot that has the IO context */
	void write(Slot* slot)
	{
		ut_ad(!srv_read_only_mode);

		dberr_t	err = os_file_write(
			slot->type,
			slot->name,
			slot->file,
			slot->ptr,
			slot->offset,
			slot->len);

		ut_a(err == DB_SUCCESS || err == DB_IO_NO_PUNCH_HOLE);
	}

	/**
	@return true they the slots are adjacent and can be merged */
	bool adjacent(const Slot* s1, const Slot* s2) const
	{
		return(s1 != s2
		       && s1->file == s2->file
		       && s1->offset == s2->offset + s2->len
		       && s1->type == s2->type);
	}

	/**
	@return true if merge limit reached or no adjacent slots found. */
	bool merge_adjacent(Slot*& current)
	{
		Slot*slot;
		ulint	offset = m_segment * m_n_slots;

		slot = m_array->at(offset);

		for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

			if (slot->is_reserved && adjacent(slot, current)) {

				current = slot;

				/* Found a consecutive i/o request */

				m_slots[m_n_elems] = slot;

				++m_n_elems;

				return(m_n_elems >= m_slots.capacity());
			}
		}

		return(true);
	}

	/** There were no old requests. Look for
	an i/o request at the lowest offset in
	the array (we ignore the high 32 bits of
	the offset in these heuristics) */
	bool select_lowest_offset()
	{
		ut_ad(m_n_elems == 0);

		ulint	offset = m_segment * m_n_slots;

		m_lowest_offset = IB_UINT64_MAX;

		for (ulint i = 0; i < m_n_slots; ++i) {
			Slot*	slot;

			slot = m_array->at(i + offset);

			if (slot->is_reserved
			    && slot->offset < m_lowest_offset) {

				/* Found an i/o request */
				m_slots[0] = slot;

				m_n_elems = 1;

				m_lowest_offset = slot->offset;
			}
		}

		return(m_n_elems > 0);
	}

	/**
	Select the slot if it is older than the current oldest slot.
	@param[in] slot		The slot to check */
	void select_if_older(Slot* slot)
	{
		ulint	age;

		age = (ulint) difftime(ut_time(), slot->reservation_time);

		if ((age >= 2 && age > m_oldest)
		    || (age >= 2
			&& age == m_oldest
			&& slot->offset < m_lowest_offset)) {

			/* Found an i/o request */
			m_slots[0] = slot;

			m_n_elems = 1;

			m_oldest = age;

			m_lowest_offset = slot->offset;
		}
	}

	/**
	Select th oldest slot in the array
	@return true if oldest slot found */
	bool select_oldest()
	{
		ut_ad(m_n_elems == 0);

		Slot*	slot;
		ulint	offset = m_n_slots * m_segment;

		slot = m_array->at(offset);

		for (ulint i = 0; i < m_n_slots; ++i, ++slot) {

			if (slot->is_reserved) {
				select_if_older(slot);
			}
		}

		return(m_n_elems > 0);
	}

	typedef std::vector<Slot*> slots_t;

private:
	ulint		m_oldest;
	ulint		m_n_elems;
	os_offset_t	m_lowest_offset;

	AIO*	m_array;
	ulint		m_n_slots;
	ulint		m_segment;

	slots_t		m_slots;

	byte*		m_ptr;
	byte*		m_buf;
};

/**
Wait for I/O requests
@return the number of slots */

ulint
SimulatedAIOHandler::check_pending(
	ulint		global_segment,
	os_event_t	event)
{
	/* NOTE! We only access constant fields in os_aio_array.
	Therefore we do not have to acquire the protecting mutex yet */

	ut_ad(os_aio_validate_skip());
	ut_ad(m_segment < m_array->n_segments);

	/* Look through n slots after the segment * n'th slot */

	if (m_array == AIO::s_reads
	    && os_aio_recommend_sleep_for_read_threads) {

		/* Give other threads chance to add several
		I/Os to the array at once. */

		srv_set_io_thread_op_info(
			global_segment, "waiting for i/o request");

		os_event_wait(event);

		return(0);
	}

	return(m_array->get_n_slots());
}
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
	ulint		global_segment,
	fil_node_t**	m1,
	void**		m2,
	IORequest*	type)
{
	Slot*	slot;
	AIO*	array;
	ulint		segment;
	os_event_t	event = os_aio_segment_wait_events[global_segment];

	segment = AIO::get_array_and_local_segment(&array, global_segment);

	SimulatedAIOHandler	handler(array, segment);

	while (true) {

		srv_set_io_thread_op_info(
			global_segment, "looking for i/o requests (a)");

		ulint	n_slots = handler.check_pending(global_segment, event);

		if (n_slots == 0) {
			continue;
		}

		handler.init(n_slots);

		srv_set_io_thread_op_info(
			global_segment, "looking for i/o requests (b)");

		mutex_enter(&array->mutex);

		ulint	n_reserved;

		slot = handler.check_completed(&n_reserved);

		if (slot != NULL) {

			break;

		} else if (n_reserved == 0
			   && srv_shutdown_state == SRV_SHUTDOWN_EXIT_THREADS) {

			/* There is no completed request. If there
			are no pending request at all, and the system
			is being shut down, exit. */

			mutex_exit(&array->mutex);

			*m1 = NULL;

			*m2 = NULL;

			return(true);

		} else if (handler.select()) {

			break;
		}

		/* No I/O requested at the moment */

		srv_set_io_thread_op_info(
			global_segment, "resetting wait event");

		/* We wait here until there again can
		be i/os in the segment of this thread */

		os_event_reset(event);

		mutex_exit(&array->mutex);

		srv_set_io_thread_op_info(
			global_segment, "waiting for i/o request");

		os_event_wait(event);
	}

	/** Found a slot that has already completed its IO */

	if (slot == NULL) {
		/* Merge adjacent requests */
		handler.merge();

		/* Check if there are several consecutive blocks
		to read or write */

		srv_set_io_thread_op_info(
			global_segment, "consecutive i/o requests");

		// FIXME:
		//ulint	total_len = handler.allocate_buffer();

		/* We release the array mutex for the time of the i/o: NOTE that
		this assumes that there is just one i/o-handler thread serving
		a single segment of slots! */

		mutex_exit(&array->mutex);

		// FIXME
		//handler.copy_to_buffer(total_len);

		srv_set_io_thread_op_info(global_segment, "doing file i/o");

		handler.io();

		srv_set_io_thread_op_info(global_segment, "file i/o done");

		handler.io_complete();

		mutex_enter(&array->mutex);

		handler.done();

		/* We return the messages for the first slot now, and if there
		were several slots, the messages will be returned with subsequent
		calls of this function */
	
		slot = handler.first_slot();
	}

	ut_ad(slot->is_reserved);

	*m1 = slot->m1;
	*m2 = slot->m2;

	*type = slot->type;

	array->release_low(slot);

	mutex_exit(&array->mutex);

	return(true);
}

/**
Validates the consistency the aio system.
@return true if ok */

bool
os_aio_validate()
{
	AIO::s_reads->validate();

	if (AIO::s_writes != NULL) {
		AIO::s_writes->validate();
	}

	if (AIO::s_ibuf != NULL) {
		AIO::s_ibuf->validate();
	}

	if (AIO::s_log != NULL) {
		AIO::s_log->validate();
	}

	if (AIO::s_sync != NULL) {
		AIO::s_sync->validate();
	}

	return(true);
}

/**
Prints pending IO requests per segment of an aio array.
We probably don't need per segment statistics but they can help us
during development phase to see if the IO requests are being
distributed as expected.
@param[in,out file	file where to print
@param[in] segments	pending IO array */
void
AIO::print_segment_info(
	FILE*		file,
	const ulint*	segments)
{
	ut_ad(n_segments > 0);

	if (n_segments > 1) {

		fprintf(file, " [");

		for (ulint i = 0; i < n_segments; ++i, ++segments) {

			if (i != 0) {
				fprintf(file, ", ");
			}

			fprintf(file, "%lu", *segments);
		}

		fprintf(file, "] ");
	}
}

/**
Prints info about the aio array.
@param[in,out] file		Where to print */
void
AIO::print(FILE* file)
{
	ulint	count = 0;
	ulint	n_res_seg[SRV_MAX_N_IO_THREADS];

	mutex_enter(&mutex);

	ut_a(n_slots > 0);
	ut_a(n_segments > 0);

	memset(n_res_seg, 0x0, sizeof(n_res_seg));

	Slot*	slot = slots;

	for (ulint i = 0; i < n_slots; ++i, ++slot) {
		ulint	segment = (i * n_segments) / n_slots;

		if (slot->is_reserved) {

			++count;

			++n_res_seg[segment];

			ut_a(slot->len > 0);
		}
	}

	ut_a(n_reserved == count);

	fprintf(file, " %lu", (ulong) n_reserved);

	print_segment_info(file, n_res_seg);

	mutex_exit(&mutex);
}

/**
Print all the AIO segments
@param[in,out] file	Where to print */
void
AIO::print_all(FILE* file)
{
	s_reads->print(file);

	if (s_writes != NULL) {
		fputs(", aio writes:", file);
		s_writes->print(file);
	}

	if (s_ibuf != NULL) {
		fputs(",\n ibuf aio reads:", file);
		s_ibuf->print(file);
	}

	if (s_log != NULL) {
		fputs(", log i/o's:", file);
		s_log->print(file);
	}

	if (s_sync != NULL) {
		fputs(", sync i/o's:", file);
		s_sync->print(file);
	}
}

/**
Prints info of the aio arrays.
@param[in/out] file	file where to print */

void
os_aio_print(FILE*	file)
{
	time_t		current_time;
	double		time_elapsed;
	double		avg_bytes_read;

	for (ulint i = 0; i < srv_n_file_io_threads; ++i) {
		fprintf(file, "I/O thread %lu state: %s (%s)",
			(ulong) i,
			srv_io_thread_op_info[i],
			srv_io_thread_function[i]);

#ifndef _WIN32
		if (os_event_is_set(os_aio_segment_wait_events[i])) {
			fprintf(file, " ev set");
		}
#endif /* _WIN32 */

		fprintf(file, "\n");
	}

	fputs("Pending normal aio reads:", file);

	AIO::print_all(file);

	putc('\n', file);
	current_time = ut_time();
	time_elapsed = 0.001 + difftime(current_time, os_last_printout);

	fprintf(file,
		"Pending flushes (fsync) log: %lu; buffer pool: %lu\n"
		"%lu OS file reads, %lu OS file writes, %lu OS fsyncs\n",
		(ulong) fil_n_pending_log_flushes,
		(ulong) fil_n_pending_tablespace_flushes,
		(ulong) os_n_file_reads,
		(ulong) os_n_file_writes,
		(ulong) os_n_fsyncs);

	if (os_n_pending_writes != 0 || os_n_pending_reads != 0) {
		fprintf(file,
			"%lu pending preads, %lu pending pwrites\n",
			(ulint) os_n_pending_reads,
			(ulong) os_n_pending_writes);
	}

	if (os_n_file_reads == os_n_file_reads_old) {
		avg_bytes_read = 0.0;
	} else {
		avg_bytes_read = (double) os_bytes_read_since_printout
			/ (os_n_file_reads - os_n_file_reads_old);
	}

	fprintf(file,
		"%.2f reads/s, %lu avg bytes/read,"
		" %.2f writes/s, %.2f fsyncs/s\n",
		(os_n_file_reads - os_n_file_reads_old)
		/ time_elapsed,
		(ulong) avg_bytes_read,
		(os_n_file_writes - os_n_file_writes_old)
		/ time_elapsed,
		(os_n_fsyncs - os_n_fsyncs_old)
		/ time_elapsed);

	os_n_file_reads_old = os_n_file_reads;
	os_n_file_writes_old = os_n_file_writes;
	os_n_fsyncs_old = os_n_fsyncs;
	os_bytes_read_since_printout = 0;

	os_last_printout = current_time;
}

/**
Refreshes the statistics used to print per-second averages. */

void
os_aio_refresh_stats()
{
	os_n_fsyncs_old = os_n_fsyncs;
	os_bytes_read_since_printout = 0;
	os_n_file_reads_old = os_n_file_reads;
	os_n_file_writes_old = os_n_file_writes;

	os_last_printout = ut_time();
}

/**
Set the file create umask
@param[in] umask	The umask to use for file creation. */

void
os_file_set_umask(ulint umask)
{
	os_innodb_umask = umask;
}

#endif /* !UNIV_HOTBACKUP */
