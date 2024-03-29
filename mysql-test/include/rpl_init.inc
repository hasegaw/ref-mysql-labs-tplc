# ==== Purpose ====
#
# Set up replication on several servers in a specified topology.
#
# By default, this script does the following:
#  - Creates the connections server_1, server_2, ..., server_N, as
#    well as extra connections server_1_1, server_2_1, ...,
#    server_N_1. server_I and server_I_1 are connections to the same
#    server.
#  - Verifies that @@server_id of all servers are different.
#  - Calls RESET MASTER, RESET SLAVE, USE test, CHANGE MASTER, START SLAVE.
#  - Sets the connection to server_1 before exiting.
#
# ==== Usage ====
#
# 1. If you are going to use more than two servers, create
#    rpl_test.cfg with the following contents:
#
#    !include ../my.cnf
#    [mysqld.1]
#    log-slave-updates
#    [mysqld.2]
#    log-slave-updates
#    ...
#    [mysqld.N]
#    log-slave-updates
#
#    [ENV]
#    SERVER_MYPORT_3= @mysqld.3.port
#    SERVER_MYPORT_4= @mysqld.4.port
#    SERVER_MYPORT_5= @mysqld.5.port
#    ...
#    SERVER_MYPORT_N= @mysqld.N.port
#
#    (It is allowed, but not required, to configure SERVER_MYPORT_1
#    and SERVER_MYPORT_2 too. If these variables are not set, the
#    variables MASTER_MYPORT and SLAVE_MYPORT, configured in the
#    default my.cnf used by the rpl and rpl_ndb suites, are used
#    instead.  In addition, in the rpl_ndb suite, SERVER_MYPORT_3 is
#    not needed since MASTER_MYPORT1 can be used instead.)
#
# 2. Execute the following near the top of the test:
#
#    [--let $rpl_server_count= 7]
#    --let $rpl_topology= 1->2->3->1->4, 2->5, 6->7
#    [--let $rpl_extra_connections_per_server= 1]
#    [--let $rpl_check_server_ids= 1]
#    [--let $rpl_skip_change_master= 1]
#    [--let $rpl_skip_start_slave= 1]
#    [--let $rpl_skip_reset_master_and_slave= 1]
#    [--let $rpl_debug= 1]
#    [--let $slave_timeout= NUMBER]
#    [--let $use_gtids= 1]
#    [--let $rpl_gtid_utils= 1]
#    --source include/rpl_init.inc
#
#    Parameters:
#
#    $rpl_server_count
#      The number of servers to configure.  If this is not set, the largest
#      number in $rpl_topology will be used.
#
#    $rpl_topology
#      A comma-separated list of replication chain
#      specifications. Each replication chain specification has the
#      form S1->S2->...->Sn, where 1 <= S1,...Sn <= $rpl_server_count.
#      This file will configure S(i+1) to be a slave of S(i).  If you
#      want to specify the empty topology (no server replicates at
#      all), you have to set $rpl_topology=none.
#
#    $rpl_extra_connections_per_server
#      By default, this script creates connections server_N and
#      server_N_1.  If you can set this variable to a number, the
#      script creates:
#      server_N, server_N_1, ..., server_N_$rpl_extra_connections_per_server
#
#    $rpl_check_server_ids
#      If $rpl_check_server_ids is set, this script checks that the
#      @@server_id of all servers are different. This is normally
#      guaranteed by mtr, so it is only useful for debugging.
#
#    $rpl_skip_reset_master_and_slave
#      By default, this script issues RESET MASTER and RESET SLAVE
#      before CHANGE MASTER and START SLAVE.  RESET MASTER and RESET
#      SLAVE are suppressed if $rpl_skip_reset_master_and_slave is
#      set.
#
#    $rpl_skip_change_master
#      By default, this script issues CHANGE MASTER so that all slaves
#      are ready to run as specified by $rpl_topology. CHANGE MASTER
#      is suppressed if $rpl_skip_change_master is set.
#
#    $rpl_skip_start_slave
#      By default, this script issues START SLAVE on all slaves
#      specified by $rpl_topology. START SLAVE is suppressed if
#      $rpl_skip_change_master is set.
#
#    $rpl_debug
#      By default, this script only outputs a static text that says
#      that rpl_init.inc was invoked. If $rpl_debug is set, additional
#      debug info is printed. The debug info may be nondeterministic,
#      so no test case should be checked in with $rpl_debug set.
#
#    $slave_timeout
#      Timeout used when waiting for the slave threads to start.
#      See include/wait_for_slave_param.inc
#
#    $use_gtids
#      Use option MASTER_AUTO_POSITION = 1 to CHANGE MASTER.
#
#    $rpl_gtid_utils
#      Create stored functions for GTID manipulation on every server (see
#      gtid_utils.inc).
#
#
# ==== Side effects ====
#
# Changes current connection to server_1.

--source include/have_log_bin.inc


--let $include_filename= rpl_init.inc [topology=$rpl_topology]
--source include/begin_include_file.inc


if ($rpl_debug)
{
  --echo ---- Check input ----
  --echo MASTER_MYPORT='$MASTER_MYPORT' SLAVE_MYPORT='$SLAVE_MYPORT' MASTER_MYPORT1='$MASTER_MYPORT1' SLAVE_MYPORT1='$SLAVE_MYPORT1'
}

# Allow $MASTER_MYPORT as alias for $SERVER_MYPORT_1
if (!$SERVER_MYPORT_1)
{
  --let SERVER_MYPORT_1= $MASTER_MYPORT
}
# Allow $SLAVE_MYPORT as alias for $SERVER_MYPORT_2
if (!$SERVER_MYPORT_2)
{
  --let SERVER_MYPORT_2= $SLAVE_MYPORT
}
# Allow $MASTER_MYPORT1 as alias for $SERVER_MYPORT_3
# (this alias is used by rpl_ndb tests)
if (!$SERVER_MYPORT_3)
{
  --let SERVER_MYPORT_3= $MASTER_MYPORT1
}
# Allow $SLAVE_MYPORT1 as alias for $SERVER_MYPORT_4
# (this alias is used by rpl_ndb tests)
if (!$SERVER_MYPORT_4)
{
  --let SERVER_MYPORT_4= $SLAVE_MYPORT1
}
# Check that $rpl_server_count is set
if (!$rpl_server_count)
{
  --let $rpl_server_count= `SELECT REPLACE('$rpl_topology', '->', ',')`
  if (`SELECT LOCATE(',', '$rpl_server_count')`)
  {
    --let $rpl_server_count= `SELECT GREATEST($rpl_server_count)`
  }
}

--let $_rpl_extra_connections_per_server= $rpl_extra_connections_per_server
if ($_rpl_extra_connections_per_server == '')
{
  --let $_rpl_extra_connections_per_server= 1
}


if ($rpl_debug)
{
  --echo ---- Setup connections and reset each server ----
}


if (!$rpl_debug)
{
  --disable_query_log
}


# Create two connections to each server; reset master/slave, select
# database, set autoinc variables.
--let $_rpl_server= $rpl_server_count
--let $underscore= _
while ($_rpl_server)
{
  # Connect.
  --let $rpl_server_number= $_rpl_server
  --let $rpl_connection_name= server_$_rpl_server
  --source include/rpl_connect.inc
  --let $_rpl_connection_number= 1
  while ($_rpl_connection_number <= $_rpl_extra_connections_per_server)
  {
    --let $rpl_connection_name= server_$_rpl_server$underscore$_rpl_connection_number
    --source include/rpl_connect.inc
    --inc $_rpl_connection_number
  }

  # Configure server.
  --let $rpl_connection_name= server_$_rpl_server
  --source include/rpl_connection.inc
  USE test;
  if ($rpl_gtid_utils)
  {
    --source include/gtid_utils.inc
  }
  if (!$rpl_skip_reset_master_and_slave)
  {
    RESET MASTER;
    RESET SLAVE;
  }
  # Only way to do this in mtr :-(
  if ($_rpl_server == 1) {
    --let $server_1_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 2) {
    --let $server_2_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 3) {
    --let $server_3_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 4) {
    --let $server_4_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 5) {
    --let $server_5_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 6) {
    --let $server_6_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 7) {
    --let $server_7_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 8) {
    --let $server_8_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 9) {
    --let $server_9_datadir= `SELECT @@datadir`
  }
  if ($_rpl_server == 10) {
    --let $server_10_datadir= `SELECT @@datadir`
  }

  --dec $_rpl_server
}


# Signal that initialization is done and all connections created.
--let $rpl_inited= 1

# Signal that the server is in a dirty state and needs to be restarted
# if the test is skipped. If the test is not skipped, it will continue
# to the end and execute its cleanup section (and check-testcase will
# report if you forget to clean up).
--source include/force_restart_if_skipped.inc


# Assert that all hosts have different server_ids
if ($rpl_check_server_ids)
{
  if ($rpl_debug)
  {
    --echo ---- Check that @@server_id is distinct for all servers ----
  }

  --let $_rpl_server= $rpl_server_count
  while ($_rpl_server)
  {
    --let $_rpl_server2= $_rpl_server
    --dec $_rpl_server2
    while ($_rpl_server2)
    {
      --let $assert_text= Servers $_rpl_server and $_rpl_server2 should have different @@server_id
      --let $assert_condition= [$_rpl_server:SELECT @@server_id AS i, i, 1] != [$_rpl_server2:SELECT @@server_id AS i, i, 1]

      --source include/assert.inc
      --dec $_rpl_server2
    }
    --dec $_rpl_server
  }
}

# $rpl_master_list must be set so that include/rpl_change_topology.inc
# knows which servers are initialized and not.
if (!`SELECT COUNT(*) = 0 OR VARIABLE_VALUE != 'ON' FROM INFORMATION_SCHEMA.GLOBAL_VARIABLES WHERE VARIABLE_NAME = 'GTID_MODE'`)
{
  --let $use_gtids=1
}
--let $rpl_master_list= `SELECT REPEAT('x', $rpl_server_count * LENGTH($rpl_server_count))`
--source include/rpl_change_topology.inc


if (!$rpl_skip_start_slave)
{
  --source include/rpl_start_slaves.inc
}


--let $rpl_connection_name= server_1
--source include/rpl_connection.inc


--let $skip_restore_connection= 1
--let $include_filename= rpl_init.inc
--source include/end_include_file.inc
