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

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/libmysql
  ${CMAKE_SOURCE_DIR}/regex
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_SOURCE_DIR}/strings
  ${SSL_INCLUDE_DIRS}
  ${SSL_INTERNAL_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIR})
ADD_DEFINITIONS(${SSL_DEFINES})

SET(CLIENT_API_FUNCTIONS
get_tty_password
handle_options
load_defaults
mysql_thread_end
mysql_thread_init
myodbc_remove_escape
mysql_affected_rows
mysql_autocommit
mysql_stmt_bind_param
mysql_stmt_bind_result
mysql_change_user
mysql_character_set_name
mysql_close
mysql_commit
mysql_data_seek
mysql_debug
mysql_dump_debug_info
mysql_eof
mysql_errno
mysql_error
mysql_escape_string
mysql_hex_string
mysql_stmt_execute
mysql_stmt_fetch
mysql_stmt_fetch_column
mysql_fetch_field
mysql_fetch_field_direct
mysql_fetch_fields
mysql_fetch_lengths
mysql_fetch_row
mysql_field_count
mysql_field_seek
mysql_field_tell
mysql_free_result
mysql_get_client_info
mysql_get_host_info
mysql_get_proto_info
mysql_get_server_info
mysql_get_client_version
mysql_get_ssl_cipher
mysql_info
mysql_init
mysql_insert_id
mysql_kill
mysql_set_server_option
mysql_list_dbs
mysql_list_fields
mysql_list_processes
mysql_list_tables
mysql_more_results
mysql_next_result
mysql_num_fields
mysql_num_rows
mysql_options
mysql_stmt_param_count
mysql_stmt_param_metadata
mysql_ping
mysql_stmt_result_metadata
mysql_query
mysql_read_query_result
mysql_real_connect
mysql_real_escape_string
mysql_real_query
mysql_refresh
mysql_rollback
mysql_row_seek
mysql_row_tell
mysql_select_db
mysql_stmt_send_long_data
mysql_send_query
mysql_shutdown
mysql_ssl_set
mysql_stat
mysql_stmt_affected_rows
mysql_stmt_close
mysql_stmt_reset
mysql_stmt_data_seek
mysql_stmt_errno
mysql_stmt_error
mysql_stmt_free_result
mysql_stmt_num_rows
mysql_stmt_row_seek
mysql_stmt_row_tell
mysql_stmt_store_result
mysql_store_result
mysql_thread_id
mysql_thread_safe
mysql_use_result
mysql_warning_count
mysql_stmt_sqlstate
mysql_sqlstate
mysql_get_server_version
mysql_stmt_prepare
mysql_stmt_init
mysql_stmt_insert_id
mysql_stmt_attr_get
mysql_stmt_attr_set
mysql_stmt_field_count
mysql_set_local_infile_default
mysql_set_local_infile_handler
mysql_embedded
mysql_server_init
mysql_server_end
mysql_set_character_set
mysql_get_character_set_info
mysql_stmt_next_result
my_init
mysql_client_find_plugin
mysql_client_register_plugin
mysql_load_plugin
mysql_load_plugin_v
mysql_options4
mysql_plugin_options
mysql_reset_connection
mysql_get_option
mysql_session_track_get_first
mysql_session_track_get_next

CACHE INTERNAL "Functions exported by client API"

)

SET(CLIENT_SOURCES
  get_password.c 
  libmysql.c
  errmsg.c
  ../sql-common/client.c 
  ../sql-common/my_time.c 
  ../sql-common/client_plugin.c 
  ../sql-common/client_authentication.cc
  ../sql/net_serv.cc
  ../sql-common/pack.c 
  ../sql/auth/password.c
)

IF (WIN32 AND OPENSSL_APPLINK_C)
  SET_SOURCE_FILES_PROPERTIES(
    ../sql-common/client_authentication.cc
    PROPERTIES COMPILE_FLAGS "-DHAVE_OPENSSL_APPLINK_C"
  )
ENDIF()

#
# Include protocol tracing infrastructure and the test
# trace plugin if enabled by build options.
#
IF (WITH_CLIENT_PROTOCOL_TRACING)

  LIST(APPEND CLIENT_SOURCES mysql_trace.c)
  ADD_DEFINITIONS(-D CLIENT_PROTOCOL_TRACING)

  IF (WITH_TEST_TRACE_PLUGIN)
    MESSAGE(STATUS "Client library contains the test trace plugin")
    LIST(APPEND CLIENT_SOURCES test_trace_plugin.cc)
    ADD_DEFINITIONS(-D TEST_TRACE_PLUGIN)
  ENDIF (WITH_TEST_TRACE_PLUGIN)

ENDIF (WITH_CLIENT_PROTOCOL_TRACING)

ADD_CONVENIENCE_LIBRARY(clientlib ${CLIENT_SOURCES})
DTRACE_INSTRUMENT(clientlib)
ADD_DEPENDENCIES(clientlib GenError)

SET(LIBS clientlib dbug strings vio mysys mysys_ssl ${ZLIB_LIBRARY} ${SSL_LIBRARIES} ${LIBDL})

#
# On Windows platform client library includes the client-side 
# Windows Native Authentication plugin.
#
IF(WIN32)
  ADD_DEFINITIONS(-DAUTHENTICATION_WIN)
  ADD_SUBDIRECTORY(authentication_win)
  LIST(APPEND LIBS auth_win_client)
ENDIF()

# Merge several convenience libraries into one big mysqlclient
MERGE_LIBRARIES(mysqlclient STATIC ${LIBS} COMPONENT Development)

# Visual Studio users need debug  static library for debug projects
INSTALL_DEBUG_SYMBOLS(clientlib)
IF(MSVC)
 INSTALL_DEBUG_TARGET(mysqlclient DESTINATION ${INSTALL_LIBDIR}/debug)
 INSTALL_DEBUG_TARGET(clientlib DESTINATION ${INSTALL_LIBDIR}/debug)
ENDIF()

MACRO(GET_TARGET_NAME target out_name)
  GET_TARGET_PROPERTY(location ${target} LOCATION)
  GET_FILENAME_COMPONENT(name ${location} NAME)
  SET(${out_name} ${name})
ENDMACRO()

IF(UNIX)
  MACRO(GET_VERSIONED_LIBNAME LIBNAME EXTENSION VERSION OUTNAME)
    SET(DOT_VERSION ".${VERSION}")
    IF(DOT_VERSION STREQUAL ".") 
      SET(DOT_VERSION "")
    ENDIF()
    IF(APPLE)
      SET(${OUTNAME} ${LIBNAME}${DOT_VERSION}${EXTENSION})
    ELSE()
      SET(${OUTNAME} ${LIBNAME}${EXTENSION}${DOT_VERSION})
    ENDIF() 
  ENDMACRO()
ENDIF()

IF(UNIX)
  GET_TARGET_NAME(mysqlclient lib_name)
  INSTALL_SYMLINK(mysqlclient
    ${lib_name} ${CMAKE_STATIC_LIBRARY_PREFIX}mysqlclient_r.a
    ${INSTALL_LIBDIR} Development)
ENDIF()

IF(NOT DISABLE_SHARED)
  # Merge several convenience libraries into one big mysqlclient
  # and link them together into shared library.
  MERGE_LIBRARIES(libmysql SHARED ${LIBS}
    EXPORTS ${CLIENT_API_FUNCTIONS}
    COMPONENT SharedLibraries)
  IF(UNIX)
    # libtool compatability
    IF(CMAKE_SYSTEM_NAME MATCHES "FreeBSD" OR APPLE)
      SET(OS_SHARED_LIB_VERSION "${SHARED_LIB_MAJOR_VERSION}")
    ELSE()
      SET(OS_SHARED_LIB_VERSION
        "${SHARED_LIB_MAJOR_VERSION}.${SHARED_LIB_MINOR_VERSION}.0")
    ENDIF()
    # Name of shared library is mysqlclient on Unix
    SET_TARGET_PROPERTIES(libmysql PROPERTIES 
      OUTPUT_NAME mysqlclient 
      VERSION "${OS_SHARED_LIB_VERSION}" 
      SOVERSION "${SHARED_LIB_MAJOR_VERSION}")
    CONFIGURE_FILE(libmysql.ver.in ${CMAKE_CURRENT_BINARY_DIR}/libmysql.ver)

    IF(APPLE)
      SET_TARGET_PROPERTIES(libmysql PROPERTIES
        INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/${INSTALL_LIBDIR}"
      )
    ENDIF()

    GET_TARGET_PROPERTY(libmysql_link_flags libmysql LINK_FLAGS)
    IF(NOT libmysql_link_flag)
      SET(libmysql_link_flags)
    ENDIF()
    SET(libmysql_link_flags
      "${CMAKE_SHARED_LIBRARY_C_FLAGS} ${libmysql_link_flags}")
    SET_TARGET_PROPERTIES(libmysql
      PROPERTIES LINK_FLAGS "${libmysql_link_flags}")

    IF(LINK_FLAG_NO_UNDEFINED)
      SET(libmysql_link_flags
        "${libmysql_link_flags} ${LINK_FLAG_NO_UNDEFINED}")
      SET(libmysql_link_flags
        "${libmysql_link_flags} -Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/libmysql.ver")
      SET_TARGET_PROPERTIES(libmysql
        PROPERTIES LINK_FLAGS "${libmysql_link_flags}")
    ENDIF() 
    # clean direct output needs to be set several targets have the same name
    #(mysqlclient in this case)
    SET_TARGET_PROPERTIES(mysqlclient PROPERTIES CLEAN_DIRECT_OUTPUT 1)
    SET_TARGET_PROPERTIES(libmysql PROPERTIES CLEAN_DIRECT_OUTPUT 1)

    # Install links to libmysqlclient.so (client_r)
    GET_VERSIONED_LIBNAME(
      "${CMAKE_SHARED_LIBRARY_PREFIX}mysqlclient_r"
      "${CMAKE_SHARED_LIBRARY_SUFFIX}"
      ""
      linkname)
    GET_TARGET_NAME(libmysql lib_name)
    GET_FILENAME_COMPONENT(lib_name_we ${lib_name} NAME_WE)
    INSTALL_SYMLINK(libmysql
      ${lib_name} ${linkname}
      ${INSTALL_LIBDIR} SharedLibraries)
    SET(OS_SHARED_LIB_SYMLINKS
      "${SHARED_LIB_MAJOR_VERSION}" "${OS_SHARED_LIB_VERSION}")
    LIST(REMOVE_DUPLICATES OS_SHARED_LIB_SYMLINKS)
    FOREACH(ver ${OS_SHARED_LIB_SYMLINKS})
      GET_VERSIONED_LIBNAME(
        "${CMAKE_SHARED_LIBRARY_PREFIX}mysqlclient_r"
        "${CMAKE_SHARED_LIBRARY_SUFFIX}"
        "${ver}"
        linkname)
      GET_VERSIONED_LIBNAME(
        ${lib_name_we} "${CMAKE_SHARED_LIBRARY_SUFFIX}" "${ver}" lib_name_ver)
      INSTALL_SYMLINK(libmysql
        ${lib_name_ver} ${linkname}
        ${INSTALL_LIBDIR} SharedLibraries)
    ENDFOREACH()
  ENDIF()
ENDIF()

#
#  Basic application for testing linking against dynamic client library.
#

#
# TODO: Consider whether these mysys functions should be exported as C API
# calls. At the moment they are not defined in <mysql.h>
#
LIST(REMOVE_ITEM CLIENT_API_FUNCTIONS
     handle_options 
     load_defaults 
     my_init)

#
# Generate a comma separated list of C API functions which is used
# to initialize api_calls[] array in api_test.c
#
SET(CLIENT_API_FUNCTION_LIST "")
FOREACH(api ${CLIENT_API_FUNCTIONS})
  SET(CLIENT_API_FUNCTION_LIST "${CLIENT_API_FUNCTION_LIST} ${api},")
ENDFOREACH()

#
# Generate api_test.c source, substituting @CLIENT_API_FUNCTION_LIST@
# with the list of API functions.
#
CONFIGURE_FILE(api_test.c.in ${CMAKE_CURRENT_BINARY_DIR}/api_test.c)

#
# Note: Compiling this test application will fail if not all symbols
# from @CLIENT_API_FUNCTIONS@ are declared by <mysql.h>. It will fail
# to run if not all of these symbols are exported by the library.
#
ADD_EXECUTABLE(libmysql_api_test ${CMAKE_CURRENT_BINARY_DIR}/api_test.c)
SET_TARGET_PROPERTIES(libmysql_api_test PROPERTIES LINKER_LANGUAGE CXX)

IF(DISABLE_SHARED)
  TARGET_LINK_LIBRARIES(libmysql_api_test ${LIBS})
ELSE()
  TARGET_LINK_LIBRARIES(libmysql_api_test libmysql)
ENDIF()
