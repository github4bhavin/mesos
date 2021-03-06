/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/pid.hpp>

#include "configurator/configuration.hpp"

#include "detector/detector.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "tests/cluster.hpp"
#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Clock;
using process::Future;
using process::PID;

using std::map;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::ByRef;
using testing::DoAll;
using testing::DoDefault;
using testing::Eq;
using testing::InSequence;
using testing::Return;
using testing::SaveArg;


class DRFAllocatorTest : public MesosClusterTest {};


// Checks that the DRF allocator implements the DRF algorithm
// correctly. The test accomplishes this by adding frameworks and
// slaves one at a time to the allocator, making sure that each time
// a new slave is added all of its resources are offered to whichever
// framework currently has the smallest share. Checking for proper DRF
// logic when resources are returned, frameworks exit, etc. is handled
// by SorterTest.DRFSorter.
TEST_F(DRFAllocatorTest, DRFAllocatorProcess)
{
  MockAllocatorProcess<HierarchicalDRFAllocatorProcess> allocator;

  EXPECT_CALL(allocator, initialize(_, _));

  Try<PID<Master> > master = cluster.masters.start(&allocator);
  ASSERT_SOME(master);

  TestingIsolator isolator1;
  slave::Flags flags1 = cluster.slaves.flags;
  flags1.resources = Option<string>("cpus:2;mem:1024;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = cluster.slaves.start(flags1, &isolator1);
  ASSERT_SOME(slave1);
  // Total cluster resources now cpus=2, mem=1024.

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get());

  EXPECT_CALL(allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_READY(offers1);

  // framework1 will be offered all of slave1's resources since it is
  // the only framework running so far, giving it cpus=2, mem=1024.
  EXPECT_THAT(offers1.get(), OfferEq(2, 1024));
  // framework1 share = 1

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_name("framework2");
  frameworkInfo2.set_user("user2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get());

  Future<Nothing> frameworkAdded2;
  EXPECT_CALL(allocator, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
		    FutureSatisfy(&frameworkAdded2)));

  EXPECT_CALL(sched2, registered(_, _, _));

  driver2.start();

  AWAIT_READY(frameworkAdded2);

  TestingIsolator isolator2;
  slave::Flags flags2 = cluster.slaves.flags;
  flags2.resources = Option<string>("cpus:1;mem:512;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  Try<PID<Slave> > slave2 = cluster.slaves.start(flags2, &isolator2);
  ASSERT_SOME(slave2);
  // Total cluster resources now cpus=3, mem=1536.
  // framework1 share = 0.66
  // framework2 share = 0

  AWAIT_READY(offers2);

  // framework2 will be offered all of slave2's resources since
  // it has the lowest share, giving it cpus=1, mem=512.
  EXPECT_THAT(offers2.get(), OfferEq(1, 512));
  // framework1 share =  0.66
  // framework2 share = 0.33

  TestingIsolator isolator3;
  slave::Flags flags3 = cluster.slaves.flags;
  flags3.resources = Option<string>("cpus:3;mem:2048;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers3;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers3));

  Try<PID<Slave> > slave3 = cluster.slaves.start(flags3, &isolator3);
  ASSERT_SOME(slave3);
  // Total cluster resources now cpus=6, mem=3584.
  // framework1 share = 0.33
  // framework2 share = 0.16

  AWAIT_READY(offers3);

  // framework2 will be offered all of slave3's resources since
  // it has the lowest share, giving it cpus=4, mem=2560.
  EXPECT_THAT(offers3.get(), OfferEq(3, 2048));
  // framework1 share = 0.33
  // framework2 share = 0.71

  FrameworkInfo frameworkInfo3;
  frameworkInfo3.set_name("framework3");
  frameworkInfo3.set_user("user1");
  MockScheduler sched3;
  MesosSchedulerDriver driver3(&sched3, frameworkInfo3, master.get());

  Future<Nothing> frameworkAdded3;
  EXPECT_CALL(allocator, frameworkAdded(_, _, _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&allocator),
		    FutureSatisfy(&frameworkAdded3)));

  EXPECT_CALL(sched3, registered(_, _, _));

  driver3.start();

  AWAIT_READY(frameworkAdded3);

  TestingIsolator isolator4;
  slave::Flags flags4 = cluster.slaves.flags;
  flags4.resources = Option<string>("cpus:4;mem:4096;disk:0");

  EXPECT_CALL(allocator, slaveAdded(_, _, _));

  Future<vector<Offer> > offers4;
  EXPECT_CALL(sched3, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers4));

  Try<PID<Slave> > slave4 = cluster.slaves.start(flags4, &isolator4);
  ASSERT_SOME(slave4);
  // Total cluster resources now cpus=10, mem=7680.
  // framework1 share = 0.2
  // framework2 share = 0.4
  // framework3 share = 0

  AWAIT_READY(offers4);

  // framework3 will be offered all of slave4's resources since
  // it has the lowest share.
  EXPECT_THAT(offers4.get(), OfferEq(4, 4096));

  // Shut everything down.
  EXPECT_CALL(allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(allocator, frameworkDeactivated(_))
    .Times(AtMost(3));

  EXPECT_CALL(allocator, frameworkRemoved(_))
    .Times(AtMost(3));

  EXPECT_CALL(allocator, slaveRemoved(_))
    .Times(AtMost(4));

  driver1.stop();
  driver2.stop();
  driver3.stop();

  cluster.shutdown();
}


template <typename T>
class AllocatorTest : public MesosClusterTest
{
protected:
  virtual void SetUp()
  {
    MesosClusterTest::SetUp();
    a = new Allocator(&allocator);
  }


  virtual void TearDown()
  {
    delete a;
    MesosClusterTest::TearDown();
  }

  MockAllocatorProcess<T> allocator;
  Allocator* a;
};


// Causes all TYPED_TEST(AllocatorTest, ...) to be run for
// each of the specified Allocator classes.
TYPED_TEST_CASE(AllocatorTest, AllocatorTypes);


// Checks that in a cluster with one slave and one framework, all of
// the slave's resources are offered to the framework.
TYPED_TEST(AllocatorTest, MockAllocator)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags = this->cluster.slaves.flags;
  flags.resources = Option<string>("cpus:2;mem:1024;disk:0");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);

  // The framework should be offered all of the resources on the slave
  // since it is the only framework in the cluster.
  EXPECT_THAT(offers.get(), OfferEq(2, 1024));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillRepeatedly(DoDefault());

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver.stop();

  AWAIT_READY(frameworkRemoved);

  Future<Nothing> slaveRemoved;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(FutureSatisfy(&slaveRemoved));

  this->cluster.slaves.shutdown();

  AWAIT_READY(slaveRemoved);

  this->cluster.masters.shutdown();
}


// Checks that when a task is launched with fewer resources than what
// the offer was for, the resources that are returned unused are
// reoffered appropriately.
TYPED_TEST(AllocatorTest, ResourcesUnused)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags1 = this->cluster.slaves.flags;
  flags1.resources = Option<string>("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->cluster.slaves.start(flags1, &isolator);
  ASSERT_SOME(slave1);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  Future<Nothing> resourcesUnused;
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesUnused(&this->allocator),
                    FutureSatisfy(&resourcesUnused)));

  EXPECT_CALL(sched1, registered(_, _, _));

  // The first offer will contain all of the slave's resources, since
  // this is the only framework running so far. Launch a task that
  // uses less than that to leave some resources unused.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(LaunchTasks(1, 1, 512))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  AWAIT_READY(resourcesUnused);

  AWAIT_READY(launchTask);

  FrameworkInfo info2;
  info2.set_user("user2");
  info2.set_name("framework2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, info2, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  driver2.start();

  AWAIT_READY(offers);

  // framework2 will be offered all of the resources on the slave not
  // being used by the task that was launched.
  EXPECT_THAT(offers.get(), OfferEq(1, 512));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .Times(2);

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(DoDefault())
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver2.stop();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();
}


// Tests the situation where a frameworkRemoved call is dispatched
// while we're doing an allocation to that framework, so that
// resourcesRecovered is called for an already removed framework.
TYPED_TEST(AllocatorTest, OutOfOrderDispatch)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator);
  ASSERT_SOME(master);

  TestingIsolator isolator;
  slave::Flags flags1 = this->cluster.slaves.flags;
  flags1.resources = Option<string>("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->cluster.slaves.start(flags1, &isolator);
  ASSERT_SOME(slave1);

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_name("framework1");
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get());

  FrameworkID frameworkId1;
  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo1), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
                    SaveArg<0>(&frameworkId1)));

  EXPECT_CALL(sched1, registered(_, _, _));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver1.start();

  AWAIT_READY(offers1);

  // framework1 will be offered all of the slave's resources, since
  // it is the only framework running right now.
  EXPECT_THAT(offers1.get(), OfferEq(2, 1024));

  FrameworkID frameworkId;
  SlaveID slaveId;
  Resources savedResources;
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    // "Catches" the resourcesRecovered call from the master, so
    // that it doesn't get processed until we redispatch it after
    // the frameworkRemoved trigger.
    .WillOnce(DoAll(SaveArg<0>(&frameworkId),
                    SaveArg<1>(&slaveId),
                    SaveArg<2>(&savedResources)));

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(frameworkId1)))
    .WillOnce(DoAll(InvokeFrameworkRemoved(&this->allocator),
                    FutureSatisfy(&frameworkRemoved)));

  driver1.stop();
  driver1.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillOnce(DoDefault());

  // Re-dispatch the resourcesRecovered call which we "caught"
  // earlier now that the framework has been removed, to test
  // that recovering resources from a removed framework works.
  this->a->resourcesRecovered(frameworkId, slaveId, savedResources);

  FrameworkInfo frameworkInfo2;
  frameworkInfo2.set_user("user2");
  frameworkInfo2.set_name("framework2");
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, frameworkInfo2, master.get());

  FrameworkID frameworkId2;
  EXPECT_CALL(this->allocator, frameworkAdded(_, Eq(frameworkInfo2), _))
    .WillOnce(DoAll(InvokeFrameworkAdded(&this->allocator),
                    SaveArg<0>(&frameworkId2)));

  EXPECT_CALL(sched2, registered(_, _, _));

  Future<vector<Offer> > offers2;
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillOnce(FutureArg<1>(&offers2));

  driver2.start();

  AWAIT_READY(offers2);

  // framework2 will be offered all of the slave's resources, since
  // it is the only framework running right now.
  EXPECT_THAT(offers2.get(), OfferEq(2, 1024));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved2;
  EXPECT_CALL(this->allocator, frameworkRemoved(Eq(frameworkId2)))
    .WillOnce(FutureSatisfy(&frameworkRemoved2));

  driver2.stop();
  driver2.join();

  AWAIT_READY(frameworkRemoved2);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();
}


// Checks that if a framework launches a task and then fails over to a
// new scheduler, the task's resources are not reoffered as long as it
// is running.
TYPED_TEST(AllocatorTest, SchedulerFailover)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags = this->cluster.slaves.flags;
  flags.resources = Option<string>("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo1;
  frameworkInfo1.set_name("framework1");
  frameworkInfo1.set_user("user1");
  frameworkInfo1.set_failover_timeout(.1);
  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, frameworkInfo1, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  // We don't filter the unused resources to make sure that
  // they get offered to the framework as soon as it fails over.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(InvokeUnusedWithFilters(&this->allocator, 0));

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers1;
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillOnce(DoAll(LaunchTasks(1, 1, 256),
                    FutureArg<1>(&offers1)))
    .WillRepeatedly(DeclineOffers());

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask));

  driver1.start();

  AWAIT_READY(offers1);

  // Initially, all cluster resources are avaliable.
  EXPECT_THAT(offers1.get(), OfferEq(3, 1024));

  // Ensures that the task has been completely launched
  // before we have the framework fail over.
  AWAIT_READY(launchTask);

  // When we shut down the first framework, we don't want it to tell
  // the master it's shutting down so that the master will wait to see
  // if it fails over.
  DROP_MESSAGES(Eq(UnregisterFrameworkMessage().GetTypeName()), _, _);

  Future<Nothing> frameworkDeactivated;
  EXPECT_CALL(this->allocator, frameworkDeactivated(_))
    .WillOnce(DoAll(InvokeFrameworkDeactivated(&this->allocator),
                    FutureSatisfy(&frameworkDeactivated)));

  driver1.stop();

  AWAIT_READY(frameworkDeactivated);

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);
  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler.
  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, framework2, master.get());

  EXPECT_CALL(this->allocator, frameworkActivated(_, _));

  EXPECT_CALL(sched2, registered(_, frameworkId, _));

  // Even though the scheduler failed over, the 1 cpu, 512 mem
  // task that it launched earlier should still be running, so
  // only 2 cpus and 768 mem are available.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers2));

  driver2.start();

  AWAIT_READY(resourceOffers2);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver2.stop();
  driver2.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();
}


// Checks that if a framework launches a task and then the framework
// is killed, the tasks resources are returned and reoffered correctly.
TYPED_TEST(AllocatorTest, FrameworkExited)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  master::Flags masterFlags = this->cluster.masters.flags;
  masterFlags.allocation_interval = Duration::parse("50ms").get();
  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(2);

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureSatisfy(&launchTask))
    .WillOnce(DoDefault());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(2));

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags = this->cluster.slaves.flags;
  flags.resources = Option<string>("cpus:3;mem:1024");

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  Future<Nothing> resourcesUnused;
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(DoAll(InvokeResourcesUnused(&this->allocator),
                    FutureSatisfy(&resourcesUnused)));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time the framework is offered resources,
  // all of the cluster's resources should be avaliable.
  Future<Nothing> resourcesOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
                    FutureSatisfy(&resourcesOffers1)));

  driver1.start();

  AWAIT_READY(resourcesOffers1);

  AWAIT_READY(resourcesUnused);

  // Ensures that framework 1's task is completely launched
  // before we kill the framework to test if its resources
  // are recovered correctly.
  AWAIT_READY(launchTask);

  MockScheduler sched2;
  MesosSchedulerDriver driver2(&sched2, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _));

  EXPECT_CALL(sched2, registered(_, _, _));

  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // The first time sched2 gets an offer, framework 1 has a
  // task running with 2 cpus and 512 mem, leaving 1 cpu and 512 mem.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(DoAll(LaunchTasks(1, 1, 256),
                    FutureSatisfy(&resourceOffers2)));

  driver2.start();

  AWAIT_READY(resourceOffers2);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  EXPECT_CALL(this->allocator, frameworkRemoved(_));

  // After we kill framework 1, all of it's resources should
  // have been returned, but framework 2 should still have a
  // task with 1 cpu and 256 mem, leaving 2 cpus and 768 mem.
  Future<Nothing> resourceOffers3;
  EXPECT_CALL(sched2, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers3));

  driver1.stop();
  driver1.join();

  AWAIT_READY(resourceOffers3);

  // Shut everything down.
  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver2.stop();
  driver2.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();
}


// Checks that if a framework launches a task and then the slave the
// task was running on gets killed, the task's resources are properly
// recovered and, along with the rest of the resources from the killed
// slave, never offered again.
TYPED_TEST(AllocatorTest, SlaveLost)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags1 = this->cluster.slaves.flags;
  flags1.resources = Option<string>("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->cluster.slaves.start(flags1, &isolator);
  ASSERT_SOME(slave1);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  Future<vector<Offer> > resourceOffers1;
  // Initially, all of slave1's resources are avaliable.
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
                    FutureArg<1>(&resourceOffers1)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  driver1.start();

  AWAIT_READY(resourceOffers1);

  EXPECT_THAT(resourceOffers1.get(), OfferEq(2, 1024));

  // Ensures the task is completely launched before we
  // kill the slave, to test that the task's resources
  // are recovered correctly (i.e. never reallocated
  // since the slave is killed)
  AWAIT_READY(launchTask);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  Future<Nothing> slaveRemoved1;
  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .WillOnce(DoAll(InvokeSlaveRemoved(&this->allocator),
                    FutureSatisfy(&slaveRemoved1)));

  Future<Nothing> shutdownCall;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdownCall));

  EXPECT_CALL(sched1, slaveLost(_, _));

  this->cluster.slaves.shutdown();

  AWAIT_READY(slaveRemoved1);

  AWAIT_READY(shutdownCall);

  MockExecutor exec2;
  TestingIsolator isolator2(DEFAULT_EXECUTOR_ID, &exec2);
  slave::Flags flags2 = this->cluster.slaves.flags;
  flags2.resources = Option<string>("cpus:3;mem:256");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  // Eventually after slave2 is launched, we should get
  // an offer that contains all of slave2's resources
  // and none of slave1's resources.
  Future<vector<Offer> > resourceOffers2;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 256)))
    .WillOnce(FutureArg<1>(&resourceOffers2));

  Try<PID<Slave> > slave2 = this->cluster.slaves.start(flags2, &isolator2);
  ASSERT_SOME(slave2);

  AWAIT_READY(resourceOffers2);

  EXPECT_THAT(resourceOffers2.get(), OfferEq(3, 256));

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();
}


// Checks that if a slave is added after some allocations have already
// occurred, its resources are added to the available pool of
// resources and offered appropriately.
TYPED_TEST(AllocatorTest, SlaveAdded)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  master::Flags masterFlags = this->cluster.masters.flags;
  masterFlags.allocation_interval = Duration::parse("50ms").get();
  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags1 = this->cluster.slaves.flags;
  flags1.resources = Option<string>("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave1 = this->cluster.slaves.start(flags1, &isolator);
  ASSERT_SOME(slave1);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  // We filter the first time so that the unused resources
  // on slave1 from the task launch won't get reoffered
  // immediately and will get combined with slave2's
  // resources for a single offer.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillOnce(InvokeUnusedWithFilters(&this->allocator, .1))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 0));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of slave1's resources are avaliable.
  Future<Nothing> resourceOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(1, 2, 512),
                    FutureSatisfy(&resourceOffers1)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)));

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  driver1.start();

  AWAIT_READY(resourceOffers1);

  AWAIT_READY(launchTask);

  slave::Flags flags2 = this->cluster.slaves.flags;
  flags2.resources = Option<string>("cpus:4;mem:2048");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  // After slave2 launches, all of its resources are
  // combined with the resources on slave1 that the
  // task isn't using.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(5, 2560)))
    .WillOnce(FutureSatisfy(&resourceOffers2));

  Try<PID<Slave> > slave2 = this->cluster.slaves.start(flags2, &isolator);
  ASSERT_SOME(slave2);

  AWAIT_READY(resourceOffers2);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(2));

  this->cluster.shutdown();
}


// Checks that if a task is launched and then finishes normally, its
// resources are recovered and reoffered correctly.
TYPED_TEST(AllocatorTest, TaskFinished)
{
  EXPECT_CALL(this->allocator, initialize(_, _));

  master::Flags masterFlags = this->cluster.masters.flags;
  masterFlags.allocation_interval = Duration::parse("50ms").get();
  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags = this->cluster.slaves.flags;
  flags.resources = Option<string>("cpus:3;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  // We don't filter because we want to see the unused resources
  // from the task launch get reoffered to us.
  EXPECT_CALL(this->allocator, resourcesUnused(_, _, _, _))
    .WillRepeatedly(InvokeUnusedWithFilters(&this->allocator, 0));

  EXPECT_CALL(sched1, registered(_, _, _));

  EXPECT_CALL(sched1, statusUpdate(_, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers());

  // Initially, all of the slave's resources.
  Future<Nothing> resourceOffers1;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(3, 1024)))
    .WillOnce(DoAll(LaunchTasks(2, 1, 256),
                    FutureSatisfy(&resourceOffers1)));

  // After the tasks are launched.
  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(1, 512)))
    .WillOnce(DoAll(DeclineOffers(),
                    FutureSatisfy(&resourceOffers2)));

  EXPECT_CALL(exec, registered(_, _, _, _));

  ExecutorDriver* execDriver;
  TaskInfo taskInfo;
  Future<Nothing> launchTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(DoAll(SaveArg<0>(&execDriver),
                    SaveArg<1>(&taskInfo),
                    SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureSatisfy(&launchTask)))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  EXPECT_CALL(isolator, resourcesChanged(_, _, _))
    .WillRepeatedly(DoDefault());

  driver1.start();

  AWAIT_READY(resourceOffers1);

  AWAIT_READY(launchTask);

  AWAIT_READY(resourceOffers2);

  TaskStatus status;
  status.mutable_task_id()->MergeFrom(taskInfo.task_id());
  status.set_state(TASK_FINISHED);

  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _));

  // After the first task gets killed.
  Future<Nothing> resourceOffers3;
  EXPECT_CALL(sched1, resourceOffers(_, OfferEq(2, 768)))
    .WillOnce(FutureSatisfy(&resourceOffers3));

  execDriver->sendStatusUpdate(status);

  AWAIT_READY(resourceOffers3);

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver1.stop();
  driver1.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();
}


// Checks that a slave that is not whitelisted will not have its
// resources get offered, and that if the whitelist is updated so
// that it is whitelisted, its resources will then be offered.
TYPED_TEST(AllocatorTest, WhitelistSlave)
{
  // Create a dummy whitelist, so that no resources will get allocated.
  string hosts = "dummy-slave";
  string path = "whitelist.txt";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags masterFlags = this->cluster.masters.flags;
  masterFlags.whitelist = "file://" + path; // TODO(benh): Put in /tmp.

  EXPECT_CALL(this->allocator, initialize(_, _));

  Future<Nothing> updateWhitelist1;
  EXPECT_CALL(this->allocator, updateWhitelist(_))
    .WillOnce(DoAll(InvokeUpdateWhitelist(&this->allocator),
                    FutureSatisfy(&updateWhitelist1)));

  Try<PID<Master> > master = this->cluster.masters.start(&this->allocator, masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);
  slave::Flags flags = this->cluster.slaves.flags;
  flags.resources = Option<string>("cpus:2;mem:1024");

  EXPECT_CALL(this->allocator, slaveAdded(_, _, _));

  Try<PID<Slave> > slave = this->cluster.slaves.start(flags, &isolator);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(this->allocator, frameworkAdded(_, _, _));

  EXPECT_CALL(sched, registered(_, _, _));

  // Once the slave gets whitelisted, all of its resources should be
  // offered to the one framework running.
  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(_, OfferEq(2, 1024)))
    .WillOnce(FutureSatisfy(&resourceOffers));

  // Make sure the allocator has been given the original, empty
  // whitelist.
  AWAIT_READY(updateWhitelist1);

  driver.start();

  // Give the allocator some time to confirm that it doesn't
  // make an allocation.
  Clock::pause();
  Clock::advance(Seconds(1));
  Clock::settle();

  EXPECT_FALSE(resourceOffers.isReady());

  // Update the whitelist to include the slave, so that
  // the allocator will start making allocations.
  Try<string> hostname = os::hostname();
  ASSERT_SOME(hostname);
  hosts = hostname.get() + "\n" + "dummy-slave";

  EXPECT_CALL(this->allocator, updateWhitelist(_));

  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  // Give the WhitelistWatcher some time to notice that
  // the whitelist has changed.
  while (resourceOffers.isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }
  Clock::resume();

  // Shut everything down.
  EXPECT_CALL(this->allocator, resourcesRecovered(_, _, _))
    .WillRepeatedly(DoDefault());

  EXPECT_CALL(this->allocator, frameworkDeactivated(_));

  Future<Nothing> frameworkRemoved;
  EXPECT_CALL(this->allocator, frameworkRemoved(_))
    .WillOnce(FutureSatisfy(&frameworkRemoved));

  driver.stop();
  driver.join();

  AWAIT_READY(frameworkRemoved);

  EXPECT_CALL(this->allocator, slaveRemoved(_))
    .Times(AtMost(1));

  this->cluster.shutdown();

  os::rm(path);
}
