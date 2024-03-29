# Copyright (c) 2009, 2014, Oracle and/or its affiliates. All rights reserved.
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
#

INCLUDE (CheckCSourceCompiles)
INCLUDE (CheckCXXSourceCompiles)
INCLUDE (CheckStructHasMember)
INCLUDE (CheckLibraryExists)
INCLUDE (CheckFunctionExists)
INCLUDE (CheckCCompilerFlag)
INCLUDE (CheckCSourceRuns)
INCLUDE (CheckCXXSourceRuns)
INCLUDE (CheckSymbolExists)


# WITH_PIC options.Not of much use, PIC is taken care of on platforms
# where it makes sense anyway.
IF(UNIX)
  IF(APPLE)  
    # OSX  executable are always PIC
    SET(WITH_PIC ON)
  ELSE()
    OPTION(WITH_PIC "Generate PIC objects" OFF)
    IF(WITH_PIC)
      SET(CMAKE_C_FLAGS 
        "${CMAKE_C_FLAGS} ${CMAKE_SHARED_LIBRARY_C_FLAGS}")
      SET(CMAKE_CXX_FLAGS 
        "${CMAKE_CXX_FLAGS} ${CMAKE_SHARED_LIBRARY_CXX_FLAGS}")
    ENDIF()
  ENDIF()
ENDIF()


IF(CMAKE_SYSTEM_NAME MATCHES "SunOS" AND CMAKE_COMPILER_IS_GNUCXX)
  ## We will be using gcc to generate .so files
  ## Add C flags (e.g. -m64) to CMAKE_SHARED_LIBRARY_C_FLAGS
  ## The client library contains C++ code, so add dependency on libstdc++
  ## See cmake --help-policy CMP0018
  SET(CMAKE_SHARED_LIBRARY_C_FLAGS
    "${CMAKE_SHARED_LIBRARY_C_FLAGS} ${CMAKE_C_FLAGS} -lstdc++")
ENDIF()


# System type affects version_compile_os variable 
IF(NOT SYSTEM_TYPE)
  IF(PLATFORM)
    SET(SYSTEM_TYPE ${PLATFORM})
  ELSE()
    SET(SYSTEM_TYPE ${CMAKE_SYSTEM_NAME})
  ENDIF()
ENDIF()

# The default C++ library for SunPro is really old, and not standards compliant.
# http://developers.sun.com/solaris/articles/cmp_stlport_libCstd.html
# Use stlport rather than Rogue Wave.
IF(CMAKE_SYSTEM_NAME MATCHES "SunOS")
  IF(CMAKE_CXX_COMPILER_ID MATCHES "SunPro")
    SET(CMAKE_CXX_FLAGS
      "${CMAKE_CXX_FLAGS} -library=stlport4")
  ENDIF()
ENDIF()

# Check to see if we are using LLVM's libc++ rather than e.g. libstd++
# Can then check HAVE_LLBM_LIBCPP later without including e.g. ciso646.
CHECK_CXX_SOURCE_RUNS("
#include <ciso646>
int main()
{
#ifdef _LIBCPP_VERSION
  return 0;
#else
  return 1;
#endif
}" HAVE_LLVM_LIBCPP)

MACRO(DIRNAME IN OUT)
  GET_FILENAME_COMPONENT(${OUT} ${IN} PATH)
ENDMACRO()

MACRO(FIND_REAL_LIBRARY SOFTLINK_NAME REALNAME)
  # We re-distribute libstlport.so/libstdc++.so which are both symlinks.
  # There is no 'readlink' on solaris, so we use perl to follow links:
  SET(PERLSCRIPT
    "my $link= $ARGV[0]; use Cwd qw(abs_path); my $file = abs_path($link); print $file;")
  EXECUTE_PROCESS(
    COMMAND perl -e "${PERLSCRIPT}" ${SOFTLINK_NAME}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE real_library
    )
  SET(REALNAME ${real_library})
ENDMACRO()

MACRO(EXTEND_CXX_LINK_FLAGS LIBRARY_PATH)
  # Using the $ORIGIN token with the -R option to locate the libraries
  # on a path relative to the executable:
  # We need an extra backslash to pass $ORIGIN to the mysql_config script...
  SET(QUOTED_CMAKE_CXX_LINK_FLAGS
    "${CMAKE_CXX_LINK_FLAGS} -R'\\$ORIGIN/../lib' -R${LIBRARY_PATH}")
  SET(CMAKE_CXX_LINK_FLAGS
    "${CMAKE_CXX_LINK_FLAGS} -R'\$ORIGIN/../lib' -R${LIBRARY_PATH}")
  MESSAGE(STATUS "CMAKE_CXX_LINK_FLAGS ${CMAKE_CXX_LINK_FLAGS}")
ENDMACRO()

MACRO(EXTEND_C_LINK_FLAGS LIBRARY_PATH)
  SET(QUOTED_CMAKE_C_LINK_FLAGS
    "${CMAKE_C_LINK_FLAGS} -R'\\$ORIGIN/../lib' -R${LIBRARY_PATH}")
  SET(CMAKE_C_LINK_FLAGS
    "${CMAKE_C_LINK_FLAGS} -R'\$ORIGIN/../lib' -R${LIBRARY_PATH}")
  MESSAGE(STATUS "CMAKE_C_LINK_FLAGS ${CMAKE_C_LINK_FLAGS}")
  SET(CMAKE_SHARED_LIBRARY_C_FLAGS
    "${CMAKE_SHARED_LIBRARY_C_FLAGS} -R'\$ORIGIN/../lib' -R${LIBRARY_PATH}")
ENDMACRO()

IF(CMAKE_SYSTEM_NAME MATCHES "SunOS" AND CMAKE_COMPILER_IS_GNUCC)
  DIRNAME(${CMAKE_CXX_COMPILER} CXX_PATH)
  SET(LIB_SUFFIX "lib")
  IF(SIZEOF_VOIDP EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "sparc")
    SET(LIB_SUFFIX "lib/sparcv9")
  ENDIF()
  IF(SIZEOF_VOIDP EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "i386")
    SET(LIB_SUFFIX "lib/amd64")
  ENDIF()
  FIND_LIBRARY(GPP_LIBRARY_NAME
    NAMES "stdc++"
    PATHS ${CXX_PATH}/../${LIB_SUFFIX}
    NO_DEFAULT_PATH
  )
  MESSAGE(STATUS "GPP_LIBRARY_NAME ${GPP_LIBRARY_NAME}")
  IF(GPP_LIBRARY_NAME)
    DIRNAME(${GPP_LIBRARY_NAME} GPP_LIBRARY_PATH)
    FIND_REAL_LIBRARY(${GPP_LIBRARY_NAME} real_library)
    MESSAGE(STATUS "INSTALL ${GPP_LIBRARY_NAME} ${real_library}")
    INSTALL(FILES ${GPP_LIBRARY_NAME} ${real_library}
            DESTINATION ${INSTALL_LIBDIR} COMPONENT SharedLibraries)
    EXTEND_CXX_LINK_FLAGS(${GPP_LIBRARY_PATH})
    EXECUTE_PROCESS(
      COMMAND sh -c "elfdump ${real_library} | grep SONAME"
      RESULT_VARIABLE result
      OUTPUT_VARIABLE sonameline
    )
    IF(NOT result)
      STRING(REGEX MATCH "libstdc.*[^\n]" soname ${sonameline})
      MESSAGE(STATUS "INSTALL ${GPP_LIBRARY_PATH}/${soname}")
      INSTALL(FILES "${GPP_LIBRARY_PATH}/${soname}"
              DESTINATION ${INSTALL_LIBDIR} COMPONENT SharedLibraries)
    ENDIF()
  ENDIF()
  FIND_LIBRARY(GCC_LIBRARY_NAME
    NAMES "gcc_s"
    PATHS ${CXX_PATH}/../${LIB_SUFFIX}
    NO_DEFAULT_PATH
  )
  IF(GCC_LIBRARY_NAME)
    DIRNAME(${GCC_LIBRARY_NAME} GCC_LIBRARY_PATH)
    FIND_REAL_LIBRARY(${GCC_LIBRARY_NAME} real_library)
    MESSAGE(STATUS "INSTALL ${GCC_LIBRARY_NAME} ${real_library}")
    INSTALL(FILES ${GCC_LIBRARY_NAME} ${real_library}
            DESTINATION ${INSTALL_LIBDIR} COMPONENT SharedLibraries)
    EXTEND_C_LINK_FLAGS(${GCC_LIBRARY_PATH})
  ENDIF()
ENDIF()

IF(CMAKE_SYSTEM_NAME MATCHES "SunOS" AND CMAKE_C_COMPILER_ID MATCHES "SunPro")
  DIRNAME(${CMAKE_CXX_COMPILER} CXX_PATH)
  # Also extract real path to the compiler(which is normally
  # in <install_path>/prod/bin) and try to find the
  # stlport libs relative to that location as well.
  GET_FILENAME_COMPONENT(CXX_REALPATH ${CMAKE_CXX_COMPILER} REALPATH)

  SET(STLPORT_SUFFIX "lib/stlport4")
  IF(SIZEOF_VOIDP EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "sparc")
    SET(STLPORT_SUFFIX "lib/stlport4/v9")
  ENDIF()
  IF(SIZEOF_VOIDP EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "i386")
    SET(STLPORT_SUFFIX "lib/stlport4/amd64")
  ENDIF()

  FIND_LIBRARY(STL_LIBRARY_NAME
    NAMES "stlport"
    PATHS ${CXX_PATH}/../${STLPORT_SUFFIX}
          ${CXX_REALPATH}/../../${STLPORT_SUFFIX}
  )
  MESSAGE(STATUS "STL_LIBRARY_NAME ${STL_LIBRARY_NAME}")
  IF(STL_LIBRARY_NAME)
    DIRNAME(${STL_LIBRARY_NAME} STLPORT_PATH)
    FIND_REAL_LIBRARY(${STL_LIBRARY_NAME} real_library)
    MESSAGE(STATUS "INSTALL ${STL_LIBRARY_NAME} ${real_library}")
    INSTALL(FILES ${STL_LIBRARY_NAME} ${real_library}
            DESTINATION ${INSTALL_LIBDIR} COMPONENT SharedLibraries)
    EXTEND_C_LINK_FLAGS(${STLPORT_PATH})
    EXTEND_CXX_LINK_FLAGS(${STLPORT_PATH})
  ELSE()
    MESSAGE(STATUS "Failed to find the reuired stlport library, print some"
                   "variables to help debugging and bail out")
    MESSAGE(STATUS "CMAKE_CXX_COMPILER ${CMAKE_CXX_COMPILER}")
    MESSAGE(STATUS "CXX_PATH ${CXX_PATH}")
    MESSAGE(STATUS "CXX_REALPATH ${CXX_REALPATH}")
    MESSAGE(STATUS "STLPORT_SUFFIX ${STLPORT_SUFFIX}")
    MESSAGE(FATAL_ERROR
      "Could not find the required stlport library.")
  ENDIF()
ENDIF()

IF(CMAKE_COMPILER_IS_GNUCXX)
  IF (CMAKE_EXE_LINKER_FLAGS MATCHES " -static " 
     OR CMAKE_EXE_LINKER_FLAGS MATCHES " -static$")
     SET(HAVE_DLOPEN FALSE CACHE "Disable dlopen due to -static flag" FORCE)
     SET(WITHOUT_DYNAMIC_PLUGINS TRUE)
  ENDIF()
ENDIF()

IF(WITHOUT_DYNAMIC_PLUGINS)
  MESSAGE("Dynamic plugins are disabled.")
ENDIF(WITHOUT_DYNAMIC_PLUGINS)

# Large files, common flag
SET(_LARGEFILE_SOURCE  1)

# If finds the size of a type, set SIZEOF_<type> and HAVE_<type>
FUNCTION(MY_CHECK_TYPE_SIZE type defbase)
  CHECK_TYPE_SIZE("${type}" SIZEOF_${defbase})
  IF(SIZEOF_${defbase})
    SET(HAVE_${defbase} 1 PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

# Same for structs, setting HAVE_STRUCT_<name> instead
FUNCTION(MY_CHECK_STRUCT_SIZE type defbase)
  CHECK_TYPE_SIZE("struct ${type}" SIZEOF_${defbase})
  IF(SIZEOF_${defbase})
    SET(HAVE_STRUCT_${defbase} 1 PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

# Searches function in libraries
# if function is found, sets output parameter result to the name of the library
# if function is found in libc, result will be empty 
FUNCTION(MY_SEARCH_LIBS func libs result)
  IF(${${result}})
    # Library is already found or was predefined
    RETURN()
  ENDIF()
  CHECK_FUNCTION_EXISTS(${func} HAVE_${func}_IN_LIBC)
  IF(HAVE_${func}_IN_LIBC)
    SET(${result} "" PARENT_SCOPE)
    RETURN()
  ENDIF()
  FOREACH(lib  ${libs})
    CHECK_LIBRARY_EXISTS(${lib} ${func} "" HAVE_${func}_IN_${lib}) 
    IF(HAVE_${func}_IN_${lib})
      SET(${result} ${lib} PARENT_SCOPE)
      SET(HAVE_${result} 1 PARENT_SCOPE)
      RETURN()
    ENDIF()
  ENDFOREACH()
ENDFUNCTION()

# Find out which libraries to use.
IF(UNIX)
  MY_SEARCH_LIBS(floor m LIBM)
  IF(NOT LIBM)
    MY_SEARCH_LIBS(__infinity m LIBM)
  ENDIF()
  MY_SEARCH_LIBS(gethostbyname_r  "nsl_r;nsl" LIBNSL)
  MY_SEARCH_LIBS(bind "bind;socket" LIBBIND)
  MY_SEARCH_LIBS(crypt crypt LIBCRYPT)
  MY_SEARCH_LIBS(setsockopt socket LIBSOCKET)
  MY_SEARCH_LIBS(dlopen dl LIBDL)
  MY_SEARCH_LIBS(sched_yield rt LIBRT)
  IF(NOT LIBRT)
    MY_SEARCH_LIBS(clock_gettime rt LIBRT)
  ENDIF()
  MY_SEARCH_LIBS(timer_create rt LIBRT)
  FIND_PACKAGE(Threads)

  SET(CMAKE_REQUIRED_LIBRARIES 
    ${LIBM} ${LIBNSL} ${LIBBIND} ${LIBCRYPT} ${LIBSOCKET} ${LIBDL} ${CMAKE_THREAD_LIBS_INIT} ${LIBRT})
  # Need explicit pthread for gcc -fsanitize=address
  IF(CMAKE_USE_PTHREADS_INIT AND CMAKE_C_FLAGS MATCHES "-fsanitize=")
    SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} pthread)
  ENDIF()

  LIST(LENGTH CMAKE_REQUIRED_LIBRARIES required_libs_length)
  IF(${required_libs_length} GREATER 0)
    LIST(REMOVE_DUPLICATES CMAKE_REQUIRED_LIBRARIES)
  ENDIF()  
  LINK_LIBRARIES(${CMAKE_THREAD_LIBS_INIT})
  
  OPTION(WITH_LIBWRAP "Compile with tcp wrappers support" OFF)
  IF(WITH_LIBWRAP)
    SET(SAVE_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
    SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} wrap)
    CHECK_C_SOURCE_COMPILES(
    "
    #include <tcpd.h>
    int allow_severity = 0;
    int deny_severity  = 0;
    int main()
    {
      hosts_access(0);
    }"
    HAVE_LIBWRAP)
    SET(CMAKE_REQUIRED_LIBRARIES ${SAVE_CMAKE_REQUIRED_LIBRARIES})
    IF(HAVE_LIBWRAP)
      SET(LIBWRAP "wrap")
    ELSE()
      MESSAGE(FATAL_ERROR 
      "WITH_LIBWRAP is defined, but can not find a working libwrap. "
      "Make sure both the header files (tcpd.h) "
      "and the library (libwrap) are installed.")
    ENDIF()
  ENDIF()
ENDIF()

#
# Tests for header files
#
INCLUDE (CheckIncludeFiles)
INCLUDE (CheckIncludeFileCXX)

CHECK_INCLUDE_FILES (sys/types.h HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILES (alloca.h HAVE_ALLOCA_H)
CHECK_INCLUDE_FILES (arpa/inet.h HAVE_ARPA_INET_H)
CHECK_INCLUDE_FILES (crypt.h HAVE_CRYPT_H)
CHECK_INCLUDE_FILE_CXX (cxxabi.h HAVE_CXXABI_H)
CHECK_INCLUDE_FILES (dirent.h HAVE_DIRENT_H)
CHECK_INCLUDE_FILES (dlfcn.h HAVE_DLFCN_H)
CHECK_INCLUDE_FILES (execinfo.h HAVE_EXECINFO_H)
CHECK_INCLUDE_FILES (fcntl.h HAVE_FCNTL_H)
CHECK_INCLUDE_FILES (fenv.h HAVE_FENV_H)
CHECK_INCLUDE_FILES (fpu_control.h HAVE_FPU_CONTROL_H)
CHECK_INCLUDE_FILES (grp.h HAVE_GRP_H)
CHECK_INCLUDE_FILES (ieeefp.h HAVE_IEEEFP_H)
CHECK_INCLUDE_FILES (inttypes.h HAVE_INTTYPES_H)
CHECK_INCLUDE_FILES (langinfo.h HAVE_LANGINFO_H)
CHECK_INCLUDE_FILES (malloc.h HAVE_MALLOC_H)
CHECK_INCLUDE_FILES (ndir.h HAVE_NDIR_H)
CHECK_INCLUDE_FILES (netinet/in.h HAVE_NETINET_IN_H)
CHECK_INCLUDE_FILES (paths.h HAVE_PATHS_H)
CHECK_INCLUDE_FILES (poll.h HAVE_POLL_H)
CHECK_INCLUDE_FILES (pwd.h HAVE_PWD_H)
CHECK_INCLUDE_FILES (sched.h HAVE_SCHED_H)
CHECK_INCLUDE_FILES (select.h HAVE_SELECT_H)
CHECK_INCLUDE_FILES ("sys/types.h;sys/dir.h" HAVE_SYS_DIR_H)
CHECK_INCLUDE_FILES (sys/ndir.h HAVE_SYS_NDIR_H)
CHECK_INCLUDE_FILES (stdint.h HAVE_STDINT_H)
SET(HAVE_STDLIB_H 1)
CHECK_INCLUDE_FILES (strings.h HAVE_STRINGS_H)
CHECK_INCLUDE_FILES (synch.h HAVE_SYNCH_H)
CHECK_INCLUDE_FILES (sysent.h HAVE_SYSENT_H)
CHECK_INCLUDE_FILES (sys/cdefs.h HAVE_SYS_CDEFS_H)
CHECK_INCLUDE_FILES (sys/ioctl.h HAVE_SYS_IOCTL_H)
CHECK_INCLUDE_FILES (sys/malloc.h HAVE_SYS_MALLOC_H)
CHECK_INCLUDE_FILES (sys/mman.h HAVE_SYS_MMAN_H)
CHECK_INCLUDE_FILES (sys/prctl.h HAVE_SYS_PRCTL_H)
CHECK_INCLUDE_FILES (sys/resource.h HAVE_SYS_RESOURCE_H)
CHECK_INCLUDE_FILES (sys/select.h HAVE_SYS_SELECT_H)
CHECK_INCLUDE_FILES (sys/socket.h HAVE_SYS_SOCKET_H)
CHECK_INCLUDE_FILES (sys/stat.h HAVE_SYS_STAT_H)
CHECK_INCLUDE_FILES ("curses.h;term.h" HAVE_TERM_H)
CHECK_INCLUDE_FILES (asm/termbits.h HAVE_ASM_TERMBITS_H)
CHECK_INCLUDE_FILES (termbits.h HAVE_TERMBITS_H)
CHECK_INCLUDE_FILES (termios.h HAVE_TERMIOS_H)
CHECK_INCLUDE_FILES (termio.h HAVE_TERMIO_H)
CHECK_INCLUDE_FILES (termcap.h HAVE_TERMCAP_H)
CHECK_INCLUDE_FILES (unistd.h HAVE_UNISTD_H)
CHECK_INCLUDE_FILES (utime.h HAVE_UTIME_H)
CHECK_INCLUDE_FILES (sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES (sys/utime.h HAVE_SYS_UTIME_H)
CHECK_INCLUDE_FILES (sys/wait.h HAVE_SYS_WAIT_H)
CHECK_INCLUDE_FILES (sys/param.h HAVE_SYS_PARAM_H)
CHECK_INCLUDE_FILES (sys/vadvise.h HAVE_SYS_VADVISE_H)
CHECK_INCLUDE_FILES (fnmatch.h HAVE_FNMATCH_H)
SET(HAVE_STDARG_H 1)
CHECK_INCLUDE_FILES ("stdlib.h;sys/un.h" HAVE_SYS_UN_H)
CHECK_INCLUDE_FILES (vis.h HAVE_VIS_H)
CHECK_INCLUDE_FILES (sasl/sasl.h HAVE_SASL_SASL_H)

# For libevent
CHECK_INCLUDE_FILES(sys/devpoll.h HAVE_DEVPOLL)
SET(HAVE_SIGNAL_H 1)
IF(HAVE_DEVPOLL)
  # Duplicate symbols, but keep it to avoid changing libevent code.
  SET(HAVE_SYS_DEVPOLL_H 1)
ENDIF()
CHECK_INCLUDE_FILES(sys/epoll.h HAVE_SYS_EPOLL_H)
CHECK_SYMBOL_EXISTS (TAILQ_FOREACH "sys/queue.h" HAVE_TAILQFOREACH)

# Figure out threading library
# Defines CMAKE_USE_PTHREADS_INIT and CMAKE_THREAD_LIBS_INIT.
FIND_PACKAGE (Threads)

FUNCTION(MY_CHECK_PTHREAD_ONCE_INIT)
  CHECK_C_COMPILER_FLAG("-Werror" HAVE_WERROR_FLAG)
  IF(HAVE_WERROR_FLAG)
    SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
  ENDIF()
  CHECK_C_SOURCE_COMPILES("
    #include <pthread.h>
    void foo(void) {}
    int main()
    {
      pthread_once_t once_control = PTHREAD_ONCE_INIT;
      pthread_once(&once_control, foo);
      return 0;
    }"
    HAVE_PTHREAD_ONCE_INIT
  )
  # http://bugs.opensolaris.org/bugdatabase/printableBug.do?bug_id=6611808
  IF(NOT HAVE_PTHREAD_ONCE_INIT)
    CHECK_C_SOURCE_COMPILES("
      #include <pthread.h>
      void foo(void) {}
      int main()
      {
        pthread_once_t once_control = { PTHREAD_ONCE_INIT };
        pthread_once(&once_control, foo);
        return 0;
      }"
      HAVE_ARRAY_PTHREAD_ONCE_INIT
    )
  ENDIF()
  IF(HAVE_PTHREAD_ONCE_INIT)
    SET(PTHREAD_ONCE_INITIALIZER "PTHREAD_ONCE_INIT" PARENT_SCOPE)
  ENDIF()
  IF(HAVE_ARRAY_PTHREAD_ONCE_INIT)
    SET(PTHREAD_ONCE_INITIALIZER "{ PTHREAD_ONCE_INIT }" PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

IF(CMAKE_USE_PTHREADS_INIT)
  MY_CHECK_PTHREAD_ONCE_INIT()
ENDIF()

#
# Tests for functions
#
CHECK_FUNCTION_EXISTS (_aligned_malloc HAVE_ALIGNED_MALLOC)
CHECK_FUNCTION_EXISTS (alarm HAVE_ALARM)
CHECK_FUNCTION_EXISTS (backtrace HAVE_BACKTRACE)
CHECK_FUNCTION_EXISTS (backtrace_symbols HAVE_BACKTRACE_SYMBOLS)
CHECK_FUNCTION_EXISTS (backtrace_symbols_fd HAVE_BACKTRACE_SYMBOLS_FD)
CHECK_FUNCTION_EXISTS (printstack HAVE_PRINTSTACK)
CHECK_FUNCTION_EXISTS (index HAVE_INDEX)
CHECK_FUNCTION_EXISTS (clock_gettime HAVE_CLOCK_GETTIME)
CHECK_FUNCTION_EXISTS (cuserid HAVE_CUSERID)
CHECK_FUNCTION_EXISTS (directio HAVE_DIRECTIO)
CHECK_FUNCTION_EXISTS (ftruncate HAVE_FTRUNCATE)
CHECK_FUNCTION_EXISTS (compress HAVE_COMPRESS)
CHECK_FUNCTION_EXISTS (crypt HAVE_CRYPT)
CHECK_FUNCTION_EXISTS (dlerror HAVE_DLERROR)
CHECK_FUNCTION_EXISTS (dlopen HAVE_DLOPEN)
CHECK_FUNCTION_EXISTS (fchmod HAVE_FCHMOD)
CHECK_FUNCTION_EXISTS (fcntl HAVE_FCNTL)
CHECK_FUNCTION_EXISTS (fdatasync HAVE_FDATASYNC)
CHECK_SYMBOL_EXISTS(fdatasync "unistd.h" HAVE_DECL_FDATASYNC)
CHECK_FUNCTION_EXISTS (fedisableexcept HAVE_FEDISABLEEXCEPT)
CHECK_FUNCTION_EXISTS (fseeko HAVE_FSEEKO)
CHECK_FUNCTION_EXISTS (fsync HAVE_FSYNC)
CHECK_FUNCTION_EXISTS (gethostbyaddr_r HAVE_GETHOSTBYADDR_R)
CHECK_FUNCTION_EXISTS (gethrtime HAVE_GETHRTIME)
CHECK_FUNCTION_EXISTS (getnameinfo HAVE_GETNAMEINFO)
CHECK_FUNCTION_EXISTS (getpass HAVE_GETPASS)
CHECK_FUNCTION_EXISTS (getpassphrase HAVE_GETPASSPHRASE)
CHECK_FUNCTION_EXISTS (getpwnam HAVE_GETPWNAM)
CHECK_FUNCTION_EXISTS (getpwuid HAVE_GETPWUID)
CHECK_FUNCTION_EXISTS (getrlimit HAVE_GETRLIMIT)
CHECK_FUNCTION_EXISTS (getrusage HAVE_GETRUSAGE)
CHECK_FUNCTION_EXISTS (initgroups HAVE_INITGROUPS)
CHECK_FUNCTION_EXISTS (issetugid HAVE_ISSETUGID)
CHECK_FUNCTION_EXISTS (getuid HAVE_GETUID)
CHECK_FUNCTION_EXISTS (geteuid HAVE_GETEUID)
CHECK_FUNCTION_EXISTS (getgid HAVE_GETGID)
CHECK_FUNCTION_EXISTS (getegid HAVE_GETEGID)
CHECK_FUNCTION_EXISTS (lstat HAVE_LSTAT)
CHECK_FUNCTION_EXISTS (madvise HAVE_MADVISE)
CHECK_FUNCTION_EXISTS (malloc_info HAVE_MALLOC_INFO)
CHECK_FUNCTION_EXISTS (mlock HAVE_MLOCK)
CHECK_FUNCTION_EXISTS (mlockall HAVE_MLOCKALL)
CHECK_FUNCTION_EXISTS (mmap HAVE_MMAP)
CHECK_FUNCTION_EXISTS (mmap64 HAVE_MMAP64)
CHECK_FUNCTION_EXISTS (poll HAVE_POLL)
CHECK_FUNCTION_EXISTS (posix_fallocate HAVE_POSIX_FALLOCATE)
CHECK_FUNCTION_EXISTS (posix_memalign HAVE_POSIX_MEMALIGN)
CHECK_FUNCTION_EXISTS (pread HAVE_PREAD) # Used by NDB
CHECK_FUNCTION_EXISTS (pthread_attr_getguardsize HAVE_PTHREAD_ATTR_GETGUARDSIZE)
CHECK_FUNCTION_EXISTS (pthread_condattr_setclock HAVE_PTHREAD_CONDATTR_SETCLOCK)
CHECK_FUNCTION_EXISTS (pthread_sigmask HAVE_PTHREAD_SIGMASK)
CHECK_FUNCTION_EXISTS (pthread_yield_np HAVE_PTHREAD_YIELD_NP)
CHECK_FUNCTION_EXISTS (readdir_r HAVE_READDIR_R)
CHECK_FUNCTION_EXISTS (readlink HAVE_READLINK)
CHECK_FUNCTION_EXISTS (realpath HAVE_REALPATH)
CHECK_FUNCTION_EXISTS (sched_yield HAVE_SCHED_YIELD)
CHECK_FUNCTION_EXISTS (setfd HAVE_SETFD)
CHECK_FUNCTION_EXISTS (sigaction HAVE_SIGACTION)
CHECK_FUNCTION_EXISTS (sigset HAVE_SIGSET)
CHECK_FUNCTION_EXISTS (sleep HAVE_SLEEP)
CHECK_FUNCTION_EXISTS (stpcpy HAVE_STPCPY)
CHECK_FUNCTION_EXISTS (stpncpy HAVE_STPNCPY)
CHECK_FUNCTION_EXISTS (strlcpy HAVE_STRLCPY)
CHECK_FUNCTION_EXISTS (strnlen HAVE_STRNLEN)
CHECK_FUNCTION_EXISTS (strlcat HAVE_STRLCAT)
CHECK_FUNCTION_EXISTS (strsignal HAVE_STRSIGNAL)
CHECK_FUNCTION_EXISTS (fgetln HAVE_FGETLN)
CHECK_FUNCTION_EXISTS (strsep HAVE_STRSEP)
SET(HAVE_STRTOK_R 1) # Used by libevent
SET(HAVE_STRTOLL 1) # Used by libevent
SET(HAVE_STRDUP 1) # Used by NDB
CHECK_FUNCTION_EXISTS (tell HAVE_TELL)
CHECK_FUNCTION_EXISTS (thr_yield HAVE_THR_YIELD)
CHECK_FUNCTION_EXISTS (vasprintf HAVE_VASPRINTF)
CHECK_FUNCTION_EXISTS (memalign HAVE_MEMALIGN)
CHECK_FUNCTION_EXISTS (nl_langinfo HAVE_NL_LANGINFO)
CHECK_FUNCTION_EXISTS (ntohll HAVE_HTONLL)

CHECK_FUNCTION_EXISTS (clock_gettime DNS_USE_CPU_CLOCK_FOR_ID)
CHECK_FUNCTION_EXISTS (epoll_create HAVE_EPOLL)
# Temperarily  Quote event port out as we encounter error in port_getn
# on solaris x86
# CHECK_FUNCTION_EXISTS (port_create HAVE_EVENT_PORTS)
CHECK_FUNCTION_EXISTS (inet_ntop HAVE_INET_NTOP)
CHECK_FUNCTION_EXISTS (kqueue HAVE_WORKING_KQUEUE)
CHECK_SYMBOL_EXISTS (timeradd "sys/time.h" HAVE_TIMERADD)
CHECK_SYMBOL_EXISTS (timerclear "sys/time.h" HAVE_TIMERCLEAR)
CHECK_SYMBOL_EXISTS (timercmp "sys/time.h" HAVE_TIMERCMP)
CHECK_SYMBOL_EXISTS (timerisset "sys/time.h" HAVE_TIMERISSET)
CHECK_FUNCTION_EXISTS (timer_create HAVE_TIMER_CREATE)
CHECK_FUNCTION_EXISTS (timer_settime HAVE_TIMER_SETTIME)
CHECK_FUNCTION_EXISTS (kqueue HAVE_KQUEUE)

#--------------------------------------------------------------------
# Support for WL#2373 (Use cycle counter for timing)
#--------------------------------------------------------------------

CHECK_INCLUDE_FILES(sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES(sys/times.h HAVE_SYS_TIMES_H)
CHECK_INCLUDE_FILES(asm/msr.h HAVE_ASM_MSR_H)
#msr.h has rdtscll()

CHECK_FUNCTION_EXISTS(times HAVE_TIMES)
CHECK_FUNCTION_EXISTS(gettimeofday HAVE_GETTIMEOFDAY)

CHECK_FUNCTION_EXISTS(rdtscll HAVE_RDTSCLL)
# I doubt that we'll ever reach the check for this.


#
# Tests for symbols
#

CHECK_SYMBOL_EXISTS(madvise "sys/mman.h" HAVE_DECL_MADVISE)
CHECK_SYMBOL_EXISTS(lrand48 "stdlib.h" HAVE_LRAND48)
CHECK_SYMBOL_EXISTS(TIOCGWINSZ "sys/ioctl.h" GWINSZ_IN_SYS_IOCTL)
CHECK_SYMBOL_EXISTS(FIONREAD "sys/ioctl.h" FIONREAD_IN_SYS_IOCTL)
CHECK_SYMBOL_EXISTS(FIONREAD "sys/filio.h" FIONREAD_IN_SYS_FILIO)
CHECK_SYMBOL_EXISTS(gettimeofday "sys/time.h" HAVE_GETTIMEOFDAY)
CHECK_SYMBOL_EXISTS(SIGEV_THREAD_ID "signal.h;time.h" HAVE_SIGEV_THREAD_ID)
CHECK_SYMBOL_EXISTS(SIGEV_PORT "signal.h;time.h" HAVE_SIGEV_PORT)
CHECK_SYMBOL_EXISTS(EVFILT_TIMER "sys/types.h;sys/event.h;sys/time.h" HAVE_EVFILT_TIMER)

CHECK_SYMBOL_EXISTS(log2  math.h HAVE_LOG2)
CHECK_SYMBOL_EXISTS(rint  math.h HAVE_RINT)

# isinf() prototype not found on Solaris
CHECK_CXX_SOURCE_COMPILES(
"#include  <math.h>
int main() { 
  isinf(0.0); 
  return 0;
}" HAVE_ISINF)


# fesetround() prototype not found in gcc compatibility file fenv.h
CHECK_CXX_SOURCE_COMPILES(
"#include  <fenv.h>
int main() { 
  fesetround(FE_TONEAREST);
  return 0;
}" HAVE_FESETROUND)

IF(HAVE_KQUEUE AND HAVE_EVFILT_TIMER)
  SET(HAVE_KQUEUE_TIMERS 1 CACHE INTERNAL "Have kqueue timer-related filter")
ELSEIF(HAVE_TIMER_CREATE AND HAVE_TIMER_SETTIME)
  IF(HAVE_SIGEV_THREAD_ID OR HAVE_SIGEV_PORT)
    SET(HAVE_POSIX_TIMERS 1 CACHE INTERNAL "Have POSIX timer-related functions")
  ENDIF()
ENDIF()

IF(WIN32)
  SET(HAVE_WINDOWS_TIMERS 1 CACHE INTERNAL "Have Windows timer-related functions")
ENDIF()

IF(HAVE_POSIX_TIMERS OR HAVE_KQUEUE_TIMERS OR HAVE_WINDOWS_TIMERS)
  SET(HAVE_MY_TIMER 1 CACHE INTERNAL "Have mysys timer-related functions")
ENDIF()

#
# Test for endianess
#
INCLUDE(TestBigEndian)
TEST_BIG_ENDIAN(WORDS_BIGENDIAN)

#
# Tests for type sizes (and presence)
#
INCLUDE (CheckTypeSize)
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS}
        -D_LARGEFILE_SOURCE -D_LARGE_FILES -D_FILE_OFFSET_BITS=64
        -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS)
SET(CMAKE_EXTRA_INCLUDE_FILES signal.h)
MY_CHECK_TYPE_SIZE(sigset_t SIGSET_T)
IF(NOT SIZEOF_SIGSET_T)
 SET(sigset_t int)
ENDIF()
MY_CHECK_TYPE_SIZE(mode_t MODE_T)
IF(NOT SIZEOF_MODE_T)
 SET(mode_t int)
ENDIF()


IF(HAVE_STDINT_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES stdint.h)
ENDIF(HAVE_STDINT_H)

SET(HAVE_VOIDP 1)
SET(HAVE_CHARP 1)
SET(HAVE_LONG 1)

MY_CHECK_TYPE_SIZE("void *" VOIDP)
MY_CHECK_TYPE_SIZE("char *" CHARP)
MY_CHECK_TYPE_SIZE(long LONG)
MY_CHECK_TYPE_SIZE(char CHAR)
MY_CHECK_TYPE_SIZE(short SHORT)
MY_CHECK_TYPE_SIZE(int INT)
MY_CHECK_TYPE_SIZE("long long" LONG_LONG)
SET(CMAKE_EXTRA_INCLUDE_FILES stdio.h sys/types.h)
MY_CHECK_TYPE_SIZE(off_t OFF_T)
MY_CHECK_TYPE_SIZE(uint UINT)
MY_CHECK_TYPE_SIZE(ulong ULONG)
MY_CHECK_TYPE_SIZE(u_int32_t U_INT32_T)
MY_CHECK_TYPE_SIZE(time_t TIME_T)
SET (CMAKE_EXTRA_INCLUDE_FILES sys/types.h)
SET(CMAKE_EXTRA_INCLUDE_FILES)
IF(HAVE_SYS_SOCKET_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES sys/socket.h)
ENDIF(HAVE_SYS_SOCKET_H)
SET(CMAKE_EXTRA_INCLUDE_FILES)

IF(HAVE_IEEEFP_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES ieeefp.h)
  MY_CHECK_TYPE_SIZE(fp_except FP_EXCEPT)
ENDIF()


#
# Code tests
#

SET(HAVE_GETADDRINFO 1) # Used by libevent
SET(HAVE_SELECT 1) # Used by NDB/libevent

IF(WIN32)
  SET(SOCKET_SIZE_TYPE int)
ELSE()
  SET(SOCKET_SIZE_TYPE socklen_t)
ENDIF()

CHECK_CXX_SOURCE_COMPILES("
#include <pthread.h>
int main()
{
  pthread_yield();
  return 0;
}
" HAVE_PTHREAD_YIELD_ZERO_ARG)

IF(NOT STACK_DIRECTION)
  IF(CMAKE_CROSSCOMPILING)
   MESSAGE(FATAL_ERROR 
   "STACK_DIRECTION is not defined.  Please specify -DSTACK_DIRECTION=1 "
   "or -DSTACK_DIRECTION=-1 when calling cmake.")
  ELSE()
    TRY_RUN(STACKDIR_RUN_RESULT STACKDIR_COMPILE_RESULT    
     ${CMAKE_BINARY_DIR} 
     ${CMAKE_SOURCE_DIR}/cmake/stack_direction.c
     )
     # Test program returns 0 (down) or 1 (up).
     # Convert to -1 or 1
     IF(STACKDIR_RUN_RESULT EQUAL 0)
       SET(STACK_DIRECTION -1 CACHE INTERNAL "Stack grows direction")
     ELSE()
       SET(STACK_DIRECTION 1 CACHE INTERNAL "Stack grows direction")
     ENDIF()
     MESSAGE(STATUS "Checking stack direction : ${STACK_DIRECTION}")
   ENDIF()
ENDIF()

CHECK_INCLUDE_FILES("time.h;sys/time.h" TIME_WITH_SYS_TIME)
CHECK_SYMBOL_EXISTS(O_NONBLOCK "unistd.h;fcntl.h" HAVE_FCNTL_NONBLOCK)
IF(NOT HAVE_FCNTL_NONBLOCK)
 SET(NO_FCNTL_NONBLOCK 1)
ENDIF()

#
# Test for how the C compiler does inline.
# If both of these tests fail, then there is probably something wrong
# in the environment (flags and/or compiling and/or linking).
#
CHECK_C_SOURCE_COMPILES("
static inline int foo(){return 0;}
int main(int argc, char *argv[]){return 0;}"
                            C_HAS_inline)
IF(NOT C_HAS_inline)
  CHECK_C_SOURCE_COMPILES("
  static __inline int foo(){return 0;}
  int main(int argc, char *argv[]){return 0;}"
                            C_HAS___inline)
  SET(C_INLINE __inline)
ENDIF()

IF(NOT C_HAS_inline AND NOT C_HAS___inline)
  MESSAGE(FATAL_ERROR "It seems like ${CMAKE_C_COMPILER} does not support "
    "inline or __inline. Please verify compiler and flags. "
    "See CMakeFiles/CMakeError.log for why the test failed to compile/link.")
ENDIF()

IF(NOT CMAKE_CROSSCOMPILING AND NOT MSVC)
  STRING(TOLOWER ${CMAKE_SYSTEM_PROCESSOR}  processor)
  IF(processor MATCHES "86" OR processor MATCHES "amd64" OR processor MATCHES "x64")
    IF(NOT CMAKE_SYSTEM_NAME MATCHES "SunOS")
      # The loader in some Solaris versions has a bug due to which it refuses to
      # start a binary that has been compiled by GCC and uses __asm__("pause")
      # with the error:
      # $ ./mysqld
      # ld.so.1: mysqld: fatal: hardware capability unsupported: 0x2000 [ PAUSE ]
      # Killed
      # $
      # Even though the CPU does have support for the instruction.
      # Binaries that have been compiled by GCC and use __asm__("pause")
      # on a non-buggy Solaris get flagged with a "uses pause" flag and
      # thus they are unusable if copied on buggy Solaris version. To
      # circumvent this we explicitly disable __asm__("pause") when
      # compiling on Solaris. Subsequently the tests here will enable
      # HAVE_FAKE_PAUSE_INSTRUCTION which will use __asm__("rep; nop")
      # which currently generates the same code as __asm__("pause") - 0xf3 0x90
      # but without flagging the binary as "uses pause".
      CHECK_C_SOURCE_RUNS("
      int main()
      {
        __asm__ __volatile__ (\"pause\");
        return 0;
      }"  HAVE_PAUSE_INSTRUCTION)
    ENDIF()
  ENDIF()
  IF (NOT HAVE_PAUSE_INSTRUCTION)
    CHECK_C_SOURCE_COMPILES("
    int main()
    {
     __asm__ __volatile__ (\"rep; nop\");
     return 0;
    }
   " HAVE_FAKE_PAUSE_INSTRUCTION)
  ENDIF()
ENDIF()
  
# Assume regular sprintf
SET(SPRINTFS_RETURNS_INT 1)

IF(CMAKE_COMPILER_IS_GNUCXX AND HAVE_CXXABI_H)
CHECK_CXX_SOURCE_COMPILES("
 #include <cxxabi.h>
 int main(int argc, char **argv) 
  {
    char *foo= 0; int bar= 0;
    foo= abi::__cxa_demangle(foo, foo, 0, &bar);
    return 0;
  }"
  HAVE_ABI_CXA_DEMANGLE)
ENDIF()

CHECK_C_SOURCE_COMPILES("
  int main(int argc, char **argv) 
  {
    extern char *__bss_start;
    return __bss_start ? 1 : 0;
  }"
HAVE_BSS_START)

CHECK_C_SOURCE_COMPILES("
int main()
{
  __builtin_unreachable();
  return 0;
}" HAVE_BUILTIN_UNREACHABLE_C)

CHECK_CXX_SOURCE_COMPILES("
int main()
{
  __builtin_unreachable();
  return 0;
}" HAVE_BUILTIN_UNREACHABLE_CXX)

IF(HAVE_BUILTIN_UNREACHABLE_C AND HAVE_BUILTIN_UNREACHABLE_CXX)
  SET(HAVE_BUILTIN_UNREACHABLE 1)
ENDIF()

CHECK_C_SOURCE_COMPILES("
int main()
{
  long l= 0;
  __builtin_expect(l, 0);
  return 0;
}" HAVE_BUILTIN_EXPECT)

# GCC has __builtin_stpcpy but still calls stpcpy
IF(NOT CMAKE_SYSTEM_NAME MATCHES "SunOS" OR NOT CMAKE_COMPILER_IS_GNUCC)
CHECK_C_SOURCE_COMPILES("
int main()
{
  char foo1[1];
  char foo2[1];
  __builtin_stpcpy(foo1, foo2);
  return 0;
}" HAVE_BUILTIN_STPCPY)
ENDIF()

CHECK_CXX_SOURCE_COMPILES("
    #undef inline
    #if !defined(__osf__) && !defined(_REENTRANT)
    #define _REENTRANT
    #endif
    #include <pthread.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    int main()
    {

       struct hostent *foo =
       gethostbyaddr_r((const char *) 0,
          0, 0, (struct hostent *) 0, (char *) NULL,  0, (int *)0);
       return 0;
    }
  "
  HAVE_SOLARIS_STYLE_GETHOST)

CHECK_CXX_SOURCE_COMPILES("
  int main()
  {
    int foo= -10; int bar= 10;
    long long int foo64= -10; long long int bar64= 10;
    if (!__sync_fetch_and_add(&foo, bar) || foo)
      return -1;
    bar= __sync_lock_test_and_set(&foo, bar);
    if (bar || foo != 10)
      return -1;
    bar= __sync_val_compare_and_swap(&bar, foo, 15);
    if (bar)
      return -1;
    if (!__sync_fetch_and_add(&foo64, bar64) || foo64)
      return -1;
    bar64= __sync_lock_test_and_set(&foo64, bar64);
    if (bar64 || foo64 != 10)
      return -1;
    bar64= __sync_val_compare_and_swap(&bar64, foo, 15);
    if (bar64)
      return -1;
    return 0;
  }"
  HAVE_GCC_ATOMIC_BUILTINS)

IF(WITH_VALGRIND)
  SET(VALGRIND_HEADERS "valgrind/memcheck.h;valgrind/valgrind.h")
  CHECK_INCLUDE_FILES("${VALGRIND_HEADERS}" HAVE_VALGRIND_HEADERS)
  IF(HAVE_VALGRIND_HEADERS)
    SET(HAVE_VALGRIND 1)
  ELSE()
    MESSAGE(FATAL_ERROR "Unable to find Valgrind header files ${VALGRIND_HEADERS}. Make sure you have them in your include path.")
  ENDIF()
ENDIF()

#--------------------------------------------------------------------
# Check for IPv6 support
#--------------------------------------------------------------------
CHECK_INCLUDE_FILE(netinet/in6.h HAVE_NETINET_IN6_H)

IF(UNIX)
  SET(CMAKE_EXTRA_INCLUDE_FILES sys/types.h netinet/in.h sys/socket.h)
  IF(HAVE_NETINET_IN6_H)
    SET(CMAKE_EXTRA_INCLUDE_FILES ${CMAKE_EXTRA_INCLUDE_FILES} netinet/in6.h)
  ENDIF()
ELSEIF(WIN32)
  SET(CMAKE_EXTRA_INCLUDE_FILES ${CMAKE_EXTRA_INCLUDE_FILES} winsock2.h ws2ipdef.h)
ENDIF()

MY_CHECK_STRUCT_SIZE("sockaddr_in6" SOCKADDR_IN6)
MY_CHECK_STRUCT_SIZE("in6_addr" IN6_ADDR)

IF(HAVE_STRUCT_SOCKADDR_IN6 OR HAVE_STRUCT_IN6_ADDR)
  SET(HAVE_IPV6 TRUE CACHE INTERNAL "")
ENDIF()


# Check for sockaddr_storage.ss_family

CHECK_STRUCT_HAS_MEMBER("struct sockaddr_storage"
 ss_family "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_STORAGE_SS_FAMILY)
IF(NOT HAVE_SOCKADDR_STORAGE_SS_FAMILY)
  CHECK_STRUCT_HAS_MEMBER("struct sockaddr_storage"
  __ss_family "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_STORAGE___SS_FAMILY)
  IF(HAVE_SOCKADDR_STORAGE___SS_FAMILY)
    SET(ss_family __ss_family)
  ENDIF()
ENDIF()

#
# Check if struct sockaddr_in::sin_len is available.
#

CHECK_STRUCT_HAS_MEMBER("struct sockaddr_in" sin_len
  "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_IN_SIN_LEN)

#
# Check if struct sockaddr_in6::sin6_len is available.
#

CHECK_STRUCT_HAS_MEMBER("struct sockaddr_in6" sin6_len
  "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_IN6_SIN6_LEN)

SET(CMAKE_EXTRA_INCLUDE_FILES) 
