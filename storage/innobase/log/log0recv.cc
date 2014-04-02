/*****************************************************************************

Copyright (c) 1997, 2014, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2012, Facebook Inc.

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
@file log/log0recv.cc
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include <vector>
#include "log0recv.h"

#ifdef UNIV_NONINL
#include "log0recv.ic"
#endif

#include "mem0mem.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "page0cur.h"
#include "page0zip.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "ibuf0ibuf.h"
#include "trx0undo.h"
#include "trx0rec.h"
#include "fil0fil.h"
#ifndef UNIV_HOTBACKUP
# include "buf0rea.h"
# include "srv0srv.h"
# include "srv0start.h"
# include "trx0roll.h"
# include "row0merge.h"
# include "sync0mutex.h"
#else /* !UNIV_HOTBACKUP */


/** This is set to FALSE if the backup was originally taken with the
ibbackup --include regexp option: then we do not want to create tables in
directories which were not included */
ibool	recv_replay_file_ops	= TRUE;
#endif /* !UNIV_HOTBACKUP */

/** Log records are stored in the hash table in chunks at most of this size;
this must be less than UNIV_PAGE_SIZE as it is stored in the buffer pool */
#define RECV_DATA_BLOCK_SIZE	(MEM_MAX_ALLOC_IN_BUF - sizeof(recv_data_t))

/** Read-ahead area in applying log records to file pages */
#define RECV_READ_AHEAD_AREA	32

/** The recovery system */
recv_sys_t*	recv_sys = NULL;
/** TRUE when applying redo log records during crash recovery; FALSE
otherwise.  Note that this is FALSE while a background thread is
rolling back incomplete transactions. */
ibool	recv_recovery_on;

#ifndef UNIV_HOTBACKUP
/** TRUE when recv_init_crash_recovery() has been called. */
ibool	recv_needed_recovery;
# ifdef UNIV_DEBUG
/** TRUE if writing to the redo log (mtr_commit) is forbidden.
Protected by log_sys->mutex. */
ibool	recv_no_log_write = FALSE;
# endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start_func(). */
ibool	recv_lsn_checks_on;

/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes TRUE if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

TRUE means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
ibool	recv_no_ibuf_operations;
/** TRUE when the redo log is being backed up */
# define recv_is_making_a_backup		FALSE
/** TRUE when recovering from a backed up redo log file */
# define recv_is_from_backup			FALSE
#else /* !UNIV_HOTBACKUP */
# define recv_needed_recovery			FALSE
/** TRUE when the redo log is being backed up */
ibool	recv_is_making_a_backup	= FALSE;
/** TRUE when recovering from a backed up redo log file */
ibool	recv_is_from_backup	= FALSE;
# define buf_pool_get_curr_size() (5 * 1024 * 1024)
#endif /* !UNIV_HOTBACKUP */
/** The following counter is used to decide when to print info on
log scan */
static ulint	recv_scan_print_counter;

/** The type of the previous parsed redo log record */
static mlog_id_t	recv_previous_parsed_rec_type;
/** The offset of the previous parsed redo log record */
static ulint	recv_previous_parsed_rec_offset;
/** The 'multi' flag of the previous parsed redo log record */
static ulint	recv_previous_parsed_rec_is_multi;

/** Maximum page number encountered in the redo log */
ulint	recv_max_parsed_page_no;

/** This many frames must be left free in the buffer pool when we scan
the log and store the scanned log records in the buffer pool: we will
use these free frames to read in pages when we start applying the
log records to the database.
This is the default value. If the actual size of the buffer pool is
larger than 10 MB we'll set this value to 512. */
ulint	recv_n_pool_free_frames;

/** The maximum lsn we see for a page during the recovery process. If this
is bigger than the lsn we are able to scan up to, that is an indication that
the recovery failed and the database may be corrupt. */
lsn_t	recv_max_page_lsn;

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t	trx_rollback_clean_thread_key;
#endif /* UNIV_PFS_THREAD */

#ifndef UNIV_HOTBACKUP
# ifdef UNIV_PFS_THREAD
mysql_pfs_key_t	recv_writer_thread_key;
# endif /* UNIV_PFS_THREAD */

/** Flag indicating if recv_writer thread is active. */
bool	recv_writer_thread_active = false;
#endif /* !UNIV_HOTBACKUP */

/* prototypes */

#ifndef UNIV_HOTBACKUP
/*******************************************************//**
Initialize crash recovery environment. Can be called iff
recv_needed_recovery == FALSE. */
static
void
recv_init_crash_recovery(void);
/*===========================*/
#endif /* !UNIV_HOTBACKUP */

/********************************************************//**
Creates the recovery system. */

void
recv_sys_create(void)
/*=================*/
{
	if (recv_sys != NULL) {

		return;
	}

	recv_sys = static_cast<recv_sys_t*>(ut_zalloc(sizeof(*recv_sys)));

	mutex_create("recv_sys", &recv_sys->mutex);
	mutex_create("recv_writer", &recv_sys->writer_mutex);

	recv_sys->heap = NULL;
	recv_sys->addr_hash = NULL;
}

/********************************************************//**
Release recovery system mutexes. */

void
recv_sys_close(void)
/*================*/
{
	if (recv_sys != NULL) {
		if (recv_sys->addr_hash != NULL) {
			hash_table_free(recv_sys->addr_hash);
		}

		if (recv_sys->heap != NULL) {
			mem_heap_free(recv_sys->heap);
		}

		ut_free(recv_sys->buf);
		ut_free(recv_sys->last_block_buf_start);

#ifndef UNIV_HOTBACKUP
		ut_ad(!recv_writer_thread_active);
		mutex_free(&recv_sys->writer_mutex);
#endif /* !UNIV_HOTBACKUP */

		mutex_free(&recv_sys->mutex);

		ut_free(recv_sys);
		recv_sys = NULL;
	}
}

/********************************************************//**
Frees the recovery system memory. */

void
recv_sys_mem_free(void)
/*===================*/
{
	if (recv_sys != NULL) {
		if (recv_sys->addr_hash != NULL) {
			hash_table_free(recv_sys->addr_hash);
		}

		if (recv_sys->heap != NULL) {
			mem_heap_free(recv_sys->heap);
		}

		ut_free(recv_sys->buf);
		ut_free(recv_sys->last_block_buf_start);
		ut_free(recv_sys);
		recv_sys = NULL;
	}
}

#ifndef UNIV_HOTBACKUP
/************************************************************
Reset the state of the recovery system variables. */

void
recv_sys_var_init(void)
/*===================*/
{
	recv_lsn_checks_on = FALSE;

	recv_n_pool_free_frames = 256;

	recv_recovery_on = FALSE;

	recv_needed_recovery = FALSE;

	recv_lsn_checks_on = FALSE;

	recv_no_ibuf_operations = FALSE;

	recv_scan_print_counter	= 0;

	recv_previous_parsed_rec_type = MLOG_SINGLE_REC_FLAG;

	recv_previous_parsed_rec_offset	= 0;

	recv_previous_parsed_rec_is_multi = 0;

	recv_max_parsed_page_no	= 0;

	recv_n_pool_free_frames	= 256;

	recv_max_page_lsn = 0;
}

/******************************************************************//**
recv_writer thread tasked with flushing dirty pages from the buffer
pools.
@return a dummy parameter */
extern "C"
os_thread_ret_t
DECLARE_THREAD(recv_writer_thread)(
/*===============================*/
	void*	arg __attribute__((unused)))
			/*!< in: a dummy parameter required by
			os_thread_create */
{
	ut_ad(!srv_read_only_mode);

#ifdef UNIV_PFS_THREAD
	pfs_register_thread(recv_writer_thread_key);
#endif /* UNIV_PFS_THREAD */

#ifdef UNIV_DEBUG_THREAD_CREATION
	ib_logf(IB_LOG_LEVEL_INFO, "recv_writer thread running, id %lu",
		os_thread_pf(os_thread_get_curr_id()));
#endif /* UNIV_DEBUG_THREAD_CREATION */

	recv_writer_thread_active = true;

	while (srv_shutdown_state == SRV_SHUTDOWN_NONE) {

		os_thread_sleep(100000);

		mutex_enter(&recv_sys->writer_mutex);

		if (!recv_recovery_on) {
			mutex_exit(&recv_sys->writer_mutex);
			break;
		}

		/* Flush pages from end of LRU if required */
		buf_flush_LRU_lists();

		mutex_exit(&recv_sys->writer_mutex);
	}

	recv_writer_thread_active = false;

	/* We count the number of threads in os_thread_exit().
	A created thread should always use that to exit and not
	use return() to exit. */
	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}
#endif /* !UNIV_HOTBACKUP */

/************************************************************
Inits the recovery system for a recovery operation. */

void
recv_sys_init(
/*==========*/
	ulint	available_memory)	/*!< in: available memory in bytes */
{
	if (recv_sys->heap != NULL) {

		return;
	}

#ifndef UNIV_HOTBACKUP
	/* Initialize red-black tree for fast insertions into the
	flush_list during recovery process.
	As this initialization is done while holding the buffer pool
	mutex we perform it before acquiring recv_sys->mutex. */
	buf_flush_init_flush_rbt();

	mutex_enter(&(recv_sys->mutex));

	recv_sys->heap = mem_heap_create_typed(256,
					MEM_HEAP_FOR_RECV_SYS);
#else /* !UNIV_HOTBACKUP */
	recv_sys->heap = mem_heap_create(256);
	recv_is_from_backup = TRUE;
#endif /* !UNIV_HOTBACKUP */

	/* Set appropriate value of recv_n_pool_free_frames. */
	if (buf_pool_get_curr_size() >= (10 * 1024 * 1024)) {
		/* Buffer pool of size greater than 10 MB. */
		recv_n_pool_free_frames = 512;
	}

	recv_sys->buf = static_cast<byte*>(ut_malloc(RECV_PARSING_BUF_SIZE));
	recv_sys->len = 0;
	recv_sys->recovered_offset = 0;

	recv_sys->addr_hash = hash_create(available_memory / 512);
	recv_sys->n_addrs = 0;

	recv_sys->apply_log_recs = FALSE;
	recv_sys->apply_batch_on = FALSE;

	recv_sys->last_block_buf_start = static_cast<byte*>(
		ut_malloc(2 * OS_FILE_LOG_BLOCK_SIZE));

	recv_sys->last_block = static_cast<byte*>(ut_align(
		recv_sys->last_block_buf_start, OS_FILE_LOG_BLOCK_SIZE));

	recv_sys->found_corrupt_log = FALSE;

	recv_max_page_lsn = 0;

	/* Call the constructor for recv_sys_t::dblwr member */
	new (&recv_sys->dblwr) recv_dblwr_t();

	mutex_exit(&(recv_sys->mutex));
}

/********************************************************//**
Empties the hash table when it has been fully processed. */
static
void
recv_sys_empty_hash(void)
/*=====================*/
{
	ut_ad(mutex_own(&(recv_sys->mutex)));

	if (recv_sys->n_addrs != 0) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"%lu pages with log records were left unprocessed!"
			" Maximum page number with log records on it is %lu",
			(ulong) recv_sys->n_addrs,
			(ulong) recv_max_parsed_page_no);
	}

	hash_table_free(recv_sys->addr_hash);
	mem_heap_empty(recv_sys->heap);

	recv_sys->addr_hash = hash_create(buf_pool_get_curr_size() / 512);
}

#ifndef UNIV_HOTBACKUP

/********************************************************//**
Frees the recovery system. */
static
void
recv_sys_debug_free(void)
/*=====================*/
{
	mutex_enter(&(recv_sys->mutex));

	hash_table_free(recv_sys->addr_hash);
	mem_heap_free(recv_sys->heap);
	ut_free(recv_sys->buf);
	ut_free(recv_sys->last_block_buf_start);

	recv_sys->buf = NULL;
	recv_sys->heap = NULL;
	recv_sys->addr_hash = NULL;
	recv_sys->last_block_buf_start = NULL;

	mutex_exit(&(recv_sys->mutex));

	/* Free up the flush_rbt. */
	buf_flush_free_flush_rbt();
}

/********************************************************//**
Copies a log segment from the most up-to-date log group to the other log
groups, so that they all contain the latest log data. Also writes the info
about the latest checkpoint to the groups, and inits the fields in the group
memory structs to up-to-date values. */
static
void
recv_synchronize_groups(void)
/*=========================*/
{
	lsn_t		start_lsn;
	lsn_t		end_lsn;
	lsn_t		recovered_lsn;

	recovered_lsn = recv_sys->recovered_lsn;

	/* Read the last recovered log block to the recovery system buffer:
	the block is always incomplete */

	start_lsn = ut_uint64_align_down(recovered_lsn,
					 OS_FILE_LOG_BLOCK_SIZE);
	end_lsn = ut_uint64_align_up(recovered_lsn, OS_FILE_LOG_BLOCK_SIZE);

	ut_a(start_lsn != end_lsn);

	log_group_read_log_seg(recv_sys->last_block,
			       UT_LIST_GET_FIRST(log_sys->log_groups),
			       start_lsn, end_lsn);

	for (log_group_t* group = UT_LIST_GET_FIRST(log_sys->log_groups);
	     group;
	     group = UT_LIST_GET_NEXT(log_groups, group)) {
		/* Update the fields in the group struct to correspond to
		recovered_lsn */

		log_group_set_fields(group, recovered_lsn);
	}

	/* Copy the checkpoint info to the groups; remember that we have
	incremented checkpoint_no by one, and the info will not be written
	over the max checkpoint info, thus making the preservation of max
	checkpoint info on disk certain */

	log_groups_write_checkpoint_info();

	mutex_exit(&(log_sys->mutex));

	/* Wait for the checkpoint write to complete */
	rw_lock_s_lock(&(log_sys->checkpoint_lock));
	rw_lock_s_unlock(&(log_sys->checkpoint_lock));

	mutex_enter(&(log_sys->mutex));
}
#endif /* !UNIV_HOTBACKUP */

/***********************************************************************//**
Checks the consistency of the checkpoint info
@return TRUE if ok */
static
ibool
recv_check_cp_is_consistent(
/*========================*/
	const byte*	buf)	/*!< in: buffer containing checkpoint info */
{
	ulint	fold;

	fold = ut_fold_binary(buf, LOG_CHECKPOINT_CHECKSUM_1);

	if ((fold & 0xFFFFFFFFUL) != mach_read_from_4(
		    buf + LOG_CHECKPOINT_CHECKSUM_1)) {
		return(FALSE);
	}

	fold = ut_fold_binary(buf + LOG_CHECKPOINT_LSN,
			      LOG_CHECKPOINT_CHECKSUM_2 - LOG_CHECKPOINT_LSN);

	if ((fold & 0xFFFFFFFFUL) != mach_read_from_4(
		    buf + LOG_CHECKPOINT_CHECKSUM_2)) {
		return(FALSE);
	}

	return(TRUE);
}

#ifndef UNIV_HOTBACKUP
/********************************************************//**
Looks for the maximum consistent checkpoint from the log groups.
@return error code or DB_SUCCESS */
static __attribute__((nonnull, warn_unused_result))
dberr_t
recv_find_max_checkpoint(
/*=====================*/
	log_group_t**	max_group,	/*!< out: max group */
	ulint*		max_field)	/*!< out: LOG_CHECKPOINT_1 or
					LOG_CHECKPOINT_2 */
{
	log_group_t*	group;
	ib_uint64_t	max_no;
	ib_uint64_t	checkpoint_no;
	ulint		field;
	byte*		buf;

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	max_no = 0;
	*max_group = NULL;
	*max_field = 0;

	buf = log_sys->checkpoint_buf;

	while (group) {
		group->state = LOG_GROUP_CORRUPTED;

		for (field = LOG_CHECKPOINT_1; field <= LOG_CHECKPOINT_2;
		     field += LOG_CHECKPOINT_2 - LOG_CHECKPOINT_1) {

			log_group_read_checkpoint_info(group, field);

			if (!recv_check_cp_is_consistent(buf)) {
				DBUG_PRINT("ib_log",
					   ("invalid checkpoint,"
					    " group " ULINTPF " at " ULINTPF
					    ", checksum %#x",
					    group->id, field,
					    (unsigned) mach_read_from_4(
						    LOG_CHECKPOINT_CHECKSUM_1
						    + buf)));
				continue;
			}

			group->state = LOG_GROUP_OK;

			group->lsn = mach_read_from_8(
				buf + LOG_CHECKPOINT_LSN);
			group->lsn_offset = mach_read_from_4(
				buf + LOG_CHECKPOINT_OFFSET_LOW32);
			group->lsn_offset |= ((lsn_t) mach_read_from_4(
				buf + LOG_CHECKPOINT_OFFSET_HIGH32)) << 32;
			checkpoint_no = mach_read_from_8(
				buf + LOG_CHECKPOINT_NO);

			DBUG_PRINT("ib_log",
				   ("checkpoint " UINT64PF
				    " found in group " ULINTPF,
				    checkpoint_no, group->id));

			if (checkpoint_no >= max_no) {
				*max_group = group;
				*max_field = field;
				max_no = checkpoint_no;
			}
		}

		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	if (*max_group == NULL) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"No valid checkpoint found. If this error appears when"
			" you are creating an InnoDB database, the problem may"
			" be that during an earlier attempt you managed to"
			" create the InnoDB data files, but log file creation"
			" failed. If that is the case; %s",
			ERROR_CREATING_MSG);
		return(DB_ERROR);
	}

	return(DB_SUCCESS);
}
#else /* !UNIV_HOTBACKUP */
/*******************************************************************//**
Reads the checkpoint info needed in hot backup.
@return TRUE if success */

ibool
recv_read_checkpoint_info_for_backup(
/*=================================*/
	const byte*	hdr,	/*!< in: buffer containing the log group
				header */
	lsn_t*		lsn,	/*!< out: checkpoint lsn */
	lsn_t*		offset,	/*!< out: checkpoint offset in the log group */
	lsn_t*		cp_no,	/*!< out: checkpoint number */
	lsn_t*		first_header_lsn)
				/*!< out: lsn of of the start of the
				first log file */
{
	ulint		max_cp		= 0;
	ib_uint64_t	max_cp_no	= 0;
	const byte*	cp_buf;

	cp_buf = hdr + LOG_CHECKPOINT_1;

	if (recv_check_cp_is_consistent(cp_buf)) {
		max_cp_no = mach_read_from_8(cp_buf + LOG_CHECKPOINT_NO);
		max_cp = LOG_CHECKPOINT_1;
	}

	cp_buf = hdr + LOG_CHECKPOINT_2;

	if (recv_check_cp_is_consistent(cp_buf)) {
		if (mach_read_from_8(cp_buf + LOG_CHECKPOINT_NO) > max_cp_no) {
			max_cp = LOG_CHECKPOINT_2;
		}
	}

	if (max_cp == 0) {
		return(FALSE);
	}

	cp_buf = hdr + max_cp;

	*lsn = mach_read_from_8(cp_buf + LOG_CHECKPOINT_LSN);
	*offset = mach_read_from_4(
		cp_buf + LOG_CHECKPOINT_OFFSET_LOW32);
	*offset |= ((lsn_t) mach_read_from_4(
			    cp_buf + LOG_CHECKPOINT_OFFSET_HIGH32)) << 32;

	*cp_no = mach_read_from_8(cp_buf + LOG_CHECKPOINT_NO);

	*first_header_lsn = mach_read_from_8(hdr + LOG_FILE_START_LSN);

	return(TRUE);
}
#endif /* !UNIV_HOTBACKUP */

/******************************************************//**
Checks the 4-byte checksum to the trailer checksum field of a log
block.  We also accept a log block in the old format before
InnoDB-3.23.52 where the checksum field contains the log block number.
@return TRUE if ok, or if the log block may be in the format of InnoDB
version predating 3.23.52 */
static
ibool
log_block_checksum_is_ok_or_old_format(
/*===================================*/
	const byte*	block)	/*!< in: pointer to a log block */
{
	if (log_block_calc_checksum(block) == log_block_get_checksum(block)) {

		return(TRUE);
	}

	if (log_block_get_hdr_no(block) == log_block_get_checksum(block)) {

		/* We assume the log block is in the format of
		InnoDB version < 3.23.52 and the block is ok */
#if 0
		fprintf(stderr,
			"InnoDB: Scanned old format < InnoDB-3.23.52"
			" log block number %lu\n",
			log_block_get_hdr_no(block));
#endif
		return(TRUE);
	}

	return(FALSE);
}

#ifdef UNIV_HOTBACKUP
/*******************************************************************//**
Scans the log segment and n_bytes_scanned is set to the length of valid
log scanned. */

void
recv_scan_log_seg_for_backup(
/*=========================*/
	byte*		buf,		/*!< in: buffer containing log data */
	ulint		buf_len,	/*!< in: data length in that buffer */
	lsn_t*		scanned_lsn,	/*!< in/out: lsn of buffer start,
					we return scanned lsn */
	ulint*		scanned_checkpoint_no,
					/*!< in/out: 4 lowest bytes of the
					highest scanned checkpoint number so
					far */
	ulint*		n_bytes_scanned)/*!< out: how much we were able to
					scan, smaller than buf_len if log
					data ended here */
{
	ulint	data_len;
	byte*	log_block;
	ulint	no;

	*n_bytes_scanned = 0;

	for (log_block = buf; log_block < buf + buf_len;
	     log_block += OS_FILE_LOG_BLOCK_SIZE) {

		no = log_block_get_hdr_no(log_block);

#if 0
		fprintf(stderr, "Log block header no %lu\n", no);
#endif

		if (no != log_block_convert_lsn_to_no(*scanned_lsn)
		    || !log_block_checksum_is_ok_or_old_format(log_block)) {
#if 0
			fprintf(stderr,
				"Log block n:o %lu, scanned lsn n:o %lu\n",
				no, log_block_convert_lsn_to_no(*scanned_lsn));
#endif
			/* Garbage or an incompletely written log block */

			log_block += OS_FILE_LOG_BLOCK_SIZE;
#if 0
			fprintf(stderr,
				"Next log block n:o %lu\n",
				log_block_get_hdr_no(log_block));
#endif
			break;
		}

		if (*scanned_checkpoint_no > 0
		    && log_block_get_checkpoint_no(log_block)
		    < *scanned_checkpoint_no
		    && *scanned_checkpoint_no
		    - log_block_get_checkpoint_no(log_block)
		    > 0x80000000UL) {

			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */
#if 0
			fprintf(stderr,
				"Scanned cp n:o %lu, block cp n:o %lu\n",
				*scanned_checkpoint_no,
				log_block_get_checkpoint_no(log_block));
#endif
			break;
		}

		data_len = log_block_get_data_len(log_block);

		*scanned_checkpoint_no
			= log_block_get_checkpoint_no(log_block);
		*scanned_lsn += data_len;

		*n_bytes_scanned += data_len;

		if (data_len < OS_FILE_LOG_BLOCK_SIZE) {
			/* Log data ends here */

#if 0
			fprintf(stderr, "Log block data len %lu\n",
				data_len);
#endif
			break;
		}
	}
}
#endif /* UNIV_HOTBACKUP */

/*******************************************************************//**
Tries to parse a single log record body and also applies it to a page if
specified. File ops are parsed, but not applied in this function.
@return log record end, NULL if not a complete record */
static
byte*
recv_parse_or_apply_log_rec_body(
/*=============================*/
	mlog_id_t	type,	/*!< in: type */
	byte*		ptr,	/*!< in: pointer to a buffer */
	byte*		end_ptr,/*!< in: pointer to the buffer end */
	buf_block_t*	block,	/*!< in/out: buffer block or NULL; if
				not NULL, then the log record is
				applied to the page, and the log
				record should be complete then */
	mtr_t*		mtr,	/*!< in: mtr or NULL; should be non-NULL
				if and only if block is non-NULL */
	ulint		space_id,
				/*!< in: tablespace id obtained by
				parsing initial log record. */
	ulint		page_no)
				/*!< in: page-number obtained by
				parsing initial log record. */
{
	dict_index_t*	index	= NULL;
	page_t*		page;
	page_zip_des_t*	page_zip;
#ifdef UNIV_DEBUG
	ulint		page_type;
#endif /* UNIV_DEBUG */

	ut_ad(!block == !mtr);

	if (block) {
		page = block->frame;
		page_zip = buf_block_get_page_zip(block);
		ut_d(page_type = fil_page_get_type(page));
	} else {
		page = NULL;
		page_zip = NULL;
		ut_d(page_type = FIL_PAGE_TYPE_ALLOCATED);
	}

	switch (type) {
#ifdef UNIV_LOG_LSN_DEBUG
	case MLOG_LSN:
		/* The LSN is checked in recv_parse_log_rec(). */
		break;
#endif /* UNIV_LOG_LSN_DEBUG */
	case MLOG_1BYTE: case MLOG_2BYTES: case MLOG_4BYTES: case MLOG_8BYTES:
#ifdef UNIV_DEBUG
		if (page && page_type == FIL_PAGE_TYPE_ALLOCATED
		    && end_ptr >= ptr + 2) {
			/* It is OK to set FIL_PAGE_TYPE and certain
			list node fields on an empty page.  Any other
			write is not OK. */

			/* NOTE: There may be bogus assertion failures for
			dict_hdr_create(), trx_rseg_header_create(),
			trx_sys_create_doublewrite_buf(), and
			trx_sysf_create().
			These are only called during database creation. */
			ulint	offs = mach_read_from_2(ptr);

			switch (type) {
			default:
				ut_error;
			case MLOG_2BYTES:
				/* Note that this can fail when the
				redo log been written with something
				older than InnoDB Plugin 1.0.4. */
				ut_ad(offs == FIL_PAGE_TYPE
				      || offs == IBUF_TREE_SEG_HEADER
				      + IBUF_HEADER + FSEG_HDR_OFFSET
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      + FIL_ADDR_SIZE
				      || offs == PAGE_BTR_SEG_LEAF
				      + PAGE_HEADER + FSEG_HDR_OFFSET
				      || offs == PAGE_BTR_SEG_TOP
				      + PAGE_HEADER + FSEG_HDR_OFFSET
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      + 0 /*FLST_PREV*/
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_BYTE
				      + FIL_ADDR_SIZE /*FLST_NEXT*/);
				break;
			case MLOG_4BYTES:
				/* Note that this can fail when the
				redo log been written with something
				older than InnoDB Plugin 1.0.4. */
				ut_ad(0
				      || offs == IBUF_TREE_SEG_HEADER
				      + IBUF_HEADER + FSEG_HDR_SPACE
				      || offs == IBUF_TREE_SEG_HEADER
				      + IBUF_HEADER + FSEG_HDR_PAGE_NO
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER/* flst_init */
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      || offs == PAGE_BTR_IBUF_FREE_LIST
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      + FIL_ADDR_SIZE
				      || offs == PAGE_BTR_SEG_LEAF
				      + PAGE_HEADER + FSEG_HDR_PAGE_NO
				      || offs == PAGE_BTR_SEG_LEAF
				      + PAGE_HEADER + FSEG_HDR_SPACE
				      || offs == PAGE_BTR_SEG_TOP
				      + PAGE_HEADER + FSEG_HDR_PAGE_NO
				      || offs == PAGE_BTR_SEG_TOP
				      + PAGE_HEADER + FSEG_HDR_SPACE
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      + 0 /*FLST_PREV*/
				      || offs == PAGE_BTR_IBUF_FREE_LIST_NODE
				      + PAGE_HEADER + FIL_ADDR_PAGE
				      + FIL_ADDR_SIZE /*FLST_NEXT*/);
				break;
			}
		}
#endif /* UNIV_DEBUG */
		ptr = mlog_parse_nbytes(type, ptr, end_ptr, page, page_zip);
		break;
	case MLOG_REC_INSERT: case MLOG_COMP_REC_INSERT:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_INSERT,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_cur_parse_insert_rec(FALSE, ptr, end_ptr,
							block, index, mtr);
		}
		break;
	case MLOG_REC_CLUST_DELETE_MARK: case MLOG_COMP_REC_CLUST_DELETE_MARK:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_CLUST_DELETE_MARK,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = btr_cur_parse_del_mark_set_clust_rec(
				ptr, end_ptr, page, page_zip, index);
		}
		break;
	case MLOG_COMP_REC_SEC_DELETE_MARK:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		/* This log record type is obsolete, but we process it for
		backward compatibility with MySQL 5.0.3 and 5.0.4. */
		ut_a(!page || page_is_comp(page));
		ut_a(!page_zip);
		ptr = mlog_parse_index(ptr, end_ptr, TRUE, &index);
		if (!ptr) {
			break;
		}
		/* Fall through */
	case MLOG_REC_SEC_DELETE_MARK:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		ptr = btr_cur_parse_del_mark_set_sec_rec(ptr, end_ptr,
							 page, page_zip);
		break;
	case MLOG_REC_UPDATE_IN_PLACE: case MLOG_COMP_REC_UPDATE_IN_PLACE:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_UPDATE_IN_PLACE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = btr_cur_parse_update_in_place(ptr, end_ptr, page,
							    page_zip, index);
		}
		break;
	case MLOG_LIST_END_DELETE: case MLOG_COMP_LIST_END_DELETE:
	case MLOG_LIST_START_DELETE: case MLOG_COMP_LIST_START_DELETE:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_LIST_END_DELETE
				     || type == MLOG_COMP_LIST_START_DELETE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_parse_delete_rec_list(type, ptr, end_ptr,
							 block, index, mtr);
		}
		break;
	case MLOG_LIST_END_COPY_CREATED: case MLOG_COMP_LIST_END_COPY_CREATED:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_LIST_END_COPY_CREATED,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_parse_copy_rec_list_to_created_page(
				ptr, end_ptr, block, index, mtr);
		}
		break;
	case MLOG_PAGE_REORGANIZE:
	case MLOG_COMP_PAGE_REORGANIZE:
	case MLOG_ZIP_PAGE_REORGANIZE:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type != MLOG_PAGE_REORGANIZE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = btr_parse_page_reorganize(
				ptr, end_ptr, index,
				type == MLOG_ZIP_PAGE_REORGANIZE,
				block, mtr);
		}
		break;
	case MLOG_PAGE_CREATE: case MLOG_COMP_PAGE_CREATE:
		/* Allow anything in page_type when creating a page. */
		ut_a(!page_zip);
		page_parse_create(block, type == MLOG_COMP_PAGE_CREATE);
		break;
	case MLOG_UNDO_INSERT:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page);
		break;
	case MLOG_UNDO_ERASE_END:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_erase_page_end(ptr, end_ptr, page, mtr);
		break;
	case MLOG_UNDO_INIT:
		/* Allow anything in page_type when creating a page. */
		ptr = trx_undo_parse_page_init(ptr, end_ptr, page, mtr);
		break;
	case MLOG_UNDO_HDR_DISCARD:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_discard_latest(ptr, end_ptr, page, mtr);
		break;
	case MLOG_UNDO_HDR_CREATE:
	case MLOG_UNDO_HDR_REUSE:
		ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG);
		ptr = trx_undo_parse_page_header(type, ptr, end_ptr,
						 page, mtr);
		break;
	case MLOG_REC_MIN_MARK: case MLOG_COMP_REC_MIN_MARK:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		/* On a compressed page, MLOG_COMP_REC_MIN_MARK
		will be followed by MLOG_COMP_REC_DELETE
		or MLOG_ZIP_WRITE_HEADER(FIL_PAGE_PREV, FIL_NULL)
		in the same mini-transaction. */
		ut_a(type == MLOG_COMP_REC_MIN_MARK || !page_zip);
		ptr = btr_parse_set_min_rec_mark(
			ptr, end_ptr, type == MLOG_COMP_REC_MIN_MARK,
			page, mtr);
		break;
	case MLOG_REC_DELETE: case MLOG_COMP_REC_DELETE:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);

		if (NULL != (ptr = mlog_parse_index(
				     ptr, end_ptr,
				     type == MLOG_COMP_REC_DELETE,
				     &index))) {
			ut_a(!page
			     || (ibool)!!page_is_comp(page)
			     == dict_table_is_comp(index->table));
			ptr = page_cur_parse_delete_rec(ptr, end_ptr,
							block, index, mtr);
		}
		break;
	case MLOG_IBUF_BITMAP_INIT:
		/* Allow anything in page_type when creating a page. */
		ptr = ibuf_parse_bitmap_init(ptr, end_ptr, block, mtr);
		break;
	case MLOG_INIT_FILE_PAGE:
		/* Allow anything in page_type when creating a page. */
		ptr = fsp_parse_init_file_page(ptr, end_ptr, block);
		break;
	case MLOG_WRITE_STRING:
		ut_ad(!page || page_type != FIL_PAGE_TYPE_ALLOCATED);
		ptr = mlog_parse_string(ptr, end_ptr, page, page_zip);
		break;
	case MLOG_FILE_CREATE:
	case MLOG_FILE_DELETE:
	case MLOG_FILE_CREATE2:
	case MLOG_FILE_RENAME:
		ut_ad(page == NULL);
		ut_ad(space_id != 0);

		{
			/* In normal MySQL crash recovery we only
			replay MLOG_FILE_RENAME operations. When
			applying the InnoDB redo log to a hot backup
			in MySQL Enterprise Backup, we will replay all
			file operations. */
#ifdef UNIV_HOTBACKUP
			const bool	parse_only = !recv_replay_file_ops;
#else /* UNIV_HOTBACKUP */
			const bool	parse_only = type != MLOG_FILE_RENAME;
#endif /* UNIV_HOTBACKUP */

			ptr = fil_op_log_parse_or_replay(
				ptr, end_ptr, type,
				space_id, page_no, parse_only);
		}

#ifdef UNIV_HOTBACKUP
		if (ptr == NULL && recv_replay_file_ops) {
			/* In ibbackup --apply-log, replay an .ibd file
			operation, if possible; note that
			fil_path_to_mysql_datadir is set in ibbackup to
			point to the datadir we should use there */

			ib_logf(IB_LOG_LEVEL_FATAL,
				"Incomplete file op log record of type "
				ULINTPF " space " ULINTPF ".",
				ulint(type), space);
		}
#endif /* UNIV_HOTBACKUP */
		break;
	case MLOG_ZIP_WRITE_NODE_PTR:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		ptr = page_zip_parse_write_node_ptr(ptr, end_ptr,
						    page, page_zip);
		break;
	case MLOG_ZIP_WRITE_BLOB_PTR:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		ptr = page_zip_parse_write_blob_ptr(ptr, end_ptr,
						    page, page_zip);
		break;
	case MLOG_ZIP_WRITE_HEADER:
		ut_ad(!page || page_type == FIL_PAGE_INDEX);
		ptr = page_zip_parse_write_header(ptr, end_ptr,
						  page, page_zip);
		break;
	case MLOG_ZIP_PAGE_COMPRESS:
		/* Allow anything in page_type when creating a page. */
		ptr = page_zip_parse_compress(ptr, end_ptr,
					      page, page_zip);
		break;
	case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
		if (NULL != (ptr = mlog_parse_index(
				ptr, end_ptr, TRUE, &index))) {

			ut_a(!page || ((ibool)!!page_is_comp(page)
				== dict_table_is_comp(index->table)));
			ptr = page_zip_parse_compress_no_data(
				ptr, end_ptr, page, page_zip, index);
		}
		break;
	default:
		ptr = NULL;
		recv_sys->found_corrupt_log = TRUE;
	}

	if (index) {
		dict_table_t*	table = index->table;

		dict_mem_index_free(index);
		dict_mem_table_free(table);
	}

	return(ptr);
}

/*********************************************************************//**
Calculates the fold value of a page file address: used in inserting or
searching for a log record in the hash table.
@return folded value */
UNIV_INLINE
ulint
recv_fold(
/*======*/
	ulint	space,	/*!< in: space */
	ulint	page_no)/*!< in: page number */
{
	return(ut_fold_ulint_pair(space, page_no));
}

/*********************************************************************//**
Calculates the hash value of a page file address: used in inserting or
searching for a log record in the hash table.
@return folded value */
UNIV_INLINE
ulint
recv_hash(
/*======*/
	ulint	space,	/*!< in: space */
	ulint	page_no)/*!< in: page number */
{
	return(hash_calc_hash(recv_fold(space, page_no), recv_sys->addr_hash));
}

/*********************************************************************//**
Gets the hashed file address struct for a page.
@return file address struct, NULL if not found from the hash table */
static
recv_addr_t*
recv_get_fil_addr_struct(
/*=====================*/
	ulint	space,	/*!< in: space id */
	ulint	page_no)/*!< in: page number */
{
	recv_addr_t*	recv_addr;

	for (recv_addr = static_cast<recv_addr_t*>(
			HASH_GET_FIRST(recv_sys->addr_hash,
				       recv_hash(space, page_no)));
	     recv_addr != 0;
	     recv_addr = static_cast<recv_addr_t*>(
		     HASH_GET_NEXT(addr_hash, recv_addr))) {

		if (recv_addr->space == space
		    && recv_addr->page_no == page_no) {

			return(recv_addr);
		}
	}

	return(NULL);
}

/*******************************************************************//**
Adds a new log record to the hash table of log records. */
static
void
recv_add_to_hash_table(
/*===================*/
	mlog_id_t	type,		/*!< in: log record type */
	ulint		space,		/*!< in: space id */
	ulint		page_no,	/*!< in: page number */
	byte*		body,		/*!< in: log record body */
	byte*		rec_end,	/*!< in: log record end */
	lsn_t		start_lsn,	/*!< in: start lsn of the mtr */
	lsn_t		end_lsn)	/*!< in: end lsn of the mtr */
{
	recv_t*		recv;
	ulint		len;
	recv_data_t*	recv_data;
	recv_data_t**	prev_field;
	recv_addr_t*	recv_addr;

	ut_ad(type != MLOG_FILE_CREATE);
	ut_ad(type != MLOG_FILE_CREATE2);
	ut_ad(type != MLOG_FILE_RENAME);
	ut_ad(type != MLOG_FILE_DELETE);

	if (fil_tablespace_deleted_or_being_deleted_in_mem(space, -1)) {
		/* The tablespace does not exist any more: do not store the
		log record */

		return;
	}

	len = rec_end - body;

	recv = static_cast<recv_t*>(
		mem_heap_alloc(recv_sys->heap, sizeof(recv_t)));

	recv->type = type;
	recv->len = rec_end - body;
	recv->start_lsn = start_lsn;
	recv->end_lsn = end_lsn;

	recv_addr = recv_get_fil_addr_struct(space, page_no);

	if (recv_addr == NULL) {
		recv_addr = static_cast<recv_addr_t*>(
			mem_heap_alloc(recv_sys->heap, sizeof(recv_addr_t)));

		recv_addr->space = space;
		recv_addr->page_no = page_no;
		recv_addr->state = RECV_NOT_PROCESSED;

		UT_LIST_INIT(recv_addr->rec_list, &recv_t::rec_list);

		HASH_INSERT(recv_addr_t, addr_hash, recv_sys->addr_hash,
			    recv_fold(space, page_no), recv_addr);
		recv_sys->n_addrs++;
#if 0
		fprintf(stderr, "Inserting log rec for space %lu, page %lu\n",
			space, page_no);
#endif
	}

	UT_LIST_ADD_LAST(recv_addr->rec_list, recv);

	prev_field = &(recv->data);

	/* Store the log record body in chunks of less than UNIV_PAGE_SIZE:
	recv_sys->heap grows into the buffer pool, and bigger chunks could not
	be allocated */

	while (rec_end > body) {

		len = rec_end - body;

		if (len > RECV_DATA_BLOCK_SIZE) {
			len = RECV_DATA_BLOCK_SIZE;
		}

		recv_data = static_cast<recv_data_t*>(
			mem_heap_alloc(recv_sys->heap,
				       sizeof(recv_data_t) + len));

		*prev_field = recv_data;

		memcpy(recv_data + 1, body, len);

		prev_field = &(recv_data->next);

		body += len;
	}

	*prev_field = NULL;
}

/*********************************************************************//**
Copies the log record body from recv to buf. */
static
void
recv_data_copy_to_buf(
/*==================*/
	byte*	buf,	/*!< in: buffer of length at least recv->len */
	recv_t*	recv)	/*!< in: log record */
{
	recv_data_t*	recv_data;
	ulint		part_len;
	ulint		len;

	len = recv->len;
	recv_data = recv->data;

	while (len > 0) {
		if (len > RECV_DATA_BLOCK_SIZE) {
			part_len = RECV_DATA_BLOCK_SIZE;
		} else {
			part_len = len;
		}

		ut_memcpy(buf, ((byte*) recv_data) + sizeof(recv_data_t),
			  part_len);
		buf += part_len;
		len -= part_len;

		recv_data = recv_data->next;
	}
}

/************************************************************************//**
Applies the hashed log records to the page, if the page lsn is less than the
lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool. */

void
recv_recover_page_func(
/*===================*/
#ifndef UNIV_HOTBACKUP
	ibool		just_read_in,
				/*!< in: TRUE if the i/o handler calls
				this for a freshly read page */
#endif /* !UNIV_HOTBACKUP */
	buf_block_t*	block)	/*!< in/out: buffer block */
{
	page_t*		page;
	page_zip_des_t*	page_zip;
	recv_addr_t*	recv_addr;
	recv_t*		recv;
	byte*		buf;
	lsn_t		start_lsn;
	lsn_t		end_lsn;
	lsn_t		page_lsn;
	lsn_t		page_newest_lsn;
	ibool		modification_to_page;
#ifndef UNIV_HOTBACKUP
	ibool		success;
#endif /* !UNIV_HOTBACKUP */
	mtr_t		mtr;

	mutex_enter(&(recv_sys->mutex));

	if (recv_sys->apply_log_recs == FALSE) {

		/* Log records should not be applied now */

		mutex_exit(&(recv_sys->mutex));

		return;
	}

	recv_addr = recv_get_fil_addr_struct(block->page.id.space(),
					     block->page.id.page_no());

	if ((recv_addr == NULL)
	    || (recv_addr->state == RECV_BEING_PROCESSED)
	    || (recv_addr->state == RECV_PROCESSED)) {

		mutex_exit(&(recv_sys->mutex));

		return;
	}

#if 0
	fprintf(stderr, "Recovering space %lu, page %lu\n",
		block->page.id.space(), block->page.id.page_no());
#endif

	recv_addr->state = RECV_BEING_PROCESSED;

	mutex_exit(&(recv_sys->mutex));

	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NONE);

	page = block->frame;
	page_zip = buf_block_get_page_zip(block);

#ifndef UNIV_HOTBACKUP
	if (just_read_in) {
		/* Move the ownership of the x-latch on the page to
		this OS thread, so that we can acquire a second
		x-latch on it.  This is needed for the operations to
		the page to pass the debug checks. */

		rw_lock_x_lock_move_ownership(&block->lock);
	}

	success = buf_page_get_known_nowait(RW_X_LATCH, block,
					    BUF_KEEP_OLD,
					    __FILE__, __LINE__,
					    &mtr);
	ut_a(success);

	buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
#endif /* !UNIV_HOTBACKUP */

	/* Read the newest modification lsn from the page */
	page_lsn = mach_read_from_8(page + FIL_PAGE_LSN);

#ifndef UNIV_HOTBACKUP
	/* It may be that the page has been modified in the buffer
	pool: read the newest modification lsn there */

	page_newest_lsn = buf_page_get_newest_modification(&block->page);

	if (page_newest_lsn) {

		page_lsn = page_newest_lsn;
	}
#else /* !UNIV_HOTBACKUP */
	/* In recovery from a backup we do not really use the buffer pool */
	page_newest_lsn = 0;
#endif /* !UNIV_HOTBACKUP */

	modification_to_page = FALSE;
	start_lsn = end_lsn = 0;

	recv = UT_LIST_GET_FIRST(recv_addr->rec_list);

	while (recv) {
		end_lsn = recv->end_lsn;

		if (recv->len > RECV_DATA_BLOCK_SIZE) {
			/* We have to copy the record body to a separate
			buffer */

			buf = static_cast<byte*>(ut_malloc(recv->len));

			recv_data_copy_to_buf(buf, recv);
		} else {
			buf = ((byte*)(recv->data)) + sizeof(recv_data_t);
		}

		if (recv->type == MLOG_INIT_FILE_PAGE) {
			page_lsn = page_newest_lsn;

			memset(FIL_PAGE_LSN + page, 0, 8);
			memset(UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM
			       + page, 0, 8);

			if (page_zip) {
				memset(FIL_PAGE_LSN + page_zip->data, 0, 8);
			}
		}

		/* Ignore applying the redo logs for tablespace that is
		truncated. Post recovery there is fixup action that will
		restore the tablespace back to normal state.
		Applying redo at this stage can result in error given that
		redo will have action recorded on page before tablespace
		was re-inited and that would lead to an error while applying
		such action. */
		if (recv->start_lsn >= page_lsn
		    && !srv_is_tablespace_truncated(recv_addr->space)) {

			lsn_t	end_lsn;

			if (!modification_to_page) {

				modification_to_page = TRUE;
				start_lsn = recv->start_lsn;
			}

			DBUG_PRINT("ib_log",
				   ("apply " LSN_PF ": %u len %u"
				    " page %u:%u", recv->start_lsn,
				    (unsigned) recv->type,
				    (unsigned) recv->len,
				    (unsigned) recv_addr->space,
				    (unsigned) recv_addr->page_no));

			recv_parse_or_apply_log_rec_body(
				recv->type, buf, buf + recv->len,
				block, &mtr, recv_addr->space,
				recv_addr->page_no);

			end_lsn = recv->start_lsn + recv->len;
			mach_write_to_8(FIL_PAGE_LSN + page, end_lsn);
			mach_write_to_8(UNIV_PAGE_SIZE
					- FIL_PAGE_END_LSN_OLD_CHKSUM
					+ page, end_lsn);

			if (page_zip) {
				mach_write_to_8(FIL_PAGE_LSN
						+ page_zip->data, end_lsn);
			}
		}

		if (recv->len > RECV_DATA_BLOCK_SIZE) {
			ut_free(buf);
		}

		recv = UT_LIST_GET_NEXT(rec_list, recv);
	}

#ifdef UNIV_ZIP_DEBUG
	if (fil_page_get_type(page) == FIL_PAGE_INDEX) {
		page_zip_des_t*	page_zip = buf_block_get_page_zip(block);

		ut_a(!page_zip
		     || page_zip_validate_low(page_zip, page, NULL, FALSE));
	}
#endif /* UNIV_ZIP_DEBUG */

#ifndef UNIV_HOTBACKUP
	if (modification_to_page) {
		ut_a(block);

		log_flush_order_mutex_enter();
		buf_flush_recv_note_modification(block, start_lsn, end_lsn);
		log_flush_order_mutex_exit();
	}
#endif /* !UNIV_HOTBACKUP */

	/* Make sure that committing mtr does not change the modification
	lsn values of page */

	mtr.discard_modifications();

	mtr_commit(&mtr);

	mutex_enter(&(recv_sys->mutex));

	if (recv_max_page_lsn < page_lsn) {
		recv_max_page_lsn = page_lsn;
	}

	recv_addr->state = RECV_PROCESSED;

	ut_a(recv_sys->n_addrs);
	recv_sys->n_addrs--;

	mutex_exit(&(recv_sys->mutex));

}

#ifndef UNIV_HOTBACKUP

/** Reads in pages which have hashed log records, from an area around a given
page number.
@param[in]	page_id	page id
@return number of pages found */
static
ulint
recv_read_in_area(
	const page_id_t&	page_id)
{
	recv_addr_t* recv_addr;
	ulint	page_nos[RECV_READ_AHEAD_AREA];
	ulint	low_limit;
	ulint	n;

	low_limit = page_id.page_no()
		- (page_id.page_no() % RECV_READ_AHEAD_AREA);

	n = 0;

	for (ulint page_no = low_limit;
	     page_no < low_limit + RECV_READ_AHEAD_AREA;
	     page_no++) {

		recv_addr = recv_get_fil_addr_struct(page_id.space(), page_no);

		const page_id_t	cur_page_id(page_id.space(), page_no);

		if (recv_addr && !buf_page_peek(cur_page_id)) {

			mutex_enter(&(recv_sys->mutex));

			if (recv_addr->state == RECV_NOT_PROCESSED) {
				recv_addr->state = RECV_BEING_READ;

				page_nos[n] = page_no;

				n++;
			}

			mutex_exit(&(recv_sys->mutex));
		}
	}

	buf_read_recv_pages(FALSE, page_id.space(), page_nos, n);
	/*
	fprintf(stderr, "Recv pages at %lu n %lu\n", page_nos[0], n);
	*/
	return(n);
}

/*******************************************************************//**
Empties the hash table of stored log records, applying them to appropriate
pages. */

void
recv_apply_hashed_log_recs(
/*=======================*/
	ibool	allow_ibuf)	/*!< in: if TRUE, also ibuf operations are
				allowed during the application; if FALSE,
				no ibuf operations are allowed, and after
				the application all file pages are flushed to
				disk and invalidated in buffer pool: this
				alternative means that no new log records
				can be generated during the application;
				the caller must in this case own the log
				mutex */
{
	recv_addr_t* recv_addr;
	ulint	i;
	ibool	has_printed	= FALSE;
	mtr_t	mtr;
loop:
	mutex_enter(&(recv_sys->mutex));

	if (recv_sys->apply_batch_on) {

		mutex_exit(&(recv_sys->mutex));

		os_thread_sleep(500000);

		goto loop;
	}

	ut_ad(!allow_ibuf == mutex_own(&log_sys->mutex));

	if (!allow_ibuf) {
		recv_no_ibuf_operations = TRUE;
	}

	recv_sys->apply_log_recs = TRUE;
	recv_sys->apply_batch_on = TRUE;

	for (i = 0; i < hash_get_n_cells(recv_sys->addr_hash); i++) {

		for (recv_addr = static_cast<recv_addr_t*>(
				HASH_GET_FIRST(recv_sys->addr_hash, i));
		     recv_addr != 0;
		     recv_addr = static_cast<recv_addr_t*>(
				HASH_GET_NEXT(addr_hash, recv_addr))) {

			const page_id_t		page_id(recv_addr->space,
							recv_addr->page_no);
			bool			found;
			const page_size_t&	page_size
				= fil_space_get_page_size(recv_addr->space,
							  &found);

			ut_ad(found);

			if (recv_addr->state == RECV_NOT_PROCESSED) {
				if (!has_printed) {
					ib_logf(IB_LOG_LEVEL_INFO,
						"Starting an apply batch"
						" of log records"
						" to the database...");
					fputs("InnoDB: Progress in percent: ",
					      stderr);
					has_printed = TRUE;
				}

				mutex_exit(&(recv_sys->mutex));

				if (buf_page_peek(page_id)) {
					buf_block_t*	block;

					mtr_start(&mtr);

					block = buf_page_get(
						page_id, page_size,
						RW_X_LATCH, &mtr);

					buf_block_dbg_add_level(
						block, SYNC_NO_ORDER_CHECK);

					recv_recover_page(FALSE, block);
					mtr_commit(&mtr);
				} else {
					recv_read_in_area(page_id);
				}

				mutex_enter(&(recv_sys->mutex));
			}
		}

		if (has_printed
		    && (i * 100) / hash_get_n_cells(recv_sys->addr_hash)
		    != ((i + 1) * 100)
		    / hash_get_n_cells(recv_sys->addr_hash)) {

			fprintf(stderr, "%lu ", (ulong)
				((i * 100)
				 / hash_get_n_cells(recv_sys->addr_hash)));
		}
	}

	/* Wait until all the pages have been processed */

	while (recv_sys->n_addrs != 0) {

		mutex_exit(&(recv_sys->mutex));

		os_thread_sleep(500000);

		mutex_enter(&(recv_sys->mutex));
	}

	if (has_printed) {

		fprintf(stderr, "\n");
	}

	if (!allow_ibuf) {

		/* Flush all the file pages to disk and invalidate them in
		the buffer pool */

		ut_d(recv_no_log_write = TRUE);
		mutex_exit(&(recv_sys->mutex));
		mutex_exit(&(log_sys->mutex));

		/* Stop the recv_writer thread from issuing any LRU
		flush batches. */
		mutex_enter(&recv_sys->writer_mutex);

		/* Wait for any currently run batch to end. */
		buf_flush_wait_LRU_batch_end();

		buf_flush_sync_all_buf_pools();

		buf_pool_invalidate();

		/* Allow batches from recv_writer thread. */
		mutex_exit(&recv_sys->writer_mutex);

		mutex_enter(&(log_sys->mutex));
		mutex_enter(&(recv_sys->mutex));
		ut_d(recv_no_log_write = FALSE);

		recv_no_ibuf_operations = FALSE;
	}

	recv_sys->apply_log_recs = FALSE;
	recv_sys->apply_batch_on = FALSE;

	recv_sys_empty_hash();

	if (has_printed) {
		ib_logf(IB_LOG_LEVEL_INFO, "Apply batch completed");
	}

	mutex_exit(&(recv_sys->mutex));
}
#else /* !UNIV_HOTBACKUP */
/*******************************************************************//**
Applies log records in the hash table to a backup. */

void
recv_apply_log_recs_for_backup(void)
/*================================*/
{
	recv_addr_t*	recv_addr;
	ulint		n_hash_cells;
	buf_block_t*	block;
	ulint		actual_size;
	bool		success;
	ulint		error;
	ulint		i;

	recv_sys->apply_log_recs = TRUE;
	recv_sys->apply_batch_on = TRUE;

	block = back_block1;

	ib_logf(IB_LOG_LEVEL_INFO,
		"Starting an apply batch of log records to the database...");

	fputs("InnoDB: Progress in percent: ", stderr);

	n_hash_cells = hash_get_n_cells(recv_sys->addr_hash);

	for (i = 0; i < n_hash_cells; i++) {
		/* The address hash table is externally chained */
		recv_addr = hash_get_nth_cell(recv_sys->addr_hash, i)->node;

		while (recv_addr != NULL) {

			bool			found;
			const page_size_t&	page_size
				= fil_space_get_page_size(recv_addr->space,
							  &found);

			if (!found) {
#if 0
				fprintf(stderr,
					"InnoDB: Warning: cannot apply"
					" log record to"
					" tablespace %lu page %lu,\n"
					"InnoDB: because tablespace with"
					" that id does not exist.\n",
					recv_addr->space, recv_addr->page_no);
#endif
				recv_addr->state = RECV_PROCESSED;

				ut_a(recv_sys->n_addrs);
				recv_sys->n_addrs--;

				goto skip_this_recv_addr;
			}

			/* We simulate a page read made by the buffer pool, to
			make sure the recovery apparatus works ok. We must init
			the block. */

			buf_page_init_for_backup_restore(
				page_id_t(recv_addr->space, recv_addr->page_no),
				page_size, block);

			/* Extend the tablespace's last file if the page_no
			does not fall inside its bounds; we assume the last
			file is auto-extending, and ibbackup copied the file
			when it still was smaller */

			success = fil_extend_space_to_desired_size(
				&actual_size,
				recv_addr->space, recv_addr->page_no + 1);
			if (!success) {
				ib_logf(IB_LOG_LEVEL_FATAL,
					"Cannot extend tablespace %u"
					" to hold %u pages",
					recv_addr->space, recv_addr->page_no);
			}

			/* Read the page from the tablespace file using the
			fil0fil.cc routines */

			const page_id_t	page_id(recv_addr->space,
						recv_addr->page_no);

			if (page_size.is_compressed()) {
				error = fil_io(OS_FILE_READ, true, page_id,
					       page_size, 0, page_size.bytes(),
					       block->page.zip.data, NULL);

				if (error == DB_SUCCESS
				    && !buf_zip_decompress(block, TRUE)) {
					ut_error;
				}
			} else {
				error = fil_io(OS_FILE_READ, true, page_id,
					       page_size, 0, page_size.bytes(),
					       block->frame, NULL);
			}

			if (error != DB_SUCCESS) {
				ib_logf(IB_LOG_LEVEL_FATAL,
					"Cannot read from tablespace"
					" %lu page number %lu",
					(ulong) recv_addr->space,
					(ulong) recv_addr->page_no);
			}

			/* Apply the log records to this page */
			recv_recover_page(FALSE, block);

			/* Write the page back to the tablespace file using the
			fil0fil.cc routines */

			buf_flush_init_for_writing(
				block->frame, buf_block_get_page_zip(block),
				mach_read_from_8(block->frame + FIL_PAGE_LSN));

			if (page_size.is_compressed()) {
				error = fil_io(OS_FILE_WRITE, true, page_id,
					       0, page_size.bytes(),
					       block->page.zip.data, NULL);
			} else {
				error = fil_io(OS_FILE_WRITE, true, page_id,
					       0, page_size.bytes(),
					       block->frame, NULL);
			}
skip_this_recv_addr:
			recv_addr = HASH_GET_NEXT(addr_hash, recv_addr);
		}

		if ((100 * i) / n_hash_cells
		    != (100 * (i + 1)) / n_hash_cells) {
			fprintf(stderr, "%lu ",
				(ulong) ((100 * i) / n_hash_cells));
			fflush(stderr);
		}
	}

	recv_sys_empty_hash();
}
#endif /* !UNIV_HOTBACKUP */

/** Tries to parse a single log record.
@param[out]	type		log record type
@param[in]	ptr		pointer to a buffer
@param[in]	end_ptr		end of the buffer
@param[out]	space_id	tablespace identifier
@param[out]	page_no		page number
@param[out]	body		start of log record body
@return length of the record, or 0 if the record was not complete */
static
ulint
recv_parse_log_rec(
	mlog_id_t*	type,
	byte*		ptr,
	byte*		end_ptr,
	ulint*		space,
	ulint*		page_no,
	byte**		body)
{
	byte*	new_ptr;

	*body = NULL;

	UNIV_MEM_INVALID(type, sizeof *type);
	UNIV_MEM_INVALID(space, sizeof *space);
	UNIV_MEM_INVALID(page_no, sizeof *page_no);
	UNIV_MEM_INVALID(body, sizeof *body);

	if (ptr == end_ptr) {

		return(0);
	}

	if (*ptr == MLOG_MULTI_REC_END) {

		*type = static_cast<mlog_id_t>(*ptr);

		return(1);
	}

	if (*ptr == MLOG_DUMMY_RECORD) {
		*type = static_cast<mlog_id_t>(*ptr);
		ut_d(*space = ULINT_UNDEFINED - 1);
		UNIV_MEM_INVALID(space, sizeof *space);
		return(1);
	}

	new_ptr = mlog_parse_initial_log_record(ptr, end_ptr, type, space,
						page_no);
	*body = new_ptr;

	if (UNIV_UNLIKELY(!new_ptr)) {

		return(0);
	}

#ifdef UNIV_LOG_LSN_DEBUG
	if (*type == MLOG_LSN) {
		lsn_t	lsn = (lsn_t) *space << 32 | *page_no;
		ut_a(lsn == recv_sys->recovered_lsn);
	}
#endif /* UNIV_LOG_LSN_DEBUG */

	new_ptr = recv_parse_or_apply_log_rec_body(
		*type, new_ptr, end_ptr, NULL, NULL, *space, *page_no);

	if (UNIV_UNLIKELY(new_ptr == NULL)) {

		return(0);
	}

	if (*page_no > recv_max_parsed_page_no) {
		recv_max_parsed_page_no = *page_no;
	}

	return(new_ptr - ptr);
}

/*******************************************************//**
Calculates the new value for lsn when more data is added to the log. */
static
lsn_t
recv_calc_lsn_on_data_add(
/*======================*/
	lsn_t		lsn,	/*!< in: old lsn */
	ib_uint64_t	len)	/*!< in: this many bytes of data is
				added, log block headers not included */
{
	ulint		frag_len;
	ib_uint64_t	lsn_len;

	frag_len = (lsn % OS_FILE_LOG_BLOCK_SIZE) - LOG_BLOCK_HDR_SIZE;
	ut_ad(frag_len < OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE
	      - LOG_BLOCK_TRL_SIZE);
	lsn_len = len;
	lsn_len += (lsn_len + frag_len)
		/ (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE
		   - LOG_BLOCK_TRL_SIZE)
		* (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);

	return(lsn + lsn_len);
}

/*******************************************************//**
Prints diagnostic info of corrupt log. */
static
void
recv_report_corrupt_log(
/*====================*/
	byte*	ptr,	/*!< in: pointer to corrupt log record */
	byte	type,	/*!< in: type of the record */
	ulint	space,	/*!< in: space id, this may also be garbage */
	ulint	page_no)/*!< in: page number, this may also be garbage */
{
	ib_logf(IB_LOG_LEVEL_INFO,
		"############### CORRUPT LOG RECORD FOUND ##################");
	ib_logf(IB_LOG_LEVEL_INFO,
		"Log record type %d, page " ULINTPF ":" ULINTPF "."
		" Log parsing proceeded successfully up to " LSN_PF "."
		" Previous log record type %d, is multi %lu"
		" Recv offset %lu, prev %lu",
		type, space, page_no,
		recv_sys->recovered_lsn,
		recv_previous_parsed_rec_type,
		(ulong) recv_previous_parsed_rec_is_multi,
		(ulong) (ptr - recv_sys->buf),
		(ulong) recv_previous_parsed_rec_offset);

	if ((ulint)(ptr - recv_sys->buf + 100)
	    > recv_previous_parsed_rec_offset
	    && (ulint)(ptr - recv_sys->buf + 100
		       - recv_previous_parsed_rec_offset)
	    < 200000) {
		fputs("InnoDB: Hex dump of corrupt log starting"
		      " 100 bytes before the start\n"
		      "InnoDB: of the previous log rec,\n"
		      "InnoDB: and ending 100 bytes after the start"
		      " of the corrupt rec:\n",
		      stderr);

		ut_print_buf(stderr,
			     recv_sys->buf
			     + recv_previous_parsed_rec_offset - 100,
			     ptr - recv_sys->buf + 200
			     - recv_previous_parsed_rec_offset);
		putc('\n', stderr);
	}

#ifndef UNIV_HOTBACKUP
	if (!srv_force_recovery) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Set innodb_force_recovery to ignore this error.");
	}
#endif /* !UNIV_HOTBACKUP */

	ib_logf(IB_LOG_LEVEL_WARN,
		"The log file may have been corrupt and it is possible"
		" that the log scan did not proceed far enough in recovery!"
		" Please run CHECK TABLE on your InnoDB tables to check"
		" that they are ok! If mysqld crashes after this recovery; %s",
		FORCE_RECOVERY_MSG);

	fflush(stderr);
}

/*******************************************************//**
Parses log records from a buffer and stores them to a hash table to wait
merging to file pages.
@return currently always returns FALSE */
static
ibool
recv_parse_log_recs(void)
{
	byte*		ptr;
	byte*		end_ptr;
	ulint		single_rec;
	ulint		len;
	ulint		total_len;
	lsn_t		new_recovered_lsn;
	lsn_t		old_lsn;
	mlog_id_t	type;
	ulint		space;
	ulint		page_no;
	byte*		body;
	ulint		n_recs;

	ut_ad(mutex_own(&(log_sys->mutex)));
	ut_ad(recv_sys->parse_start_lsn != 0);
loop:
	ptr = recv_sys->buf + recv_sys->recovered_offset;

	end_ptr = recv_sys->buf + recv_sys->len;

	if (ptr == end_ptr) {

		return(FALSE);
	}

	single_rec = (ulint)*ptr & MLOG_SINGLE_REC_FLAG;

	if (single_rec || *ptr == MLOG_DUMMY_RECORD) {
		/* The mtr only modified a single page, or this is a file op */

		old_lsn = recv_sys->recovered_lsn;

		/* Try to parse a log record, fetching its type, space id,
		page no, and a pointer to the body of the log record */

		len = recv_parse_log_rec(&type, ptr, end_ptr, &space,
					 &page_no, &body);

		if (len == 0 || recv_sys->found_corrupt_log) {
			if (recv_sys->found_corrupt_log) {

				recv_report_corrupt_log(
					ptr, type, space, page_no);
			}

			return(FALSE);
		}

		new_recovered_lsn = recv_calc_lsn_on_data_add(old_lsn, len);

		if (new_recovered_lsn > recv_sys->scanned_lsn) {
			/* The log record filled a log block, and we require
			that also the next log block should have been scanned
			in */

			return(FALSE);
		}

		recv_previous_parsed_rec_type = type;
		recv_previous_parsed_rec_offset = recv_sys->recovered_offset;
		recv_previous_parsed_rec_is_multi = 0;

		recv_sys->recovered_offset += len;
		recv_sys->recovered_lsn = new_recovered_lsn;

		DBUG_PRINT("ib_log",
			   ("scan " LSN_PF ": log rec %d"
			    " len " ULINTPF
			    " page " ULINTPF ":" ULINTPF,
			    old_lsn, type, len, space, page_no));

		switch (type) {
		case MLOG_DUMMY_RECORD:
			/* Do nothing */
			break;
		case MLOG_FILE_CREATE:
		case MLOG_FILE_CREATE2:
		case MLOG_FILE_RENAME:
		case MLOG_FILE_DELETE:
			/* These were already handled by
			recv_parse_log_rec() and
			recv_parse_or_apply_log_rec_body(). */
			break;
#ifdef UNIV_LOG_LSN_DEBUG
		case MLOG_LSN:
			/* Do not add these records to the hash table.
			The page number and space id fields are misused
			for something else. */
			break;
#endif /* UNIV_LOG_LSN_DEBUG */
		default:
			recv_add_to_hash_table(type, space, page_no, body,
					       ptr + len, old_lsn,
					       recv_sys->recovered_lsn);
		}
	} else {
		/* Check that all the records associated with the single mtr
		are included within the buffer */

		total_len = 0;
		n_recs = 0;

		for (;;) {
			len = recv_parse_log_rec(&type, ptr, end_ptr, &space,
						 &page_no, &body);
			if (len == 0 || recv_sys->found_corrupt_log) {

				if (recv_sys->found_corrupt_log) {

					recv_report_corrupt_log(
						ptr, type, space, page_no);
				}

				return(FALSE);
			}

			recv_previous_parsed_rec_type = type;
			recv_previous_parsed_rec_offset
				= recv_sys->recovered_offset + total_len;
			recv_previous_parsed_rec_is_multi = 1;

			DBUG_PRINT("ib_log",
				   ("scan " LSN_PF ": multi-log rec %d"
				    " len " ULINTPF " page " ULINTPF ":" ULINTPF,
				    recv_sys->recovered_lsn,
				    type, len, space, page_no));

			total_len += len;
			n_recs++;

			ptr += len;

			if (type == MLOG_MULTI_REC_END) {

				/* Found the end mark for the records */

				break;
			}
		}

		new_recovered_lsn = recv_calc_lsn_on_data_add(
			recv_sys->recovered_lsn, total_len);

		if (new_recovered_lsn > recv_sys->scanned_lsn) {
			/* The log record filled a log block, and we require
			that also the next log block should have been scanned
			in */

			return(FALSE);
		}

		/* Add all the records to the hash table */

		ptr = recv_sys->buf + recv_sys->recovered_offset;

		for (;;) {
			old_lsn = recv_sys->recovered_lsn;
			len = recv_parse_log_rec(
				&type, ptr, end_ptr, &space, &page_no, &body);

			if (recv_sys->found_corrupt_log) {

				recv_report_corrupt_log(ptr,
							type, space, page_no);
			}

			ut_a(len != 0);
			ut_a(0 == ((ulint)*ptr & MLOG_SINGLE_REC_FLAG));

			recv_sys->recovered_offset += len;
			recv_sys->recovered_lsn
				= recv_calc_lsn_on_data_add(old_lsn, len);

			switch (type) {
			case MLOG_MULTI_REC_END:
				/* Found the end mark for the records */
				goto loop;
#ifdef UNIV_LOG_LSN_DEBUG
			case MLOG_LSN:
				/* Do not add these records to the hash table.
				The page number and space id fields are misused
				for something else. */
				break;
#endif /* UNIV_LOG_LSN_DEBUG */
			case MLOG_FILE_CREATE:
			case MLOG_FILE_CREATE2:
			case MLOG_FILE_RENAME:
			case MLOG_FILE_DELETE:
				/* These were already handled by
				recv_parse_log_rec() and
				recv_parse_or_apply_log_rec_body(). */
				break;
			default:
				recv_add_to_hash_table(type, space, page_no,
						       body, ptr + len,
						       old_lsn,
						       new_recovered_lsn);
			}

			ptr += len;
		}
	}

	goto loop;
}

/*******************************************************//**
Adds data from a new log block to the parsing buffer of recv_sys if
recv_sys->parse_start_lsn is non-zero.
@return TRUE if more data added */
static
ibool
recv_sys_add_to_parsing_buf(
/*========================*/
	const byte*	log_block,	/*!< in: log block */
	lsn_t		scanned_lsn)	/*!< in: lsn of how far we were able
					to find data in this log block */
{
	ulint	more_len;
	ulint	data_len;
	ulint	start_offset;
	ulint	end_offset;

	ut_ad(scanned_lsn >= recv_sys->scanned_lsn);

	if (!recv_sys->parse_start_lsn) {
		/* Cannot start parsing yet because no start point for
		it found */

		return(FALSE);
	}

	data_len = log_block_get_data_len(log_block);

	if (recv_sys->parse_start_lsn >= scanned_lsn) {

		return(FALSE);

	} else if (recv_sys->scanned_lsn >= scanned_lsn) {

		return(FALSE);

	} else if (recv_sys->parse_start_lsn > recv_sys->scanned_lsn) {
		more_len = (ulint) (scanned_lsn - recv_sys->parse_start_lsn);
	} else {
		more_len = (ulint) (scanned_lsn - recv_sys->scanned_lsn);
	}

	if (more_len == 0) {

		return(FALSE);
	}

	ut_ad(data_len >= more_len);

	start_offset = data_len - more_len;

	if (start_offset < LOG_BLOCK_HDR_SIZE) {
		start_offset = LOG_BLOCK_HDR_SIZE;
	}

	end_offset = data_len;

	if (end_offset > OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
		end_offset = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE;
	}

	ut_ad(start_offset <= end_offset);

	if (start_offset < end_offset) {
		ut_memcpy(recv_sys->buf + recv_sys->len,
			  log_block + start_offset, end_offset - start_offset);

		recv_sys->len += end_offset - start_offset;

		ut_a(recv_sys->len <= RECV_PARSING_BUF_SIZE);
	}

	return(TRUE);
}

/*******************************************************//**
Moves the parsing buffer data left to the buffer start. */
static
void
recv_sys_justify_left_parsing_buf(void)
/*===================================*/
{
	ut_memmove(recv_sys->buf, recv_sys->buf + recv_sys->recovered_offset,
		   recv_sys->len - recv_sys->recovered_offset);

	recv_sys->len -= recv_sys->recovered_offset;

	recv_sys->recovered_offset = 0;
}

/*******************************************************//**
Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.  Unless
UNIV_HOTBACKUP is defined, this function will apply log records
automatically when the hash table becomes full.
@return TRUE if not able to scan any more in this log group */
static
ibool
recv_scan_log_recs(
/*===============*/
	ulint		available_memory,/*!< in: we let the hash table of recs
					to grow to this size, at the maximum */
	const byte*	buf,		/*!< in: buffer containing a log
					segment or garbage */
	ulint		len,		/*!< in: buffer length */
	lsn_t		start_lsn,	/*!< in: buffer start lsn */
	lsn_t*		contiguous_lsn,	/*!< in/out: it is known that all log
					groups contain contiguous log data up
					to this lsn */
	lsn_t*		group_scanned_lsn)/*!< out: scanning succeeded up to
					this lsn */
{
	const byte*	log_block;
	ulint		no;
	lsn_t		scanned_lsn;
	ibool		finished;
	ulint		data_len;
	ibool		more_data;

	ut_ad(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(len % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_ad(len >= OS_FILE_LOG_BLOCK_SIZE);

	finished = FALSE;

	log_block = buf;
	scanned_lsn = start_lsn;
	more_data = FALSE;

	do {
		no = log_block_get_hdr_no(log_block);
		/*
		fprintf(stderr, "Log block header no %lu\n", no);

		fprintf(stderr, "Scanned lsn no %lu\n",
		log_block_convert_lsn_to_no(scanned_lsn));
		*/
		if (no != log_block_convert_lsn_to_no(scanned_lsn)
		    || !log_block_checksum_is_ok_or_old_format(log_block)) {

			if (no == log_block_convert_lsn_to_no(scanned_lsn)
			    && !log_block_checksum_is_ok_or_old_format(
				    log_block)) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Log block no %lu at"
					" lsn " LSN_PF " has"
					" ok header, but checksum field"
					" contains %lu, should be %lu",
					(ulong) no,
					scanned_lsn,
					(ulong) log_block_get_checksum(
						log_block),
					(ulong) log_block_calc_checksum(
						log_block));
			}

			/* Garbage or an incompletely written log block */

			finished = TRUE;

			break;
		}

		if (log_block_get_flush_bit(log_block)) {
			/* This block was a start of a log flush operation:
			we know that the previous flush operation must have
			been completed for all log groups before this block
			can have been flushed to any of the groups. Therefore,
			we know that log data is contiguous up to scanned_lsn
			in all non-corrupt log groups. */

			if (scanned_lsn > *contiguous_lsn) {
				*contiguous_lsn = scanned_lsn;
			}
		}

		data_len = log_block_get_data_len(log_block);

		if (scanned_lsn + data_len > recv_sys->scanned_lsn
		    && (recv_sys->scanned_checkpoint_no > 0)
		    && (log_block_get_checkpoint_no(log_block)
			< recv_sys->scanned_checkpoint_no)
		    && (recv_sys->scanned_checkpoint_no
			- log_block_get_checkpoint_no(log_block)
			> 0x80000000UL)) {

			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */

			finished = TRUE;
			break;
		}

		if (!recv_sys->parse_start_lsn
		    && (log_block_get_first_rec_group(log_block) > 0)) {

			/* We found a point from which to start the parsing
			of log records */

			recv_sys->parse_start_lsn = scanned_lsn
				+ log_block_get_first_rec_group(log_block);
			recv_sys->scanned_lsn = recv_sys->parse_start_lsn;
			recv_sys->recovered_lsn = recv_sys->parse_start_lsn;
		}

		scanned_lsn += data_len;

		if (scanned_lsn > recv_sys->scanned_lsn) {

			/* We have found more entries. If this scan is
			of startup type, we must initiate crash recovery
			environment before parsing these log records. */

#ifndef UNIV_HOTBACKUP
			if (!recv_needed_recovery) {

				if (!srv_read_only_mode) {
					ib_logf(IB_LOG_LEVEL_INFO,
						"Log scan progressed past the"
						" checkpoint lsn " LSN_PF "",
						recv_sys->scanned_lsn);

					recv_init_crash_recovery();
				} else {

					ib_logf(IB_LOG_LEVEL_WARN,
						"Recovery skipped,"
						" --innodb-read-only set!");

					return(TRUE);
				}
			}
#endif /* !UNIV_HOTBACKUP */

			/* We were able to find more log data: add it to the
			parsing buffer if parse_start_lsn is already
			non-zero */

			if (recv_sys->len + 4 * OS_FILE_LOG_BLOCK_SIZE
			    >= RECV_PARSING_BUF_SIZE) {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Log parsing buffer overflow."
					" Recovery may have failed!");

				recv_sys->found_corrupt_log = TRUE;

#ifndef UNIV_HOTBACKUP
				if (!srv_force_recovery) {
					ib_logf(IB_LOG_LEVEL_FATAL,
						"Set innodb_force_recovery"
						" to ignore this error.");
				}
#endif /* !UNIV_HOTBACKUP */

			} else if (!recv_sys->found_corrupt_log) {
				more_data = recv_sys_add_to_parsing_buf(
					log_block, scanned_lsn);
			}

			recv_sys->scanned_lsn = scanned_lsn;
			recv_sys->scanned_checkpoint_no
				= log_block_get_checkpoint_no(log_block);
		}

		if (data_len < OS_FILE_LOG_BLOCK_SIZE) {
			/* Log data for this group ends here */

			finished = TRUE;
			break;
		} else {
			log_block += OS_FILE_LOG_BLOCK_SIZE;
		}
	} while (log_block < buf + len && !finished);

	*group_scanned_lsn = scanned_lsn;

	if (recv_needed_recovery
	    || (recv_is_from_backup && !recv_is_making_a_backup)) {
		recv_scan_print_counter++;

		if (finished || (recv_scan_print_counter % 80 == 0)) {

			ib_logf(IB_LOG_LEVEL_INFO,
				"Doing recovery: scanned up to"
				" log sequence number " LSN_PF "",
				*group_scanned_lsn);
		}
	}

	if (more_data && !recv_sys->found_corrupt_log) {
		/* Try to parse more log records */

		recv_parse_log_recs();

#ifndef UNIV_HOTBACKUP
		if (mem_heap_get_size(recv_sys->heap) > available_memory) {

			/* Hash table of log records has grown too big:
			empty it; FALSE means no ibuf operations
			allowed, as we cannot add new records to the
			log yet: they would be produced by ibuf
			operations */

			recv_apply_hashed_log_recs(FALSE);
		}
#endif /* !UNIV_HOTBACKUP */

		if (recv_sys->recovered_offset > RECV_PARSING_BUF_SIZE / 4) {
			/* Move parsing buffer data to the buffer start */

			recv_sys_justify_left_parsing_buf();
		}
	}

	return(finished);
}

#ifndef UNIV_HOTBACKUP
/*******************************************************//**
Scans log from a buffer and stores new log data to the parsing buffer. Parses
and hashes the log records if new data found. */
static
void
recv_group_scan_log_recs(
/*=====================*/
	log_group_t*	group,		/*!< in: log group */
	lsn_t*		contiguous_lsn,	/*!< in/out: it is known that all log
					groups contain contiguous log data up
					to this lsn */
	lsn_t*		group_scanned_lsn)/*!< out: scanning succeeded up to
					this lsn */
{
	ibool	finished;
	lsn_t	start_lsn;
	lsn_t	end_lsn;

	finished = FALSE;

	start_lsn = *contiguous_lsn;

	while (!finished) {
		end_lsn = start_lsn + RECV_SCAN_SIZE;

		log_group_read_log_seg(
			log_sys->buf, group, start_lsn, end_lsn);

		finished = recv_scan_log_recs(
			(buf_pool_get_n_pages()
			- (recv_n_pool_free_frames * srv_buf_pool_instances))
			* UNIV_PAGE_SIZE,
			log_sys->buf, RECV_SCAN_SIZE,
			start_lsn, contiguous_lsn, group_scanned_lsn);
		start_lsn = end_lsn;
	}

	DBUG_PRINT("ib_log", ("scan " LSN_PF " completed for log group %u",
			      *group_scanned_lsn, unsigned(group->id)));
}

/*******************************************************//**
Initialize crash recovery environment. Can be called iff
recv_needed_recovery == FALSE. */
static
void
recv_init_crash_recovery(void)
/*==========================*/
{
	ut_ad(!srv_read_only_mode);
	ut_a(!recv_needed_recovery);

	recv_needed_recovery = TRUE;

	ib_logf(IB_LOG_LEVEL_INFO, "Database was not shutdown normally!");
	ib_logf(IB_LOG_LEVEL_INFO, "Starting crash recovery.");
	ib_logf(IB_LOG_LEVEL_INFO,
		"Reading tablespace information from the .ibd files...");

	fil_load_single_table_tablespaces();

	/* If we are using the doublewrite method, we will
	check if there are half-written pages in data files,
	and restore them from the doublewrite buffer if
	possible */

	if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) {

		ib_logf(IB_LOG_LEVEL_INFO,
			"Restoring possible half-written data pages ");

		ib_logf(IB_LOG_LEVEL_INFO,
			"from the doublewrite buffer...");

		buf_dblwr_process();

		/* Spawn the background thread to flush dirty pages
		from the buffer pools. */
		os_thread_create(recv_writer_thread, 0, 0);
	}
}

/********************************************************//**
Recovers from a checkpoint. When this function returns, the database is able
to start processing of new user transactions, but the function
recv_recovery_from_checkpoint_finish should be called later to complete
the recovery and free the resources used in it.
@return error code or DB_SUCCESS */

dberr_t
recv_recovery_from_checkpoint_start(
/*================================*/
	lsn_t	min_flushed_lsn,/*!< in: min flushed lsn from data files */
	lsn_t	max_flushed_lsn)/*!< in: max flushed lsn from data files */
{
	log_group_t*	group;
	log_group_t*	max_cp_group;
	ulint		max_cp_field;
	lsn_t		checkpoint_lsn;
	ib_uint64_t	checkpoint_no;
	lsn_t		group_scanned_lsn = 0;
	lsn_t		contiguous_lsn;
	byte*		buf;
	byte		log_hdr_buf[LOG_FILE_HDR_SIZE];
	dberr_t		err;
	ut_when_dtor<recv_dblwr_t> tmp(recv_sys->dblwr);

	if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO) {

		ib_logf(IB_LOG_LEVEL_INFO,
			"The user has set SRV_FORCE_NO_LOG_REDO on,"
			" skipping log redo");

		return(DB_SUCCESS);
	}

	recv_recovery_on = TRUE;

	mutex_enter(&(log_sys->mutex));

	/* Look for the latest checkpoint from any of the log groups */

	err = recv_find_max_checkpoint(&max_cp_group, &max_cp_field);

	if (err != DB_SUCCESS) {

		mutex_exit(&(log_sys->mutex));

		return(err);
	}

	log_group_read_checkpoint_info(max_cp_group, max_cp_field);

	buf = log_sys->checkpoint_buf;

	checkpoint_lsn = mach_read_from_8(buf + LOG_CHECKPOINT_LSN);
	checkpoint_no = mach_read_from_8(buf + LOG_CHECKPOINT_NO);

	/* Read the first log file header to print a note if this is
	a recovery from a restored InnoDB Hot Backup */

	const page_id_t	page_id(max_cp_group->space_id, 0);

	fil_io(OS_FILE_READ | OS_FILE_LOG, true, page_id, univ_page_size, 0,
	       LOG_FILE_HDR_SIZE, log_hdr_buf, max_cp_group);

	if (0 == ut_memcmp(log_hdr_buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP,
			   (byte*)"ibbackup", (sizeof "ibbackup") - 1)) {

		if (srv_read_only_mode) {

			ib_logf(IB_LOG_LEVEL_ERROR,
				"Cannot restore from ibbackup, InnoDB running"
				" in read-only mode!");

			return(DB_ERROR);
		}

		/* This log file was created by ibbackup --restore: print
		a note to the user about it */

		ib_logf(IB_LOG_LEVEL_INFO,
			"The log file was created by ibbackup --apply-log"
			" at %s. The following crash recovery is part of a"
			" normal restore.",
			log_hdr_buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP);

		/* Wipe over the label now */

		memset(log_hdr_buf + LOG_FILE_WAS_CREATED_BY_HOT_BACKUP,
		       ' ', 4);
		/* Write to the log file to wipe over the label */
		fil_io(OS_FILE_WRITE | OS_FILE_LOG, true, page_id,
		       univ_page_size, 0, OS_FILE_LOG_BLOCK_SIZE, log_hdr_buf,
		       max_cp_group);
	}

	/* Start reading the log groups from the checkpoint lsn up. The
	variable contiguous_lsn contains an lsn up to which the log is
	known to be contiguously written to all log groups. */

	recv_sys->parse_start_lsn = checkpoint_lsn;
	recv_sys->scanned_lsn = checkpoint_lsn;
	recv_sys->scanned_checkpoint_no = 0;
	recv_sys->recovered_lsn = checkpoint_lsn;

	srv_start_lsn = checkpoint_lsn;

	contiguous_lsn = ut_uint64_align_down(recv_sys->scanned_lsn,
					      OS_FILE_LOG_BLOCK_SIZE);
	ut_ad(RECV_SCAN_SIZE <= log_sys->buf_size);

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	while (group) {
		recv_group_scan_log_recs(group, &contiguous_lsn,
					 &group_scanned_lsn);
		group->scanned_lsn = group_scanned_lsn;

		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	/* NOTE: we always do a 'recovery' at startup, but only if
	there is something wrong we will print a message to the
	user about recovery: */

	if (checkpoint_lsn != max_flushed_lsn
	    || checkpoint_lsn != min_flushed_lsn) {

		if (checkpoint_lsn < max_flushed_lsn) {

			ib_logf(IB_LOG_LEVEL_WARN,
				"The log sequence number in the ibdata files is"
				" higher than the log sequence number in the"
				" ib_logfiles! Are you sure you are using the"
				" right ib_logfiles to start up the database."
				" Log sequence number in the ib_logfiles is"
				LSN_PF ", log sequence numbers stamped to"
				" ibdata file headers are between"
				" " LSN_PF " and " LSN_PF ".",
				checkpoint_lsn,
				min_flushed_lsn,
				max_flushed_lsn);
		}

		if (!recv_needed_recovery) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"The log sequence numbers " LSN_PF " and"
				" " LSN_PF " in ibdata files do not match"
				" the log sequence number " LSN_PF
				" in the ib_logfiles!",
				min_flushed_lsn,
				max_flushed_lsn,
				checkpoint_lsn);

			if (!srv_read_only_mode) {
				recv_init_crash_recovery();
			} else {
				ib_logf(IB_LOG_LEVEL_ERROR,
					"Can't initiate database recovery,"
					" running in read-only-mode.");
				return(DB_READ_ONLY);
			}
		}
	}

	/* We currently have only one log group */
	if (group_scanned_lsn < checkpoint_lsn
	    || group_scanned_lsn < recv_max_page_lsn) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"We scanned the log up to " LSN_PF ". A checkpoint"
			" was at " LSN_PF " and the maximum LSN on a database"
			" page was " LSN_PF ". It is possible that the"
			" database is now corrupt!",
			group_scanned_lsn, checkpoint_lsn, recv_max_page_lsn);
	}

	if (recv_sys->recovered_lsn < checkpoint_lsn) {

		mutex_exit(&(log_sys->mutex));

		if (recv_sys->recovered_lsn >= LSN_MAX) {

			return(DB_SUCCESS);
		}

		/* No harm in trying to do RO access. */
		if (!srv_read_only_mode) {
			ut_error;
		}

		return(DB_ERROR);
	}

	/* Synchronize the uncorrupted log groups to the most up-to-date log
	group; we also copy checkpoint info to groups */

	log_sys->next_checkpoint_lsn = checkpoint_lsn;
	log_sys->next_checkpoint_no = checkpoint_no + 1;

	recv_synchronize_groups();

	if (!recv_needed_recovery) {
		ut_a(checkpoint_lsn == recv_sys->recovered_lsn);
	} else {
		srv_start_lsn = recv_sys->recovered_lsn;
	}

	log_sys->lsn = recv_sys->recovered_lsn;

	ut_memcpy(log_sys->buf, recv_sys->last_block, OS_FILE_LOG_BLOCK_SIZE);

	log_sys->buf_free = (ulint) log_sys->lsn % OS_FILE_LOG_BLOCK_SIZE;
	log_sys->buf_next_to_write = log_sys->buf_free;
	log_sys->write_lsn = log_sys->lsn;

	log_sys->last_checkpoint_lsn = checkpoint_lsn;

	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    log_sys->lsn - log_sys->last_checkpoint_lsn);

	log_sys->next_checkpoint_no = checkpoint_no + 1;

	mutex_enter(&recv_sys->mutex);

	recv_sys->apply_log_recs = TRUE;

	mutex_exit(&recv_sys->mutex);

	mutex_exit(&log_sys->mutex);

	recv_lsn_checks_on = TRUE;

	/* The database is now ready to start almost normal processing of user
	transactions: transaction rollbacks and the application of the log
	records in the hash table can be run in background. */

	return(DB_SUCCESS);
}

/********************************************************//**
Completes recovery from a checkpoint. */

void
recv_recovery_from_checkpoint_finish(void)
/*======================================*/
{
	/* Apply the hashed log records to the respective file pages */

	if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) {

		recv_apply_hashed_log_recs(TRUE);
	}

	DBUG_PRINT("ib_log", ("apply completed"));

	if (recv_needed_recovery) {
		trx_sys_print_mysql_binlog_offset();
	}

	if (recv_sys->found_corrupt_log) {

		ib_logf(IB_LOG_LEVEL_WARN,
			"The log file may have been corrupt and it"
			" is possible that the log scan or parsing"
			" did not proceed far enough in recovery."
			" Please run CHECK TABLE on your InnoDB tables"
			" to check that they are ok!"
			" It may be safest to recover your"
			" InnoDB database from a backup!");
	}

	/* Make sure that the recv_writer thread is done. This is
	required because it grabs various mutexes and we want to
	ensure that when we enable sync_order_checks there is no
	mutex currently held by any thread. */
	mutex_enter(&recv_sys->writer_mutex);

	/* Free the resources of the recovery system */
	recv_recovery_on = FALSE;

	/* By acquring the mutex we ensure that the recv_writer thread
	won't trigger any more LRU batchtes. Now wait for currently
	in progress batches to finish. */
	buf_flush_wait_LRU_batch_end();

	mutex_exit(&recv_sys->writer_mutex);

	ulint count = 0;
	while (recv_writer_thread_active) {
		++count;
		os_thread_sleep(100000);
		if (srv_print_verbose_log && count > 600) {
			ib_logf(IB_LOG_LEVEL_INFO,
				"Waiting for recv_writer to"
				" finish flushing of buffer pool");
			count = 0;
		}
	}

	recv_sys_debug_free();

	/* Roll back any recovered data dictionary transactions, so
	that the data dictionary tables will be free of any locks.
	The data dictionary latch should guarantee that there is at
	most one data dictionary transaction active at a time. */
	if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO) {
		trx_rollback_or_clean_recovered(FALSE);
	}
}

/********************************************************//**
Initiates the rollback of active transactions. */

void
recv_recovery_rollback_active(void)
/*===============================*/
{
	ut_ad(!recv_writer_thread_active);

	/* We can't start any (DDL) transactions if UNDO logging
	has been disabled, additionally disable ROLLBACK of recovered
	user transactions. */
	if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    && !srv_read_only_mode) {

		/* Drop partially created indexes. */
		row_merge_drop_temp_indexes();
		/* Drop temporary tables. */
		row_mysql_drop_temp_tables();

		/* Drop any auxiliary tables that were not dropped when the
		parent table was dropped. This can happen if the parent table
		was dropped but the server crashed before the auxiliary tables
		were dropped. */
		fts_drop_orphaned_tables();

		/* Rollback the uncommitted transactions which have no user
		session */

		os_thread_create(trx_rollback_or_clean_all_recovered, 0, 0);
	}
}

/******************************************************//**
Resets the logs. The contents of log files will be lost! */

void
recv_reset_logs(
/*============*/
	lsn_t		lsn)		/*!< in: reset to this lsn
					rounded up to be divisible by
					OS_FILE_LOG_BLOCK_SIZE, after
					which we add
					LOG_BLOCK_HDR_SIZE */
{
	log_group_t*	group;

	ut_ad(mutex_own(&(log_sys->mutex)));

	log_sys->lsn = ut_uint64_align_up(lsn, OS_FILE_LOG_BLOCK_SIZE);

	group = UT_LIST_GET_FIRST(log_sys->log_groups);

	while (group) {
		group->lsn = log_sys->lsn;
		group->lsn_offset = LOG_FILE_HDR_SIZE;
		group = UT_LIST_GET_NEXT(log_groups, group);
	}

	log_sys->buf_next_to_write = 0;
	log_sys->write_lsn = log_sys->lsn;

	log_sys->next_checkpoint_no = 0;
	log_sys->last_checkpoint_lsn = 0;

	log_block_init(log_sys->buf, log_sys->lsn);
	log_block_set_first_rec_group(log_sys->buf, LOG_BLOCK_HDR_SIZE);

	log_sys->buf_free = LOG_BLOCK_HDR_SIZE;
	log_sys->lsn += LOG_BLOCK_HDR_SIZE;

	MONITOR_SET(MONITOR_LSN_CHECKPOINT_AGE,
		    (log_sys->lsn - log_sys->last_checkpoint_lsn));

	mutex_exit(&(log_sys->mutex));

	/* Reset the checkpoint fields in logs */

	log_make_checkpoint_at(LSN_MAX, TRUE);

	mutex_enter(&(log_sys->mutex));
}
#endif /* !UNIV_HOTBACKUP */

#ifdef UNIV_HOTBACKUP
/******************************************************//**
Creates new log files after a backup has been restored. */

void
recv_reset_log_files_for_backup(
/*============================*/
	const char*	log_dir,	/*!< in: log file directory path */
	ulint		n_log_files,	/*!< in: number of log files */
	lsn_t		log_file_size,	/*!< in: log file size */
	lsn_t		lsn)		/*!< in: new start lsn, must be
					divisible by OS_FILE_LOG_BLOCK_SIZE */
{
	os_file_t	log_file;
	bool		success;
	byte*		buf;
	ulint		i;
	ulint		log_dir_len;
	char		name[5000];
	static const char ib_logfile_basename[] = "ib_logfile";

	log_dir_len = strlen(log_dir);
	/* full path name of ib_logfile consists of log dir path + basename
	+ number. This must fit in the name buffer.
	*/
	ut_a(log_dir_len + strlen(ib_logfile_basename) + 11  < sizeof(name));

	buf = ut_malloc(LOG_FILE_HDR_SIZE + OS_FILE_LOG_BLOCK_SIZE);
	memset(buf, '\0', LOG_FILE_HDR_SIZE + OS_FILE_LOG_BLOCK_SIZE);

	for (i = 0; i < n_log_files; i++) {

		sprintf(name, "%s%s%lu", log_dir,
			ib_logfile_basename, (ulong) i);

		log_file = os_file_create_simple(innodb_log_file_key,
						 name, OS_FILE_CREATE,
						 OS_FILE_READ_WRITE,
						 &success);
		if (!success) {
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Cannot create %s. Check that"
				" the file does not exist yet.", name);
		}

		ib_logf(IB_LOG_LEVEL_INFO,
			"Setting log file size to %llu",
			log_file_size);

		success = os_file_set_size(name, log_file, log_file_size);

		if (!success) {
			ib_logf(IB_LOG_LEVEL_FATAL,
				"Cannot set %s size to %llu",
				name, log_file_size);
		}

		os_file_flush(log_file);
		os_file_close(log_file);
	}

	/* We pretend there is a checkpoint at lsn + LOG_BLOCK_HDR_SIZE */

	log_reset_first_header_and_checkpoint(buf, lsn);

	log_block_init_in_old_format(buf + LOG_FILE_HDR_SIZE, lsn);
	log_block_set_first_rec_group(buf + LOG_FILE_HDR_SIZE,
				      LOG_BLOCK_HDR_SIZE);
	sprintf(name, "%s%s%lu", log_dir, ib_logfile_basename, (ulong)0);

	log_file = os_file_create_simple(innodb_log_file_key,
					 name, OS_FILE_OPEN,
					 OS_FILE_READ_WRITE, &success);
	if (!success) {
		ib_logf(IB_LOG_LEVEL_FATAL, "Cannot open %s.", name);
	}

	os_file_write(name, log_file, buf, 0,
		      LOG_FILE_HDR_SIZE + OS_FILE_LOG_BLOCK_SIZE);
	os_file_flush(log_file);
	os_file_close(log_file);

	ut_free(buf);
}
#endif /* UNIV_HOTBACKUP */

/** Find a doublewrite copy of a page.
@param[in]	space_id	tablespace identifier
@param[in]	page_no		page number
@return	page frame
@retval NULL if no page was found */

const byte*
recv_dblwr_t::find_page(ulint space_id, ulint page_no)
{
	std::vector<const byte*> matches;
	const byte*	result = 0;

	for (list::iterator i = pages.begin(); i != pages.end(); ++i) {
		if (page_get_space_id(*i) == space_id
		    && page_get_page_no(*i) == page_no) {
			matches.push_back(*i);
		}
	}

	if (matches.size() == 1) {
		result = matches[0];
	} else if (matches.size() > 1) {

		lsn_t max_lsn	= 0;
		lsn_t page_lsn	= 0;

		for (std::vector<const byte*>::iterator i = matches.begin();
		     i != matches.end(); ++i) {

			page_lsn = mach_read_from_8(*i + FIL_PAGE_LSN);

			if (page_lsn > max_lsn) {
				max_lsn = page_lsn;
				result = *i;
			}
		}
	}

	return(result);
}

