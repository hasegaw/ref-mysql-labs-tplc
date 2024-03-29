# Copyright (c) 2006, 2014, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

# This is the CMakeLists for InnoDB

INCLUDE(CheckFunctionExists)
INCLUDE(CheckCSourceCompiles)
INCLUDE(CheckCSourceRuns)

CHECK_INCLUDE_FILES(lz4.h HAVE_LZ4_H)
CHECK_LIBRARY_EXISTS(lz4 LZ4_compress "" HAVE_LIBLZ4)

IF(HAVE_LIBLZ4 AND HAVE_LZ4_H)
  ADD_DEFINITIONS(-DHAVE_LZ4=1)
  LINK_LIBRARIES(lz4)
  CHECK_LIBRARY_EXISTS(lz4 LZ4_compressHC2_limitedOutput "" HAVE_LZ4_COMPRESSHC2_LIMITEDOUTPUT)
  IF(HAVE_LZ4_COMPRESSHC2_LIMITEDOUTPUT)
    ADD_DEFINITIONS(-DHAVE_LZ4_COMPRESSHC2_LIMITEDOUTPUT=1)
  ENDIF()
ENDIF()

##
## Note: Do not enable yet
##

#CHECK_INCLUDE_FILES(lzma.h HAVE_LZMA_H)
#CHECK_LIBRARY_EXISTS(lzma lzma_easy_buffer_encode "" HAVE_LIBLZMA)

#IF(HAVE_LIBLZMA AND HAVE_LZMA_H)
#ADD_DEFINITIONS(-DHAVE_LZMA=1)
#LINK_LIBRARIES(lzma)
#ENDIF()

#CHECK_INCLUDE_FILES(lzo/lzo1x.h HAVE_LZO1X_H)
#CHECK_LIBRARY_EXISTS(lzo2 lzo1x_1_compress "" HAVE_LIBLZO2)

#IF(HAVE_LIBLZO2 AND HAVE_LZO1X_H)
#ADD_DEFINITIONS(-DHAVE_LZO1X=1)
#LINK_LIBRARIES(lzo2)
#ENDIF()

# OS tests
IF(UNIX)
  IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")

    ADD_DEFINITIONS("-DUNIV_LINUX -D_GNU_SOURCE=1")

    CHECK_INCLUDE_FILES (libaio.h HAVE_LIBAIO_H)
    CHECK_LIBRARY_EXISTS(aio io_queue_init "" HAVE_LIBAIO)

    IF(HAVE_LIBAIO_H AND HAVE_LIBAIO)
      ADD_DEFINITIONS(-DLINUX_NATIVE_AIO=1)
      LINK_LIBRARIES(aio)
    ENDIF()

  ELSEIF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
    ADD_DEFINITIONS("-DUNIV_SOLARIS")
  ENDIF()
ENDIF()

OPTION(INNODB_COMPILER_HINTS "Compile InnoDB with compiler hints" ON)
MARK_AS_ADVANCED(INNODB_COMPILER_HINTS)

IF(INNODB_COMPILER_HINTS)
   ADD_DEFINITIONS("-DCOMPILER_HINTS")
ENDIF()

SET(MUTEXTYPE "event" CACHE STRING "Mutex type: event, sys or futex")

OPTION(INNODB_PAGE_ATOMIC_REF_COUNT "Atomics for the page reference count" ON)
MARK_AS_ADVANCED(INNODB_PAGE_ATOMIC_REF_COUNT)

IF(INNODB_PAGE_ATOMIC_REF_COUNT)
   ADD_DEFINITIONS("-DPAGE_ATOMIC_REF_COUNT")
ENDIF()

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
# After: WL#5825 Using C++ Standard Library with MySQL code
#       we no longer use -fno-exceptions
#	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")

# Add -Wconversion if compiling with GCC
## As of Mar 15 2011 this flag causes 3573+ warnings. If you are reading this
## please fix them and enable the following code:
#SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wconversion")

  IF (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR
      CMAKE_SYSTEM_PROCESSOR MATCHES "i386")
    INCLUDE(CheckCXXCompilerFlag)
    CHECK_CXX_COMPILER_FLAG("-fno-builtin-memcmp" HAVE_NO_BUILTIN_MEMCMP)
    IF (HAVE_NO_BUILTIN_MEMCMP)
      # Work around http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43052
      SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_SOURCE_DIR}/rem/rem0cmp.cc
	PROPERTIES COMPILE_FLAGS -fno-builtin-memcmp)
    ENDIF()
  ENDIF()
ENDIF()

# Enable InnoDB's UNIV_DEBUG and UNIV_SYNC_DEBUG in debug builds
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DUNIV_DEBUG -DUNIV_SYNC_DEBUG")

OPTION(WITH_INNODB_EXTRA_DEBUG "Enable extra InnoDB debug checks" OFF)
IF(WITH_INNODB_EXTRA_DEBUG)
  IF(NOT WITH_DEBUG)
    MESSAGE(FATAL_ERROR "WITH_INNODB_EXTRA_DEBUG can be enabled only when WITH_DEBUG is enabled")
  ENDIF()

  SET(EXTRA_DEBUG_FLAGS "")
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_AHI_DEBUG")
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_DDL_DEBUG")
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_DEBUG_FILE_ACCESSES")
  SET(EXTRA_DEBUG_FLAGS "${EXTRA_DEBUG_FLAGS} -DUNIV_ZIP_DEBUG")

  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${EXTRA_DEBUG_FLAGS}")
  SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${EXTRA_DEBUG_FLAGS}")
ENDIF()

CHECK_FUNCTION_EXISTS(sched_getcpu  HAVE_SCHED_GETCPU)
IF(HAVE_SCHED_GETCPU)
 ADD_DEFINITIONS(-DHAVE_SCHED_GETCPU=1)
ENDIF()

CHECK_FUNCTION_EXISTS(nanosleep HAVE_NANOSLEEP)
IF(HAVE_NANOSLEEP)
 ADD_DEFINITIONS(-DHAVE_NANOSLEEP=1)
ENDIF()

IF(NOT MSVC)
  CHECK_C_SOURCE_RUNS(
  "
  #define _GNU_SOURCE
  #include <fcntl.h>
  #include <linux/falloc.h>
  int main()
  {
    /* TODO: Ignore the return value for now. Simply check if the flags exist */
    fallocate(0, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 0);

    return(0);
  }"
  HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
  )
ENDIF()

IF(HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE)
 ADD_DEFINITIONS(-DHAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE=1)
ENDIF()

IF(NOT MSVC)
# either define HAVE_IB_GCC_ATOMIC_BUILTINS or not
IF(NOT CMAKE_CROSSCOMPILING)
  CHECK_C_SOURCE_RUNS(
  "
  int main()
  {
    long	x;
    long	y;
    long	res;
    char	c;

    x = 10;
    y = 123;
    res = __sync_bool_compare_and_swap(&x, x, y);
    if (!res || x != y) {
      return(1);
    }

    x = 10;
    y = 123;
    res = __sync_bool_compare_and_swap(&x, x + 1, y);
    if (res || x != 10) {
      return(1);
    }
    x = 10;
    y = 123;
    res = __sync_add_and_fetch(&x, y);
    if (res != 123 + 10 || x != 123 + 10) {
      return(1);
    }

    c = 10;
    res = __sync_lock_test_and_set(&c, 123);
    if (res != 10 || c != 123) {
      return(1);
    }
    return(0);
  }"
  HAVE_IB_GCC_ATOMIC_BUILTINS
  )
  CHECK_C_SOURCE_RUNS(
  "#include<stdint.h>
  int main()
  {
    int64_t	x,y,res;

    x = 10;
    y = 123;
    res = __sync_sub_and_fetch(&y, x);
    if (res != y || y != 113) {
      return(1);
    }
    res = __sync_add_and_fetch(&y, x);
    if (res != y || y != 123) {
      return(1);
    }
    return(0);
  }"
  HAVE_IB_GCC_ATOMIC_BUILTINS_64
  )
ENDIF()

IF(HAVE_IB_GCC_ATOMIC_BUILTINS)
 ADD_DEFINITIONS(-DHAVE_IB_GCC_ATOMIC_BUILTINS=1)
ENDIF()

IF(HAVE_IB_GCC_ATOMIC_BUILTINS_64)
 ADD_DEFINITIONS(-DHAVE_IB_GCC_ATOMIC_BUILTINS_64=1)
ENDIF()

 # either define HAVE_IB_ATOMIC_PTHREAD_T_GCC or not
IF(NOT CMAKE_CROSSCOMPILING)
  CHECK_C_SOURCE_RUNS(
  "
  #include <pthread.h>
  #include <string.h>

  int main() {
    pthread_t       x1;
    pthread_t       x2;
    pthread_t       x3;

    memset(&x1, 0x0, sizeof(x1));
    memset(&x2, 0x0, sizeof(x2));
    memset(&x3, 0x0, sizeof(x3));

    __sync_bool_compare_and_swap(&x1, x2, x3);

    return(0);
  }"
  HAVE_IB_ATOMIC_PTHREAD_T_GCC)
ENDIF()
IF(HAVE_IB_ATOMIC_PTHREAD_T_GCC)
  ADD_DEFINITIONS(-DHAVE_IB_ATOMIC_PTHREAD_T_GCC=1)
ENDIF()

IF(NOT CMAKE_CROSSCOMPILING)
  CHECK_C_SOURCE_RUNS(
  "
  #include <stdio.h>
  #include <unistd.h>
  #include <errno.h>
  #include <assert.h>
  #include <linux/futex.h>
  #include <sys/syscall.h>

   int futex_wait(int* futex, int v) {
	return(syscall(SYS_futex, futex, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0));
   }

   int futex_signal(int* futex) {
	return(syscall(SYS_futex, futex, FUTEX_WAKE, 1, NULL, NULL, 0));
   }

  int main() {
	int	ret;
	int	m = 1;

	/* It is setup to fail and return EWOULDBLOCK. */
	ret = futex_wait(&m, 0);
	assert(ret == -1 && errno == EWOULDBLOCK);
	/* Shouldn't wake up any threads. */
	assert(futex_signal(&m) == 0);

	return(0);
  }"
  HAVE_IB_LINUX_FUTEX)
ENDIF()
IF(HAVE_IB_LINUX_FUTEX)
  ADD_DEFINITIONS(-DHAVE_IB_LINUX_FUTEX=1)
ENDIF()

ENDIF(NOT MSVC)

CHECK_FUNCTION_EXISTS(asprintf  HAVE_ASPRINTF)
CHECK_FUNCTION_EXISTS(vasprintf  HAVE_VASPRINTF)

# Solaris atomics
IF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  CHECK_FUNCTION_EXISTS(atomic_cas_ulong  HAVE_ATOMIC_CAS_ULONG)
  CHECK_FUNCTION_EXISTS(atomic_cas_32 HAVE_ATOMIC_CAS_32)
  CHECK_FUNCTION_EXISTS(atomic_cas_64 HAVE_ATOMIC_CAS_64)
  CHECK_FUNCTION_EXISTS(atomic_add_long_nv HAVE_ATOMIC_ADD_LONG_NV)
  CHECK_FUNCTION_EXISTS(atomic_swap_uchar HAVE_ATOMIC_SWAP_UCHAR)
  IF(HAVE_ATOMIC_CAS_ULONG AND
     HAVE_ATOMIC_CAS_32 AND
     HAVE_ATOMIC_CAS_64 AND
     HAVE_ATOMIC_ADD_LONG_NV AND
     HAVE_ATOMIC_SWAP_UCHAR)
    SET(HAVE_IB_SOLARIS_ATOMICS 1)
  ENDIF()

  IF(HAVE_IB_SOLARIS_ATOMICS)
    ADD_DEFINITIONS(-DHAVE_IB_SOLARIS_ATOMICS=1)
  ENDIF()

  IF(NOT CMAKE_CROSSCOMPILING)
  # either define HAVE_IB_ATOMIC_PTHREAD_T_SOLARIS or not
  CHECK_C_SOURCE_COMPILES(
  "   #include <pthread.h>
      #include <string.h>

      int main(int argc, char** argv) {
        pthread_t       x1;
        pthread_t       x2;
        pthread_t       x3;

        memset(&x1, 0x0, sizeof(x1));
        memset(&x2, 0x0, sizeof(x2));
        memset(&x3, 0x0, sizeof(x3));

        if (sizeof(pthread_t) == 4) {

          atomic_cas_32(&x1, x2, x3);

        } else if (sizeof(pthread_t) == 8) {

          atomic_cas_64(&x1, x2, x3);

        } else {

          return(1);
        }

      return(0);
    }
  " HAVE_IB_ATOMIC_PTHREAD_T_SOLARIS)
  ENDIF()
  IF(HAVE_IB_ATOMIC_PTHREAD_T_SOLARIS)
    ADD_DEFINITIONS(-DHAVE_IB_ATOMIC_PTHREAD_T_SOLARIS=1)
  ENDIF()
ENDIF()


IF(UNIX)
# this is needed to know which one of atomic_cas_32() or atomic_cas_64()
# to use in the source
SET(CMAKE_EXTRA_INCLUDE_FILES pthread.h)
CHECK_TYPE_SIZE(pthread_t SIZEOF_PTHREAD_T)
SET(CMAKE_EXTRA_INCLUDE_FILES)
ENDIF()

IF(SIZEOF_PTHREAD_T)
  ADD_DEFINITIONS(-DSIZEOF_PTHREAD_T=${SIZEOF_PTHREAD_T})
ENDIF()

IF(MSVC)
  ADD_DEFINITIONS(-DHAVE_WINDOWS_ATOMICS)
ENDIF()

IF(NOT DEFINED HAVE_IB_GCC_ATOMIC_BUILTINS
   AND NOT DEFINED HAVE_WINDOWS_ATOMICS
   AND NOT DEFINED HAVE_IB_SOLARIS_ATOMICS)

   UNSET(MUTEXTYPE CACHE)
   UNSET(HAVE_IB_LINUX_FUTEX CACHE)
   REMOVE_DEFINITIONS(-DHAVE_IB_LINUX_FUTEX=1)

ENDIF()

IF(MUTEXTYPE MATCHES "event")
  ADD_DEFINITIONS(-DMUTEX_EVENT)
ELSEIF(MUTEXTYPE MATCHES "futex" AND DEFINED HAVE_IB_LINUX_FUTEX)
  ADD_DEFINITIONS(-DMUTEX_FUTEX)
ELSE()
   ADD_DEFINITIONS(-DMUTEX_SYS)
ENDIF()

# Include directories under innobase
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/storage/innobase/include
		    ${CMAKE_SOURCE_DIR}/storage/innobase/handler)

# Sun Studio bug with -xO2
IF(CMAKE_CXX_COMPILER_ID MATCHES "SunPro"
	AND CMAKE_CXX_FLAGS_RELEASE MATCHES "O2"
	AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
	# Sun Studio 12 crashes with -xO2 flag, but not with higher optimization
	# -xO3
	SET_SOURCE_FILES_PROPERTIES(${CMAKE_CURRENT_SOURCE_DIR}/rem/rem0rec.cc
    PROPERTIES COMPILE_FLAGS -xO3)
ENDIF()

# Removing compiler optimizations for innodb/mem/* files on 64-bit Windows
# due to 64-bit compiler error, See MySQL Bug #19424, #36366, #34297
# This was a bug found in VS2005.  Assume there is no problem starting with VS2010
IF (MSVC AND MSVC_VERSION LESS 1600 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
	SET_SOURCE_FILES_PROPERTIES(mem/mem0mem.cc PROPERTIES COMPILE_FLAGS -Od)
ENDIF()
