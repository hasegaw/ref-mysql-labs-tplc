#ifndef OPT_COSTMODEL_INCLUDED
#define OPT_COSTMODEL_INCLUDED

/*
   Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "my_dbug.h"                            // DBUG_ASSERT
#include "sql_const.h"                          // defines for cost constants


/**
  API for getting cost estimates for server operations that are not
  directly related to a table object.
*/

class Cost_model_server
{
public:
  /**
    Temporary table types that the cost model differentiate between.
  */
  enum enum_tmptable_type { MEMORY_TMPTABLE, DISK_TMPTABLE };

  Cost_model_server()
  {
#if !defined(DBUG_OFF)
    m_initialized= false;
#endif
  }

  /**
    Initialize the cost model object for a query.

    This function must be called before calling any cost estimation
    functions for a query. It should also be called when starting
    optimization of a new query in case any cost estimate constants
    have changed.
  */

  void init();

  /**
    Cost of processing a number of records and evaluating the query condition
    on the records.

    @param rows number of rows to evaluate

    @return Cost of evaluating the records
  */

  double row_evaluate_cost(double rows) const
  {
    DBUG_ASSERT(m_initialized);
    DBUG_ASSERT(rows >= 0.0);
    return rows * ROW_EVALUATE_COST;
  }

  /**
    Cost of doing a number of key compare operations.

    @param keys number of key compare operations

    @return Cost of comparing the keys
  */

  double key_compare_cost(double keys) const
  {
    DBUG_ASSERT(m_initialized);
    DBUG_ASSERT(keys >= 0.0);
    return keys * ROWID_COMPARE_COST;
  }

private:
  /**
    Cost of creating a temporary table in the memory storage engine.
  */

  double memory_tmptable_create_cost() const
  {
    return HEAP_TEMPTABLE_CREATE_COST;
  }

  /**
    Cost of storing or retrieving a row using the memory storage engine.
  */

  double memory_tmptable_row_cost() const { return HEAP_TEMPTABLE_ROW_COST; }

  /**
    Cost of creating a temporary table using a disk based storage engine.
  */

  double disk_tmptable_create_cost() const
  {
    return DISK_TEMPTABLE_CREATE_COST;
  }

  /**
    Cost of storing or retriving a row using a disk based storage engine.
  */

  double disk_tmptable_row_cost() const { return DISK_TEMPTABLE_ROW_COST; }

  /**
    Cost estimate for a row operation (insert, read) on a temporary table.

    @param tmptable_type storage type for the temporary table

    @return The estimated cost
  */

  double tmptable_row_cost(enum_tmptable_type tmptable_type) const
  {
    if (tmptable_type == MEMORY_TMPTABLE)
      return memory_tmptable_row_cost();
    return disk_tmptable_row_cost();
  }

public:
  /**
    Cost estimate for creating a temporary table.

    @param tmptable_type storage type for the temporary table

    @return Cost estimate
  */

  double tmptable_create_cost(enum_tmptable_type tmptable_type) const
  {
    DBUG_ASSERT(m_initialized);

    if (tmptable_type == MEMORY_TMPTABLE)
      return memory_tmptable_create_cost();
    return disk_tmptable_create_cost();
  }

  /**
    Cost estimate for inserting and reading records from a
    temporary table.

    @param tmptable_type storage type for the temporary table
    @param write_rows    number of rows that will be written to table
    @param read_rows     number of rows that will be read from table

    @return The estimated cost
  */

  double tmptable_readwrite_cost(enum_tmptable_type tmptable_type, 
                                 double write_rows, double read_rows) const
  {
    DBUG_ASSERT(m_initialized);
    DBUG_ASSERT(write_rows >= 0.0);
    DBUG_ASSERT(read_rows >= 0.0);

    return (write_rows + read_rows) * tmptable_row_cost(tmptable_type);
  }

private:
#if !defined(DBUG_OFF)
  /**
    Used for detecting if this object is used without having been initialized.
  */
  bool m_initialized;
#endif
};


/**
  API for getting cost estimates for operations on table data.

  @note The initial implementation mostly has functions for accessing
  cost constants for basic operations.
*/

class Cost_model_table
{
public:
  Cost_model_table() : m_cost_model_server(NULL)
  {
#if !defined(DBUG_OFF)
    m_initialized= false;
#endif
  }

  /**
    Initializes the cost model object.

    This function must be called before calling any cost estimation
    functions for a query. It should also be called when starting
    optimization of a new query in case any cost estimate constants
    have changed.
 
    @param cost_model_server the main cost model object for this query
  */

  void init(const Cost_model_server *cost_model_server);

  /**
    Cost of processing a number of records and evaluating the query condition
    on the records.

    @param rows number of rows to evaluate

    @return Cost of evaluating the records
  */

  double row_evaluate_cost(double rows) const
  {
    DBUG_ASSERT(m_initialized);
    DBUG_ASSERT(rows >= 0.0);

    return m_cost_model_server->row_evaluate_cost(rows);
  }

  /**
    Cost of doing a number of key compare operations.

    @param keys number of key compare operations

    @return Cost of comparing the keys
  */

  double key_compare_cost(double keys) const
   {
    DBUG_ASSERT(m_initialized);
    DBUG_ASSERT(keys >= 0.0);

    return m_cost_model_server->key_compare_cost(keys);
  }

  /**
    Cost of reading one random block from disk.
  */

  double io_block_read_cost() const
  {
    DBUG_ASSERT(m_initialized);

    return 1.0;
  }

  /**
    Cost of reading a number of random blocks from a table.

    @param blocks number of blocks to read

    @return Cost estimate
  */

  double io_block_read_cost(double blocks) const
  {
    DBUG_ASSERT(m_initialized);
    DBUG_ASSERT(blocks >= 0.0);

    return blocks * io_block_read_cost();
  }

  /**
    The fixed part of the cost for doing a sequential seek on disk.

    For a harddisk, this corresponds to half a rotation (see comment 
    for get_sweep_read_cost() in handler.cc).
  */

  double disk_seek_base_cost() const
  {
    DBUG_ASSERT(m_initialized);

    return DISK_SEEK_BASE_COST * io_block_read_cost();
  }

private:
  /**
    The cost for seeking past one block in a sequential seek.

    For a harddisk, this represents the cost of having to move the
    disk head to the correct cylinder.

    @todo Check that the BLOCKS_IN_AV_SEEK is correct to include in the
          DISK_SEEK_PROP_COST (@see sql_const.h).

    See the comments for this constant in sql_const.h.
  */

  double disk_seek_prop_cost() const
  {
    return DISK_SEEK_PROP_COST * io_block_read_cost();
  }

public:
  /**
    Cost estimate for a sequential disk seek where a given number of blocks
    are skipped.

    @param seek_blocks number of blocks to seek past

    @return The cost estimate for the seek operation
  */

  double disk_seek_cost(double seek_blocks) const
  {
    DBUG_ASSERT(seek_blocks >= 0.0);
    DBUG_ASSERT(m_initialized);

    const double cost= disk_seek_base_cost() +
                       disk_seek_prop_cost() * seek_blocks;
    return cost;
  }

private:
  /**
    Pointer to the cost model for the query. This is used for getting
    cost estimates for server operations.
  */
  const Cost_model_server *m_cost_model_server;

#if !defined(DBUG_OFF)
  /**
    Used for detecting if this object is used without having been initialized.
  */
  bool m_initialized;
#endif
};

#endif /* OPT_COSTMODEL_INCLUDED */
