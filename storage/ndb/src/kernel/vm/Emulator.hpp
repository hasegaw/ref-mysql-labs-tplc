/*
   Copyright (c) 2003, 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef EMULATOR_H
#define EMULATOR_H

//===========================================================================
//
// .DESCRIPTION
//      This is the main fuction for the AXE VM emulator.
//      It contains some global objects and a run method.
//
//===========================================================================
#include <kernel_types.h>

#define JAM_FILE_ID 260


extern class  JobTable            globalJobTable;
extern class  TimeQueue           globalTimeQueue;
extern class  FastScheduler       globalScheduler;
extern class  TransporterRegistry globalTransporterRegistry;
extern struct GlobalData          globalData;

#ifdef VM_TRACE
extern class SignalLoggerManager globalSignalLoggers;
#endif

/* EMULATED_JAM_SIZE must be a power of two, so JAM_MASK will work. */
#ifdef NDEBUG
// Keep jam buffer small for optimized build to improve locality of reference.
#define EMULATED_JAM_SIZE 1024
#else
#define EMULATED_JAM_SIZE 4096
#endif
#define JAM_MASK (EMULATED_JAM_SIZE - 1)

/**
 * JamEvents are used for recording that control passes a given point int the
 * code, reperesented by a JAM_FILE_ID value (which uniquely identifies a 
 * source file, and a line number. The reason for using JAM_FILE_ID rather
 * than the predefined __FILE__ is that is faster to store a 16-bit integer
 * than a pointer. For a description of how to maintain and debug JAM_FILE_IDs,
 * please refer to the comments for jamFileNames in Emulator.cpp.
 */
class JamEvent
{
public:
  /**
   * This method is used for verifying that JAM_FILE_IDs matches the contents 
   * of the jamFileNames table. The file name may include driectory names, 
   * which will be ignored.
   * @returns: true if fileId and pathName matches the jamFileNames table.
   */
  static bool verifyId(Uint32 fileId, const char* pathName);

  explicit JamEvent()
    :m_jamVal(0xffffffff){}

  explicit JamEvent(Uint32 fileId, Uint32 lineNo)
    :m_jamVal(fileId << 16 | lineNo){}

  Uint32 getFileId() const
  {
    return m_jamVal >> 16;
  }

  // Get the name of the source file, or NULL if unknown.
  const char* getFileName() const;

  Uint32 getLineNo() const
  {
    return m_jamVal & 0xffff;
  }

  bool isEmpty() const
  {
    return m_jamVal == 0xffffffff;
  }

private:
  // Upper 16 bits are the JAM_FILE_ID, lower 16 bits are the line number.
  Uint32 m_jamVal;
};

/***
 * This is a ring buffer of JamEvents for a thread.
 */
struct EmulatedJamBuffer
{
  // Index of the next entry.
  Uint32 theEmulatedJamIndex;
  JamEvent theEmulatedJam[EMULATED_JAM_SIZE];
};

struct EmulatorData {
  class Configuration * theConfiguration;
  class WatchDog      * theWatchDog;
  class ThreadConfig  * theThreadConfig;
  class SimBlockList  * theSimBlockList;
  class SocketServer  * m_socket_server;
  class Ndbd_mem_manager * m_mem_manager;

  /**
   * Constructor
   *
   *  Sets all the pointers to NULL
   */
  EmulatorData();
  
  /**
   * Create all the objects
   */
  void create();
  
  /**
   * Destroys all the objects
   */
  void destroy();
};

extern struct EmulatorData globalEmulatorData;

/**
 * Get number of extra send buffer pages to use
 */
Uint32 mt_get_extra_send_buffer_pages(Uint32 curr_num_pages,
                                      Uint32 extra_mem_pages);

/**
 * Compute no of pages to be used as job-buffer
 */
Uint32 compute_jb_pages(struct EmulatorData* ed);


#undef JAM_FILE_ID

#endif 
