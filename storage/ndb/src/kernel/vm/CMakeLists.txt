# Copyright (c) 2008, 2012, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA


INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/storage/ndb/src/mgmapi)
INCLUDE(${CMAKE_SOURCE_DIR}/storage/ndb/config/type_kernel.cmake)

ADD_CONVENIENCE_LIBRARY(ndbkernel
    VMSignal.cpp
    Emulator.cpp
    Configuration.cpp
    WatchDog.cpp
    SectionReader.cpp
    Mutex.cpp
    SafeCounter.cpp
    Rope.cpp
    ndbd_malloc_impl.cpp
    ndbd_malloc.cpp
    Pool.cpp
    WOPool.cpp
    RWPool.cpp
    DynArr256.cpp
    LockQueue.cpp
    GlobalData.cpp
    SafeMutex.cpp
    Ndbinfo.cpp
    NdbinfoTables.cpp
    ArenaPool.cpp
    mt_thr_config.cpp
)

ADD_CONVENIENCE_LIBRARY(ndbsched
    TimeQueue.cpp
    ThreadConfig.cpp
    FastScheduler.cpp
    TransporterCallback_nonmt.cpp
    SimulatedBlock_nonmt.cpp
    LongSignal_nonmt.cpp
    SimplePropertiesSection_nonmt.cpp
    dummy_nonmt.cpp
)

IF(NDB_BUILD_NDBMTD)
  ADD_CONVENIENCE_LIBRARY(ndbsched_mt
    SimulatedBlock_mt.cpp
    TransporterCallback_mt.cpp
    LongSignal_mt.cpp
    SimplePropertiesSection_mt.cpp
    mt.cpp
    dummy_mt.cpp)
ENDIF()

ADD_CONVENIENCE_LIBRARY(ndbtest
    test_context.cpp
    ../blocks/dbtup/tuppage.cpp
)

ADD_EXECUTABLE(mt_thr_config-t mt_thr_config.cpp)
SET_TARGET_PROPERTIES(mt_thr_config-t
                      PROPERTIES COMPILE_FLAGS "-DTEST_MT_THR_CONFIG")
TARGET_LINK_LIBRARIES(mt_thr_config-t ndbgeneral)

FOREACH(testprog CountingPool DynArr256 IntrusiveList LHLevel)
  ADD_EXECUTABLE(${testprog}-t ${testprog}.cpp)
  SET_TARGET_PROPERTIES(${testprog}-t PROPERTIES COMPILE_FLAGS "-DTAP_TEST")
  TARGET_LINK_LIBRARIES(${testprog}-t ndbtest ndbkernel ndbsched ndberror
                ndbtransport ndbmgmcommon ndbmgmapi ndbportlib ndbgeneral)
ENDFOREACH(testprog)

IF(NDB_BUILD_NDBMTD)
  ADD_EXECUTABLE(mt-send-t mt-send-t.cpp)
  TARGET_LINK_LIBRARIES(mt-send-t ndbtest ndbkernel ndbsched ndberror
                 ndbtransport ndbmgmcommon ndbmgmapi ndbportlib ndbgeneral)
ENDIF()
