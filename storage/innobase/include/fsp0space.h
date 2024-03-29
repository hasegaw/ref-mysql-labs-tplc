/*****************************************************************************

Copyright (c) 2013, 2014, Oracle and/or its affiliates. All Rights Reserved.

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
@file include/fsp0space.h
General shared tablespace implementation.

Created 2013-7-26 by Kevin Lewis
*******************************************************/

#ifndef fsp0space_h
#define fsp0space_h

#include "univ.i"
#include "fsp0file.h"
#include "fsp0fsp.h"
#include <vector>

/** Data structure that contains the information about shared tablespaces.
Currently this can be the system tablespace or a temporary table tablespace */
class Tablespace {

public:
	typedef std::vector<Datafile> files_t;

	/** Data file information - each Datafile can be accessed globally */
	files_t		m_files;

	Tablespace()
		:
		m_files(),
		m_name(),
		m_space_id(ULINT_UNDEFINED),
		m_path(),
		m_flags(),
		m_min_flushed_lsn(LSN_MAX),
		m_max_flushed_lsn(0)
	{
		/* No op */
	}

	virtual ~Tablespace()
	{
		shutdown();
		ut_ad(m_files.empty());
		ut_ad(m_space_id == ULINT_UNDEFINED);
		if (m_name != NULL) {
			::free(m_name);
			m_name = NULL;
		}
		if (m_path != NULL) {
			::free(m_path);
			m_path = NULL;
		}
	}

	// Disable copying
	Tablespace(const Tablespace&);
	Tablespace& operator=(const Tablespace&);

	/** Set tablespace name
	@param name	Tablespace name */
	void set_name(const char* name)
	{
		ut_ad(m_name == NULL);
		m_name = ::strdup(name);
		ut_ad(m_name != NULL);
	}

	/** Get tablespace name
	@return tablespace name */
	const char* name()	const
	{
		return(m_name);
	}

	/** Set tablespace path and filename members.
	@param path	where tablespace file(s) resides */
	void set_path(const char* path)
	{
		ut_ad(m_path == NULL);
		m_path = ::strdup(path);
		ut_ad(m_path != NULL);

		os_normalize_path_for_win(m_path);
	}

	/** Get tablespace path
	@return tablespace path */
	const char* path()	const
	{
		return(m_path);
	}

	/** Set the space id of the tablespace
	@param space_id	 Tablespace ID to set */
	void set_space_id(ulint space_id)
	{
		ut_ad(m_space_id == ULINT_UNDEFINED);
		m_space_id = space_id;
	}

	/** Get the space id of the tablespace
	@return m_space_id space id of the tablespace */
	ulint space_id()	const
	{
		return(m_space_id);
	}

	/** Set the tablespace flags
	@param fsp_flags	Tablespace flags */
	void set_flags(ulint fsp_flags)
	{
		ut_ad(fsp_flags_is_valid(fsp_flags));
		m_flags = fsp_flags;
	}

	/** Get the tablespace flags
	@return m_flags tablespace flags */
	ulint flags()	const
	{
		return(m_flags);
	}

	/** Set the minimum flushed LSN found in this tablespace
	@param lsn	A flushed lsn for a datafile */
	void set_min_flushed_lsn(lsn_t lsn)
	{
		if (m_min_flushed_lsn > lsn) {
			m_min_flushed_lsn = lsn;
		}
	}

	/** Get the minimum flushed LSN found in this tablespace
	@return m_min_flushed_lsn for this tablespaces */
	lsn_t min_flushed_lsn()	const
	{
		return(m_min_flushed_lsn);
	}

	/** Set the maximum flushed LSN found in this tablespace
	@param lsn	A flushed lsn for a datafile */
	void set_max_flushed_lsn(lsn_t lsn)
	{
		if (m_max_flushed_lsn < lsn) {
			m_max_flushed_lsn = lsn;
		}
	}

	/** Get the maximum flushed LSN found in this tablespace
	@return m_max_flushed_lsn for this tablespaces */
	lsn_t max_flushed_lsn()	const
	{
		return(m_max_flushed_lsn);
	}

	/** Free the memory allocated by the Tablespace object */
	void shutdown();

	/**
	@return ULINT_UNDEFINED if the size is invalid else the sum of sizes */
	ulint get_sum_of_sizes() const;

	/** Open or Create the data files if they do not exist.
	@param[in]	is_temp	whether this is a temporary tablespace
	@return DB_SUCCESS or error code */
	dberr_t open_or_create(bool is_temp)
		__attribute__((warn_unused_result));

	/** Delete all the data files. */
	void delete_files();

	/** Check if two tablespaces have common data file names.
	@param other_space	Tablespace to check against this.
	@return true if they have the same data filenames and paths */
	bool intersection(const Tablespace* other_space);

	/* Return a pointer to the first Datafile for this Tablespace
	@return pointer to the first Datafile for this Tablespace*/
	Datafile* first_datafile()
	{
		return(&m_files.front());
	}

private:
	/**
	@param filename	Name to lookup in the data files.
	@return true if the filename exists in the data files */
	bool find(const char* filename);

	/** Note that the data file was found.
	@param file	data file object */
	void file_found(Datafile& file);

	/* DATA MEMBERS */

	/** Name of the tablespace. */
	char*		m_name;

	/** Tablespace ID */
	ulint		m_space_id;

	/** Path where tablespace files will reside, not including a filename. */
	char*		m_path;

	/** Tablespace flags */
	ulint		m_flags;

	/** Minimum of flushed LSN values in data files */
	lsn_t		m_min_flushed_lsn;

	/** Maximum of flushed LSN values in data files */
	lsn_t		m_max_flushed_lsn;
};

#endif /* fsp0space_h */
