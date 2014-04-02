/* Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA */

/*
  Unit tests for MDL subsystem which require DEBUG_SYNC facility
  (mostly because they are very sensitive to thread scheduling).
*/

#include "my_config.h"
#include <gtest/gtest.h>

#include "mdl.h"
#include "test_utils.h"
#include "thread_utils.h"
#include "debug_sync.h"

#if defined(ENABLED_DEBUG_SYNC)
namespace mdl_sync_unittest {

using thread::Thread;
using thread::Notification;
using my_testing::Server_initializer;
using my_testing::Mock_error_handler;


class MDLSyncTest : public ::testing::Test
{
protected:
  MDLSyncTest() {}

  void SetUp()
  {
    /* Set debug sync timeout of 60 seconds. */
    opt_debug_sync_timeout= 60;
    debug_sync_init();
    /* Force immediate destruction of unused MDL_lock objects. */
    mdl_locks_unused_locks_low_water= 0;
    mdl_init();

    m_initializer.SetUp();
    m_thd= m_initializer.thd();
  }

  void TearDown()
  {

    m_initializer.TearDown();
    mdl_destroy();
    debug_sync_end();
    opt_debug_sync_timeout= 0;
    mdl_locks_unused_locks_low_water= MDL_LOCKS_UNUSED_LOCKS_LOW_WATER_DEFAULT;
  }

  Server_initializer m_initializer;
  THD *m_thd;
};


/** Wrapper function which simplify usage of debug_sync_set_action(). */

bool debug_sync_set_action(THD *thd, const char *sync)
{
  return debug_sync_set_action(thd, sync, strlen(sync));
}


/**
  Set sync point and acquires and then releases the specified type of lock
  on the table.
  Allows to pause thread execution after lock acquisition and before its
  release by using notifications.
*/

class MDLSyncThread : public Thread
{
public:
  MDLSyncThread(enum_mdl_type mdl_type_arg, const char *sync_arg,
                Notification *grabbed_arg, Notification *release_arg)
    : m_mdl_type(mdl_type_arg), m_sync(sync_arg),
      m_lock_grabbed(grabbed_arg), m_lock_release(release_arg)
  {}

  virtual void run()
  {
    m_initializer.SetUp();
    m_thd= m_initializer.thd();
    m_mdl_context= &m_thd->mdl_context;

    /*
      Use a block to ensure that Mock_error_handler dtor is called
      before m_initalizer.TearDown().
    */
    {
      Mock_error_handler error_handler(m_thd, 0);

      if (m_sync)
      {
        EXPECT_FALSE(debug_sync_set_action(m_thd, m_sync));
      }

      MDL_request request;
      MDL_REQUEST_INIT(&request, MDL_key::TABLE, "db", "table", m_mdl_type,
                       MDL_TRANSACTION);

      EXPECT_FALSE(m_mdl_context->acquire_lock(&request, 3600));
      EXPECT_TRUE(m_mdl_context->
                  is_lock_owner(MDL_key::TABLE, "db", "table", m_mdl_type));

      if (m_lock_grabbed)
        m_lock_grabbed->notify();
      if (m_lock_release)
        m_lock_release->wait_for_notification();

      m_mdl_context->release_transactional_locks();

      /* The above should not generate any warnings (e.g. about timeouts). */
      EXPECT_EQ(0, error_handler.handle_called());
    }

    m_initializer.TearDown();
  }

private:
  Server_initializer m_initializer;
  THD *m_thd;
  MDL_context *m_mdl_context;
  enum_mdl_type m_mdl_type;
  const char *m_sync;
  Notification *m_lock_grabbed;
  Notification *m_lock_release;
};


/*
  Checks that "fast path" lock acquisition correctly handles MDL_lock objects
  which already have been marked as destroyed but still present in MDL_map.
*/

TEST_F(MDLSyncTest, IsDestroyedFastPath)
{
  MDLSyncThread thread1(MDL_SHARED,
                        "mdl_remove_random_unused_after_is_destroyed_set "
                        "SIGNAL marked WAIT_FOR resume_removal",
                        NULL, NULL);
  MDLSyncThread thread2(MDL_SHARED,
                        "mdl_acquire_lock_is_destroyed_fast_path "
                        "SIGNAL resume_removal", NULL, NULL);

  /*
    Start the first thread which acquires S lock on a table and immediately
    releases it. As result the MDL_lock object for the table becomes unused
    and attempt to destroy this unused object is made. MDL_lock is marked
    as destroyed, but the thread hits sync point before the object is removed
    from the MDL_map hash.
  */
  thread1.start();

  /* Wait until MDL_lock object is marked as destroyed. */
  Mock_error_handler error_handler(m_thd, 0);
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now WAIT_FOR marked"));
  /* The above should not generate warnings about timeouts. */
  EXPECT_EQ(0, error_handler.handle_called());

  /*
    Start the second thread which will try to acquire S lock on the same
    table. It should notice that corresponding MDL_lock object is marked
    as destroyed. Sync point in code responsible for handling this situation
    will emit signal which should unblock the first thread.
  */
  thread2.start();

  /* Check that both threads finish. */
  thread1.join();
  thread2.join();
}


/*
  Checks that "slow path" lock acquisition correctly handles MDL_lock objects
  which already have been marked as destroyed but still present in MDL_map.
*/

TEST_F(MDLSyncTest, IsDestroyedSlowPath)
{
  MDLSyncThread thread1(MDL_SHARED,
                        "mdl_remove_random_unused_after_is_destroyed_set "
                        "SIGNAL marked WAIT_FOR resume_removal",
                        NULL, NULL);
  MDLSyncThread thread2(MDL_SHARED_NO_READ_WRITE,
                        "mdl_acquire_lock_is_destroyed_slow_path "
                        "SIGNAL resume_removal", NULL, NULL);

  /*
    Start the first thread which acquires S lock on a table and immediately
    releases it. As result the MDL_lock object for the table becomes unused
    and attempt to destroy this unused object is made. MDL_lock is marked
    as destroyed, but the thread hits sync point before the object is removed
    from the MDL_map hash.
  */
  thread1.start();

  /* Wait until MDL_lock object is marked as destroyed. */
  Mock_error_handler error_handler(m_thd, 0);
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now WAIT_FOR marked"));
  /* The above should not generate warnings about timeouts. */
  EXPECT_EQ(0, error_handler.handle_called());

  /*
    Start the second thread which will try to acquire SNRW lock on the same
    table. It should notice that corresponding MDL_lock object is marked
    as destroyed. Sync point in code responsible for handling this situation
    will emit signal which should unblock the first thread.
  */
  thread2.start();

  /* Check that both threads finish. */
  thread1.join();
  thread2.join();
}


/*
  Checks that code responsible for destroying of random unused MDL_lock
  object correctly handles situation then it fails to find such an object.
*/

TEST_F(MDLSyncTest, DoubleDestroyTakeOne)
{
  MDLSyncThread thread1(MDL_SHARED,
                        "mdl_remove_random_unused_before_search "
                        "SIGNAL before_search WAIT_FOR start_search",
                        NULL, NULL);
  MDLSyncThread thread2(MDL_SHARED, NULL, NULL, NULL);

  /*
    Start the first thread which acquires S lock on a table and immediately
    releases it. As result the MDL_lock object for the table becomes unused
    and attempt to destroy random unused object is made. Thread hits sync
    point before it starts search for unused object.
  */
  thread1.start();

  /* Wait until thread stops before searching for unused object. */
  Mock_error_handler error_handler(m_thd, 0);
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now WAIT_FOR before_search"));
  /* The above should not generate warnings about timeouts. */
  EXPECT_EQ(0, error_handler.handle_called());

  /*
    Start the second thread which will acquire and release S lock.
    As result MDL_lock object will become unused and destroyed.
    There should be no unused MDL_lock objects after its completion.
  */
  thread2.start();
  thread2.join();
  EXPECT_EQ(0, mdl_get_unused_locks_count());

  /*
    Resume the first thread. At this point it should find no unused objects.
    Check that it finishes succesfully.
  */
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now SIGNAL start_search"));
  thread1.join();
}


/*
  Checks that code responsible for destroying of random unused MDL_lock
  object correctly handles situation then it discovers that object
  which was chosen for destruction is already destroyed.
*/

TEST_F(MDLSyncTest, DoubleDestroyTakeTwo)
{
  MDLSyncThread thread1(MDL_SHARED,
                        "mdl_remove_random_unused_after_search "
                        "SIGNAL found WAIT_FOR resume_destroy",
                        NULL, NULL);
  MDLSyncThread thread2(MDL_SHARED,
                        "mdl_remove_random_unused_after_is_destroyed_set "
                        "SIGNAL resume_destroy", NULL, NULL);

  /*
    Start the first thread which acquires S lock on a table and immediately
    releases it. As result the MDL_lock object for the table becomes unused
    and attempt to destroy random unused object is made. Thread hits sync
    point after it has found the only unused MDL_lock object, but before
    it has acquired its MDL_lock::m_rwlock.
  */
  thread1.start();

  /* Wait until thread finds unused object. */
  Mock_error_handler error_handler(m_thd, 0);
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now WAIT_FOR found"));
  /* The above should not generate warnings about timeouts. */
  EXPECT_EQ(0, error_handler.handle_called());

  /*
    Start the second thread which will acquire and release S lock.
    As result MDL_lock object will become unused and we will try
    to destroy it again.
    Thread will hit the sync point once the object is marked as destroyed.
    As result signal unblocking the first thread will be emitted.

  */
  thread2.start();

  /*
    The first thread should discover that the only unused MDL_lock object
    already has been marked as destroyed and still complete successfully.
  */
  thread1.join();

  /* So does the second thread. */
  thread2.join();
}


/*
  Checks that code responsible for destroying of random unused MDL_lock
  object correctly handles situation then it discovers that object
  which was chosen for destruction is re-used again and can't be destroyed.
*/

TEST_F(MDLSyncTest, DestroyUsed)
{
  Notification lock_grabbed, lock_release;
  MDLSyncThread thread1(MDL_SHARED,
                        "mdl_remove_random_unused_after_search "
                        "SIGNAL found WAIT_FOR resume_destroy",
                        NULL, NULL);
  MDLSyncThread thread2(MDL_SHARED,
                        NULL, &lock_grabbed, &lock_release);

  /*
    Start the first thread which acquires S lock on a table and immediately
    releases it. As result the MDL_lock object for the table becomes unused
    and attempt to destroy random unused object is made. Thread hits sync
    point after it has found the only unused MDL_lock object, but before
    it has acquired its MDL_lock::m_rwlock.
  */
  thread1.start();

  /* Wait until thread finds unused object. */
  Mock_error_handler error_handler(m_thd, 0);
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now WAIT_FOR found"));
  /* The above should not generate warnings about timeouts. */
  EXPECT_EQ(0, error_handler.handle_called());

  /*
    Start the second thread which will acquire S lock and re-use
    the same MDL_lock object.
  */
  thread2.start();
  /* Wait until lock is acquired. */
  lock_grabbed.wait_for_notification();

  /*
    Resume the first thread. It should discover that the MDL_lock object
    it has found is used again and can't be destroyed.
  */
  EXPECT_FALSE(debug_sync_set_action(m_thd, "now SIGNAL resume_destroy"));

  /* Still it should complete successfully.*/
  thread1.join();

  /* So does the second thread after it releases S lock. */
  lock_release.notify();
  thread2.join();
}


}
#endif
