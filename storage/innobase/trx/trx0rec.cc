/*****************************************************************************

Copyright (c) 1996, 2014, Oracle and/or its affiliates. All Rights Reserved.

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
@file trx/trx0rec.cc
Transaction undo log record

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#include "trx0rec.h"

#ifdef UNIV_NONINL
#include "trx0rec.ic"
#endif

#include "fsp0fsp.h"
#include "mach0data.h"
#include "trx0undo.h"
#include "mtr0log.h"
#ifndef UNIV_HOTBACKUP
#include "dict0dict.h"
#include "ut0mem.h"
#include "read0read.h"
#include "row0ext.h"
#include "row0upd.h"
#include "que0que.h"
#include "trx0purge.h"
#include "trx0rseg.h"
#include "row0row.h"
#include "fsp0sysspace.h"

/*=========== UNDO LOG RECORD CREATION AND DECODING ====================*/

/**********************************************************************//**
Writes the mtr log entry of the inserted undo log record on the undo log
page. */
UNIV_INLINE
void
trx_undof_page_add_undo_rec_log(
/*============================*/
	page_t* undo_page,	/*!< in: undo log page */
	ulint	old_free,	/*!< in: start offset of the inserted entry */
	ulint	new_free,	/*!< in: end offset of the entry */
	mtr_t*	mtr)		/*!< in: mtr */
{
	byte*		log_ptr;
	const byte*	log_end;
	ulint		len;

	log_ptr = mlog_open(mtr, 11 + 13 + MLOG_BUF_MARGIN);

	if (log_ptr == NULL) {

		return;
	}

	log_end = &log_ptr[11 + 13 + MLOG_BUF_MARGIN];
	log_ptr = mlog_write_initial_log_record_fast(
		undo_page, MLOG_UNDO_INSERT, log_ptr, mtr);
	len = new_free - old_free - 4;

	mach_write_to_2(log_ptr, len);
	log_ptr += 2;

	if (log_ptr + len <= log_end) {
		memcpy(log_ptr, undo_page + old_free + 2, len);
		mlog_close(mtr, log_ptr + len);
	} else {
		mlog_close(mtr, log_ptr);
		mlog_catenate_string(mtr, undo_page + old_free + 2, len);
	}
}
#endif /* !UNIV_HOTBACKUP */

/***********************************************************//**
Parses a redo log record of adding an undo log record.
@return end of log record or NULL */

byte*
trx_undo_parse_add_undo_rec(
/*========================*/
	byte*	ptr,	/*!< in: buffer */
	byte*	end_ptr,/*!< in: buffer end */
	page_t*	page)	/*!< in: page or NULL */
{
	ulint	len;
	byte*	rec;
	ulint	first_free;

	if (end_ptr < ptr + 2) {

		return(NULL);
	}

	len = mach_read_from_2(ptr);
	ptr += 2;

	if (end_ptr < ptr + len) {

		return(NULL);
	}

	if (page == NULL) {

		return(ptr + len);
	}

	first_free = mach_read_from_2(page + TRX_UNDO_PAGE_HDR
				      + TRX_UNDO_PAGE_FREE);
	rec = page + first_free;

	mach_write_to_2(rec, first_free + 4 + len);
	mach_write_to_2(rec + 2 + len, first_free);

	mach_write_to_2(page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE,
			first_free + 4 + len);
	ut_memcpy(rec + 2, ptr, len);

	return(ptr + len);
}

#ifndef UNIV_HOTBACKUP
/**********************************************************************//**
Calculates the free space left for extending an undo log record.
@return bytes left */
UNIV_INLINE
ulint
trx_undo_left(
/*==========*/
	const page_t*	page,	/*!< in: undo log page */
	const byte*	ptr)	/*!< in: pointer to page */
{
	/* The '- 10' is a safety margin, in case we have some small
	calculation error below */

	return(UNIV_PAGE_SIZE - (ptr - page) - 10 - FIL_PAGE_DATA_END);
}

/**********************************************************************//**
Set the next and previous pointers in the undo page for the undo record
that was written to ptr. Update the first free value by the number of bytes
written for this undo record.
@return offset of the inserted entry on the page if succeeded, 0 if fail */
static
ulint
trx_undo_page_set_next_prev_and_add(
/*================================*/
	page_t*		undo_page,	/*!< in/out: undo log page */
	byte*		ptr,		/*!< in: ptr up to where data has been
					written on this undo page. */
	mtr_t*		mtr)		/*!< in: mtr */
{
	ulint		first_free;	/*!< offset within undo_page */
	ulint		end_of_rec;	/*!< offset within undo_page */
	byte*		ptr_to_first_free;
					/* pointer within undo_page
					that points to the next free
					offset value within undo_page.*/

	ut_ad(ptr > undo_page);
	ut_ad(ptr < undo_page + UNIV_PAGE_SIZE);

	if (UNIV_UNLIKELY(trx_undo_left(undo_page, ptr) < 2)) {

		return(0);
	}

	ptr_to_first_free = undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE;

	first_free = mach_read_from_2(ptr_to_first_free);

	/* Write offset of the previous undo log record */
	mach_write_to_2(ptr, first_free);
	ptr += 2;

	end_of_rec = ptr - undo_page;

	/* Write offset of the next undo log record */
	mach_write_to_2(undo_page + first_free, end_of_rec);

	/* Update the offset to first free undo record */
	mach_write_to_2(ptr_to_first_free, end_of_rec);

	/* Write this log entry to the UNDO log */
	trx_undof_page_add_undo_rec_log(undo_page, first_free,
					end_of_rec, mtr);

	return(first_free);
}

/**********************************************************************//**
Reports in the undo log of an insert of a clustered index record.
@return offset of the inserted entry on the page if succeed, 0 if fail */
static
ulint
trx_undo_page_report_insert(
/*========================*/
	page_t*		undo_page,	/*!< in: undo log page */
	trx_t*		trx,		/*!< in: transaction */
	dict_index_t*	index,		/*!< in: clustered index */
	const dtuple_t*	clust_entry,	/*!< in: index entry which will be
					inserted to the clustered index */
	mtr_t*		mtr)		/*!< in: mtr */
{
	ulint		first_free;
	byte*		ptr;
	ulint		i;

	ut_ad(dict_index_is_clust(index));
	ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
			       + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT);

	first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
				      + TRX_UNDO_PAGE_FREE);
	ptr = undo_page + first_free;

	ut_ad(first_free <= UNIV_PAGE_SIZE);

	if (trx_undo_left(undo_page, ptr) < 2 + 1 + 11 + 11) {

		/* Not enough space for writing the general parameters */

		return(0);
	}

	/* Reserve 2 bytes for the pointer to the next undo log record */
	ptr += 2;

	/* Store first some general parameters to the undo log */
	*ptr++ = TRX_UNDO_INSERT_REC;
	ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);
	ptr += mach_u64_write_much_compressed(ptr, index->table->id);
	/*----------------------------------------*/
	/* Store then the fields required to uniquely determine the record
	to be inserted in the clustered index */

	for (i = 0; i < dict_index_get_n_unique(index); i++) {

		const dfield_t*	field	= dtuple_get_nth_field(clust_entry, i);
		ulint		flen	= dfield_get_len(field);

		if (trx_undo_left(undo_page, ptr) < 5) {

			return(0);
		}

		ptr += mach_write_compressed(ptr, flen);

		if (flen != UNIV_SQL_NULL) {
			if (trx_undo_left(undo_page, ptr) < flen) {

				return(0);
			}

			ut_memcpy(ptr, dfield_get_data(field), flen);
			ptr += flen;
		}
	}

	return(trx_undo_page_set_next_prev_and_add(undo_page, ptr, mtr));
}

/**********************************************************************//**
Reads from an undo log record the general parameters.
@return remaining part of undo log record after reading these values */

byte*
trx_undo_rec_get_pars(
/*==================*/
	trx_undo_rec_t*	undo_rec,	/*!< in: undo log record */
	ulint*		type,		/*!< out: undo record type:
					TRX_UNDO_INSERT_REC, ... */
	ulint*		cmpl_info,	/*!< out: compiler info, relevant only
					for update type records */
	bool*		updated_extern,	/*!< out: true if we updated an
					externally stored fild */
	undo_no_t*	undo_no,	/*!< out: undo log record number */
	table_id_t*	table_id)	/*!< out: table id */
{
	const byte*	ptr;
	ulint		type_cmpl;

	ptr = undo_rec + 2;

	type_cmpl = mach_read_from_1(ptr);
	ptr++;

	*updated_extern = !!(type_cmpl & TRX_UNDO_UPD_EXTERN);
	type_cmpl &= ~TRX_UNDO_UPD_EXTERN;

	*type = type_cmpl & (TRX_UNDO_CMPL_INFO_MULT - 1);
	*cmpl_info = type_cmpl / TRX_UNDO_CMPL_INFO_MULT;

	*undo_no = mach_read_next_much_compressed(&ptr);
	*table_id = mach_read_next_much_compressed(&ptr);

	return(const_cast<byte*>(ptr));
}

/** Read from an undo log record a stored column value.
@param[in,out]	ptr		pointer to remaining part of the undo record
@param[out]	field		stored field
@param[out]	len		length of the field, or UNIV_SQL_NULL
@param[out]	orig_len	original length of the locally stored part
of an externally stored column, or 0
@return remaining part of undo log record after reading these values */
static
byte*
trx_undo_rec_get_col_val(
/*=====================*/
	const byte*	ptr,
	const byte**	field,
	ulint*		len,
	ulint*		orig_len)
{
	*len = mach_read_next_compressed(&ptr);
	*orig_len = 0;

	switch (*len) {
	case UNIV_SQL_NULL:
		*field = NULL;
		break;
	case UNIV_EXTERN_STORAGE_FIELD:
		*orig_len = mach_read_next_compressed(&ptr);
		*len = mach_read_next_compressed(&ptr);
		*field = ptr;
		ptr += *len;

		ut_ad(*orig_len >= BTR_EXTERN_FIELD_REF_SIZE);
		ut_ad(*len > *orig_len);
		/* @see dtuple_convert_big_rec() */
		ut_ad(*len >= BTR_EXTERN_FIELD_REF_SIZE);

		/* we do not have access to index->table here
		ut_ad(dict_table_get_format(index->table) >= UNIV_FORMAT_B
		      || *len >= col->max_prefix
		      + BTR_EXTERN_FIELD_REF_SIZE);
		*/

		*len += UNIV_EXTERN_STORAGE_FIELD;
		break;
	default:
		*field = ptr;
		if (*len >= UNIV_EXTERN_STORAGE_FIELD) {
			ptr += *len - UNIV_EXTERN_STORAGE_FIELD;
		} else {
			ptr += *len;
		}
	}

	return(const_cast<byte*>(ptr));
}

/*******************************************************************//**
Builds a row reference from an undo log record.
@return pointer to remaining part of undo record */

byte*
trx_undo_rec_get_row_ref(
/*=====================*/
	byte*		ptr,	/*!< in: remaining part of a copy of an undo log
				record, at the start of the row reference;
				NOTE that this copy of the undo log record must
				be preserved as long as the row reference is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	dtuple_t**	ref,	/*!< out, own: row reference */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory
				needed is allocated */
{
	ulint		ref_len;
	ulint		i;

	ut_ad(index && ptr && ref && heap);
	ut_a(dict_index_is_clust(index));

	ref_len = dict_index_get_n_unique(index);

	*ref = dtuple_create(heap, ref_len);

	dict_index_copy_types(*ref, index, ref_len);

	for (i = 0; i < ref_len; i++) {
		dfield_t*	dfield;
		const byte*	field;
		ulint		len;
		ulint		orig_len;

		dfield = dtuple_get_nth_field(*ref, i);

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

		dfield_set_data(dfield, field, len);
	}

	return(ptr);
}

/*******************************************************************//**
Skips a row reference from an undo log record.
@return pointer to remaining part of undo record */

byte*
trx_undo_rec_skip_row_ref(
/*======================*/
	byte*		ptr,	/*!< in: remaining part in update undo log
				record, at the start of the row reference */
	dict_index_t*	index)	/*!< in: clustered index */
{
	ulint	ref_len;
	ulint	i;

	ut_ad(index && ptr);
	ut_a(dict_index_is_clust(index));

	ref_len = dict_index_get_n_unique(index);

	for (i = 0; i < ref_len; i++) {
		const byte*	field;
		ulint		len;
		ulint		orig_len;

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);
	}

	return(ptr);
}

/** Fetch a prefix of an externally stored column, for writing to the undo
log of an update or delete marking of a clustered index record.
@param[out]	ext_buf		buffer to hold the prefix data and BLOB pointer
@param[in]	prefix_len	prefix size to store in the undo log
@param[in]	page_size	page size
@param[in]	field		an externally stored column
@param[in,out]	len		input: length of field; output: used length of
ext_buf
@return ext_buf */
static
byte*
trx_undo_page_fetch_ext(
	byte*			ext_buf,
	ulint			prefix_len,
	const page_size_t&	page_size,
	const byte*		field,
	ulint*			len)
{
	/* Fetch the BLOB. */
	ulint	ext_len = btr_copy_externally_stored_field_prefix(
		ext_buf, prefix_len, page_size, field, *len);
	/* BLOBs should always be nonempty. */
	ut_a(ext_len);
	/* Append the BLOB pointer to the prefix. */
	memcpy(ext_buf + ext_len,
	       field + *len - BTR_EXTERN_FIELD_REF_SIZE,
	       BTR_EXTERN_FIELD_REF_SIZE);
	*len = ext_len + BTR_EXTERN_FIELD_REF_SIZE;
	return(ext_buf);
}

/** Writes to the undo log a prefix of an externally stored column.
@param[out]	ptr		undo log position, at least 15 bytes must be
available
@param[out]	ext_buf		a buffer of DICT_MAX_FIELD_LEN_BY_FORMAT()
size, or NULL when should not fetch a longer prefix
@param[in]	prefix_len	prefix size to store in the undo log
@param[in]	page_size	page size
@param[in,out]	field		the locally stored part of the externally
stored column
@param[in,out]	len		length of field, in bytes
@return undo log position */
static
byte*
trx_undo_page_report_modify_ext(
	byte*			ptr,
	byte*			ext_buf,
	ulint			prefix_len,
	const page_size_t&	page_size,
	const byte**		field,
	ulint*			len)
{
	if (ext_buf) {
		ut_a(prefix_len > 0);

		/* If an ordering column is externally stored, we will
		have to store a longer prefix of the field.  In this
		case, write to the log a marker followed by the
		original length and the real length of the field. */
		ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD);

		ptr += mach_write_compressed(ptr, *len);

		*field = trx_undo_page_fetch_ext(ext_buf, prefix_len,
						 page_size, *field, len);

		ptr += mach_write_compressed(ptr, *len);
	} else {
		ptr += mach_write_compressed(ptr, UNIV_EXTERN_STORAGE_FIELD
					     + *len);
	}

	return(ptr);
}

/**********************************************************************//**
Reports in the undo log of an update or delete marking of a clustered index
record.
@return byte offset of the inserted undo log entry on the page if
succeed, 0 if fail */
static
ulint
trx_undo_page_report_modify(
/*========================*/
	page_t*		undo_page,	/*!< in: undo log page */
	trx_t*		trx,		/*!< in: transaction */
	dict_index_t*	index,		/*!< in: clustered index where update or
					delete marking is done */
	const rec_t*	rec,		/*!< in: clustered index record which
					has NOT yet been modified */
	const ulint*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	const upd_t*	update,		/*!< in: update vector which tells the
					columns to be updated; in the case of
					a delete, this should be set to NULL */
	ulint		cmpl_info,	/*!< in: compiler info on secondary
					index updates */
	mtr_t*		mtr)		/*!< in: mtr */
{
	dict_table_t*	table;
	ulint		first_free;
	byte*		ptr;
	const byte*	field;
	ulint		flen;
	ulint		col_no;
	ulint		type_cmpl;
	byte*		type_cmpl_ptr;
	ulint		i;
	trx_id_t	trx_id;
	trx_undo_ptr_t*	undo_ptr;
	ibool		ignore_prefix = FALSE;
	byte		ext_buf[REC_VERSION_56_MAX_INDEX_COL_LEN
				+ BTR_EXTERN_FIELD_REF_SIZE];

	ut_a(dict_index_is_clust(index));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
			       + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_UPDATE);
	table = index->table;

	/* If table instance is temporary then select noredo rseg as changes
	to undo logs don't need REDO logging given that they are not
	restored on restart as corresponding object doesn't exist on restart.*/
	undo_ptr = dict_table_is_temporary(index->table)
		   ? &trx->rsegs.m_noredo : &trx->rsegs.m_redo;

	first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
				      + TRX_UNDO_PAGE_FREE);
	ptr = undo_page + first_free;

	ut_ad(first_free <= UNIV_PAGE_SIZE);

	if (trx_undo_left(undo_page, ptr) < 50) {

		/* NOTE: the value 50 must be big enough so that the general
		fields written below fit on the undo log page */

		return(0);
	}

	/* Reserve 2 bytes for the pointer to the next undo log record */
	ptr += 2;

	/* Store first some general parameters to the undo log */

	if (!update) {
		ut_ad(!rec_get_deleted_flag(rec, dict_table_is_comp(table)));
		type_cmpl = TRX_UNDO_DEL_MARK_REC;
	} else if (rec_get_deleted_flag(rec, dict_table_is_comp(table))) {
		type_cmpl = TRX_UNDO_UPD_DEL_REC;
		/* We are about to update a delete marked record.
		We don't typically need the prefix in this case unless
		the delete marking is done by the same transaction
		(which we check below). */
		ignore_prefix = TRUE;
	} else {
		type_cmpl = TRX_UNDO_UPD_EXIST_REC;
	}

	type_cmpl |= cmpl_info * TRX_UNDO_CMPL_INFO_MULT;
	type_cmpl_ptr = ptr;

	*ptr++ = (byte) type_cmpl;
	ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);

	ptr += mach_u64_write_much_compressed(ptr, table->id);

	/*----------------------------------------*/
	/* Store the state of the info bits */

	*ptr++ = (byte) rec_get_info_bits(rec, dict_table_is_comp(table));

	/* Store the values of the system columns */
	field = rec_get_nth_field(rec, offsets,
				  dict_index_get_sys_col_pos(
					  index, DATA_TRX_ID), &flen);
	ut_ad(flen == DATA_TRX_ID_LEN);

	trx_id = trx_read_trx_id(field);

	/* If it is an update of a delete marked record, then we are
	allowed to ignore blob prefixes if the delete marking was done
	by some other trx as it must have committed by now for us to
	allow an over-write. */
	if (ignore_prefix) {
		ignore_prefix = (trx_id != trx->id);
	}
	ptr += mach_u64_write_compressed(ptr, trx_id);

	field = rec_get_nth_field(rec, offsets,
				  dict_index_get_sys_col_pos(
					  index, DATA_ROLL_PTR), &flen);
	ut_ad(flen == DATA_ROLL_PTR_LEN);

	ptr += mach_u64_write_compressed(ptr, trx_read_roll_ptr(field));

	/*----------------------------------------*/
	/* Store then the fields required to uniquely determine the
	record which will be modified in the clustered index */

	for (i = 0; i < dict_index_get_n_unique(index); i++) {

		field = rec_get_nth_field(rec, offsets, i, &flen);

		/* The ordering columns must not be stored externally. */
		ut_ad(!rec_offs_nth_extern(offsets, i));
		ut_ad(dict_index_get_nth_col(index, i)->ord_part);

		if (trx_undo_left(undo_page, ptr) < 5) {

			return(0);
		}

		ptr += mach_write_compressed(ptr, flen);

		if (flen != UNIV_SQL_NULL) {
			if (trx_undo_left(undo_page, ptr) < flen) {

				return(0);
			}

			ut_memcpy(ptr, field, flen);
			ptr += flen;
		}
	}

	/*----------------------------------------*/
	/* Save to the undo log the old values of the columns to be updated. */

	if (update) {
		if (trx_undo_left(undo_page, ptr) < 5) {

			return(0);
		}

		ptr += mach_write_compressed(ptr, upd_get_n_fields(update));

		for (i = 0; i < upd_get_n_fields(update); i++) {

			ulint	pos = upd_get_nth_field(update, i)->field_no;

			/* Write field number to undo log */
			if (trx_undo_left(undo_page, ptr) < 5) {

				return(0);
			}

			ptr += mach_write_compressed(ptr, pos);

			/* Save the old value of field */
			field = rec_get_nth_field(rec, offsets, pos, &flen);

			if (trx_undo_left(undo_page, ptr) < 15) {

				return(0);
			}

			if (rec_offs_nth_extern(offsets, pos)) {
				const dict_col_t*	col
					= dict_index_get_nth_col(index, pos);
				ulint			prefix_len
					= dict_max_field_len_store_undo(
						table, col);

				ut_ad(prefix_len + BTR_EXTERN_FIELD_REF_SIZE
				      <= sizeof ext_buf);

				ptr = trx_undo_page_report_modify_ext(
					ptr,
					col->ord_part
					&& !ignore_prefix
					&& flen < REC_ANTELOPE_MAX_INDEX_COL_LEN
					? ext_buf : NULL, prefix_len,
					dict_table_page_size(table),
					&field, &flen);

				/* Notify purge that it eventually has to
				free the old externally stored field */

				undo_ptr->update_undo->del_marks = TRUE;

				*type_cmpl_ptr |= TRX_UNDO_UPD_EXTERN;
			} else {
				ptr += mach_write_compressed(ptr, flen);
			}

			if (flen != UNIV_SQL_NULL) {
				if (trx_undo_left(undo_page, ptr) < flen) {

					return(0);
				}

				ut_memcpy(ptr, field, flen);
				ptr += flen;
			}
		}
	}

	/*----------------------------------------*/
	/* In the case of a delete marking, and also in the case of an update
	where any ordering field of any index changes, store the values of all
	columns which occur as ordering fields in any index. This info is used
	in the purge of old versions where we use it to build and search the
	delete marked index records, to look if we can remove them from the
	index tree. Note that starting from 4.0.14 also externally stored
	fields can be ordering in some index. Starting from 5.2, we no longer
	store REC_MAX_INDEX_COL_LEN first bytes to the undo log record,
	but we can construct the column prefix fields in the index by
	fetching the first page of the BLOB that is pointed to by the
	clustered index. This works also in crash recovery, because all pages
	(including BLOBs) are recovered before anything is rolled back. */

	if (!update || !(cmpl_info & UPD_NODE_NO_ORD_CHANGE)) {
		byte*	old_ptr = ptr;

		undo_ptr->update_undo->del_marks = TRUE;

		if (trx_undo_left(undo_page, ptr) < 5) {

			return(0);
		}

		/* Reserve 2 bytes to write the number of bytes the stored
		fields take in this undo record */

		ptr += 2;

		for (col_no = 0; col_no < dict_table_get_n_cols(table);
		     col_no++) {

			const dict_col_t*	col
				= dict_table_get_nth_col(table, col_no);

			if (col->ord_part) {
				ulint	pos;

				/* Write field number to undo log */
				if (trx_undo_left(undo_page, ptr) < 5 + 15) {

					return(0);
				}

				pos = dict_index_get_nth_col_pos(index,
								 col_no);
				ptr += mach_write_compressed(ptr, pos);

				/* Save the old value of field */
				field = rec_get_nth_field(rec, offsets, pos,
							  &flen);

				if (rec_offs_nth_extern(offsets, pos)) {
					const dict_col_t*	col =
						dict_index_get_nth_col(
							index, pos);
					ulint			prefix_len =
						dict_max_field_len_store_undo(
							table, col);

					ut_a(prefix_len < sizeof ext_buf);

					ptr = trx_undo_page_report_modify_ext(
						ptr,
						flen < REC_ANTELOPE_MAX_INDEX_COL_LEN
						&& !ignore_prefix
						? ext_buf : NULL, prefix_len,
						dict_table_page_size(table),
						&field, &flen);
				} else {
					ptr += mach_write_compressed(
						ptr, flen);
				}

				if (flen != UNIV_SQL_NULL) {
					if (trx_undo_left(undo_page, ptr)
					    < flen) {

						return(0);
					}

					ut_memcpy(ptr, field, flen);
					ptr += flen;
				}
			}
		}

		mach_write_to_2(old_ptr, ptr - old_ptr);
	}

	/*----------------------------------------*/
	/* Write pointers to the previous and the next undo log records */
	if (trx_undo_left(undo_page, ptr) < 2) {

		return(0);
	}

	mach_write_to_2(ptr, first_free);
	ptr += 2;
	mach_write_to_2(undo_page + first_free, ptr - undo_page);

	mach_write_to_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE,
			ptr - undo_page);

	/* Write to the REDO log about this change in the UNDO log */

	trx_undof_page_add_undo_rec_log(undo_page, first_free,
					ptr - undo_page, mtr);
	return(first_free);
}

/**********************************************************************//**
Reads from an undo log update record the system field values of the old
version.
@return remaining part of undo log record after reading these values */

byte*
trx_undo_update_rec_get_sys_cols(
/*=============================*/
	const byte*	ptr,		/*!< in: remaining part of undo
					log record after reading
					general parameters */
	trx_id_t*	trx_id,		/*!< out: trx id */
	roll_ptr_t*	roll_ptr,	/*!< out: roll ptr */
	ulint*		info_bits)	/*!< out: info bits state */
{
	/* Read the state of the info bits */
	*info_bits = mach_read_from_1(ptr);
	ptr += 1;

	/* Read the values of the system columns */

	*trx_id = mach_u64_read_next_compressed(&ptr);
	*roll_ptr = mach_u64_read_next_compressed(&ptr);

	return(const_cast<byte*>(ptr));
}

/*******************************************************************//**
Builds an update vector based on a remaining part of an undo log record.
@return remaining part of the record, NULL if an error detected, which
means that the record is corrupted */

byte*
trx_undo_update_rec_get_update(
/*===========================*/
	const byte*	ptr,	/*!< in: remaining part in update undo log
				record, after reading the row reference
				NOTE that this copy of the undo log record must
				be preserved as long as the update vector is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	ulint		type,	/*!< in: TRX_UNDO_UPD_EXIST_REC,
				TRX_UNDO_UPD_DEL_REC, or
				TRX_UNDO_DEL_MARK_REC; in the last case,
				only trx id and roll ptr fields are added to
				the update vector */
	trx_id_t	trx_id,	/*!< in: transaction id from this undo record */
	roll_ptr_t	roll_ptr,/*!< in: roll pointer from this undo record */
	ulint		info_bits,/*!< in: info bits from this undo record */
	trx_t*		trx,	/*!< in: transaction */
	mem_heap_t*	heap,	/*!< in: memory heap from which the memory
				needed is allocated */
	upd_t**		upd)	/*!< out, own: update vector */
{
	upd_field_t*	upd_field;
	upd_t*		update;
	ulint		n_fields;
	byte*		buf;
	ulint		i;

	ut_a(dict_index_is_clust(index));

	if (type != TRX_UNDO_DEL_MARK_REC) {
		n_fields = mach_read_next_compressed(&ptr);
	} else {
		n_fields = 0;
	}

	update = upd_create(n_fields + 2, heap);

	update->info_bits = info_bits;

	/* Store first trx id and roll ptr to update vector */

	upd_field = upd_get_nth_field(update, n_fields);

	buf = static_cast<byte*>(mem_heap_alloc(heap, DATA_TRX_ID_LEN));

	trx_write_trx_id(buf, trx_id);

	upd_field_set_field_no(upd_field,
			       dict_index_get_sys_col_pos(index, DATA_TRX_ID),
			       index, trx);
	dfield_set_data(&(upd_field->new_val), buf, DATA_TRX_ID_LEN);

	upd_field = upd_get_nth_field(update, n_fields + 1);

	buf = static_cast<byte*>(mem_heap_alloc(heap, DATA_ROLL_PTR_LEN));

	trx_write_roll_ptr(buf, roll_ptr);

	upd_field_set_field_no(
		upd_field, dict_index_get_sys_col_pos(index, DATA_ROLL_PTR),
		index, trx);
	dfield_set_data(&(upd_field->new_val), buf, DATA_ROLL_PTR_LEN);

	/* Store then the updated ordinary columns to the update vector */

	for (i = 0; i < n_fields; i++) {

		const byte*	field;
		ulint		len;
		ulint		field_no;
		ulint		orig_len;

		field_no = mach_read_next_compressed(&ptr);

		if (field_no >= dict_index_get_n_fields(index)) {
			std::string	str = ut_get_name(
							  trx, TRUE,
							  index->table_name);
			ib_logf(IB_LOG_LEVEL_ERROR,
				"Trying to access"
				" update undo rec field %lu in"
				" index %s of table %s"
				" but index has only %lu fields"
				" %s. Run also CHECK TABLE %s."
				" n_fields = %lu, i = %lu, ptr %p",
				(ulong) field_no,
				ut_get_name(trx, FALSE, index->name).c_str(),
				str.c_str(),
				(ulong) dict_index_get_n_fields(index),
				BUG_REPORT_MSG,
				str.c_str(),
				(ulong) n_fields, (ulong) i, ptr);
			ut_ad(0);
			*upd = NULL;
			return(NULL);
		}

		upd_field = upd_get_nth_field(update, i);

		upd_field_set_field_no(upd_field, field_no, index, trx);

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

		upd_field->orig_len = orig_len;

		if (len == UNIV_SQL_NULL) {
			dfield_set_null(&upd_field->new_val);
		} else if (len < UNIV_EXTERN_STORAGE_FIELD) {
			dfield_set_data(&upd_field->new_val, field, len);
		} else {
			len -= UNIV_EXTERN_STORAGE_FIELD;

			dfield_set_data(&upd_field->new_val, field, len);
			dfield_set_ext(&upd_field->new_val);
		}
	}

	*upd = update;

	return(const_cast<byte*>(ptr));
}

/*******************************************************************//**
Builds a partial row from an update undo log record, for purge.
It contains the columns which occur as ordering in any index of the table.
Any missing columns are indicated by col->mtype == DATA_MISSING.
@return pointer to remaining part of undo record */

byte*
trx_undo_rec_get_partial_row(
/*=========================*/
	const byte*	ptr,	/*!< in: remaining part in update undo log
				record of a suitable type, at the start of
				the stored index columns;
				NOTE that this copy of the undo log record must
				be preserved as long as the partial row is
				used, as we do NOT copy the data in the
				record! */
	dict_index_t*	index,	/*!< in: clustered index */
	dtuple_t**	row,	/*!< out, own: partial row */
	ibool		ignore_prefix, /*!< in: flag to indicate if we
				expect blob prefixes in undo. Used
				only in the assertion. */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory
				needed is allocated */
{
	const byte*	end_ptr;
	ulint		row_len;

	ut_ad(index);
	ut_ad(ptr);
	ut_ad(row);
	ut_ad(heap);
	ut_ad(dict_index_is_clust(index));

	row_len = dict_table_get_n_cols(index->table);

	*row = dtuple_create(heap, row_len);

	/* Mark all columns in the row uninitialized, so that
	we can distinguish missing fields from fields that are SQL NULL. */
	for (ulint i = 0; i < row_len; i++) {
		dfield_get_type(dtuple_get_nth_field(*row, i))
			->mtype = DATA_MISSING;
	}

	end_ptr = ptr + mach_read_from_2(ptr);
	ptr += 2;

	while (ptr != end_ptr) {
		dfield_t*		dfield;
		const byte*		field;
		ulint			field_no;
		const dict_col_t*	col;
		ulint			col_no;
		ulint			len;
		ulint			orig_len;

		field_no = mach_read_next_compressed(&ptr);

		col = dict_index_get_nth_col(index, field_no);
		col_no = dict_col_get_no(col);

		ptr = trx_undo_rec_get_col_val(ptr, &field, &len, &orig_len);

		dfield = dtuple_get_nth_field(*row, col_no);
		dict_col_copy_type(
			dict_table_get_nth_col(index->table, col_no),
			dfield_get_type(dfield));
		dfield_set_data(dfield, field, len);

		if (len != UNIV_SQL_NULL
		    && len >= UNIV_EXTERN_STORAGE_FIELD) {
			dfield_set_len(dfield,
				       len - UNIV_EXTERN_STORAGE_FIELD);
			dfield_set_ext(dfield);
			/* If the prefix of this column is indexed,
			ensure that enough prefix is stored in the
			undo log record. */
			if (!ignore_prefix && col->ord_part) {
				ut_a(dfield_get_len(dfield)
				     >= BTR_EXTERN_FIELD_REF_SIZE);
				ut_a(dict_table_get_format(index->table)
				     >= UNIV_FORMAT_B
				     || dfield_get_len(dfield)
				     >= REC_ANTELOPE_MAX_INDEX_COL_LEN
				     + BTR_EXTERN_FIELD_REF_SIZE);
			}
		}
	}

	return(const_cast<byte*>(ptr));
}
#endif /* !UNIV_HOTBACKUP */

/***********************************************************************//**
Erases the unused undo log page end.
@return TRUE if the page contained something, FALSE if it was empty */
static __attribute__((nonnull))
ibool
trx_undo_erase_page_end(
/*====================*/
	page_t*	undo_page,	/*!< in/out: undo page whose end to erase */
	mtr_t*	mtr)		/*!< in/out: mini-transaction */
{
	ulint	first_free;

	first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR
				      + TRX_UNDO_PAGE_FREE);
	memset(undo_page + first_free, 0xff,
	       (UNIV_PAGE_SIZE - FIL_PAGE_DATA_END) - first_free);

	mlog_write_initial_log_record(undo_page, MLOG_UNDO_ERASE_END, mtr);
	return(first_free != TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE);
}

/***********************************************************//**
Parses a redo log record of erasing of an undo page end.
@return end of log record or NULL */

byte*
trx_undo_parse_erase_page_end(
/*==========================*/
	byte*	ptr,	/*!< in: buffer */
	byte*	end_ptr __attribute__((unused)), /*!< in: buffer end */
	page_t*	page,	/*!< in: page or NULL */
	mtr_t*	mtr)	/*!< in: mtr or NULL */
{
	ut_ad(ptr && end_ptr);

	if (page == NULL) {

		return(ptr);
	}

	trx_undo_erase_page_end(page, mtr);

	return(ptr);
}

#ifndef UNIV_HOTBACKUP
/***********************************************************************//**
Writes information to an undo log about an insert, update, or a delete marking
of a clustered index record. This information is used in a rollback of the
transaction and in consistent reads that must look to the history of this
transaction.
@return DB_SUCCESS or error code */

dberr_t
trx_undo_report_row_operation(
/*==========================*/
	ulint		flags,		/*!< in: if BTR_NO_UNDO_LOG_FLAG bit is
					set, does nothing */
	ulint		op_type,	/*!< in: TRX_UNDO_INSERT_OP or
					TRX_UNDO_MODIFY_OP */
	que_thr_t*	thr,		/*!< in: query thread */
	dict_index_t*	index,		/*!< in: clustered index */
	const dtuple_t*	clust_entry,	/*!< in: in the case of an insert,
					index entry to insert into the
					clustered index, otherwise NULL */
	const upd_t*	update,		/*!< in: in the case of an update,
					the update vector, otherwise NULL */
	ulint		cmpl_info,	/*!< in: compiler info on secondary
					index updates */
	const rec_t*	rec,		/*!< in: in case of an update or delete
					marking, the record in the clustered
					index, otherwise NULL */
	const ulint*	offsets,	/*!< in: rec_get_offsets(rec) */
	roll_ptr_t*	roll_ptr)	/*!< out: rollback pointer to the
					inserted undo log record,
					0 if BTR_NO_UNDO_LOG
					flag was specified */
{
	trx_t*		trx;
	trx_undo_t*	undo;
	ulint		page_no;
	buf_block_t*	undo_block;
	trx_undo_ptr_t*	undo_ptr;
	mtr_t		mtr;
	dberr_t		err		= DB_SUCCESS;
#ifdef UNIV_DEBUG
	int		loop_count	= 0;
#endif /* UNIV_DEBUG */

	ut_ad(!srv_read_only_mode);
	ut_a(dict_index_is_clust(index));
	ut_ad(!rec || rec_offs_validate(rec, index, offsets));

	if (flags & BTR_NO_UNDO_LOG_FLAG) {

		*roll_ptr = 0;

		return(DB_SUCCESS);
	}

	ut_ad(thr);
	ut_ad((op_type != TRX_UNDO_INSERT_OP)
	      || (clust_entry && !update && !rec));

	trx = thr_get_trx(thr);

	bool	is_temp_table = dict_table_is_temporary(index->table);

	/* Temporary tables do not go into INFORMATION_SCHEMA.TABLES,
	so do not bother adding it to the list of modified tables by
	the transaction - this list is only used for maintaining
	INFORMATION_SCHEMA.TABLES.UPDATE_TIME. */
	if (!is_temp_table) {
		trx->mod_tables.insert(index->table);
	}

	/* If trx is read-only then only temp-tables can be written.
	If trx is read-write and involves temp-table only then we
	assign temporary rseg. */
	if (trx->read_only || is_temp_table) {

		ut_ad(!srv_read_only_mode || is_temp_table);

		/* MySQL should block writes to non-temporary tables. */
		ut_a(is_temp_table);

		if (trx->rsegs.m_noredo.rseg == 0) {
			trx_assign_rseg(trx);
		}
	}

	/* If object is temporary, disable REDO logging that is done to track
	changes done to UNDO logs. This is feasible given that temporary tables
	are not restored on restart. */
	mtr_start(&mtr);
	dict_disable_redo_if_temporary(index->table, &mtr);
	mutex_enter(&trx->undo_mutex);

	/* If object is temp-table then select noredo rseg as changes
	to undo logs don't need REDO logging given that they are not
	restored on restart as corresponding object doesn't exist on restart.*/
	undo_ptr = is_temp_table ? &trx->rsegs.m_noredo : &trx->rsegs.m_redo;

	switch (op_type) {
	case TRX_UNDO_INSERT_OP:
		undo = undo_ptr->insert_undo;

		if (undo == NULL) {

			err = trx_undo_assign_undo(
				trx, undo_ptr, TRX_UNDO_INSERT);
			undo = undo_ptr->insert_undo;

			if (undo == NULL) {
				/* Did not succeed */
				ut_ad(err != DB_SUCCESS);
				goto err_exit;
			}

			ut_ad(err == DB_SUCCESS);
		}
		break;
	default:
		ut_ad(op_type == TRX_UNDO_MODIFY_OP);

		undo = undo_ptr->update_undo;

		if (undo == NULL) {
			err = trx_undo_assign_undo(
				trx, undo_ptr, TRX_UNDO_UPDATE);
			undo = undo_ptr->update_undo;

			if (undo == NULL) {
				/* Did not succeed */
				ut_ad(err != DB_SUCCESS);
				goto err_exit;
			}
		}

		ut_ad(err == DB_SUCCESS);
	}

	page_no = undo->last_page_no;

	undo_block = buf_page_get_gen(
		page_id_t(undo->space, page_no), undo->page_size,
		RW_X_LATCH, undo->guess_block, BUF_GET, __FILE__, __LINE__,
		&mtr);

	buf_block_dbg_add_level(undo_block, SYNC_TRX_UNDO_PAGE);

	do {
		page_t*		undo_page;
		ulint		offset;

		undo_page = buf_block_get_frame(undo_block);
		ut_ad(page_no == undo_block->page.id.page_no());

		switch (op_type) {
		case TRX_UNDO_INSERT_OP:
			offset = trx_undo_page_report_insert(
				undo_page, trx, index, clust_entry, &mtr);
			break;
		default:
			ut_ad(op_type == TRX_UNDO_MODIFY_OP);
			offset = trx_undo_page_report_modify(
				undo_page, trx, index, rec, offsets, update,
				cmpl_info, &mtr);
		}

		if (UNIV_UNLIKELY(offset == 0)) {
			/* The record did not fit on the page. We erase the
			end segment of the undo log page and write a log
			record of it: this is to ensure that in the debug
			version the replicate page constructed using the log
			records stays identical to the original page */

			if (!trx_undo_erase_page_end(undo_page, &mtr)) {
				/* The record did not fit on an empty
				undo page. Discard the freshly allocated
				page and return an error. */

				/* When we remove a page from an undo
				log, this is analogous to a
				pessimistic insert in a B-tree, and we
				must reserve the counterpart of the
				tree latch, which is the rseg
				mutex. We must commit the mini-transaction
				first, because it may be holding lower-level
				latches, such as SYNC_FSP and SYNC_FSP_PAGE. */

				mtr_commit(&mtr);
				mtr_start(&mtr);
				dict_disable_redo_if_temporary(
					index->table, &mtr);

				mutex_enter(&undo_ptr->rseg->mutex);
				trx_undo_free_last_page(trx, undo, &mtr);
				mutex_exit(&undo_ptr->rseg->mutex);

				err = DB_UNDO_RECORD_TOO_BIG;
				goto err_exit;
			}

			mtr_commit(&mtr);
		} else {
			/* Success */

			mtr_commit(&mtr);

			undo->empty = FALSE;
			undo->top_page_no = page_no;
			undo->top_offset  = offset;
			undo->top_undo_no = trx->undo_no;
			undo->guess_block = undo_block;

			trx->undo_no++;
			trx->undo_rseg_space = undo_ptr->rseg->space;

			mutex_exit(&trx->undo_mutex);

			*roll_ptr = trx_undo_build_roll_ptr(
				op_type == TRX_UNDO_INSERT_OP,
				undo_ptr->rseg->id, page_no, offset);
			return(DB_SUCCESS);
		}

		ut_ad(page_no == undo->last_page_no);

		/* We have to extend the undo log by one page */

		ut_ad(++loop_count < 2);
		mtr_start(&mtr);
		dict_disable_redo_if_temporary(index->table, &mtr);

		/* When we add a page to an undo log, this is analogous to
		a pessimistic insert in a B-tree, and we must reserve the
		counterpart of the tree latch, which is the rseg mutex. */

		mutex_enter(&undo_ptr->rseg->mutex);
		undo_block = trx_undo_add_page(trx, undo, undo_ptr, &mtr);
		mutex_exit(&undo_ptr->rseg->mutex);

		page_no = undo->last_page_no;

		DBUG_EXECUTE_IF("ib_err_ins_undo_page_add_failure",
				undo_block = NULL;);
	} while (undo_block != NULL);

	ib_errf(trx->mysql_thd, IB_LOG_LEVEL_ERROR,
		ER_INNODB_UNDO_LOG_FULL,
		"No more space left over in %s tablespace for allocating UNDO"
		" log pages. Please add new data file to the tablespace or"
		" check if filesystem is full or enable auto-extension for"
		" the tablespace",
		((undo->space == srv_sys_space.space_id())
		? "system" :
		  ((undo->space == srv_tmp_space.space_id())
		   ? "temporary" : "undo")));

	/* Did not succeed: out of space */
	err = DB_OUT_OF_FILE_SPACE;

err_exit:
	mutex_exit(&trx->undo_mutex);
	mtr_commit(&mtr);
	return(err);
}

/*============== BUILDING PREVIOUS VERSION OF A RECORD ===============*/

/******************************************************************//**
Copies an undo record to heap. This function can be called if we know that
the undo log record exists.
@return own: copy of the record */

trx_undo_rec_t*
trx_undo_get_undo_rec_low(
/*======================*/
	roll_ptr_t	roll_ptr,	/*!< in: roll pointer to record */
	mem_heap_t*	heap,		/*!< in: memory heap where copied */
	bool		is_redo_rseg)	/*!< in: true if redo rseg. */
{
	trx_undo_rec_t*	undo_rec;
	ulint		rseg_id;
	ulint		page_no;
	ulint		offset;
	const page_t*	undo_page;
	trx_rseg_t*	rseg;
	ibool		is_insert;
	mtr_t		mtr;

	trx_undo_decode_roll_ptr(roll_ptr, &is_insert, &rseg_id, &page_no,
				 &offset);
	rseg = trx_rseg_get_on_id(rseg_id, is_redo_rseg);

	mtr_start(&mtr);

	undo_page = trx_undo_page_get_s_latched(
		page_id_t(rseg->space, page_no), rseg->page_size,
		&mtr);

	undo_rec = trx_undo_rec_copy(undo_page + offset, heap);

	mtr_commit(&mtr);

	return(undo_rec);
}

/******************************************************************//**
Copies an undo record to heap.

NOTE: the caller must have latches on the clustered index page.

@retval true if the undo log has been
truncated and we cannot fetch the old version
@retval false if the undo log record is available */
static __attribute__((warn_unused_result))
bool
trx_undo_get_undo_rec(
/*==================*/
	roll_ptr_t	roll_ptr,	/*!< in: roll pointer to record */
	trx_id_t	trx_id,		/*!< in: id of the trx that generated
					the roll pointer: it points to an
					undo log of this transaction */
	trx_undo_rec_t**undo_rec,	/*!< out, own: copy of the record */
	mem_heap_t*	heap,		/*!< in: memory heap where copied */
	bool		is_redo_rseg)	/*!< in: true if redo rseg. */
{
	bool		missing_history;

	rw_lock_s_lock(&purge_sys->latch);

	missing_history = purge_sys->view.changes_visible(trx_id);

	if (!missing_history) {
		*undo_rec = trx_undo_get_undo_rec_low(
			roll_ptr, heap, is_redo_rseg);
	}

	rw_lock_s_unlock(&purge_sys->latch);

	return(missing_history);
}

#ifdef UNIV_DEBUG
#define ATTRIB_USED_ONLY_IN_DEBUG
#else /* UNIV_DEBUG */
#define ATTRIB_USED_ONLY_IN_DEBUG	__attribute__((unused))
#endif /* UNIV_DEBUG */

/*******************************************************************//**
Build a previous version of a clustered index record. The caller must
hold a latch on the index page of the clustered index record.
@retval true if previous version was built, or if it was an insert
or the table has been rebuilt
@retval false if the previous version is earlier than purge_view,
which means that it may have been removed */

bool
trx_undo_prev_version_build(
/*========================*/
	const rec_t*	index_rec ATTRIB_USED_ONLY_IN_DEBUG,
				/*!< in: clustered index record in the
				index tree */
	mtr_t*		index_mtr ATTRIB_USED_ONLY_IN_DEBUG,
				/*!< in: mtr which contains the latch to
				index_rec page and purge_view */
	const rec_t*	rec,	/*!< in: version of a clustered index record */
	dict_index_t*	index,	/*!< in: clustered index */
	ulint*		offsets,/*!< in/out: rec_get_offsets(rec, index) */
	mem_heap_t*	heap,	/*!< in: memory heap from which the memory
				needed is allocated */
	rec_t**		old_vers)/*!< out, own: previous version, or NULL if
				rec is the first inserted version, or if
				history data has been deleted (an error),
				or if the purge COULD have removed the version
				though it has not yet done so */
{
	trx_undo_rec_t*	undo_rec	= NULL;
	dtuple_t*	entry;
	trx_id_t	rec_trx_id;
	ulint		type;
	undo_no_t	undo_no;
	table_id_t	table_id;
	trx_id_t	trx_id;
	roll_ptr_t	roll_ptr;
	upd_t*		update;
	byte*		ptr;
	ulint		info_bits;
	ulint		cmpl_info;
	bool		dummy_extern;
	byte*		buf;
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!rw_lock_own(&purge_sys->latch, RW_LOCK_S));
#endif /* UNIV_SYNC_DEBUG */
	ut_ad(mtr_memo_contains_page(index_mtr, index_rec, MTR_MEMO_PAGE_S_FIX)
	      || mtr_memo_contains_page(index_mtr, index_rec,
					MTR_MEMO_PAGE_X_FIX));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_a(dict_index_is_clust(index));

	roll_ptr = row_get_rec_roll_ptr(rec, index, offsets);

	*old_vers = NULL;

	if (trx_undo_roll_ptr_is_insert(roll_ptr)) {
		/* The record rec is the first inserted version */
		return(true);
	}

	rec_trx_id = row_get_rec_trx_id(rec, index, offsets);

	/* REDO rollback segment are used only for non-temporary objects.
	For temporary objects NON-REDO rollback segments are used. */
	bool is_redo_rseg =
		dict_table_is_temporary(index->table) ? false : true;
	if (trx_undo_get_undo_rec(
		roll_ptr, rec_trx_id, &undo_rec, heap, is_redo_rseg)) {
		/* The undo record may already have been purged,
		during purge or semi-consistent read. */
		return(false);
	}

	ptr = trx_undo_rec_get_pars(undo_rec, &type, &cmpl_info,
				    &dummy_extern, &undo_no, &table_id);

	if (table_id != index->table->id) {
		/* The table should have been rebuilt, but purge has
		not yet removed the undo log records for the
		now-dropped old table (table_id). */
		return(true);
	}

	ptr = trx_undo_update_rec_get_sys_cols(ptr, &trx_id, &roll_ptr,
					       &info_bits);

	/* (a) If a clustered index record version is such that the
	trx id stamp in it is bigger than purge_sys->view, then the
	BLOBs in that version are known to exist (the purge has not
	progressed that far);

	(b) if the version is the first version such that trx id in it
	is less than purge_sys->view, and it is not delete-marked,
	then the BLOBs in that version are known to exist (the purge
	cannot have purged the BLOBs referenced by that version
	yet).

	This function does not fetch any BLOBs.  The callers might, by
	possibly invoking row_ext_create() via row_build().  However,
	they should have all needed information in the *old_vers
	returned by this function.  This is because *old_vers is based
	on the transaction undo log records.  The function
	trx_undo_page_fetch_ext() will write BLOB prefixes to the
	transaction undo log that are at least as long as the longest
	possible column prefix in a secondary index.  Thus, secondary
	index entries for *old_vers can be constructed without
	dereferencing any BLOB pointers. */

	ptr = trx_undo_rec_skip_row_ref(ptr, index);

	ptr = trx_undo_update_rec_get_update(ptr, index, type, trx_id,
					     roll_ptr, info_bits,
					     NULL, heap, &update);
	ut_a(ptr);

# if defined UNIV_DEBUG || defined UNIV_BLOB_LIGHT_DEBUG
	ut_a(!rec_offs_any_null_extern(rec, offsets));
# endif /* UNIV_DEBUG || UNIV_BLOB_LIGHT_DEBUG */

	if (row_upd_changes_field_size_or_external(index, offsets, update)) {
		ulint	n_ext;

		/* We should confirm the existence of disowned external data,
		if the previous version record is delete marked. If the trx_id
		of the previous record is seen by purge view, we should treat
		it as missing history, because the disowned external data
		might be purged already.

		The inherited external data (BLOBs) can be freed (purged)
		after trx_id was committed, provided that no view was started
		before trx_id. If the purge view can see the committed
		delete-marked record by trx_id, no transactions need to access
		the BLOB. */

		/* the row_upd_changes_disowned_external(update) call could be
		omitted, but the synchronization on purge_sys->latch is likely
		more expensive. */

		if ((update->info_bits & REC_INFO_DELETED_FLAG)
		    && row_upd_changes_disowned_external(update)) {
			bool	missing_extern;

			rw_lock_s_lock(&purge_sys->latch);

			missing_extern = purge_sys->view.changes_visible(
				trx_id);

			rw_lock_s_unlock(&purge_sys->latch);

			if (missing_extern) {
				/* treat as a fresh insert, not to
				cause assertion error at the caller. */
				return(true);
			}
		}

		/* We have to set the appropriate extern storage bits in the
		old version of the record: the extern bits in rec for those
		fields that update does NOT update, as well as the bits for
		those fields that update updates to become externally stored
		fields. Store the info: */

		entry = row_rec_to_index_entry(
			rec, index, offsets, &n_ext, heap);
		n_ext += btr_push_update_extern_fields(entry, update, heap);
		/* The page containing the clustered index record
		corresponding to entry is latched in mtr.  Thus the
		following call is safe. */
		row_upd_index_replace_new_col_vals(entry, index, update, heap);

		buf = static_cast<byte*>(
			mem_heap_alloc(
				heap,
				rec_get_converted_size(index, entry, n_ext)));

		*old_vers = rec_convert_dtuple_to_rec(buf, index,
						      entry, n_ext);
	} else {
		buf = static_cast<byte*>(
			mem_heap_alloc(heap, rec_offs_size(offsets)));

		*old_vers = rec_copy(buf, rec, offsets);
		rec_offs_make_valid(*old_vers, index, offsets);
		row_upd_rec_in_place(*old_vers, index, offsets, update, NULL);
	}

	return(true);
}
#endif /* !UNIV_HOTBACKUP */
