/* Copyright (c) 2000, 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/**
  @file

  @brief
  Sorts a database
*/

#include "sql_priv.h"
#include "filesort.h"
#include "unireg.h"                      // REQUIRED by other includes
#include <m_ctype.h>
#include "sql_sort.h"
#include "probes_mysql.h"
#include "opt_range.h"                          // SQL_SELECT
#include "bounded_queue.h"
#include "filesort_utils.h"
#include "sql_select.h"
#include "debug_sync.h"
#include "opt_trace.h"
#include "sql_optimizer.h"              // JOIN
#include "sql_base.h"
#include "opt_costmodel.h"

#include <algorithm>
#include <utility>
using std::max;
using std::min;

	/* functions defined in this file */

static ha_rows find_all_keys(Sort_param *param,SQL_SELECT *select,
                             Filesort_info *fs_info,
                             IO_CACHE *buffer_file,
                             IO_CACHE *chunk_file,
                             Bounded_queue<uchar *, uchar *, Sort_param> *pq,
                             ha_rows *found_rows);
static int write_keys(Sort_param *param, Filesort_info *fs_info,
                      uint count, IO_CACHE *buffer_file, IO_CACHE *tempfile);
static void register_used_fields(Sort_param *param);
static int merge_index(Sort_param *param,
                       Sort_buffer sort_buffer,
                       Merge_chunk_array chunk_array,
                       IO_CACHE *tempfile,
                       IO_CACHE *outfile);
static bool save_index(Sort_param *param, uint count,
                       Filesort_info *table_sort);
static uint suffix_length(ulong string_length);

static bool check_if_pq_applicable(Opt_trace_context *trace,
                                   Sort_param *param, Filesort_info *info,
                                   TABLE *table,
                                   ha_rows records, ulong memory_available,
                                   bool keep_addon_fields);


void Sort_param::init_for_filesort(Filesort *file_sort,
                                   uint sortlen, TABLE *table,
                                   ulong max_length_for_sort_data,
                                   ha_rows maxrows, bool sort_positions)
{
  DBUG_ASSERT(max_rows == 0);   // function should not be called twice
  sort_length= sortlen;
  ref_length= table->file->ref_length;
  if (!(table->file->ha_table_flags() & HA_FAST_KEY_READ) &&
      !table->fulltext_searched && !sort_positions)
  {
    /* 
      Get the descriptors of all fields whose values are appended 
      to sorted fields and get its total length in addon_length.
    */
    addon_fields=
      file_sort->get_addon_fields(max_length_for_sort_data,
                                  table->field, sort_length, &addon_length,
                                  &m_packable_length);
  }
  if (using_addon_fields())
  {
    res_length= addon_length;
  }
  else
  {
    res_length= ref_length;
    /* 
      The reference to the record is considered 
      as an additional sorted field
    */
    sort_length+= ref_length;
  }
  rec_length= sort_length + addon_length;
  max_rows= maxrows;
}


void Sort_param::try_to_pack_addons(ulong max_length_for_sort_data)
{
  if (!using_addon_fields() ||                  // no addons, or
      using_packed_addons())                    // already packed
    return;

  if (!Addon_fields::can_pack_addon_fields(res_length))
    return;

  const uint sz= Addon_fields::size_of_length_field;
  if (rec_length + sz > max_length_for_sort_data)
    return;

  // Heuristic: skip packing if potential savings are less than 10 bytes.
  if (m_packable_length < (10 + sz))
    return;

  Addon_fields_array::iterator addonf= addon_fields->begin();
  for ( ; addonf != addon_fields->end(); ++addonf)
  {
    addonf->offset+= sz;
    addonf->null_offset+= sz;
  }
  addon_fields->set_using_packed_addons(true);
  m_using_packed_addons= true;

  addon_length+= sz;
  res_length+= sz;
  rec_length+= sz;
}


static void trace_filesort_information(Opt_trace_context *trace,
                                       const SORT_FIELD *sortorder,
                                       uint s_length)
{
  if (!trace->is_started())
    return;

  Opt_trace_array trace_filesort(trace, "filesort_information");
  for (; s_length-- ; sortorder++)
  {
    Opt_trace_object oto(trace);
    oto.add_alnum("direction", sortorder->reverse ? "desc" : "asc");

    if (sortorder->field)
    {
      if (strlen(sortorder->field->table->alias) != 0)
        oto.add_utf8_table(sortorder->field->table);
      else
        oto.add_alnum("table", "intermediate_tmp_table");
      oto.add_alnum("field", sortorder->field->field_name ?
                    sortorder->field->field_name : "tmp_table_column");
    }
    else
      oto.add("expression", sortorder->item);
  }
}


/**
  Sort a table.
  Creates a set of pointers that can be used to read the rows
  in sorted order. This should be done with the functions
  in records.cc.

  Before calling filesort, one must have done
  table->file->info(HA_STATUS_VARIABLE)

  The result set is stored in table->sort.io_cache or
  table->sort.sorted_result, or left in the main filesort buffer.

  @param      thd            Current thread
  @param      table          Table to sort
  @param      filesort       How to sort the table
  @param      sort_positions Set to TRUE if we want to force sorting by position
                             (Needed by UPDATE/INSERT or ALTER TABLE or
                              when rowids are required by executor)
  @param[out] examined_rows  Store number of examined rows here
                             This is the number of found rows before
                             applying WHERE condition.
  @param[out] found_rows     Store the number of found rows here.
                             This is the number of found rows after
                             applying WHERE condition.

  @note
    If we sort by position (like if sort_positions is 1) filesort() will
    call table->prepare_for_position().

  @returns
    HA_POS_ERROR	Error
  @returns
    \#			Number of rows in the result, could be less than
                        found_rows if LIMIT were provided.
*/

ha_rows filesort(THD *thd, TABLE *table, Filesort *filesort,
                 bool sort_positions, ha_rows *examined_rows,
                 ha_rows *found_rows)
{
  int error;
  ulong memory_available= thd->variables.sortbuff_size;
  size_t num_chunks;
  ha_rows num_rows= HA_POS_ERROR;
  IO_CACHE tempfile;   // Temporary file for storing intermediate results.
  IO_CACHE chunk_file; // For saving Merge_chunk structs.
  IO_CACHE *outfile;   // Contains the final, sorted result.
  Sort_param param;
  bool multi_byte_charset;
  Bounded_queue<uchar *, uchar *, Sort_param> pq;
  Opt_trace_context * const trace= &thd->opt_trace;
  SQL_SELECT *const select= filesort->select;
  ha_rows max_rows= filesort->limit;
  uint s_length= 0;

  DBUG_ENTER("filesort");

  if (!(s_length= filesort->make_sortorder()))
    DBUG_RETURN(HA_POS_ERROR);  /* purecov: inspected */

  /*
    We need a nameless wrapper, since we may be inside the "steps" of
    "join_execution".
  */
  Opt_trace_object trace_wrapper(trace);
  trace_filesort_information(trace, filesort->sortorder, s_length);

  Item_subselect *subselect= table->reginfo.join_tab ?
     table->reginfo.join_tab->join->select_lex->master_unit()->item :
     NULL;

  MYSQL_FILESORT_START(table->s->db.str, table->s->table_name.str);
  DEBUG_SYNC(thd, "filesort_start");

  /*
   Release InnoDB's adaptive hash index latch (if holding) before
   running a sort.
  */
  ha_release_temporary_latches(thd);

  /* 
    Don't use table->sort in filesort as it is also used by 
    QUICK_INDEX_MERGE_SELECT. Work with a copy and put it back at the end 
    when index_merge select has finished with it.
  */
  Filesort_info table_sort= table->sort;
  table->sort.io_cache= NULL;
  DBUG_ASSERT(table_sort.sorted_result == NULL);
  table_sort.sorted_result_in_fsbuf= false;

  outfile= table_sort.io_cache;
  my_b_clear(&tempfile);
  my_b_clear(&chunk_file);
  error= 1;

  param.init_for_filesort(filesort,
                          sortlength(thd, filesort->sortorder, s_length,
                                     &multi_byte_charset),
                          table,
                          thd->variables.max_length_for_sort_data,
                          max_rows, sort_positions);

  table_sort.addon_fields= param.addon_fields;

  if (select && select->quick)
    thd->inc_status_sort_range();
  else
    thd->inc_status_sort_scan();

  // If number of rows is not known, use as much of sort buffer as possible. 
  num_rows= table->file->estimate_rows_upper_bound();

  if (multi_byte_charset &&
      !(param.tmp_buffer= (char*) my_malloc(key_memory_Sort_param_tmp_buffer,
                                            param.sort_length,MYF(MY_WME))))
    goto err;

  if (check_if_pq_applicable(trace, &param, &table_sort,
                             table, num_rows, memory_available,
                             subselect != NULL))
  {
    DBUG_PRINT("info", ("filesort PQ is applicable"));
    /*
      For PQ queries (with limit) we know exactly how many pointers/records
      we have in the buffer, so to simplify things, we initialize
      all pointers here. (We cannot pack fields anyways, so there is no
      point in doing lazy initialization).
     */
    table_sort.init_record_pointers();

    if (pq.init(param.max_rows,
                true,                           // max_at_top
                NULL,                           // compare_function
                &param, table_sort.get_sort_keys()))
    {
      /*
       If we fail to init pq, we have to give up:
       out of memory means my_malloc() will call my_error().
      */
      DBUG_PRINT("info", ("failed to allocate PQ"));
      table_sort.free_sort_buffer();
      DBUG_ASSERT(thd->is_error());
      goto err;
    }
    filesort->using_pq= true;
    param.using_pq= true;
  }
  else
  {
    DBUG_PRINT("info", ("filesort PQ is not applicable"));
    filesort->using_pq= false;
    param.using_pq= false;

    /*
      When sorting using priority queue, we cannot use packed addons.
      Without PQ, we can try.
    */
    param.try_to_pack_addons(thd->variables.max_length_for_sort_data);

    /*
      We need space for at least one record from each merge chunk, i.e.
        param->max_keys_per_buffer >= MERGEBUFF2
      See merge_buffers()),
      memory_available must be large enough for
        param->max_keys_per_buffer * (record + record pointer) bytes
      (the main sort buffer, see alloc_sort_buffer()).
      Hence this minimum:
    */
    const ulong min_sort_memory=
      max<ulong>(MIN_SORT_MEMORY,
                 ALIGN_SIZE(MERGEBUFF2 * (param.rec_length + sizeof(uchar*))));
    while (memory_available >= min_sort_memory)
    {
      ha_rows keys= memory_available / (param.rec_length + sizeof(char*));
      // If the table is empty, allocate space for one row.
      param.max_keys_per_buffer= (uint) min(num_rows > 0 ? num_rows : 1, keys);

      table_sort.alloc_sort_buffer(param.max_keys_per_buffer, param.rec_length);
      if (table_sort.sort_buffer_size() > 0)
        break;
      ulong old_memory_available= memory_available;
      memory_available= memory_available/4*3;
      if (memory_available < min_sort_memory &&
          old_memory_available > min_sort_memory)
        memory_available= min_sort_memory;
    }
    if (memory_available < min_sort_memory)
    {
      my_error(ER_OUT_OF_SORTMEMORY,MYF(ME_ERROR + ME_FATALERROR));
      goto err;
    }
  }

  if (open_cached_file(&chunk_file,mysql_tmpdir,TEMP_PREFIX,
		       DISK_BUFFER_SIZE, MYF(MY_WME)))
    goto err;

  param.sort_form= table;
  param.local_sortorder=
    Bounds_checked_array<SORT_FIELD>(filesort->sortorder, s_length);
  // New scope, because subquery execution must be traced within an array.
  {
    Opt_trace_array ota(trace, "filesort_execution");
    num_rows= find_all_keys(&param, select,
                            &table_sort,
                            &chunk_file,
                            &tempfile,
                            pq.is_initialized() ? &pq : NULL,
                            found_rows);
    if (num_rows == HA_POS_ERROR)
      goto err;
  }

  num_chunks= my_b_tell(&chunk_file)/sizeof(Merge_chunk);

  Opt_trace_object(trace, "filesort_summary")
    .add("rows", num_rows)
    .add("examined_rows", param.examined_rows)
    .add("number_of_tmp_files", num_chunks)
    .add("sort_buffer_size", table_sort.sort_buffer_size())
    .add_alnum("sort_mode",
               param.using_packed_addons() ?
               "<sort_key, packed_additional_fields>" :
               param.using_addon_fields() ?
               "<sort_key, additional_fields>" : "<sort_key, rowid>");

  if (num_chunks == 0)                   // The whole set is in memory
  {
    if (save_index(&param, (uint) num_rows, &table_sort))
      goto err;
  }
  else
  {
    /* filesort cannot handle zero-length records during merge. */
    DBUG_ASSERT(param.sort_length != 0);

    // We will need an extra buffer in rr_unpack_from_tempfile()
    if (table_sort.using_addon_fields() &&
        !(table_sort.addon_fields->allocate_addon_buf(param.addon_length)))
      goto err;                                 /* purecov: inspected */

    table_sort.read_chunk_descriptors(&chunk_file, num_chunks);
    if (table_sort.merge_chunks.is_null())
      goto err;                                 /* purecov: inspected */

    close_cached_file(&chunk_file);

    /* Open cached file if it isn't open */
    if (! my_b_inited(outfile) &&
	open_cached_file(outfile,mysql_tmpdir,TEMP_PREFIX,READ_RECORD_BUFFER,
			  MYF(MY_WME)))
      goto err;
    if (reinit_io_cache(outfile,WRITE_CACHE,0L,0,0))
      goto err;

    /*
      Use also the space previously used by string pointers in sort_buffer
      for temporary key storage.
    */
    param.max_keys_per_buffer=
      table_sort.sort_buffer_size() / param.rec_length;

    if (merge_many_buff(&param,
                        table_sort.get_raw_buf(),
                        table_sort.merge_chunks,
                        &num_chunks,
                        &tempfile))
      goto err;
    if (flush_io_cache(&tempfile) ||
	reinit_io_cache(&tempfile,READ_CACHE,0L,0,0))
      goto err;
    if (merge_index(&param,
                    table_sort.get_raw_buf(),
                    Merge_chunk_array(table_sort.merge_chunks.begin(),
                                      num_chunks),
                    &tempfile,
                    outfile))
      goto err;
  }

  if (num_rows > param.max_rows)
  {
    // If find_all_keys() produced more results than the query LIMIT.
    num_rows= param.max_rows;
  }
  error= 0;

 err:
  my_free(param.tmp_buffer);
  if (!subselect || !subselect->is_uncacheable())
  {
    if (!table_sort.sorted_result_in_fsbuf)
      table_sort.free_sort_buffer();
    my_free(table_sort.merge_chunks.array());
    table_sort.merge_chunks= Merge_chunk_array(NULL, 0);
  }
  close_cached_file(&tempfile);
  close_cached_file(&chunk_file);
  if (my_b_inited(outfile))
  {
    if (flush_io_cache(outfile))
      error=1;
    {
      my_off_t save_pos=outfile->pos_in_file;
      /* For following reads */
      if (reinit_io_cache(outfile,READ_CACHE,0L,0,0))
	error=1;
      outfile->end_of_file=save_pos;
    }
  }
  if (error)
  {
    int kill_errno= thd->killed_errno();

    DBUG_ASSERT(thd->is_error() || kill_errno);

    /*
      We replace the table->sort at the end.
      Hence calling free_io_cache to make sure table->sort.io_cache
      used for QUICK_INDEX_MERGE_SELECT is free.
    */
    free_io_cache(table);

    /*
      Guard against Bug#11745656 -- KILL QUERY should not send "server shutdown"
      to client!
    */
    const char *cause= kill_errno
                       ? ((kill_errno == THD::KILL_CONNECTION && !abort_loop)
                         ? ER(THD::KILL_QUERY)
                         : ER(kill_errno))
                       : thd->get_stmt_da()->message_text();
    const char *msg=   ER_THD(thd, ER_FILSORT_ABORT);

    my_printf_error(ER_FILSORT_ABORT,
                    "%s: %s",
                    MYF(0),
                    msg,
                    cause);

    if (thd->is_fatal_error)
      sql_print_information("%s, host: %s, user: %s, "
                            "thread: %lu, error: %s, query: %-.4096s",
                            msg,
                            thd->security_ctx->host_or_ip,
                            &thd->security_ctx->priv_user[0],
                            thd->thread_id,
                            cause,
                            thd->query().str);
  }
  else
    thd->inc_status_sort_rows(num_rows);
  *examined_rows= param.examined_rows;

  /* table->sort.io_cache should be free by this time */
  DBUG_ASSERT(NULL == table->sort.io_cache);

  // Assign the copy back!
  table->sort= table_sort;

  DBUG_PRINT("exit",
             ("num_rows: %ld examined_rows: %ld found_rows: %ld",
              (long) num_rows, (long) *examined_rows, (long) *found_rows));
  MYSQL_FILESORT_DONE(error, num_rows);
  DBUG_RETURN(error ? HA_POS_ERROR : num_rows);
} /* filesort */


void filesort_free_buffers(TABLE *table, bool full)
{
  DBUG_ENTER("filesort_free_buffers");
  my_free(table->sort.sorted_result);
  table->sort.sorted_result= NULL;
  table->sort.sorted_result_in_fsbuf= false;

  if (full)
  {
    table->sort.free_sort_buffer();
    my_free(table->sort.merge_chunks.array());
    table->sort.merge_chunks= Merge_chunk_array(NULL, 0);
  }

  table->sort.addon_fields= NULL;
  DBUG_VOID_RETURN;
}

void Filesort::cleanup()
{
  if (select && own_select)
  {
    delete select;
    select= NULL;
  }
}

uint Filesort::make_sortorder()
{
  uint count;
  SORT_FIELD *sort,*pos;
  ORDER *ord;
  DBUG_ENTER("make_sortorder");


  count=0;
  for (ord = order; ord; ord= ord->next)
    count++;
  if (!sortorder)
    sortorder= (SORT_FIELD*) sql_alloc(sizeof(SORT_FIELD) * (count + 1));
  pos= sort= sortorder;

  if (!pos)
    DBUG_RETURN(0);

  for (ord= order; ord; ord= ord->next, pos++)
  {
    Item *item= ord->item[0]->real_item();
    pos->field= 0; pos->item= 0;
    if (item->type() == Item::FIELD_ITEM)
      pos->field= ((Item_field*) item)->field;
    else if (item->type() == Item::SUM_FUNC_ITEM && !item->const_item())
      pos->field= ((Item_sum*) item)->get_tmp_table_field();
    else if (item->type() == Item::COPY_STR_ITEM)
    {						// Blob patch
      pos->item= ((Item_copy*) item)->get_item();
    }
    else
      pos->item= *ord->item;
    pos->reverse= (ord->direction == ORDER::ORDER_DESC);
    DBUG_ASSERT(pos->field != NULL || pos->item != NULL);
  }
  DBUG_RETURN(count);
}


void Filesort_info::read_chunk_descriptors(IO_CACHE *chunk_file, uint count)
{
  DBUG_ENTER("Filesort_info::read_chunk_descriptors");

  // If we already have a chunk array, we're doing sort in a subquery.
  if (!merge_chunks.is_null() &&
      merge_chunks.size() < count)
  {
    my_free(merge_chunks.array());              /* purecov: inspected */
    merge_chunks= Merge_chunk_array(NULL, 0);   /* purecov: inspected */
  }

  void *rawmem= merge_chunks.array();
  const size_t length= sizeof(Merge_chunk) * count;
  if (NULL == rawmem)
  {
    rawmem= my_malloc(key_memory_Filesort_info_merge, length, MYF(MY_WME));
    if (rawmem == NULL)
      DBUG_VOID_RETURN;                         /* purecov: inspected */
  }

  if (reinit_io_cache(chunk_file, READ_CACHE, 0L, 0, 0) ||
      my_b_read(chunk_file, static_cast<uchar*>(rawmem), length))
  {
    my_free(rawmem);                            /* purecov: inspected */
    rawmem= NULL;                               /* purecov: inspected */
    count= 0;                                   /* purecov: inspected */
  }

  merge_chunks= Merge_chunk_array(static_cast<Merge_chunk*>(rawmem), count);
  DBUG_VOID_RETURN;
}

#ifndef DBUG_OFF
/*
  Print a text, SQL-like record representation into dbug trace.

  Note: this function is a work in progress: at the moment
   - column read bitmap is ignored (can print garbage for unused columns)
   - there is no quoting
*/
static void dbug_print_record(TABLE *table, bool print_rowid)
{
  char buff[1024];
  Field **pfield;
  String tmp(buff,sizeof(buff),&my_charset_bin);
  DBUG_LOCK_FILE;
  
  fprintf(DBUG_FILE, "record (");
  for (pfield= table->field; *pfield ; pfield++)
    fprintf(DBUG_FILE, "%s%s", (*pfield)->field_name, (pfield[1])? ", ":"");
  fprintf(DBUG_FILE, ") = ");

  fprintf(DBUG_FILE, "(");
  for (pfield= table->field; *pfield ; pfield++)
  {
    Field *field=  *pfield;

    if (field->is_null()) {
      if (fwrite("NULL", sizeof(char), 4, DBUG_FILE) != 4) {
        goto unlock_file_and_quit;
      }
    }
   
    if (field->type() == MYSQL_TYPE_BIT)
      (void) field->val_int_as_str(&tmp, 1);
    else
      field->val_str(&tmp);

    if (fwrite(tmp.ptr(),sizeof(char),tmp.length(),DBUG_FILE) != tmp.length()) {
      goto unlock_file_and_quit;
    }

    if (pfield[1]) {
      if (fwrite(", ", sizeof(char), 2, DBUG_FILE) != 2) {
        goto unlock_file_and_quit;
      }
    }
  }
  fprintf(DBUG_FILE, ")");
  if (print_rowid)
  {
    fprintf(DBUG_FILE, " rowid ");
    for (uint i=0; i < table->file->ref_length; i++)
    {
      fprintf(DBUG_FILE, "%x", (uchar)table->file->ref[i]);
    }
  }
  fprintf(DBUG_FILE, "\n");
unlock_file_and_quit:
  DBUG_UNLOCK_FILE;
}
#endif 

static const Item::enum_walk walk_subquery=
  Item::enum_walk(Item::WALK_POSTFIX | Item::WALK_SUBQUERY);

/**
  Search after sort_keys, and write them into tempfile
  (if we run out of space in the sort buffer).
  All produced sequences are guaranteed to be non-empty.

  @param param             Sorting parameter
  @param select            Use this to get source data
  @param fs_info           Struct containing sort buffer etc.
  @param chunk_file        File to write Merge_chunks describing sorted segments
                           in tempfile.
  @param tempfile          File to write sorted sequences of sortkeys to.
  @param pq                If !NULL, use it for keeping top N elements
  @param [out] found_rows  The number of FOUND_ROWS().
                           For a query with LIMIT, this value will typically
                           be larger than the function return value.

  @note
    Basic idea:
    @verbatim
     while (get_next_sortkey())
     {
       if (using priority queue)
         push sort key into queue
       else
       {
         if (no free space in sort buffer)
         {
           sort buffer;
           dump sorted sequence to 'tempfile';
           dump Merge_chunk describing sequence location into 'chunk_file';
         }
         put sort key into buffer;
         if (key was packed)
           tell sort buffer the actual number of bytes used;
       }
     }
     if (buffer has some elements && dumped at least once)
       sort-dump-dump as above;
     else
       don't sort, leave sort buffer to be sorted by caller.
  @endverbatim

  @returns
    Number of records written on success.
  @returns
    HA_POS_ERROR on error.
*/

static ha_rows find_all_keys(Sort_param *param, SQL_SELECT *select,
                             Filesort_info *fs_info,
                             IO_CACHE *chunk_file,
                             IO_CACHE *tempfile,
                             Bounded_queue<uchar *, uchar *, Sort_param> *pq,
                             ha_rows *found_rows)
{
  int error,flag,quick_select;
  uint idx,indexpos,ref_length;
  uchar *ref_pos,*next_pos,ref_buff[MAX_REFLENGTH];
  my_off_t record;
  TABLE *sort_form;
  THD *thd= current_thd;
  volatile THD::killed_state *killed= &thd->killed;
  handler *file;
  MY_BITMAP *save_read_set, *save_write_set;
  bool skip_record;
  ha_rows num_records= 0;
  const bool packed_addon_fields= param->using_packed_addons();

  DBUG_ENTER("find_all_keys");
  DBUG_PRINT("info",("using: %s",
                     (select ? select->quick ? "ranges" : "where":
                      "every row")));

  idx=indexpos=0;
  error=quick_select=0;
  sort_form=param->sort_form;
  file=sort_form->file;
  ref_length=param->ref_length;
  ref_pos= ref_buff;
  quick_select=select && select->quick;
  record=0;
  *found_rows= 0;
  flag= ((file->ha_table_flags() & HA_REC_NOT_IN_SEQ) || quick_select);
  if (flag)
    ref_pos= &file->ref[0];
  next_pos=ref_pos;
  if (!quick_select)
  {
    next_pos=(uchar*) 0;			/* Find records in sequence */
    DBUG_EXECUTE_IF("bug14365043_1",
                    DBUG_SET("+d,ha_rnd_init_fail"););
    if ((error= file->ha_rnd_init(1)))
    {
      file->print_error(error, MYF(0));
      DBUG_RETURN(HA_POS_ERROR);
    }
    file->extra_opt(HA_EXTRA_CACHE,
		    current_thd->variables.read_buff_size);
  }

  if (quick_select)
  {
    if ((error= select->quick->reset()))
    {
      file->print_error(error, MYF(0));
      DBUG_RETURN(HA_POS_ERROR);
    }
  }

  /* Remember original bitmaps */
  save_read_set=  sort_form->read_set;
  save_write_set= sort_form->write_set;
  /*
    Set up temporary column read map for columns used by sort and verify
    it's not used
  */
  DBUG_ASSERT(bitmap_is_clear_all(&sort_form->tmp_set));

  /* Temporary set for register_used_fields and register_field_in_read_map */
  sort_form->read_set= &sort_form->tmp_set;
  // Include fields used for sorting in the read_set.
  register_used_fields(param); 

  // Include fields used by conditions in the read_set.
  if (select && select->cond)
    select->cond->walk(&Item::register_field_in_read_map, walk_subquery,
                       (uchar*) sort_form);

  // Include fields used by pushed conditions in the read_set.
  if (select && select->icp_cond)
    select->icp_cond->walk(&Item::register_field_in_read_map, walk_subquery,
                           (uchar*) sort_form);

  sort_form->column_bitmaps_set(&sort_form->tmp_set, &sort_form->tmp_set);

  DEBUG_SYNC(thd, "after_index_merge_phase1");
  for (;;)
  {
    if (quick_select)
    {
      if ((error= select->quick->get_next()))
        break;
      file->position(sort_form->record[0]);
      DBUG_EXECUTE_IF("debug_filesort", dbug_print_record(sort_form, TRUE););
    }
    else					/* Not quick-select */
    {
      {
	error= file->ha_rnd_next(sort_form->record[0]);
	if (!flag)
	{
	  my_store_ptr(ref_pos,ref_length,record); // Position to row
	  record+= sort_form->s->db_record_offset;
	}
	else if (!error)
	  file->position(sort_form->record[0]);
      }
      if (error && error != HA_ERR_RECORD_DELETED)
	break;
    }

    if (*killed)
    {
      DBUG_PRINT("info",("Sort killed by user"));
      if (!quick_select)
      {
        (void) file->extra(HA_EXTRA_NO_CACHE);
        file->ha_rnd_end();
      }
      num_records= HA_POS_ERROR;
      goto cleanup;
    }
    if (error == 0)
      param->examined_rows++;
    if (!error && (!select ||
                   (!select->skip_record(thd, &skip_record) && !skip_record)))
    {
      ++(*found_rows);
      if (pq)
        pq->push(ref_pos);
      else
      {
        if (fs_info->isfull())
        {
          if (write_keys(param, fs_info, idx, chunk_file, tempfile))
          {
            num_records= HA_POS_ERROR;
            goto cleanup;
          }
          idx= 0;
          indexpos++;
        }
        if (idx == 0)
          fs_info->init_next_record_pointer();
        uchar *start_of_rec= fs_info->get_next_record_pointer();

        const uint rec_sz= param->make_sortkey(start_of_rec, ref_pos);
        if (packed_addon_fields && rec_sz != param->rec_length)
          fs_info->adjust_next_record_pointer(rec_sz);

        idx++;
        num_records++;
      }
    }
    /*
      Don't try unlocking the row if skip_record reported an error since in
      this case the transaction might have been rolled back already.
    */
    else if (!thd->is_error())
      file->unlock_row();
    /* It does not make sense to read more keys in case of a fatal error */
    if (thd->is_error())
      break;
  }
  if (!quick_select)
  {
    (void) file->extra(HA_EXTRA_NO_CACHE);	/* End cacheing of records */
    if (!next_pos)
      file->ha_rnd_end();
  }

  if (thd->is_error())
  {
    num_records= HA_POS_ERROR;
    goto cleanup;
  }
  
  /* Signal we should use orignal column read and write maps */
  sort_form->column_bitmaps_set(save_read_set, save_write_set);

  DBUG_PRINT("test",("error: %d  indexpos: %d",error,indexpos));
  if (error != HA_ERR_END_OF_FILE)
  {
    file->print_error(error,MYF(ME_ERROR | ME_WAITTANG)); // purecov: inspected
    num_records= HA_POS_ERROR;                            // purecov: inspected
    goto cleanup;
  }
  if (indexpos && idx &&
      write_keys(param, fs_info, idx, chunk_file, tempfile))
  {
    num_records= HA_POS_ERROR;                            // purecov: inspected
    goto cleanup;
  }

  if (pq)
    num_records= pq->num_elements();

cleanup:
  // Clear tmp_set so it can be used elsewhere
  bitmap_clear_all(&sort_form->tmp_set);

  DBUG_PRINT("info", ("find_all_keys return %lu", (ulong) num_records));

  DBUG_RETURN(num_records);
} /* find_all_keys */


/**
  @details
  Sort the buffer and write:
  -# the sorted sequence to tempfile
  -# a Merge_chunk describing the sorted sequence position to chunk_file

  @param param          Sort parameters
  @param fs_info        Contains the buffer to be sorted and written.
  @param count          Number of records to write.
  @param chunk_file     One 'Merge_chunk' struct will be written into this file.
                        The Merge_chunk::{file_pos, count} will indicate where
                        the sorted data was stored.
  @param tempfile       The sorted sequence will be written into this file.

  @returns
    0 OK
  @returns
    1 Error
*/

static int
write_keys(Sort_param *param, Filesort_info *fs_info, uint count,
           IO_CACHE *chunk_file, IO_CACHE *tempfile)
{
  Merge_chunk merge_chunk;
  DBUG_ENTER("write_keys");

  fs_info->sort_buffer(param, count);

  if (!my_b_inited(tempfile) &&
      open_cached_file(tempfile, mysql_tmpdir, TEMP_PREFIX, DISK_BUFFER_SIZE,
                       MYF(MY_WME)))
    DBUG_RETURN(1);                             /* purecov: inspected */

  // Check that we wont have more chunks than we can possibly keep in memory.
  if (my_b_tell(chunk_file) + sizeof(Merge_chunk) > (ulonglong)UINT_MAX)
    DBUG_RETURN(1);                             /* purecov: inspected */

  merge_chunk.set_file_position(my_b_tell(tempfile));
  if (static_cast<ha_rows>(count) > param->max_rows)
  {
    // Write only SELECT LIMIT rows to the file
    count= static_cast<uint>(param->max_rows);  /* purecov: inspected */
  }
  merge_chunk.set_rowcount(static_cast<ha_rows>(count));

  const bool packed_addon_fields= param->using_packed_addons();
  for (uint ix= 0; ix < count; ++ix)
  {
    uint rec_length;
    uchar *record= fs_info->get_sorted_record(ix);
    if (packed_addon_fields)
    {
      rec_length= param->sort_length +
        Addon_fields::read_addon_length(record + param->sort_length);
    }
    else
      rec_length= param->rec_length;

    if (my_b_write(tempfile, record, rec_length))
      DBUG_RETURN(1);                           /* purecov: inspected */
  }

  if (my_b_write(chunk_file, &merge_chunk, sizeof(merge_chunk)))
    DBUG_RETURN(1);                             /* purecov: inspected */

  DBUG_RETURN(0);
} /* write_keys */


/**
  Store length as suffix in high-byte-first order.
*/

static inline void store_length(uchar *to, uint length, uint pack_length)
{
  switch (pack_length) {
  case 1:
    *to= (uchar) length;
    break;
  case 2:
    mi_int2store(to, length);
    break;
  case 3:
    mi_int3store(to, length);
    break;
  default:
    mi_int4store(to, length);
    break;
  }
}


#ifdef WORDS_BIGENDIAN
const bool Is_big_endian= true;
#else
const bool Is_big_endian= false;
#endif
void copy_native_longlong(uchar *to, size_t to_length,
                          longlong val, bool is_unsigned)
{
  copy_integer<Is_big_endian>(to, to_length,
                              static_cast<uchar*>(static_cast<void*>(&val)),
                              sizeof(longlong),
                              is_unsigned);
}

uint Sort_param::make_sortkey(uchar *to, const uchar *ref_pos)
{
  uchar *orig_to= to;
  const SORT_FIELD *sort_field;

  for (sort_field= local_sortorder.begin() ;
       sort_field != local_sortorder.end() ;
       sort_field++)
  {
    bool maybe_null= false;
    if (sort_field->field)
    {
      Field *field= sort_field->field;
      if (field->maybe_null())
      {
	if (field->is_null())
	{
	  if (sort_field->reverse)
	    memset(to, 255, sort_field->length+1);
	  else
	    memset(to, 0, sort_field->length+1);
	  to+= sort_field->length+1;
	  continue;
	}
	else
	  *to++=1;
      }
      field->make_sort_key(to, sort_field->length);
    }
    else
    {						// Item
      Item *item=sort_field->item;
      maybe_null= item->maybe_null;
      switch (sort_field->result_type) {
      case STRING_RESULT:
      {
        const CHARSET_INFO *cs=item->collation.collation;
        char fill_char= ((cs->state & MY_CS_BINSORT) ? (char) 0 : ' ');

        if (maybe_null)
          *to++=1;
        /* All item->str() to use some extra byte for end null.. */
        String tmp((char*) to,sort_field->length+4,cs);
        String *res= item->str_result(&tmp);
        if (!res)
        {
          if (maybe_null)
            memset(to-1, 0, sort_field->length+1);
          else
          {
            /* purecov: begin deadcode */
            /*
              This should only happen during extreme conditions if we run out
              of memory or have an item marked not null when it can be null.
              This code is here mainly to avoid a hard crash in this case.
            */
            DBUG_ASSERT(0);
            DBUG_PRINT("warning",
                       ("Got null on something that shouldn't be null"));
            memset(to, 0, sort_field->length);	// Avoid crash
            /* purecov: end */
          }
          break;
        }
        uint length= res->length();
        if (sort_field->need_strxnfrm)
        {
          char *from=(char*) res->ptr();
          if ((uchar*) from == to)
          {
            DBUG_ASSERT(sort_field->length >= length);
            set_if_smaller(length,sort_field->length);
            memcpy(tmp_buffer, from, length);
            from= tmp_buffer;
          }
#ifndef DBUG_OFF
          uint tmp_length=
#endif
            cs->coll->strnxfrm(cs, to, sort_field->length,
                               item->max_char_length(),
                               (uchar*) from, length,
                               MY_STRXFRM_PAD_WITH_SPACE |
                               MY_STRXFRM_PAD_TO_MAXLEN);
          DBUG_ASSERT(tmp_length == sort_field->length);
        }
        else
        {
          uint diff;
          uint sort_field_length= sort_field->length -
            sort_field->suffix_length;
          if (sort_field_length < length)
          {
            diff= 0;
            length= sort_field_length;
          }
          else
            diff= sort_field_length - length;
          if (sort_field->suffix_length)
          {
            /* Store length last in result_string */
            store_length(to + sort_field_length, length,
                         sort_field->suffix_length);
          }

          my_strnxfrm(cs,(uchar*)to,length,(const uchar*)res->ptr(),length);
          cs->cset->fill(cs, (char *)to+length,diff,fill_char);
        }
        break;
      }
      case INT_RESULT:
	{
          longlong value= item->field_type() == MYSQL_TYPE_TIME ?
                          item->val_time_temporal_result() :
                          item->is_temporal_with_date() ?
                          item->val_date_temporal_result() :
                          item->val_int_result();
          if (maybe_null)
          {
	    *to++=1;				/* purecov: inspected */
            if (item->null_value)
            {
              if (maybe_null)
                memset(to-1, 0, sort_field->length+1);
              else
              {
                DBUG_PRINT("warning",
                           ("Got null on something that shouldn't be null"));
                memset(to, 0, sort_field->length);
              }
              break;
            }
          }
          copy_native_longlong(to, sort_field->length,
                               value, item->unsigned_flag);
	  break;
	}
      case DECIMAL_RESULT:
        {
          my_decimal dec_buf, *dec_val= item->val_decimal_result(&dec_buf);
          if (maybe_null)
          {
            if (item->null_value)
            { 
              memset(to, 0, sort_field->length+1);
              to++;
              break;
            }
            *to++=1;
          }
          if (sort_field->length < DECIMAL_MAX_FIELD_SIZE)
          {
            uchar buf[DECIMAL_MAX_FIELD_SIZE];
            my_decimal2binary(E_DEC_FATAL_ERROR, dec_val, buf,
                              item->max_length - (item->decimals ? 1:0),
                              item->decimals);
            memcpy(to, buf, sort_field->length);
          }
          else
          {
            my_decimal2binary(E_DEC_FATAL_ERROR, dec_val, to,
                              item->max_length - (item->decimals ? 1:0),
                              item->decimals);
          }
         break;
        }
      case REAL_RESULT:
	{
          double value= item->val_result();
	  if (maybe_null)
          {
            if (item->null_value)
            {
              memset(to, 0, sort_field->length+1);
              to++;
              break;
            }
	    *to++=1;
          }
          if (sort_field->length < sizeof(double))
          {
            uchar buf[sizeof(double)];
            change_double_for_sort(value, buf);
            memcpy(to, buf, sort_field->length);
          }
          else
          {
            change_double_for_sort(value, (uchar*) to);
          }
	  break;
	}
      case ROW_RESULT:
      default: 
	// This case should never be choosen
	DBUG_ASSERT(0);
	break;
      }
    }
    if (sort_field->reverse)
    {							/* Revers key */
      if (maybe_null)
        to[-1]= ~to[-1];
      uint length= sort_field->length;
      while (length--)
      {
	*to = (uchar) (~ *to);
	to++;
      }
    }
    else
      to+= sort_field->length;
  }

  if (using_addon_fields())
  {
    /* 
      Save field values appended to sorted fields.
      First null bit indicators are appended then field values follow.
    */
    uchar *nulls= to;
    uchar *p_len= to;

    Addon_fields_array::const_iterator addonf= addon_fields->begin();
    uint32 res_len= addonf->offset;
    const bool packed_addon_fields= addon_fields->using_packed_addons();
    memset(nulls, 0, addonf->offset);
    to+= addonf->offset;
    for ( ; addonf != addon_fields->end(); ++addonf)
    {
      Field *field= addonf->field;
      if (addonf->null_bit && field->is_null())
      {
        nulls[addonf->null_offset]|= addonf->null_bit;
        if (!packed_addon_fields)
          to+= addonf->max_length;
      }
      else
      {
        uchar *ptr= field->pack(to, field->ptr);
        int sz= ptr - to;
        res_len += sz;
        if (packed_addon_fields)
          to+= sz;
        else
          to+= addonf->max_length;
      }
    }
    if (packed_addon_fields)
      Addon_fields::store_addon_length(p_len, res_len);
    DBUG_PRINT("info", ("make_sortkey %p %u", orig_to, res_len));
  }
  else
  {
    /* Save filepos last */
    memcpy(to, ref_pos, ref_length);
    to+= ref_length;
  }
  return to - orig_to;
}


/*
  Register fields used by sorting in the sorted table's read set
*/

static void register_used_fields(Sort_param *param)
{
  Bounds_checked_array<SORT_FIELD>::const_iterator sort_field;
  TABLE *table=param->sort_form;
  MY_BITMAP *bitmap= table->read_set;

  for (sort_field= param->local_sortorder.begin() ;
       sort_field != param->local_sortorder.end() ;
       sort_field++)
  {
    Field *field;
    if ((field= sort_field->field))
    {
      if (field->table == table)
      bitmap_set_bit(bitmap, field->field_index);
    }
    else
    {						// Item
      sort_field->item->walk(&Item::register_field_in_read_map, walk_subquery,
                             (uchar *) table);
    }
  }

  if (param->using_addon_fields())
  {
    Addon_fields_array::const_iterator addonf= param->addon_fields->begin();
    for ( ; addonf != param->addon_fields->end(); ++addonf)
      bitmap_set_bit(bitmap, addonf->field->field_index);
  }
  else
  {
    /* Save filepos last */
    table->prepare_for_position();
  }
}

/**
  This function is used only if the entire result set fits in memory.

  For addon fields, we keep the result in the filesort buffer.
  This saves us a lot of memcpy calls.

  For row references, we copy the final sorted result into a buffer,
  but we do not copy the actual sort-keys, as they are no longer needed.
  We could have kept the result in the sort buffere here as well,
  but the new buffer - containing only row references - is probably a
  lot smaller. 

  The result data will be unpacked by rr_unpack_from_buffer()
  or rr_from_pointers()

  @param [in]     param      Sort parameters.
  @param          count      Number of records
  @param [in,out] table_sort Information used by rr_unpack_from_buffer() /
                             rr_from_pointers()
 */
static bool save_index(Sort_param *param, uint count, Filesort_info *table_sort)
{
  uchar *to;
  DBUG_ENTER("save_index");

  table_sort->sort_buffer(param, count);

  if (param->using_addon_fields())
  {
    table_sort->sorted_result_in_fsbuf= true;
    table_sort->set_sort_length(param->sort_length);
    DBUG_RETURN(0);
  }

  table_sort->sorted_result_in_fsbuf= false;
  const size_t buf_size= param->res_length * count;

  DBUG_ASSERT(table_sort->sorted_result == NULL);
  if (!(to= table_sort->sorted_result=
        static_cast<uchar*>(my_malloc(key_memory_Filesort_info_record_pointers,
                                      buf_size, MYF(MY_WME)))))
    DBUG_RETURN(1);                 /* purecov: inspected */
  table_sort->sorted_result_end=
    table_sort->sorted_result + buf_size;

  uint res_length= param->res_length;
  uint offset= param->rec_length - res_length;
  for (uint ix= 0; ix < count; ++ix)
  {
    uchar *record= table_sort->get_sorted_record(ix);
    memcpy(to, record + offset, res_length);
    to+= res_length;
  }
  DBUG_RETURN(0);
}


/**
  Test whether priority queue is worth using to get top elements of an
  ordered result set. If it is, then allocates buffer for required amount of
  records

  @param trace            Current trace context.
  @param param            Sort parameters.
  @param filesort_info    Filesort information.
  @param table            Table to sort.
  @param num_rows         Estimate of number of rows in source record set.
  @param memory_available Memory available for sorting.
  @param keep_addon_fields Do not try to strip off addon fields.

  DESCRIPTION
    Given a query like this:
      SELECT ... FROM t ORDER BY a1,...,an LIMIT max_rows;
    This function tests whether a priority queue should be used to keep
    the result. Necessary conditions are:
    - estimate that it is actually cheaper than merge-sort
    - enough memory to store the <max_rows> records.

    If we don't have space for <max_rows> records, but we *do* have
    space for <max_rows> keys, we may rewrite 'table' to sort with
    references to records instead of additional data.
    (again, based on estimates that it will actually be cheaper).

   @returns
    true  - if it's ok to use PQ
    false - PQ will be slower than merge-sort, or there is not enough memory.
*/

bool check_if_pq_applicable(Opt_trace_context *trace,
                            Sort_param *param,
                            Filesort_info *filesort_info,
                            TABLE *table, ha_rows num_rows,
                            ulong memory_available,
                            bool keep_addon_fields)
{
  DBUG_ENTER("check_if_pq_applicable");

  /*
    How much Priority Queue sort is slower than qsort.
    Measurements (see unit test) indicate that PQ is roughly 3 times slower.
  */
  const double PQ_slowness= 3.0;

  Opt_trace_object trace_filesort(trace,
                                  "filesort_priority_queue_optimization");
  if (param->max_rows == HA_POS_ERROR)
  {
    trace_filesort
      .add("usable", false)
      .add_alnum("cause", "not applicable (no LIMIT)");
    DBUG_RETURN(false);
  }

  trace_filesort
    .add("limit", param->max_rows)
    .add("rows_estimate", num_rows)
    .add("row_size", param->rec_length)
    .add("memory_available", memory_available);

  if (param->max_rows + 2 >= UINT_MAX)
  {
    trace_filesort.add("usable", false).add_alnum("cause", "limit too large");
    DBUG_RETURN(false);
  }

  ulong num_available_keys=
    memory_available / (param->rec_length + sizeof(char*));
  // We need 1 extra record in the buffer, when using PQ.
  param->max_keys_per_buffer= (uint) param->max_rows + 1;

  if (num_rows < num_available_keys)
  {
    // The whole source set fits into memory.
    if (param->max_rows < num_rows/PQ_slowness )
    {
      filesort_info->
        alloc_sort_buffer(param->max_keys_per_buffer, param->rec_length);
      trace_filesort.add("chosen", true);
      DBUG_RETURN(filesort_info->sort_buffer_size() > 0);
    }
    else
    {
      // PQ will be slower.
      trace_filesort.add("chosen", false)
        .add_alnum("cause", "quicksort_is_cheaper");
      DBUG_RETURN(false);
    }
  }

  // Do we have space for LIMIT rows in memory?
  if (param->max_keys_per_buffer < num_available_keys)
  {
    filesort_info->alloc_sort_buffer(param->max_keys_per_buffer,
                                     param->rec_length);
    trace_filesort.add("chosen", true);
    DBUG_RETURN(filesort_info->sort_buffer_size() > 0);
  }

  // Try to strip off addon fields.
  if (!keep_addon_fields && param->using_addon_fields())
  {
    const ulong row_length=
      param->sort_length + param->ref_length + sizeof(char*);
    num_available_keys= memory_available / row_length;

    Opt_trace_object trace_addon(trace, "strip_additional_fields");
    trace_addon.add("row_size", row_length);

    // Can we fit all the keys in memory?
    if (param->max_keys_per_buffer >= num_available_keys)
    {
      trace_addon.add("chosen", false).add_alnum("cause", "not_enough_space");
    }
    else
    {
      const Cost_model_table *cost_model= table->cost_model();
      const double sort_merge_cost=
        get_merge_many_buffs_cost_fast(num_rows,
                                       num_available_keys,
                                       row_length, cost_model);
      trace_addon.add("sort_merge_cost", sort_merge_cost);
      /*
        PQ has cost:
        (insert + qsort) * log(queue size) * key_compare_cost() +
        cost of file lookup afterwards.
        The lookup cost is a bit pessimistic: we take table scan cost and
        assume that on average we find the row after scanning half of the file.
        A better estimate would be lookup cost, but note that we are doing
        random lookups here, rather than sequential scan.
      */
      const double pq_cpu_cost= 
        (PQ_slowness * num_rows + param->max_keys_per_buffer) *
        cost_model->key_compare_cost(log((double) param->max_keys_per_buffer));
      const Cost_estimate scan_cost= table->file->table_scan_cost();
      const double pq_io_cost=
        param->max_rows * scan_cost.total_cost() / 2.0;
      const double pq_cost= pq_cpu_cost + pq_io_cost;
      trace_addon.add("priority_queue_cost", pq_cost);

      if (sort_merge_cost < pq_cost)
      {
        trace_addon.add("chosen", false);
        DBUG_RETURN(false);
      }

      trace_addon.add("chosen", true);
      filesort_info->alloc_sort_buffer(param->max_keys_per_buffer,
                                       param->sort_length + param->ref_length);
      if (filesort_info->sort_buffer_size() > 0)
      {
        // Make attached data to be references instead of fields.
        filesort_info->addon_fields= NULL;
        param->addon_fields= NULL;

        param->res_length= param->ref_length;
        param->sort_length+= param->ref_length;
        param->rec_length= param->sort_length;

        DBUG_RETURN(true);
      }
    }
  }
  DBUG_RETURN(false);
}


/**
  Merges buffers to make < MERGEBUFF2 buffers.

  @param param        Sort parameters.
  @param sort_buffer  The main memory buffer.
  @param chunk_array  Array of chunk descriptors to merge.
  @param p_num_chunks [out]
                      output: the number of chunks left in the output file.
  @param t_file       Where to store the result.
*/
int merge_many_buff(Sort_param *param, Sort_buffer sort_buffer,
                    Merge_chunk_array chunk_array,
                    size_t *p_num_chunks, IO_CACHE *t_file)
{
  uint i;
  IO_CACHE t_file2,*from_file,*to_file,*temp;
  DBUG_ENTER("merge_many_buff");

  size_t num_chunks= chunk_array.size();
  *p_num_chunks= num_chunks;

  if (num_chunks <= MERGEBUFF2)
    DBUG_RETURN(0);				/* purecov: inspected */
  if (flush_io_cache(t_file) ||
      open_cached_file(&t_file2,mysql_tmpdir,TEMP_PREFIX,DISK_BUFFER_SIZE,
                       MYF(MY_WME)))
    DBUG_RETURN(1);				/* purecov: inspected */

  from_file= t_file ; to_file= &t_file2;
  while (num_chunks > MERGEBUFF2)
  {
    if (reinit_io_cache(from_file,READ_CACHE,0L,0,0))
      goto cleanup;
    if (reinit_io_cache(to_file,WRITE_CACHE,0L,0,0))
      goto cleanup;
    Merge_chunk *last_chunk= chunk_array.begin();;
    for (i=0 ; i < num_chunks - MERGEBUFF * 3 / 2 ; i+= MERGEBUFF)
    {
      if (merge_buffers(param,                  // param
                        from_file,              // from_file
                        to_file,                // to_file
                        sort_buffer,            // sort_buffer
                        last_chunk++,           // last_chunk [out]
                        Merge_chunk_array(&chunk_array[i], MERGEBUFF),
                        0))                     // flag
      goto cleanup;
    }
    if (merge_buffers(param,
                      from_file,
                      to_file,
                      sort_buffer,
                      last_chunk++,
                      Merge_chunk_array(&chunk_array[i], num_chunks - i),
                      0))
      break;					/* purecov: inspected */
    if (flush_io_cache(to_file))
      break;					/* purecov: inspected */
    temp=from_file; from_file=to_file; to_file=temp;
    setup_io_cache(from_file);
    setup_io_cache(to_file);
    num_chunks= last_chunk - chunk_array.begin();
  }
cleanup:
  close_cached_file(to_file);			// This holds old result
  if (to_file == t_file)
  {
    *t_file=t_file2;				// Copy result file
    setup_io_cache(t_file);
  }

  *p_num_chunks= num_chunks;
  DBUG_RETURN(num_chunks > MERGEBUFF2);  /* Return 1 if interrupted */
} /* merge_many_buff */


/**
  Read data to buffer.

  @returns
    (uint)-1 if something goes wrong
*/

uint read_to_buffer(IO_CACHE *fromfile,
                    Merge_chunk *merge_chunk,
                    Sort_param *param)
{
  DBUG_ENTER("read_to_buffer");
  uint rec_length= param->rec_length;
  ha_rows count;

  if ((count= min<ha_rows>(merge_chunk->max_keys(), merge_chunk->rowcount())))
  {
    size_t bytes_to_read;
    if (param->using_packed_addons())
    {
      count= merge_chunk->rowcount();
      bytes_to_read=
        min<my_off_t>(merge_chunk->buffer_size(),
                      fromfile->end_of_file - merge_chunk->file_position());
    }
    else
      bytes_to_read= rec_length * count;

    DBUG_PRINT("info", ("read_to_buffer %p at file_pos %llu bytes %llu",
                        merge_chunk,
                        static_cast<ulonglong>(merge_chunk->file_position()),
                        static_cast<ulonglong>(bytes_to_read)));
    if (mysql_file_pread(fromfile->file,
                         merge_chunk->buffer_start(),
                         bytes_to_read,
                         merge_chunk->file_position(), MYF_RW))
      DBUG_RETURN((uint) -1);			/* purecov: inspected */

    size_t num_bytes_read;
    if (param->using_packed_addons())
    {
      /*
        The last record read is most likely not complete here.
        We need to loop through all the records, reading the length fields,
        and then "chop off" the final incomplete record.
       */
      uchar *record= merge_chunk->buffer_start();
      uint ix= 0;
      for (; ix < count; ++ix)
      {
        if (record + param->sort_length + Addon_fields::size_of_length_field >=
            merge_chunk->buffer_end())
          break;                                // Incomplete record.
        uchar *plen= record + param->sort_length;
        uint res_length= Addon_fields::read_addon_length(plen);
        if (plen + res_length >= merge_chunk->buffer_end())
          break;                                // Incomplete record.
        DBUG_ASSERT(res_length > 0);
        record+= param->sort_length;
        record+= res_length;
      }
      DBUG_ASSERT(ix > 0);
      count= ix;
      num_bytes_read= record - merge_chunk->buffer_start();
      DBUG_PRINT("info", ("read %llu bytes of complete records",
                          static_cast<ulonglong>(bytes_to_read)));
    }
    else
      num_bytes_read= bytes_to_read;

    merge_chunk->init_current_key();
    merge_chunk->advance_file_position(num_bytes_read);
    merge_chunk->decrement_rowcount(count);
    merge_chunk->set_mem_count(count);
    DBUG_RETURN(num_bytes_read);
  }

  DBUG_RETURN (0);
} /* read_to_buffer */


/**
  Put all room used by freed buffer to use in adjacent buffer.

  Note, that we can't simply distribute memory evenly between all buffers,
  because new areas must not overlap with old ones.

  @param[in] queue      list of non-empty buffers, without freed buffer
*/
void Merge_chunk::reuse_freed_buff(QUEUE *queue)
{
  for (uint i= 0; i < queue->elements; ++i)
  {
    Merge_chunk *mc=
      static_cast<Merge_chunk*>(static_cast<void*>(queue_element(queue, i)));
    if (mc->m_buffer_end == m_buffer_start)
    {
      mc->m_buffer_end= m_buffer_end;
      mc->m_max_keys+= m_max_keys;
      return;
    }
    else if (mc->m_buffer_start == m_buffer_end)
    {
      mc->m_buffer_start= m_buffer_start;
      mc->m_max_keys+= m_max_keys;
      return;
    }
  }
  DBUG_ASSERT(0);
}


/**
  Merge buffers to one buffer.

  @param param          Sort parameter
  @param from_file      File with source data (Merge_chunks point to this file)
  @param to_file        File to write the sorted result data.
  @param sort_buffer    Buffer for data to store up to MERGEBUFF2 sort keys.
  @param [out] last_chunk Store here Merge_chunk describing data written to
                        to_file.
  @param chunk_array    Array of chunks to merge.
  @param flag

  @returns
    0      OK
  @returns
    other  error
*/

int merge_buffers(Sort_param *param, IO_CACHE *from_file,
                  IO_CACHE *to_file, Sort_buffer sort_buffer,
                  Merge_chunk *last_chunk,
                  Merge_chunk_array chunk_array,
                  int flag)
{
  int error;
  uint rec_length,res_length;
  size_t sort_length;
  ulong maxcount;
  ha_rows max_rows,org_max_rows;
  my_off_t to_start_filepos;
  uchar *strpos;
  Merge_chunk *merge_chunk;
  QUEUE queue;
  qsort2_cmp cmp;
  void *first_cmp_arg;
  volatile THD::killed_state *killed= &current_thd->killed;
  THD::killed_state not_killable;
  DBUG_ENTER("merge_buffers");

  current_thd->inc_status_sort_merge_passes();
  if (param->not_killable)
  {
    killed= &not_killable;
    not_killable= THD::NOT_KILLED;
  }

  error=0;
  rec_length= param->rec_length;
  res_length= param->res_length;
  sort_length= param->sort_length;
  const uint offset= (flag == 0) ? 0 : (rec_length - res_length);
  maxcount= (ulong) (param->max_keys_per_buffer / chunk_array.size());
  to_start_filepos= my_b_tell(to_file);
  strpos= sort_buffer.array();
  org_max_rows=max_rows= param->max_rows;

  /* The following will fire if there is not enough space in sort_buffer */
  DBUG_ASSERT(maxcount!=0);

  if (param->unique_buff)
  {
    cmp= param->compare;
    first_cmp_arg= &param->cmp_context;
  }
  else
  {
    cmp= get_ptr_compare(sort_length);
    first_cmp_arg= &sort_length;
  }
  if (init_queue(&queue, (uint) chunk_array.size(),
                 Merge_chunk::offset_to_key(), 0,
                 (queue_compare) cmp, first_cmp_arg))
    DBUG_RETURN(1);                                /* purecov: inspected */
  for (merge_chunk= chunk_array.begin() ;
       merge_chunk != chunk_array.end() ; merge_chunk++)
  {
    merge_chunk->set_buffer(strpos,
                            strpos + (sort_buffer.size()/(chunk_array.size())));
    merge_chunk->set_max_keys(maxcount);
    strpos+=
      (uint) (error= (int)read_to_buffer(from_file, merge_chunk, param));
    merge_chunk->set_buffer_end(strpos);
    if (error == -1)
      goto err;					/* purecov: inspected */
    // If less data in buffers than expected
    merge_chunk->set_max_keys(merge_chunk->mem_count());
    queue_insert(&queue, (uchar*) merge_chunk);
  }

  if (param->unique_buff)
  {
    DBUG_ASSERT(!param->using_packed_addons());
    /* 
       Called by Unique::get()
       Copy the first argument to param->unique_buff for unique removal.
       Store it also in 'to_file'.

       This is safe as we know that there is always more than one element
       in each block to merge (This is guaranteed by the Unique:: algorithm
    */
    merge_chunk= (Merge_chunk*) queue_top(&queue);
    memcpy(param->unique_buff, merge_chunk->current_key(), rec_length);
    if (my_b_write(to_file, merge_chunk->current_key(), rec_length))
    {
      error=1; goto err;                        /* purecov: inspected */
    }
    merge_chunk->advance_current_key(rec_length);
    merge_chunk->decrement_mem_count();
    if (!--max_rows)
    {
      error= 0;                                       /* purecov: inspected */
      goto end;                                       /* purecov: inspected */
    }
    queue_replaced(&queue);                        // Top element has been used
  }
  else
    cmp= 0;                                        // Not unique

  while (queue.elements > 1)
  {
    if (*killed)
    {
      error= 1; goto err;                        /* purecov: inspected */
    }
    for (;;)
    {
      merge_chunk= (Merge_chunk*) queue_top(&queue);
      if (cmp)                                        // Remove duplicates
      {
        DBUG_ASSERT(!param->using_packed_addons());
        uchar *current_key= merge_chunk->current_key();
        if (!(*cmp)(first_cmp_arg, &(param->unique_buff), &current_key))
          goto skip_duplicate;
        memcpy(param->unique_buff, merge_chunk->current_key(), rec_length);
      }
      {
        param->get_rec_and_res_len(merge_chunk->current_key(),
                                   &rec_length, &res_length);
        const uint bytes_to_write= (flag == 0) ? rec_length : res_length;

        DBUG_PRINT("info", ("write record at %llu len %u",
                            my_b_tell(to_file), bytes_to_write));
        if (my_b_write(to_file,
                       merge_chunk->current_key() + offset, bytes_to_write))
        {
          error=1; goto err;                    /* purecov: inspected */
        }
        if (!--max_rows)
        {
          error= 0;                             /* purecov: inspected */
          goto end;                             /* purecov: inspected */
        }
      }

    skip_duplicate:
      merge_chunk->advance_current_key(rec_length);
      merge_chunk->decrement_mem_count();
      if (0 == merge_chunk->mem_count())
      {
        if (!(error= (int) read_to_buffer(from_file, merge_chunk, param)))
        {
          (void) queue_remove(&queue,0);
          merge_chunk->reuse_freed_buff(&queue);
          break;                        /* One buffer have been removed */
        }
        else if (error == -1)
          goto err;                        /* purecov: inspected */
      }
      /*
        The Merge_chunk at the queue's top had one of its keys consumed, thus
        it may now rank differently in the comparison order of the queue, so:
      */
      queue_replaced(&queue);
    }
  }
  merge_chunk= (Merge_chunk*) queue_top(&queue);
  merge_chunk->set_buffer(sort_buffer.array(),
                          sort_buffer.array() + sort_buffer.size());
  merge_chunk->set_max_keys(param->max_keys_per_buffer);

  /*
    As we know all entries in the buffer are unique, we only have to
    check if the first one is the same as the last one we wrote
  */
  if (cmp)
  {
    uchar *current_key= merge_chunk->current_key();
    if (!(*cmp)(first_cmp_arg, &(param->unique_buff), &current_key))
    {
      merge_chunk->advance_current_key(rec_length); // Remove duplicate
      merge_chunk->decrement_mem_count();
    }
  }

  do
  {
    if ((ha_rows) merge_chunk->mem_count() > max_rows)
    {
      merge_chunk->set_mem_count(max_rows); /* Don't write too many records */
      merge_chunk->set_rowcount(0);         /* Don't read more */
    }
    max_rows-= merge_chunk->mem_count();

    for (uint ix= 0; ix < merge_chunk->mem_count(); ++ix)
    {
      param->get_rec_and_res_len(merge_chunk->current_key(),
                                 &rec_length, &res_length);
      const uint bytes_to_write= (flag == 0) ? rec_length : res_length;
      if (my_b_write(to_file,
                     merge_chunk->current_key() + offset,
                     bytes_to_write))
      {
        error= 1; goto err;                   /* purecov: inspected */
      }
      merge_chunk->advance_current_key(rec_length);
    }
  }
  while ((error=(int) read_to_buffer(from_file, merge_chunk, param))
         != -1 && error != 0);

end:
  last_chunk->set_rowcount(min(org_max_rows-max_rows, param->max_rows));
  last_chunk->set_file_position(to_start_filepos);
err:
  delete_queue(&queue);
  DBUG_RETURN(error);
} /* merge_buffers */


	/* Do a merge to output-file (save only positions) */

static int merge_index(Sort_param *param, Sort_buffer sort_buffer,
                       Merge_chunk_array chunk_array,
                       IO_CACHE *tempfile, IO_CACHE *outfile)
{
  DBUG_ENTER("merge_index");
  if (merge_buffers(param,                    // param
                    tempfile,                 // from_file
                    outfile,                  // to_file
                    sort_buffer,              // sort_buffer
                    chunk_array.begin(),      // last_chunk [out]
                    chunk_array,
                    1))                       // flag
    DBUG_RETURN(1);                           /* purecov: inspected */
  DBUG_RETURN(0);
} /* merge_index */


static uint suffix_length(ulong string_length)
{
  if (string_length < 256)
    return 1;
  if (string_length < 256L*256L)
    return 2;
  if (string_length < 256L*256L*256L)
    return 3;
  return 4;                                     // Can't sort longer than 4G
}



/**
  Calculate length of sort key.

  @param thd			  Thread handler
  @param sortorder		  Order of items to sort
  @param s_length	          Number of items to sort
  @param[out] multi_byte_charset Set to 1 if we are using multi-byte charset
                                 (In which case we have to use strxnfrm())

  @note
    sortorder->length is updated for each sort item.
  @n
    sortorder->need_strxnfrm is set 1 if we have to use strxnfrm

  @return
    Total length of sort buffer in bytes
*/

uint
sortlength(THD *thd, SORT_FIELD *sortorder, uint s_length,
           bool *multi_byte_charset)
{
  uint total_length= 0;
  const CHARSET_INFO *cs;
  *multi_byte_charset= false;

  for (; s_length-- ; sortorder++)
  {
    sortorder->need_strxnfrm= 0;
    sortorder->suffix_length= 0;
    if (sortorder->field)
    {
      cs= sortorder->field->sort_charset();
      sortorder->length= sortorder->field->sort_length();

      if (use_strnxfrm((cs=sortorder->field->sort_charset())))
      {
        sortorder->need_strxnfrm= 1;
        *multi_byte_charset= 1;
        sortorder->length= cs->coll->strnxfrmlen(cs, sortorder->length);
      }
      if (sortorder->field->maybe_null())
        total_length++;                       // Place for NULL marker

      if (sortorder->field->result_type() == STRING_RESULT &&
          !sortorder->field->is_temporal())
      {
        set_if_smaller(sortorder->length, thd->variables.max_sort_length);
      }
    }
    else
    {
      sortorder->result_type= sortorder->item->result_type();
      if (sortorder->item->is_temporal())
        sortorder->result_type= INT_RESULT;
      switch (sortorder->result_type) {
      case STRING_RESULT:
	sortorder->length= sortorder->item->max_length;
        set_if_smaller(sortorder->length, thd->variables.max_sort_length);
	if (use_strnxfrm((cs=sortorder->item->collation.collation)))
	{ 
          sortorder->length= cs->coll->strnxfrmlen(cs, sortorder->length);
	  sortorder->need_strxnfrm= 1;
	  *multi_byte_charset= 1;
	}
        else if (cs == &my_charset_bin)
        {
          /* Store length last to be able to sort blob/varbinary */
          sortorder->suffix_length= suffix_length(sortorder->length);
          sortorder->length+= sortorder->suffix_length;
        }
	break;
      case INT_RESULT:
#if SIZEOF_LONG_LONG > 4
	sortorder->length=8;			// Size of intern longlong
#else
	sortorder->length=4;
#endif
	break;
      case DECIMAL_RESULT:
        sortorder->length=
          my_decimal_get_binary_size(sortorder->item->max_length - 
                                     (sortorder->item->decimals ? 1 : 0),
                                     sortorder->item->decimals);
        break;
      case REAL_RESULT:
	sortorder->length=sizeof(double);
	break;
      case ROW_RESULT:
      default: 
	// This case should never be choosen
	DBUG_ASSERT(0);
	break;
      }
      if (sortorder->item->maybe_null)
        total_length++;                       // Place for NULL marker
    }
    total_length+= sortorder->length;
  }
  sortorder->field= NULL;                       // end marker
  DBUG_PRINT("info",("sort_length: %u", total_length));
  return total_length;
}


/**
  Get descriptors of fields appended to sorted fields and
  calculate their total length.

  The function first finds out what fields are used in the result set.
  Then it calculates the length of the buffer to store the values of
  these fields together with the value of sort values. 
  If the calculated length is not greater than max_length_for_sort_data
  the function allocates memory for an array of descriptors containing
  layouts for the values of the non-sorted fields in the buffer and
  fills them.

  @param max_length_for_sort_data Value of session variable.
  @param ptabfield             Array of references to the table fields
  @param sortlength            Total length of sorted fields
  @param[out] plength          Total length of appended fields
  @param[out] ppackable_length Total length of appended fields having a
                               packable type

  @note
    The null bits for the appended values are supposed to be put together
    and stored into the buffer just ahead of the value of the first field.

  @return
    Pointer to the layout descriptors for the appended fields, if any
  @returns
    NULL   if we do not store field values with sort data.
*/

Addon_fields *
Filesort::get_addon_fields(ulong max_length_for_sort_data,
                           Field **ptabfield, uint sortlength, uint *plength,
                           uint *ppackable_length)
{
  Field **pfield;
  Field *field;
  uint total_length= 0;
  uint packable_length= 0;
  uint num_fields= 0;
  uint null_fields= 0;
  MY_BITMAP *read_set= (*ptabfield)->table->read_set;

  /*
    If there is a reference to a field in the query add it
    to the the set of appended fields.
    Note for future refinement:
    This this a too strong condition.
    Actually we need only the fields referred in the
    result set. And for some of them it makes sense to use 
    the values directly from sorted fields.
  */
  *plength= *ppackable_length= 0;

  for (pfield= ptabfield; (field= *pfield) ; pfield++)
  {
    if (!bitmap_is_set(read_set, field->field_index))
      continue;
    if (field->flags & BLOB_FLAG)
    {
      DBUG_ASSERT(addon_fields == NULL);
      return NULL;
    }

    const uint field_length= field->max_packed_col_length();
    total_length+= field_length;

    const enum_field_types field_type= field->type();
    if (field->maybe_null() ||
        field_type == MYSQL_TYPE_STRING ||
        field_type == MYSQL_TYPE_VARCHAR ||
        field_type == MYSQL_TYPE_VAR_STRING)
      packable_length+= field_length;
    if (field->maybe_null())
      null_fields++;
    num_fields++;
  }
  if (0 == num_fields)
    return NULL;

  total_length+= (null_fields + 7) / 8;

  *ppackable_length= packable_length;

  if (total_length + sortlength > max_length_for_sort_data)
  {
    DBUG_ASSERT(addon_fields == NULL);
    return NULL;
  }

  if (addon_fields == NULL)
  {
    void *rawmem1= sql_alloc(sizeof(Addon_fields));
    void *rawmem2= sql_alloc(sizeof(Sort_addon_field) * num_fields);
    if (rawmem1 == NULL || rawmem2 == NULL)
      return NULL;                            /* purecov: inspected */
    Addon_fields_array
      addon_array(static_cast<Sort_addon_field*>(rawmem2), num_fields);
    addon_fields= new (rawmem1) Addon_fields(addon_array);
  }
  else
  {
    /*
      Allocate memory only once, reuse descriptor array and buffer.
      Set using_packed_addons here, and size/offset details below.
     */
    DBUG_ASSERT(num_fields == addon_fields->num_field_descriptors());
    addon_fields->set_using_packed_addons(false);
  }

  *plength= total_length;

  uint length= (null_fields + 7) / 8;
  null_fields= 0;
  Addon_fields_array::iterator addonf= addon_fields->begin();
  for (pfield= ptabfield; (field= *pfield) ; pfield++)
  {
    if (!bitmap_is_set(read_set, field->field_index))
      continue;
    DBUG_ASSERT(addonf != addon_fields->end());

    addonf->field= field;
    addonf->offset= length;
    if (field->maybe_null())
    {
      addonf->null_offset= null_fields / 8;
      addonf->null_bit= 1 << (null_fields & 7);
      null_fields++;
    }
    else
    {
      addonf->null_offset= 0;
      addonf->null_bit= 0;
    }
    addonf->max_length= field->max_packed_col_length();
    DBUG_PRINT("info", ("addon_field %s max_length %u",
                        addonf->field->field_name, addonf->max_length));

    length+= addonf->max_length;
    addonf++;
  }

  DBUG_PRINT("info",("addon_length: %d",length));
  return addon_fields;
}


/*
** functions to change a double or float to a sortable string
** The following should work for IEEE
*/

#define DBL_EXP_DIG (sizeof(double)*8-DBL_MANT_DIG)

void change_double_for_sort(double nr,uchar *to)
{
  uchar *tmp=(uchar*) to;
  if (nr == 0.0)
  {						/* Change to zero string */
    tmp[0]=(uchar) 128;
    memset(tmp+1, 0, sizeof(nr)-1);
  }
  else
  {
#ifdef WORDS_BIGENDIAN
    memcpy(tmp, &nr, sizeof(nr));
#else
    {
      uchar *ptr= (uchar*) &nr;
#if defined(__FLOAT_WORD_ORDER) && (__FLOAT_WORD_ORDER == __BIG_ENDIAN)
      tmp[0]= ptr[3]; tmp[1]=ptr[2]; tmp[2]= ptr[1]; tmp[3]=ptr[0];
      tmp[4]= ptr[7]; tmp[5]=ptr[6]; tmp[6]= ptr[5]; tmp[7]=ptr[4];
#else
      tmp[0]= ptr[7]; tmp[1]=ptr[6]; tmp[2]= ptr[5]; tmp[3]=ptr[4];
      tmp[4]= ptr[3]; tmp[5]=ptr[2]; tmp[6]= ptr[1]; tmp[7]=ptr[0];
#endif
    }
#endif
    if (tmp[0] & 128)				/* Negative */
    {						/* make complement */
      uint i;
      for (i=0 ; i < sizeof(nr); i++)
	tmp[i]=tmp[i] ^ (uchar) 255;
    }
    else
    {					/* Set high and move exponent one up */
      ushort exp_part=(((ushort) tmp[0] << 8) | (ushort) tmp[1] |
		       (ushort) 32768);
      exp_part+= (ushort) 1 << (16-1-DBL_EXP_DIG);
      tmp[0]= (uchar) (exp_part >> 8);
      tmp[1]= (uchar) exp_part;
    }
  }
}
