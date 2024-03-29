/*****************************************************************************

Copyright (c) 1996, 2013, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/trx0purge.h
Purge old versions

Created 3/26/1996 Heikki Tuuri
*******************************************************/

#ifndef trx0purge_h
#define trx0purge_h

#include "univ.i"
#include "trx0types.h"
#include "mtr0mtr.h"
#include "trx0sys.h"
#include "que0types.h"
#include "page0page.h"
#include "usr0sess.h"
#include "fil0fil.h"
#include "read0types.h"

// Forward declaration
struct ib_bh_t;

/** The global data structure coordinating a purge */
extern trx_purge_t*	purge_sys;

/** A dummy undo record used as a return value when we have a whole undo log
which needs no purge */
extern trx_undo_rec_t	trx_purge_dummy_rec;

/********************************************************************//**
Calculates the file address of an undo log header when we have the file
address of its history list node.
@return file address of the log */
UNIV_INLINE
fil_addr_t
trx_purge_get_log_from_hist(
/*========================*/
	fil_addr_t	node_addr);	/*!< in: file address of the history
					list node of the log */
/********************************************************************//**
Creates the global purge system control structure and inits the history
mutex. */

void
trx_purge_sys_create(
/*=================*/
	ulint		n_purge_threads,/*!< in: number of purge threads */
	purge_pq_t*	purge_queue);	/*!< in/own: UNDO log min binary heap*/
/********************************************************************//**
Frees the global purge system control structure. */

void
trx_purge_sys_close(void);
/*======================*/
/************************************************************************
Adds the update undo log as the first log in the history list. Removes the
update undo log segment from the rseg slot if it is too big for reuse. */

void
trx_purge_add_update_undo_to_history(
/*=================================*/
	trx_t*		trx,		/*!< in: transaction */
	trx_undo_ptr_t*	undo_ptr,	/*!< in: update undo log. */
	page_t*		undo_page,	/*!< in: update undo log header page,
					x-latched */
	bool		update_rseg_history_len,
					/*!< in: if true: update rseg history
					len else skip updating it. */
	ulint		n_added_logs,	/*!< in: number of logs added */
	mtr_t*		mtr);		/*!< in: mtr */
/*******************************************************************//**
This function runs a purge batch.
@return number of undo log pages handled in the batch */

ulint
trx_purge(
/*======*/
	ulint	n_purge_threads,	/*!< in: number of purge tasks to
					submit to task queue. */
	ulint	limit,			/*!< in: the maximum number of
					records to purge in one batch */
	bool	truncate);		/*!< in: truncate history if true */
/*******************************************************************//**
Stop purge and wait for it to stop, move to PURGE_STATE_STOP. */

void
trx_purge_stop(void);
/*================*/
/*******************************************************************//**
Resume purge, move to PURGE_STATE_RUN. */

void
trx_purge_run(void);
/*================*/

/** Purge states */
enum purge_state_t {
	PURGE_STATE_INIT,		/*!< Purge instance created */
	PURGE_STATE_RUN,		/*!< Purge should be running */
	PURGE_STATE_STOP,		/*!< Purge should be stopped */
	PURGE_STATE_EXIT,		/*!< Purge has been shutdown */
	PURGE_STATE_DISABLED		/*!< Purge was never started */
};

/*******************************************************************//**
Get the purge state.
@return purge state. */

purge_state_t
trx_purge_state(void);
/*=================*/

// Forward declaration
struct TrxUndoRsegsIterator;

/** This is the purge pointer/iterator. We need both the undo no and the
transaction no up to which purge has parsed and applied the records. */
struct purge_iter_t {
	purge_iter_t()
		:
		trx_no(),
		undo_no(),
		undo_rseg_space(ULINT_UNDEFINED)
	{
		// Do nothing
	}

	trx_id_t	trx_no;		/*!< Purge has advanced past all
					transactions whose number is less
					than this */
	undo_no_t	undo_no;	/*!< Purge has advanced past all records
					whose undo number is less than this */
	ulint		undo_rseg_space;
					/*!< Last undo record resided in this
					space id. */
};

/** The control structure used in the purge operation */
struct trx_purge_t{
	sess_t*		sess;		/*!< System session running the purge
					query */
	trx_t*		trx;		/*!< System transaction running the
					purge query: this trx is not in the
					trx list of the trx system and it
					never ends */
	rw_lock_t	latch;		/*!< The latch protecting the purge
					view. A purge operation must acquire an
					x-latch here for the instant at which
					it changes the purge view: an undo
					log operation can prevent this by
					obtaining an s-latch here. It also
					protects state and running */
	os_event_t	event;		/*!< State signal event */
	ulint		n_stop;		/*!< Counter to track number stops */
	volatile bool	running;	/*!< true, if purge is active,
					we check this without the latch too */
	volatile purge_state_t	state;	/*!< Purge coordinator thread states,
					we check this in several places
					without holding the latch. */
	que_t*		query;		/*!< The query graph which will do the
					parallelized purge operation */
	ReadView	view;		/*!< The purge will not remove undo logs
					which are >= this view (purge view) */
	bool		view_active;	/*!< true if view is active */
	volatile ulint	n_submitted;	/*!< Count of total tasks submitted
					to the task queue */
	volatile ulint	n_completed;	/*!< Count of total tasks completed */

	/*------------------------------*/
	/* The following two fields form the 'purge pointer' which advances
	during a purge, and which is used in history list truncation */

	purge_iter_t	iter;		/* Limit up to which we have read and
					parsed the UNDO log records.  Not
					necessarily purged from the indexes.
					Note that this can never be less than
					the limit below, we check for this
					invariant in trx0purge.cc */
	purge_iter_t	limit;		/* The 'purge pointer' which advances
					during a purge, and which is used in
					history list truncation */
#ifdef UNIV_DEBUG
	purge_iter_t	done;		/* Indicate 'purge pointer' which have
					purged already accurately. */
#endif /* UNIV_DEBUG */
	/*-----------------------------*/
	ibool		next_stored;	/*!< TRUE if the info of the next record
					to purge is stored below: if yes, then
					the transaction number and the undo
					number of the record are stored in
					purge_trx_no and purge_undo_no above */
	trx_rseg_t*	rseg;		/*!< Rollback segment for the next undo
					record to purge */
	ulint		page_no;	/*!< Page number for the next undo
					record to purge, page number of the
					log header, if dummy record */
	ulint		offset;		/*!< Page offset for the next undo
					record to purge, 0 if the dummy
					record */
	ulint		hdr_page_no;	/*!< Header page of the undo log where
					the next record to purge belongs */
	ulint		hdr_offset;	/*!< Header byte offset on the page */


	TrxUndoRsegsIterator*
			rseg_iter;	/*!< Iterator to get the next rseg
					to process */

	/*-----------------------------*/
	ib_bh_t*	ib_bh;		/*!< Binary min-heap, ordered on
					rseg_queue_t::trx_no. It is protected
					by the bh_mutex */
	purge_pq_t*	purge_queue;	/*!< Binary min-heap, ordered on
					TrxUndoRsegs::trx_no. It is protected
					by the pq_mutex */
	PQMutex		pq_mutex;	/*!< Mutex protecting purge_queue */
};

/** Info required to purge a record */
struct trx_purge_rec_t {
	trx_undo_rec_t*	undo_rec;	/*!< Record to purge */
	roll_ptr_t	roll_ptr;	/*!< File pointr to UNDO record */
};

/**
Chooses the rollback segment with the smallest trx_no. */
struct TrxUndoRsegsIterator {

	/** Constructor */
	TrxUndoRsegsIterator(trx_purge_t* purge_sys);

	/** Sets the next rseg to purge in m_purge_sys.
	@return page size of the table for which the log is.
	NOTE: if rseg is NULL when this function returns this means that
	there are no rollback segments to purge and then the returned page
	size object should not be used. */
	const page_size_t set_next();

private:
	// Disable copying
	TrxUndoRsegsIterator(const TrxUndoRsegsIterator&);
	TrxUndoRsegsIterator& operator=(const TrxUndoRsegsIterator&);

	/** The purge system pointer */
	trx_purge_t*			m_purge_sys;

	/** The current element to process */
	TrxUndoRsegs			m_trx_undo_rsegs;

	/** Track the current element in m_trx_undo_rseg */
	TrxUndoRsegs::iterator		m_iter;

	/** Sentinel value */
	static const TrxUndoRsegs	NullElement;
};

#ifndef UNIV_NONINL
#include "trx0purge.ic"
#endif /* UNIV_NOINL */

#endif /* trx0purge_h */
