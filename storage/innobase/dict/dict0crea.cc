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
@file dict/dict0crea.cc
Database object creation

Created 1/8/1996 Heikki Tuuri
*******************************************************/

#include "ha_prototypes.h"

#include "dict0crea.h"

#ifdef UNIV_NONINL
#include "dict0crea.ic"
#endif

#include "btr0pcur.h"
#include "btr0btr.h"
#include "page0page.h"
#include "mach0data.h"
#include "dict0boot.h"
#include "dict0dict.h"
#include "que0que.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "pars0pars.h"
#include "trx0roll.h"
#include "usr0sess.h"
#include "ut0vec.h"
#include "dict0priv.h"
#include "fts0priv.h"
#include "fsp0sysspace.h"

/*****************************************************************//**
Based on a table object, this function builds the entry to be inserted
in the SYS_TABLES system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_tables_tuple(
/*=========================*/
	const dict_table_t*	table,	/*!< in: table */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dict_table_t*	sys_tables;
	dtuple_t*	entry;
	dfield_t*	dfield;
	byte*		ptr;
	ulint		type;

	ut_ad(table);
	ut_ad(heap);

	sys_tables = dict_sys->sys_tables;

	entry = dtuple_create(heap, 8 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, sys_tables);

	/* 0: NAME -----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__NAME);

	dfield_set_data(dfield, table->name, ut_strlen(table->name));

	/* 1: DB_TRX_ID added later */
	/* 2: DB_ROLL_PTR added later */
	/* 3: ID -------------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 4: N_COLS ---------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__N_COLS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, table->n_def
			| ((table->flags & DICT_TF_COMPACT) << 31));
	dfield_set_data(dfield, ptr, 4);

	/* 5: TYPE (table flags) -----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__TYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));

	/* Validate the table flags and convert them to what is saved in
	SYS_TABLES.TYPE.  Table flag values 0 and 1 are both written to
	SYS_TABLES.TYPE as 1. */
	type = dict_tf_to_sys_tables_type(table->flags);
	mach_write_to_4(ptr, type);

	dfield_set_data(dfield, ptr, 4);

	/* 6: MIX_ID (obsolete) ---------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__MIX_ID);

	ptr = static_cast<byte*>(mem_heap_zalloc(heap, 8));

	dfield_set_data(dfield, ptr, 8);

	/* 7: MIX_LEN (additional flags) --------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__MIX_LEN);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	/* Be sure all non-used bits are zero. */
	ut_a(!(table->flags2 & ~DICT_TF2_BIT_MASK));
	mach_write_to_4(ptr, table->flags2);

	dfield_set_data(dfield, ptr, 4);

	/* 8: CLUSTER_NAME ---------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__CLUSTER_ID);
	dfield_set_null(dfield); /* not supported */

	/* 9: SPACE ----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_TABLES__SPACE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, table->space);

	dfield_set_data(dfield, ptr, 4);
	/*----------------------------------*/

	return(entry);
}

/*****************************************************************//**
Based on a table object, this function builds the entry to be inserted
in the SYS_COLUMNS system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_columns_tuple(
/*==========================*/
	const dict_table_t*	table,	/*!< in: table */
	ulint			i,	/*!< in: column number */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dict_table_t*		sys_columns;
	dtuple_t*		entry;
	const dict_col_t*	column;
	dfield_t*		dfield;
	byte*			ptr;
	const char*		col_name;

	ut_ad(table);
	ut_ad(heap);

	column = dict_table_get_nth_col(table, i);

	sys_columns = dict_sys->sys_columns;

	entry = dtuple_create(heap, 7 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, sys_columns);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__TABLE_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: POS ----------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__POS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, i);

	dfield_set_data(dfield, ptr, 4);

	/* 2: DB_TRX_ID added later */
	/* 3: DB_ROLL_PTR added later */
	/* 4: NAME ---------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__NAME);

	col_name = dict_table_get_col_name(table, i);
	dfield_set_data(dfield, col_name, ut_strlen(col_name));

	/* 5: MTYPE --------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__MTYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, column->mtype);

	dfield_set_data(dfield, ptr, 4);

	/* 6: PRTYPE -------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__PRTYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, column->prtype);

	dfield_set_data(dfield, ptr, 4);

	/* 7: LEN ----------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__LEN);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, column->len);

	dfield_set_data(dfield, ptr, 4);

	/* 8: PREC ---------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_COLUMNS__PREC);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, 0/* unused */);

	dfield_set_data(dfield, ptr, 4);
	/*---------------------------------*/

	return(entry);
}

/***************************************************************//**
Builds a table definition to insert.
@return DB_SUCCESS or error code */
static __attribute__((nonnull, warn_unused_result))
dberr_t
dict_build_table_def_step(
/*======================*/
	que_thr_t*	thr,	/*!< in: query thread */
	tab_node_t*	node)	/*!< in: table create node */
{
	dict_table_t*	table;
	dtuple_t*	row;
	dberr_t		err = DB_SUCCESS;

	table = node->table;

	err = dict_build_tablespace(table, thr_get_trx(thr));
	if (err != DB_SUCCESS) {
		return(err);
	}

	row = dict_create_sys_tables_tuple(table, node->heap);

	ins_node_set_new_row(node->tab_def, row);

	return(err);
}

/***************************************************************//**
Builds a tablespace, if configured (using file-per-table=1).
@return DB_SUCCESS or error code */

dberr_t
dict_build_tablespace(
/*===================*/
	dict_table_t*	table,	/*!< in/out: table */
	trx_t*		trx)	/*!< in/out: InnoDB transaction handle */
{
	dberr_t		err	= DB_SUCCESS;
	const char*	path;
	mtr_t		mtr;
	ulint		space = 0;
	bool		use_file_per_table;

	ut_ad(mutex_own(&dict_sys->mutex));

	dict_hdr_get_new_id(&table->id, NULL, NULL, table, false);

	trx->table_id = table->id;

	use_file_per_table = dict_table_use_file_per_table(table);

	/* Always set this bit for all new created tables */
	DICT_TF2_FLAG_SET(table, DICT_TF2_FTS_AUX_HEX_NAME);
	DBUG_EXECUTE_IF("innodb_test_wrong_fts_aux_table_name",
			DICT_TF2_FLAG_UNSET(table,
					    DICT_TF2_FTS_AUX_HEX_NAME););

	if (use_file_per_table) {
		/* This table will not use the system tablespace.
		Get a new space id. */
		dict_hdr_get_new_id(NULL, NULL, &space, table, false);

		DBUG_EXECUTE_IF(
			"ib_create_table_fail_out_of_space_ids",
			space = ULINT_UNDEFINED;
		);

		if (space == ULINT_UNDEFINED) {
			return(DB_ERROR);
		}

		/* We create a new single-table tablespace for the table.
		We initially let it be 4 pages:
		- page 0 is the fsp header and an extent descriptor page,
		- page 1 is an ibuf bitmap page,
		- page 2 is the first inode page,
		- page 3 will contain the root of the clustered index of the
		table we create here. */

		path = table->data_dir_path ? table->data_dir_path
					    : table->dir_path_of_temp_table;

		ut_ad(dict_table_get_format(table) <= UNIV_FORMAT_MAX);
		ut_ad(!dict_table_page_size(table).is_compressed()
		      || dict_table_get_format(table) >= UNIV_FORMAT_B);

		err = fil_create_new_single_table_tablespace(
			space, table->name, path,
			dict_tf_to_fsp_flags(table->flags),
			table->flags2,
			FIL_IBD_FILE_INITIAL_SIZE);

		table->space = (unsigned int) space;

		if (err != DB_SUCCESS) {

			return(err);
		}

		mtr_start(&mtr);
		dict_disable_redo_if_temporary(table, &mtr);

		fsp_header_init(table->space, FIL_IBD_FILE_INITIAL_SIZE, &mtr);

		mtr_commit(&mtr);
	} else {
		/* All non-compressed temporary tables are stored in
		shared temp-tablespace. Note: Even if table is residing
		in temp-tablespace row_format is honored as against
		over-written in non-temp-table case */
		if (dict_table_is_temporary(table)) {
			table->space = srv_tmp_space.space_id();
		} else {
			/* Create in the system tablespace.
			Disallow Barracuda (Dynamic & Compressed)
			page creation as they need file-per-table.
			Update table flags accordingly */
			rec_format_t rec_format = table->flags == 0
						? REC_FORMAT_REDUNDANT
						: REC_FORMAT_COMPACT;
			ulint flags;
			dict_tf_set(&flags, rec_format, 0, 0);
			table->flags = static_cast<unsigned int>(flags);
		}

		DBUG_EXECUTE_IF("ib_ddl_crash_during_tablespace_alloc",
				DBUG_SUICIDE(););
	}

	return(DB_SUCCESS);
}

/***************************************************************//**
Builds a column definition to insert. */
static
void
dict_build_col_def_step(
/*====================*/
	tab_node_t*	node)	/*!< in: table create node */
{
	dtuple_t*	row;

	row = dict_create_sys_columns_tuple(node->table, node->col_no,
					    node->heap);
	ins_node_set_new_row(node->col_def, row);
}

/*****************************************************************//**
Based on an index object, this function builds the entry to be inserted
in the SYS_INDEXES system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_indexes_tuple(
/*==========================*/
	const dict_index_t*	index,	/*!< in: index */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dict_table_t*	sys_indexes;
	dict_table_t*	table;
	dtuple_t*	entry;
	dfield_t*	dfield;
	byte*		ptr;

	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(index);
	ut_ad(heap);

	sys_indexes = dict_sys->sys_indexes;

	table = dict_table_get_low(index->table_name);

	entry = dtuple_create(heap, 7 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, sys_indexes);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__TABLE_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, table->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: ID ----------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, index->id);

	dfield_set_data(dfield, ptr, 8);

	/* 2: DB_TRX_ID added later */
	/* 3: DB_ROLL_PTR added later */
	/* 4: NAME --------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__NAME);

	dfield_set_data(dfield, index->name, ut_strlen(index->name));

	/* 5: N_FIELDS ----------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__N_FIELDS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, index->n_fields);

	dfield_set_data(dfield, ptr, 4);

	/* 6: TYPE --------------------------*/
	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__TYPE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, index->type);

	dfield_set_data(dfield, ptr, 4);

	/* 7: SPACE --------------------------*/

	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__SPACE);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, index->space);

	dfield_set_data(dfield, ptr, 4);

	/* 8: PAGE_NO --------------------------*/

	dfield = dtuple_get_nth_field(
		entry, DICT_COL__SYS_INDEXES__PAGE_NO);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));
	mach_write_to_4(ptr, FIL_NULL);

	dfield_set_data(dfield, ptr, 4);

	/*--------------------------------*/

	return(entry);
}

/*****************************************************************//**
Based on an index object, this function builds the entry to be inserted
in the SYS_FIELDS system table.
@return the tuple which should be inserted */
static
dtuple_t*
dict_create_sys_fields_tuple(
/*=========================*/
	const dict_index_t*	index,	/*!< in: index */
	ulint			fld_no,	/*!< in: field number */
	mem_heap_t*		heap)	/*!< in: memory heap from
					which the memory for the built
					tuple is allocated */
{
	dict_table_t*	sys_fields;
	dtuple_t*	entry;
	dict_field_t*	field;
	dfield_t*	dfield;
	byte*		ptr;
	ibool		index_contains_column_prefix_field	= FALSE;
	ulint		j;

	ut_ad(index);
	ut_ad(heap);

	for (j = 0; j < index->n_fields; j++) {
		if (dict_index_get_nth_field(index, j)->prefix_len > 0) {
			index_contains_column_prefix_field = TRUE;
			break;
		}
	}

	field = dict_index_get_nth_field(index, fld_no);

	sys_fields = dict_sys->sys_fields;

	entry = dtuple_create(heap, 3 + DATA_N_SYS_COLS);

	dict_table_copy_types(entry, sys_fields);

	/* 0: INDEX_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_FIELDS__INDEX_ID);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 8));
	mach_write_to_8(ptr, index->id);

	dfield_set_data(dfield, ptr, 8);

	/* 1: POS; FIELD NUMBER & PREFIX LENGTH -----------------------*/

	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_FIELDS__POS);

	ptr = static_cast<byte*>(mem_heap_alloc(heap, 4));

	if (index_contains_column_prefix_field) {
		/* If there are column prefix fields in the index, then
		we store the number of the field to the 2 HIGH bytes
		and the prefix length to the 2 low bytes, */

		mach_write_to_4(ptr, (fld_no << 16) + field->prefix_len);
	} else {
		/* Else we store the number of the field to the 2 LOW bytes.
		This is to keep the storage format compatible with
		InnoDB versions < 4.0.14. */

		mach_write_to_4(ptr, fld_no);
	}

	dfield_set_data(dfield, ptr, 4);

	/* 2: DB_TRX_ID added later */
	/* 3: DB_ROLL_PTR added later */
	/* 4: COL_NAME -------------------------*/
	dfield = dtuple_get_nth_field(entry, DICT_COL__SYS_FIELDS__COL_NAME);

	dfield_set_data(dfield, field->name,
			ut_strlen(field->name));
	/*---------------------------------*/

	return(entry);
}

/*****************************************************************//**
Creates the tuple with which the index entry is searched for writing the index
tree root page number, if such a tree is created.
@return the tuple for search */
static
dtuple_t*
dict_create_search_tuple(
/*=====================*/
	const dtuple_t*	tuple,	/*!< in: the tuple inserted in the SYS_INDEXES
				table */
	mem_heap_t*	heap)	/*!< in: memory heap from which the memory for
				the built tuple is allocated */
{
	dtuple_t*	search_tuple;
	const dfield_t*	field1;
	dfield_t*	field2;

	ut_ad(tuple && heap);

	search_tuple = dtuple_create(heap, 2);

	field1 = dtuple_get_nth_field(tuple, 0);
	field2 = dtuple_get_nth_field(search_tuple, 0);

	dfield_copy(field2, field1);

	field1 = dtuple_get_nth_field(tuple, 1);
	field2 = dtuple_get_nth_field(search_tuple, 1);

	dfield_copy(field2, field1);

	ut_ad(dtuple_validate(search_tuple));

	return(search_tuple);
}

/***************************************************************//**
Builds an index definition row to insert.
@return DB_SUCCESS or error code */
static __attribute__((nonnull, warn_unused_result))
dberr_t
dict_build_index_def_step(
/*======================*/
	que_thr_t*	thr,	/*!< in: query thread */
	ind_node_t*	node)	/*!< in: index create node */
{
	dict_table_t*	table;
	dict_index_t*	index;
	dtuple_t*	row;
	trx_t*		trx;

	ut_ad(mutex_own(&(dict_sys->mutex)));

	trx = thr_get_trx(thr);

	index = node->index;

	table = dict_table_get_low(index->table_name);

	if (table == NULL) {
		return(DB_TABLE_NOT_FOUND);
	}

	if (!trx->table_id) {
		/* Record only the first table id. */
		trx->table_id = table->id;
	}

	node->table = table;

	ut_ad((UT_LIST_GET_LEN(table->indexes) > 0)
	      || dict_index_is_clust(index));

	dict_hdr_get_new_id(NULL, &index->id, NULL, table, false);

	/* Inherit the space id from the table; we store all indexes of a
	table in the same tablespace */

	index->space = table->space;
	node->page_no = FIL_NULL;
	row = dict_create_sys_indexes_tuple(index, node->heap);
	node->ind_row = row;

	ins_node_set_new_row(node->ind_def, row);

	/* Note that the index was created by this transaction. */
	index->trx_id = trx->id;
	ut_ad(table->def_trx_id <= trx->id);
	table->def_trx_id = trx->id;

	return(DB_SUCCESS);
}

/***************************************************************//**
Builds an index definition without updating SYSTEM TABLES.
@return DB_SUCCESS or error code */

void
dict_build_index_def(
/*=================*/
	const dict_table_t*	table,	/*!< in: table */
	dict_index_t*		index,	/*!< in/out: index */
	trx_t*			trx)	/*!< in/out: InnoDB transaction handle */
{
	ut_ad(mutex_own(&dict_sys->mutex));

	if (trx->table_id == 0) {
		/* Record only the first table id. */
		trx->table_id = table->id;
	}

	ut_ad((UT_LIST_GET_LEN(table->indexes) > 0)
	      || dict_index_is_clust(index));

	dict_hdr_get_new_id(NULL, &index->id, NULL, table, false);

	/* Inherit the space id from the table; we store all indexes of a
	table in the same tablespace */

	index->space = table->space;

	/* Note that the index was created by this transaction. */
	index->trx_id = trx->id;
}

/***************************************************************//**
Builds a field definition row to insert. */
static
void
dict_build_field_def_step(
/*======================*/
	ind_node_t*	node)	/*!< in: index create node */
{
	dict_index_t*	index;
	dtuple_t*	row;

	index = node->index;

	row = dict_create_sys_fields_tuple(index, node->field_no, node->heap);

	ins_node_set_new_row(node->field_def, row);
}

/***************************************************************//**
Creates an index tree for the index if it is not a member of a cluster.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */
static __attribute__((nonnull, warn_unused_result))
dberr_t
dict_create_index_tree_step(
/*========================*/
	ind_node_t*	node)	/*!< in: index create node */
{
	mtr_t		mtr;
	btr_pcur_t	pcur;
	dict_index_t*	index;
	dict_table_t*	sys_indexes;
	dtuple_t*	search_tuple;

	ut_ad(mutex_own(&(dict_sys->mutex)));

	index = node->index;

	sys_indexes = dict_sys->sys_indexes;

	if (index->type == DICT_FTS) {
		/* FTS index does not need an index tree */
		return(DB_SUCCESS);
	}

	/* Run a mini-transaction in which the index tree is allocated for
	the index and its root address is written to the index entry in
	sys_indexes */

	mtr_start(&mtr);

	search_tuple = dict_create_search_tuple(node->ind_row, node->heap);

	btr_pcur_open(UT_LIST_GET_FIRST(sys_indexes->indexes),
		      search_tuple, PAGE_CUR_L, BTR_MODIFY_LEAF,
		      &pcur, &mtr);

	btr_pcur_move_to_next_user_rec(&pcur, &mtr);


	dberr_t		err = DB_SUCCESS;

	if (node->index->table->ibd_file_missing
	    || dict_table_is_discarded(node->index->table)) {

		node->page_no = FIL_NULL;
	} else {
		node->page_no = btr_create(
			index->type, index->space,
			dict_table_page_size(index->table),
			index->id, index, NULL, &mtr);

		if (node->page_no == FIL_NULL) {
			err = DB_OUT_OF_FILE_SPACE;
		}

		DBUG_EXECUTE_IF("ib_import_create_index_failure_1",
				node->page_no = FIL_NULL;
				err = DB_OUT_OF_FILE_SPACE; );
	}

	page_rec_write_field(
		btr_pcur_get_rec(&pcur), DICT_FLD__SYS_INDEXES__PAGE_NO,
		node->page_no, &mtr);

	btr_pcur_close(&pcur);

	mtr_commit(&mtr);

	return(err);
}

/***************************************************************//**
Creates an index tree for the index if it is not a member of a cluster.
Don't update SYSTEM TABLES.
@return DB_SUCCESS or DB_OUT_OF_FILE_SPACE */

dberr_t
dict_create_index_tree_in_mem(
/*==========================*/
	dict_index_t*	index,	/*!< in/out: index */
	const trx_t*	trx)	/*!< in: InnoDB transaction handle */
{
	mtr_t		mtr;
	ulint		page_no = FIL_NULL;

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(dict_table_is_temporary(index->table));

	if (index->type == DICT_FTS) {
		/* FTS index does not need an index tree */
		return(DB_SUCCESS);
	}

	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	dberr_t		err = DB_SUCCESS;

	/* Currently this function is being used by temp-tables only.
	Import/Discard of temp-table is blocked and so this assert. */
	ut_ad(index->table->ibd_file_missing == 0
	      && !dict_table_is_discarded(index->table));

	page_no = btr_create(
		index->type, index->space,
		dict_table_page_size(index->table),
		index->id, index, NULL, &mtr);

	index->page = page_no;
	index->trx_id = trx->id;

	if (page_no == FIL_NULL) {
		err = DB_OUT_OF_FILE_SPACE;
	}

	mtr_commit(&mtr);

	return(err);
}

/*******************************************************************//**
Drops the index tree associated with a row in SYS_INDEXES table.
@return index root page number of FIL_NULL if it was already freed. */

ulint
dict_drop_index_tree(
/*=================*/
	rec_t*		rec,		/*!< in/out: record in the clustered
					index of SYS_INDEXES table */
	btr_pcur_t*	pcur,		/*!< in/out: persistent cursor pointing
					to record in the clustered index of
					SYS_INDEXES table. The cursor may be
					repositioned in this call. */
	bool		is_drop,	/*!< in: true if we are dropping
					a table */
	mtr_t*		mtr)		/*!< in/out: mtr having the latch on
					the record page */
{
	const byte*	ptr;
	ulint		len;
	ulint		space;
	ulint		root_page_no;

	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_a(!dict_table_is_comp(dict_sys->sys_indexes));

	ptr = rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);

	ut_ad(len == 4);

	root_page_no = mtr_read_ulint(ptr, MLOG_4BYTES, mtr);

	if (root_page_no == FIL_NULL) {
		/* The tree has already been freed */

		return(root_page_no);
	}

	ptr = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__SPACE, &len);

	ut_ad(len == 4);

	space = mtr_read_ulint(ptr, MLOG_4BYTES, mtr);

	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(space,
								  &found));

	if (!found) {
		/* It is a single table tablespace and the .ibd file is
		missing: do nothing */

		return(FIL_NULL);
	}

	if (is_drop && fil_index_tree_is_freed(space, root_page_no,
					       page_size)) {
		/* The tree has already been freed but not marked */
		page_rec_write_field(
			rec, DICT_FLD__SYS_INDEXES__PAGE_NO,
			FIL_NULL, mtr);
		return(FIL_NULL);
	}

	/* We free all the pages but the root page first; this operation
	may span several mini-transactions */

	const page_id_t	root_page_id(space, root_page_no);

	btr_free_but_not_root(root_page_id, page_size, mtr_get_log_mode(mtr));

	/* Then we free the root page in the same mini-transaction where
	we write FIL_NULL to the appropriate field in the SYS_INDEXES
	record: this mini-transaction marks the B-tree totally freed */
	btr_block_get(root_page_id, page_size, RW_X_LATCH, NULL, mtr);
	/* printf("Dropping index tree in space %lu root page %lu\n", space,
	root_page_no); */
	btr_free_root(root_page_id, page_size, mtr);

	if (is_drop) {
		page_rec_write_field(
			rec, DICT_FLD__SYS_INDEXES__PAGE_NO,
			FIL_NULL, mtr);
	}

	btr_pcur_store_position(pcur, mtr);

	return(root_page_no);
}

/*******************************************************************//**
Drops the index tree but don't update SYS_INDEXES table. */

void
dict_drop_index_tree_in_mem(
/*========================*/
	const dict_index_t*	index,		/*!< in: index */
	ulint			page_no)	/*!< in: index page-no */
{
	mtr_t		mtr;

	ut_ad(mutex_own(&dict_sys->mutex));

	mtr_start(&mtr);

	dict_disable_redo_if_temporary(index->table, &mtr);

	ulint			root_page_no = page_no;
	ulint			space = index->space;
	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(space,
								  &found));

	/* If tree has already been freed or it is a single table
	tablespace and the .ibd file is missing do nothing,
	else free the all the pages */
	if (root_page_no != FIL_NULL && found) {

		const page_id_t	root_page_id(space, root_page_no);

		/* We free all the pages but the root page first; this operation
		may span several mini-transactions */
		btr_free_but_not_root(
			root_page_id, page_size,
			(dict_table_is_temporary(index->table)
			 ? MTR_LOG_NO_REDO : mtr_get_log_mode(&mtr)));

		/* Then we free the root page. */
		btr_free_root(root_page_id, page_size, &mtr);
	}

	mtr_commit(&mtr);
}

/*******************************************************************//**
Recreate the index tree associated with a row in SYS_INDEXES table.
@return	new root page number, or FIL_NULL on failure */

ulint
dict_recreate_index_tree(
/*=====================*/
	const dict_table_t*
			table,	/*!< in/out: the table the index belongs to */
	btr_pcur_t*	pcur,	/*!< in/out: persistent cursor pointing to
				record in the clustered index of
				SYS_INDEXES table. The cursor may be
				repositioned in this call. */
	mtr_t*		mtr)	/*!< in/out: mtr having the latch
				on the record page. */
{
	ut_ad(mutex_own(&dict_sys->mutex));
	ut_a(!dict_table_is_comp(dict_sys->sys_indexes));

	ulint		len;
	rec_t*		rec = btr_pcur_get_rec(pcur);

	const byte*	ptr = rec_get_nth_field_old(
		rec, DICT_FLD__SYS_INDEXES__PAGE_NO, &len);

	ut_ad(len == 4);

	ulint	root_page_no = mtr_read_ulint(ptr, MLOG_4BYTES, mtr);

	ptr = rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__SPACE, &len);
	ut_ad(len == 4);

	ut_a(table->space == mtr_read_ulint(ptr, MLOG_4BYTES, mtr));

	ulint			space = table->space;
	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(space,
								  &found));

	if (!found) {
		/* It is a single table tablespae and the .ibd file is
		missing: do nothing. */

		ib_logf(IB_LOG_LEVEL_WARN,
			"Trying to TRUNCATE a missing .ibd file of table"
			" %s!", table->name);

		return(FIL_NULL);
	}

	ptr = rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__TYPE, &len);
	ut_ad(len == 4);
	ulint	type = mach_read_from_4(ptr);

	ptr = rec_get_nth_field_old(rec, DICT_FLD__SYS_INDEXES__ID, &len);
	ut_ad(len == 8);
	index_id_t	index_id = mach_read_from_8(ptr);

	/* We will need to commit the mini-transaction in order to avoid
	deadlocks in the btr_create() call, because otherwise we would
	be freeing and allocating pages in the same mini-transaction. */
	btr_pcur_store_position(pcur, mtr);
	mtr_commit(mtr);

	mtr_start(mtr);
	btr_pcur_restore_position(BTR_MODIFY_LEAF, pcur, mtr);

	/* Find the index corresponding to this SYS_INDEXES record. */
	for (dict_index_t* index = UT_LIST_GET_FIRST(table->indexes);
	     index != NULL;
	     index = UT_LIST_GET_NEXT(indexes, index)) {
		if (index->id == index_id) {
			if (index->type & DICT_FTS) {
				return(FIL_NULL);
			} else {
				root_page_no = btr_create(
					type, space, page_size, index_id,
					index, NULL, mtr);
				index->page = (unsigned int) root_page_no;
				return(root_page_no);
			}
		}
	}

	ib_logf(IB_LOG_LEVEL_ERROR,
		"Failed to create index with index id " IB_ID_FMT
		" of table '%s'", index_id, table->name);

	return(FIL_NULL);
}

/*******************************************************************//**
Truncates the index tree but don't update SYSTEM TABLES.
@return DB_SUCCESS or error */

dberr_t
dict_truncate_index_tree_in_mem(
/*============================*/
	dict_index_t*	index)		/*!< in/out: index */
{
	mtr_t		mtr;
	bool		truncate;
	ulint		space = index->space;

	ut_ad(mutex_own(&dict_sys->mutex));
	ut_ad(dict_table_is_temporary(index->table));

	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	ulint		type = index->type;
	ulint		root_page_no = index->page;

	if (root_page_no == FIL_NULL) {

		/* The tree has been freed. */
		ib_logf(IB_LOG_LEVEL_WARN,
			"Trying to TRUNCATE a missing index of table %s!",
			index->table->name);

		truncate = false;
	} else {
		truncate = true;
	}

	bool			found;
	const page_size_t	page_size(fil_space_get_page_size(space,
								  &found));

	if (!found) {

		/* It is a single table tablespace and the .ibd file is
		missing: do nothing */

		ib_logf(IB_LOG_LEVEL_WARN,
			"Trying to TRUNCATE a missing .ibd file of table %s!",
			index->table->name);
	}

	/* If table to truncate resides in its on own tablespace that will
	be re-created on truncate then we can ignore freeing of existing
	tablespace objects. */

	if (truncate) {

		/* We free all the pages but the root page first; this operation
		may span several mini-transactions */

		const page_id_t	root_page_id(space, root_page_no);

		btr_free_but_not_root(
			root_page_id, page_size,
			(dict_table_is_temporary(index->table)
			 ? MTR_LOG_NO_REDO : mtr_get_log_mode(&mtr)));

		/* Then we free the root page in the same mini-transaction where
		we create the b-tree and write its new root page number to the
		appropriate field in the SYS_INDEXES record: this
		mini-transaction marks the B-tree totally truncated */

		btr_block_get(root_page_id, page_size, RW_X_LATCH, NULL, &mtr);

		btr_free_root(root_page_id, page_size, &mtr);
	}

	mtr_commit(&mtr);

	mtr_start(&mtr);
	mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

	root_page_no = btr_create(
		type, space, page_size, index->id, index, NULL, &mtr);

	DBUG_EXECUTE_IF("ib_err_trunc_temp_recreate_index",
			root_page_no = FIL_NULL;);

	index->page = root_page_no;

	mtr_commit(&mtr);

	return(index->page == FIL_NULL ? DB_ERROR : DB_SUCCESS);
}

/*********************************************************************//**
Creates a table create graph.
@return own: table create node */

tab_node_t*
tab_create_graph_create(
/*====================*/
	dict_table_t*	table,	/*!< in: table to create, built as a memory data
				structure */
	mem_heap_t*	heap,	/*!< in: heap where created */
	bool		commit)	/*!< in: true if the commit node should be
				added to the query graph */
{
	tab_node_t*	node;

	node = static_cast<tab_node_t*>(
		mem_heap_alloc(heap, sizeof(tab_node_t)));

	node->common.type = QUE_NODE_CREATE_TABLE;

	node->table = table;

	node->state = TABLE_BUILD_TABLE_DEF;
	node->heap = mem_heap_create(256);

	node->tab_def = ins_node_create(INS_DIRECT, dict_sys->sys_tables,
					heap);
	node->tab_def->common.parent = node;

	node->col_def = ins_node_create(INS_DIRECT, dict_sys->sys_columns,
					heap);
	node->col_def->common.parent = node;

	if (commit) {
		node->commit_node = trx_commit_node_create(heap);
		node->commit_node->common.parent = node;
	} else {
		node->commit_node = 0;
	}

	return(node);
}

/*********************************************************************//**
Creates an index create graph.
@return own: index create node */

ind_node_t*
ind_create_graph_create(
/*====================*/
	dict_index_t*	index,	/*!< in: index to create, built as a memory data
				structure */
	mem_heap_t*	heap,	/*!< in: heap where created */
	bool		commit)	/*!< in: true if the commit node should be
				added to the query graph */
{
	ind_node_t*	node;

	node = static_cast<ind_node_t*>(
		mem_heap_alloc(heap, sizeof(ind_node_t)));

	node->common.type = QUE_NODE_CREATE_INDEX;

	node->index = index;

	node->state = INDEX_BUILD_INDEX_DEF;
	node->page_no = FIL_NULL;
	node->heap = mem_heap_create(256);

	node->ind_def = ins_node_create(INS_DIRECT,
					dict_sys->sys_indexes, heap);
	node->ind_def->common.parent = node;

	node->field_def = ins_node_create(INS_DIRECT,
					  dict_sys->sys_fields, heap);
	node->field_def->common.parent = node;

	if (commit) {
		node->commit_node = trx_commit_node_create(heap);
		node->commit_node->common.parent = node;
	} else {
		node->commit_node = 0;
	}

	return(node);
}

/***********************************************************//**
Creates a table. This is a high-level function used in SQL execution graphs.
@return query thread to run next or NULL */

que_thr_t*
dict_create_table_step(
/*===================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	tab_node_t*	node;
	dberr_t		err	= DB_ERROR;
	trx_t*		trx;

	ut_ad(thr);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	trx = thr_get_trx(thr);

	node = static_cast<tab_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_CREATE_TABLE);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = TABLE_BUILD_TABLE_DEF;
	}

	if (node->state == TABLE_BUILD_TABLE_DEF) {

		/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */

		err = dict_build_table_def_step(thr, node);

		if (err != DB_SUCCESS) {

			goto function_exit;
		}

		node->state = TABLE_BUILD_COL_DEF;
		node->col_no = 0;

		thr->run_node = node->tab_def;

		return(thr);
	}

	if (node->state == TABLE_BUILD_COL_DEF) {

		if (node->col_no < (node->table)->n_def) {

			dict_build_col_def_step(node);

			node->col_no++;

			thr->run_node = node->col_def;

			return(thr);
		} else {
			node->state = TABLE_COMMIT_WORK;
		}
	}

	if (node->state == TABLE_COMMIT_WORK) {

		/* Table was correctly defined: do NOT commit the transaction
		(CREATE TABLE does NOT do an implicit commit of the current
		transaction) */

		node->state = TABLE_ADD_TO_CACHE;

		/* thr->run_node = node->commit_node;

		return(thr); */
		DBUG_EXECUTE_IF("ib_ddl_crash_during_create", DBUG_SUICIDE(););
	}

	if (node->state == TABLE_ADD_TO_CACHE) {

		dict_table_add_to_cache(node->table, TRUE, node->heap);

		err = DB_SUCCESS;
	}

function_exit:
	trx->error_state = err;

	if (err == DB_SUCCESS) {
		/* Ok: do nothing */

	} else if (err == DB_LOCK_WAIT) {

		return(NULL);
	} else {
		/* SQL error detected */

		return(NULL);
	}

	thr->run_node = que_node_get_parent(node);

	return(thr);
}

/***********************************************************//**
Creates an index. This is a high-level function used in SQL execution
graphs.
@return query thread to run next or NULL */

que_thr_t*
dict_create_index_step(
/*===================*/
	que_thr_t*	thr)	/*!< in: query thread */
{
	ind_node_t*	node;
	dberr_t		err	= DB_ERROR;
	trx_t*		trx;

	ut_ad(thr);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	trx = thr_get_trx(thr);

	node = static_cast<ind_node_t*>(thr->run_node);

	ut_ad(que_node_get_type(node) == QUE_NODE_CREATE_INDEX);

	if (thr->prev_node == que_node_get_parent(node)) {
		node->state = INDEX_BUILD_INDEX_DEF;
	}

	if (node->state == INDEX_BUILD_INDEX_DEF) {
		/* DO THE CHECKS OF THE CONSISTENCY CONSTRAINTS HERE */
		err = dict_build_index_def_step(thr, node);

		if (err != DB_SUCCESS) {

			goto function_exit;
		}

		node->state = INDEX_BUILD_FIELD_DEF;
		node->field_no = 0;

		thr->run_node = node->ind_def;

		return(thr);
	}

	if (node->state == INDEX_BUILD_FIELD_DEF) {

		if (node->field_no < (node->index)->n_fields) {

			dict_build_field_def_step(node);

			node->field_no++;

			thr->run_node = node->field_def;

			return(thr);
		} else {
			node->state = INDEX_ADD_TO_CACHE;
		}
	}

	if (node->state == INDEX_ADD_TO_CACHE) {

		index_id_t	index_id = node->index->id;

		err = dict_index_add_to_cache(
			node->table, node->index, FIL_NULL,
			trx_is_strict(trx)
			|| dict_table_get_format(node->table)
			>= UNIV_FORMAT_B);

		node->index = dict_index_get_if_in_cache_low(index_id);
		ut_a(!node->index == (err != DB_SUCCESS));

		if (err != DB_SUCCESS) {

			goto function_exit;
		}

		node->state = INDEX_CREATE_INDEX_TREE;
	}

	if (node->state == INDEX_CREATE_INDEX_TREE) {

		err = dict_create_index_tree_step(node);

		DBUG_EXECUTE_IF("ib_dict_create_index_tree_fail",
				err = DB_OUT_OF_MEMORY;);

		if (err != DB_SUCCESS) {
			/* If this is a FTS index, we will need to remove
			it from fts->cache->indexes list as well */
			if ((node->index->type & DICT_FTS)
			    && node->table->fts) {
				fts_index_cache_t*	index_cache;

				rw_lock_x_lock(
					&node->table->fts->cache->init_lock);

				index_cache = (fts_index_cache_t*)
					 fts_find_index_cache(
						node->table->fts->cache,
						node->index);

				if (index_cache->words) {
					rbt_free(index_cache->words);
					index_cache->words = 0;
				}

				ib_vector_remove(
					node->table->fts->cache->indexes,
					*reinterpret_cast<void**>(index_cache));

				rw_lock_x_unlock(
					&node->table->fts->cache->init_lock);
			}

			dict_index_remove_from_cache(node->table, node->index);
			node->index = NULL;

			goto function_exit;
		}

		node->index->page = node->page_no;
		/* These should have been set in
		dict_build_index_def_step() and
		dict_index_add_to_cache(). */
		ut_ad(node->index->trx_id == trx->id);
		ut_ad(node->index->table->def_trx_id == trx->id);
		node->state = INDEX_COMMIT_WORK;
	}

	if (node->state == INDEX_COMMIT_WORK) {

		/* Index was correctly defined: do NOT commit the transaction
		(CREATE INDEX does NOT currently do an implicit commit of
		the current transaction) */

		node->state = INDEX_CREATE_INDEX_TREE;

		/* thr->run_node = node->commit_node;

		return(thr); */
	}

function_exit:
	trx->error_state = err;

	if (err == DB_SUCCESS) {
		/* Ok: do nothing */

	} else if (err == DB_LOCK_WAIT) {

		return(NULL);
	} else {
		/* SQL error detected */

		return(NULL);
	}

	thr->run_node = que_node_get_parent(node);

	return(thr);
}

/****************************************************************//**
Check whether a system table exists.  Additionally, if it exists,
move it to the non-LRU end of the table LRU list.  This is oly used
for system tables that can be upgraded or added to an older database,
which include SYS_FOREIGN, SYS_FOREIGN_COLS, SYS_TABLESPACES and
SYS_DATAFILES.
@return DB_SUCCESS if the sys table exists, DB_CORRUPTION if it exists
but is not current, DB_TABLE_NOT_FOUND if it does not exist*/
static
dberr_t
dict_check_if_system_table_exists(
/*==============================*/
	const char*	tablename,	/*!< in: name of table */
	ulint		num_fields,	/*!< in: number of fields */
	ulint		num_indexes)	/*!< in: number of indexes */
{
	dict_table_t*	sys_table;
	dberr_t		error = DB_SUCCESS;

	ut_a(srv_get_active_thread_type() == SRV_NONE);

	mutex_enter(&dict_sys->mutex);

	sys_table = dict_table_get_low(tablename);

	if (sys_table == NULL) {
		error = DB_TABLE_NOT_FOUND;

	} else if (UT_LIST_GET_LEN(sys_table->indexes) != num_indexes
		   || sys_table->n_cols != num_fields) {
		error = DB_CORRUPTION;

	} else {
		/* This table has already been created, and it is OK.
		Ensure that it can't be evicted from the table LRU cache. */

		dict_table_move_from_lru_to_non_lru(sys_table);
	}

	mutex_exit(&dict_sys->mutex);

	return(error);
}

/****************************************************************//**
Creates the foreign key constraints system tables inside InnoDB
at server bootstrap or server start if they are not found or are
not of the right form.
@return DB_SUCCESS or error code */

dberr_t
dict_create_or_check_foreign_constraint_tables(void)
/*================================================*/
{
	trx_t*		trx;
	my_bool		srv_file_per_table_backup;
	dberr_t		err;
	dberr_t		sys_foreign_err;
	dberr_t		sys_foreign_cols_err;

	ut_a(srv_get_active_thread_type() == SRV_NONE);

	/* Note: The master thread has not been started at this point. */


	sys_foreign_err = dict_check_if_system_table_exists(
		"SYS_FOREIGN", DICT_NUM_FIELDS__SYS_FOREIGN + 1, 3);
	sys_foreign_cols_err = dict_check_if_system_table_exists(
		"SYS_FOREIGN_COLS", DICT_NUM_FIELDS__SYS_FOREIGN_COLS + 1, 1);

	if (sys_foreign_err == DB_SUCCESS
	    && sys_foreign_cols_err == DB_SUCCESS) {
		return(DB_SUCCESS);
	}

	trx = trx_allocate_for_mysql();

	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	trx->op_info = "creating foreign key sys tables";

	row_mysql_lock_data_dictionary(trx);

	/* Check which incomplete table definition to drop. */

	if (sys_foreign_err == DB_CORRUPTION) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Dropping incompletely created"
			" SYS_FOREIGN table.");
		row_drop_table_for_mysql("SYS_FOREIGN", trx, TRUE);
	}

	if (sys_foreign_cols_err == DB_CORRUPTION) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Dropping incompletely created"
			" SYS_FOREIGN_COLS table.");

		row_drop_table_for_mysql("SYS_FOREIGN_COLS", trx, TRUE);
	}

	ib_logf(IB_LOG_LEVEL_WARN,
		"Creating foreign key constraint system tables.");

	/* NOTE: in dict_load_foreigns we use the fact that
	there are 2 secondary indexes on SYS_FOREIGN, and they
	are defined just like below */

	/* NOTE: when designing InnoDB's foreign key support in 2001, we made
	an error and made the table names and the foreign key id of type
	'CHAR' (internally, really a VARCHAR). We should have made the type
	VARBINARY, like in other InnoDB system tables, to get a clean
	design. */

	srv_file_per_table_backup = srv_file_per_table;

	/* We always want SYSTEM tables to be created inside the system
	tablespace. */

	srv_file_per_table = 0;

	err = que_eval_sql(
		NULL,
		"PROCEDURE CREATE_FOREIGN_SYS_TABLES_PROC () IS\n"
		"BEGIN\n"
		"CREATE TABLE\n"
		"SYS_FOREIGN(ID CHAR, FOR_NAME CHAR,"
		" REF_NAME CHAR, N_COLS INT);\n"
		"CREATE UNIQUE CLUSTERED INDEX ID_IND"
		" ON SYS_FOREIGN (ID);\n"
		"CREATE INDEX FOR_IND"
		" ON SYS_FOREIGN (FOR_NAME);\n"
		"CREATE INDEX REF_IND"
		" ON SYS_FOREIGN (REF_NAME);\n"
		"CREATE TABLE\n"
		"SYS_FOREIGN_COLS(ID CHAR, POS INT,"
		" FOR_COL_NAME CHAR, REF_COL_NAME CHAR);\n"
		"CREATE UNIQUE CLUSTERED INDEX ID_IND"
		" ON SYS_FOREIGN_COLS (ID, POS);\n"
		"END;\n",
		FALSE, trx);

	if (err != DB_SUCCESS) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Creation of SYS_FOREIGN and SYS_FOREIGN_COLS"
			" has failed with error %lu. Tablespace is full."
			" Dropping incompletely created tables.",
			(ulong) err);

		ut_ad(err == DB_OUT_OF_FILE_SPACE
		      || err == DB_TOO_MANY_CONCURRENT_TRXS);

		row_drop_table_for_mysql("SYS_FOREIGN", trx, TRUE);
		row_drop_table_for_mysql("SYS_FOREIGN_COLS", trx, TRUE);

		if (err == DB_OUT_OF_FILE_SPACE) {
			err = DB_MUST_GET_MORE_FILE_SPACE;
		}
	}

	trx_commit_for_mysql(trx);

	row_mysql_unlock_data_dictionary(trx);

	trx_free_for_mysql(trx);

	srv_file_per_table = srv_file_per_table_backup;

	if (err == DB_SUCCESS) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Foreign key constraint system tables created");
	}

	/* Note: The master thread has not been started at this point. */
	/* Confirm and move to the non-LRU part of the table LRU list. */
	sys_foreign_err = dict_check_if_system_table_exists(
		"SYS_FOREIGN", DICT_NUM_FIELDS__SYS_FOREIGN + 1, 3);
	ut_a(sys_foreign_err == DB_SUCCESS);

	sys_foreign_cols_err = dict_check_if_system_table_exists(
		"SYS_FOREIGN_COLS", DICT_NUM_FIELDS__SYS_FOREIGN_COLS + 1, 1);
	ut_a(sys_foreign_cols_err == DB_SUCCESS);

	return(err);
}

/****************************************************************//**
Evaluate the given foreign key SQL statement.
@return error code or DB_SUCCESS */
static __attribute__((nonnull, warn_unused_result))
dberr_t
dict_foreign_eval_sql(
/*==================*/
	pars_info_t*	info,	/*!< in: info struct */
	const char*	sql,	/*!< in: SQL string to evaluate */
	const char*	name,	/*!< in: table name (for diagnostics) */
	const char*	id,	/*!< in: foreign key id */
	trx_t*		trx)	/*!< in/out: transaction */
{
	dberr_t	error;
	FILE*	ef	= dict_foreign_err_file;

	error = que_eval_sql(info, sql, FALSE, trx);

	if (error == DB_DUPLICATE_KEY) {
		mutex_enter(&dict_foreign_err_mutex);
		rewind(ef);
		ut_print_timestamp(ef);
		fputs(" Error in foreign key constraint creation for table ",
		      ef);
		ut_print_name(ef, trx, TRUE, name);
		fputs(".\nA foreign key constraint of name ", ef);
		ut_print_name(ef, trx, TRUE, id);
		fputs("\nalready exists."
		      " (Note that internally InnoDB adds 'databasename'\n"
		      "in front of the user-defined constraint name.)\n"
		      "Note that InnoDB's FOREIGN KEY system tables store\n"
		      "constraint names as case-insensitive, with the\n"
		      "MySQL standard latin1_swedish_ci collation. If you\n"
		      "create tables or databases whose names differ only in\n"
		      "the character case, then collisions in constraint\n"
		      "names can occur. Workaround: name your constraints\n"
		      "explicitly with unique names.\n",
		      ef);

		mutex_exit(&dict_foreign_err_mutex);

		return(error);
	}

	if (error != DB_SUCCESS) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Foreign key constraint creation failed:"
			" internal error number %lu", (ulong) error);

		mutex_enter(&dict_foreign_err_mutex);
		ut_print_timestamp(ef);
		fputs(" Internal error in foreign key constraint creation"
		      " for table ", ef);
		ut_print_name(ef, trx, TRUE, name);
		fputs(".\n"
		      "See the MySQL .err log in the datadir"
		      " for more information.\n", ef);
		mutex_exit(&dict_foreign_err_mutex);

		return(error);
	}

	return(DB_SUCCESS);
}

/********************************************************************//**
Add a single foreign key field definition to the data dictionary tables in
the database.
@return error code or DB_SUCCESS */
static __attribute__((nonnull, warn_unused_result))
dberr_t
dict_create_add_foreign_field_to_dictionary(
/*========================================*/
	ulint			field_nr,	/*!< in: field number */
	const char*		table_name,	/*!< in: table name */
	const dict_foreign_t*	foreign,	/*!< in: foreign */
	trx_t*			trx)		/*!< in/out: transaction */
{
	DBUG_ENTER("dict_create_add_foreign_field_to_dictionary");

	pars_info_t*	info = pars_info_create();

	pars_info_add_str_literal(info, "id", foreign->id);

	pars_info_add_int4_literal(info, "pos", field_nr);

	pars_info_add_str_literal(info, "for_col_name",
				  foreign->foreign_col_names[field_nr]);

	pars_info_add_str_literal(info, "ref_col_name",
				  foreign->referenced_col_names[field_nr]);

	DBUG_RETURN(dict_foreign_eval_sql(
		       info,
		       "PROCEDURE P () IS\n"
		       "BEGIN\n"
		       "INSERT INTO SYS_FOREIGN_COLS VALUES"
		       "(:id, :pos, :for_col_name, :ref_col_name);\n"
		       "END;\n",
		       table_name, foreign->id, trx));
}

/********************************************************************//**
Add a foreign key definition to the data dictionary tables.
@return error code or DB_SUCCESS */

dberr_t
dict_create_add_foreign_to_dictionary(
/*==================================*/
	const char*		name,	/*!< in: table name */
	const dict_foreign_t*	foreign,/*!< in: foreign key */
	trx_t*			trx)	/*!< in/out: dictionary transaction */
{
	dberr_t		error;

	DBUG_ENTER("dict_create_add_foreign_to_dictionary");

	pars_info_t*	info = pars_info_create();

	pars_info_add_str_literal(info, "id", foreign->id);

	pars_info_add_str_literal(info, "for_name", name);

	pars_info_add_str_literal(info, "ref_name",
				  foreign->referenced_table_name);

	pars_info_add_int4_literal(info, "n_cols",
				   foreign->n_fields + (foreign->type << 24));

	DBUG_PRINT("dict_create_add_foreign_to_dictionary",
		   ("'%s', '%s', '%s', %d", foreign->id, name,
		    foreign->referenced_table_name,
		    foreign->n_fields + (foreign->type << 24)));

	error = dict_foreign_eval_sql(info,
				      "PROCEDURE P () IS\n"
				      "BEGIN\n"
				      "INSERT INTO SYS_FOREIGN VALUES"
				      "(:id, :for_name, :ref_name, :n_cols);\n"
				      "END;\n"
				      , name, foreign->id, trx);

	if (error != DB_SUCCESS) {

		DBUG_RETURN(error);
	}

	for (ulint i = 0; i < foreign->n_fields; i++) {
		error = dict_create_add_foreign_field_to_dictionary(
			i, name, foreign, trx);

		if (error != DB_SUCCESS) {

			DBUG_RETURN(error);
		}
	}

	DBUG_RETURN(error);
}

/********************************************************************//**
Adds foreign key definitions to data dictionary tables in the database.
@return error code or DB_SUCCESS */

dberr_t
dict_create_add_foreigns_to_dictionary(
/*===================================*/
	ulint		start_id,/*!< in: if we are actually doing ALTER TABLE
				ADD CONSTRAINT, we want to generate constraint
				numbers which are bigger than in the table so
				far; we number the constraints from
				start_id + 1 up; start_id should be set to 0 if
				we are creating a new table, or if the table
				so far has no constraints for which the name
				was generated here */
	dict_table_t*	table,	/*!< in: table */
	trx_t*		trx)	/*!< in: transaction */
{
	dict_foreign_t*	foreign;
	ulint		number	= start_id + 1;
	dberr_t		error;

	ut_ad(mutex_own(&(dict_sys->mutex)));

	if (NULL == dict_table_get_low("SYS_FOREIGN")) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Table SYS_FOREIGN not found"
			" in internal data dictionary");

		return(DB_ERROR);
	}

	for (foreign = UT_LIST_GET_FIRST(table->foreign_list);
	     foreign;
	     foreign = UT_LIST_GET_NEXT(foreign_list, foreign)) {

		error = dict_create_add_foreign_id(&number, table->name,
						   foreign);

		if (error != DB_SUCCESS) {

			return(error);
		}

		error = dict_create_add_foreign_to_dictionary(table->name,
							      foreign, trx);

		if (error != DB_SUCCESS) {

			return(error);
		}
	}

	trx->op_info = "committing foreign key definitions";

	if (trx->state != TRX_STATE_NOT_STARTED) {
		trx_commit(trx);
	}

	trx->op_info = "";

	return(DB_SUCCESS);
}

/****************************************************************//**
Creates the tablespaces and datafiles system tables inside InnoDB
at server bootstrap or server start if they are not found or are
not of the right form.
@return DB_SUCCESS or error code */

dberr_t
dict_create_or_check_sys_tablespace(void)
/*=====================================*/
{
	trx_t*		trx;
	my_bool		srv_file_per_table_backup;
	dberr_t		err;
	dberr_t		sys_tablespaces_err;
	dberr_t		sys_datafiles_err;

	ut_a(srv_get_active_thread_type() == SRV_NONE);

	/* Note: The master thread has not been started at this point. */

	sys_tablespaces_err = dict_check_if_system_table_exists(
		"SYS_TABLESPACES", DICT_NUM_FIELDS__SYS_TABLESPACES + 1, 1);
	sys_datafiles_err = dict_check_if_system_table_exists(
		"SYS_DATAFILES", DICT_NUM_FIELDS__SYS_DATAFILES + 1, 1);

	if (sys_tablespaces_err == DB_SUCCESS
	    && sys_datafiles_err == DB_SUCCESS) {
		return(DB_SUCCESS);
	}

	trx = trx_allocate_for_mysql();

	trx_set_dict_operation(trx, TRX_DICT_OP_TABLE);

	trx->op_info = "creating tablepace and datafile sys tables";

	row_mysql_lock_data_dictionary(trx);

	/* Check which incomplete table definition to drop. */

	if (sys_tablespaces_err == DB_CORRUPTION) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Dropping incompletely created"
			" SYS_TABLESPACES table.");
		row_drop_table_for_mysql("SYS_TABLESPACES", trx, TRUE);
	}

	if (sys_datafiles_err == DB_CORRUPTION) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Dropping incompletely created"
			" SYS_DATAFILES table.");

		row_drop_table_for_mysql("SYS_DATAFILES", trx, TRUE);
	}

	ib_logf(IB_LOG_LEVEL_INFO,
		"Creating tablespace and datafile system tables.");

	/* We always want SYSTEM tables to be created inside the system
	tablespace. */
	srv_file_per_table_backup = srv_file_per_table;
	srv_file_per_table = 0;

	err = que_eval_sql(
		NULL,
		"PROCEDURE CREATE_SYS_TABLESPACE_PROC () IS\n"
		"BEGIN\n"
		"CREATE TABLE SYS_TABLESPACES(\n"
		" SPACE INT, NAME CHAR, FLAGS INT);\n"
		"CREATE UNIQUE CLUSTERED INDEX SYS_TABLESPACES_SPACE"
		" ON SYS_TABLESPACES (SPACE);\n"
		"CREATE TABLE SYS_DATAFILES(\n"
		" SPACE INT, PATH CHAR);\n"
		"CREATE UNIQUE CLUSTERED INDEX SYS_DATAFILES_SPACE"
		" ON SYS_DATAFILES (SPACE);\n"
		"END;\n",
		FALSE, trx);

	if (err != DB_SUCCESS) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Creation of SYS_TABLESPACES and SYS_DATAFILES"
			" has failed with error %lu. Tablespace is full."
			" Dropping incompletely created tables.",
			(ulong) err);

		ut_a(err == DB_OUT_OF_FILE_SPACE
		     || err == DB_TOO_MANY_CONCURRENT_TRXS);

		row_drop_table_for_mysql("SYS_TABLESPACES", trx, TRUE);
		row_drop_table_for_mysql("SYS_DATAFILES", trx, TRUE);

		if (err == DB_OUT_OF_FILE_SPACE) {
			err = DB_MUST_GET_MORE_FILE_SPACE;
		}
	}

	trx_commit_for_mysql(trx);

	row_mysql_unlock_data_dictionary(trx);

	trx_free_for_mysql(trx);

	srv_file_per_table = srv_file_per_table_backup;

	if (err == DB_SUCCESS) {
		ib_logf(IB_LOG_LEVEL_INFO,
			"Tablespace and datafile system tables created.");
	}

	/* Note: The master thread has not been started at this point. */
	/* Confirm and move to the non-LRU part of the table LRU list. */

	sys_tablespaces_err = dict_check_if_system_table_exists(
		"SYS_TABLESPACES", DICT_NUM_FIELDS__SYS_TABLESPACES + 1, 1);
	ut_a(sys_tablespaces_err == DB_SUCCESS);

	sys_datafiles_err = dict_check_if_system_table_exists(
		"SYS_DATAFILES", DICT_NUM_FIELDS__SYS_DATAFILES + 1, 1);
	ut_a(sys_datafiles_err == DB_SUCCESS);

	return(err);
}

/********************************************************************//**
Add a single tablespace definition to the data dictionary tables in the
database.
@return error code or DB_SUCCESS */

dberr_t
dict_create_add_tablespace_to_dictionary(
/*=====================================*/
	ulint		space,		/*!< in: tablespace id */
	const char*	name,		/*!< in: tablespace name */
	ulint		flags,		/*!< in: tablespace flags */
	const char*	path,		/*!< in: tablespace path */
	trx_t*		trx,		/*!< in/out: transaction */
	bool		commit)		/*!< in: if true then commit the
					transaction */
{
	dberr_t		error;

	pars_info_t*	info = pars_info_create();

	ut_a(!is_system_tablespace(space));

	pars_info_add_int4_literal(info, "space", space);

	pars_info_add_str_literal(info, "name", name);

	pars_info_add_int4_literal(info, "flags", flags);

	pars_info_add_str_literal(info, "path", path);

	error = que_eval_sql(info,
			     "PROCEDURE P () IS\n"
			     "BEGIN\n"
			     "INSERT INTO SYS_TABLESPACES VALUES"
			     "(:space, :name, :flags);\n"
			     "INSERT INTO SYS_DATAFILES VALUES"
			     "(:space, :path);\n"
			     "END;\n",
			     FALSE, trx);

	if (error != DB_SUCCESS) {
		return(error);
	}

	if (commit) {
		trx->op_info = "committing tablespace and datafile definition";
		trx_commit(trx);
	}

	trx->op_info = "";

	return(error);
}
