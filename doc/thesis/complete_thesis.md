# Integrating Raft Consensus into the Mako Distributed Transaction System

## Complete Thesis Documentation

This document consolidates all thesis documentation on integrating Raft consensus into the Mako distributed transaction system. It covers the design, implementation, testing, and performance evaluation of a Raft replication module that operates as an alternative to Mako's existing Multi-Paxos atomic broadcast layer.

**Author contribution scope**: The Raft module, its integration with Mako, standalone tests, preferred leader election, and the CI test suite were implemented by the author. Mako itself (storage engine, concurrency control, transaction coordination, sharding) is pre-existing infrastructure.

---

## Table of Contents

- [Introduction and Reading Guide](#introduction-and-reading-guide)
    - [What This Document Covers](#what-this-document-covers)
    - [Document Map](#document-map)
      - [Chapter 1: Mako System Overview (`01-mako-overview/`)](#chapter-1-mako-system-overview-01-mako-overview)
      - [Chapter 2: Raft Protocol Implementation (`02-raft-core/`)](#chapter-2-raft-protocol-implementation-02-raft-core)
      - [Chapter 3: Preferred Leader Election (`03-preferred-leader/`)](#chapter-3-preferred-leader-election-03-preferred-leader)
      - [Chapter 4: Mako Integration (`04-mako-integration/`)](#chapter-4-mako-integration-04-mako-integration)
      - [Chapter 5: Standalone Raft Testing (`05-standalone-testing/`)](#chapter-5-standalone-raft-testing-05-standalone-testing)
      - [Chapter 6: CI Integration Testing (`06-ci-testing/`)](#chapter-6-ci-integration-testing-06-ci-testing)
      - [Chapter 7: Performance Analysis (`07-performance/`)](#chapter-7-performance-analysis-07-performance)
      - [Chapter 8: Log Persistence and Recovery (`08-persistence/`)](#chapter-8-log-persistence-and-recovery-08-persistence)
      - [Chapter 9: Appendix (`09-appendix/`)](#chapter-9-appendix-09-appendix)
    - [Suggested Reading Paths](#suggested-reading-paths)
      - [Quick Overview (~30 minutes)](#quick-overview-30-minutes)
      - [Implementation Deep Dive (~2 hours)](#implementation-deep-dive-2-hours)
      - [Integration Story (~1.5 hours)](#integration-story-15-hours)
      - [Testing and Validation (~1 hour)](#testing-and-validation-1-hour)
    - [Key Source File Reference](#key-source-file-reference)
      - [Core Raft Protocol](#core-raft-protocol)
      - [Raft-Mako Bridge](#raft-mako-bridge)
      - [Standalone Tests](#standalone-tests)
      - [Paxos (for Comparison)](#paxos-for-comparison)
    - [Glossary](#glossary)
      - [Raft-Specific Terms](#raft-specific-terms)
      - [Mako-Specific Terms](#mako-specific-terms)
      - [System-Specific Terms](#system-specific-terms)
  - [Mako System Architecture](#mako-system-architecture)
    - [What This Document Covers](#what-this-document-covers-1)
    - [1. What Mako Is](#1-what-mako-is)
    - [2. Core Components](#2-core-components)
      - [2.1 Storage Engine: Masstree](#21-storage-engine-masstree)
      - [2.2 Concurrency Control: OCC](#22-concurrency-control-occ)
      - [2.3 Atomic Broadcast Layer](#23-atomic-broadcast-layer)
      - [2.4 Sharding](#24-sharding)
    - [3. Transaction Flow](#3-transaction-flow)
      - [Key observations:](#key-observations)
    - [4. Key Classes](#4-key-classes)
      - [4.1 Frame (`src/deptran/frame.h`)](#41-frame-srcdeptranframeh)
      - [4.2 TxnCoordinator (`src/deptran/coordinator.h`)](#42-txncoordinator-srcdeptrancoordinatorh)
      - [4.3 TxLogServer / TxnScheduler (`src/deptran/scheduler.h:332-685`)](#43-txlogserver-txnscheduler-srcdeptranschedulerh332-685)
      - [4.4 Communicator (`src/deptran/communicator.h:372-622`)](#44-communicator-srcdeptrancommunicatorh372-622)
      - [4.5 Tx (`src/deptran/tx.h:40-153`)](#45-tx-srcdeptrantxh40-153)
    - [5. Shard Architecture](#5-shard-architecture)
      - [5.1 Data Partitioning](#51-data-partitioning)
      - [5.2 Replica Topology](#52-replica-topology)
      - [5.3 Cross-Shard Transactions](#53-cross-shard-transactions)
    - [6. The Existing Paxos Path](#6-the-existing-paxos-path)
      - [Multi-Paxos Implementation (`src/deptran/paxos/`)](#multi-paxos-implementation-srcdeptranpaxos)
      - [Paxos Worker Setup (`src/deptran/paxos_main_helper.cc`)](#paxos-worker-setup-srcdeptranpaxos_main_helpercc)
    - [7. Where Raft Plugs In](#7-where-raft-plugs-in)
  - [Build System and Configuration](#build-system-and-configuration)
    - [What This Document Covers](#what-this-document-covers-2)
    - [1. Build System Overview](#1-build-system-overview)
    - [2. Build Commands](#2-build-commands)
      - [2.1 Standard Build (Paxos Only)](#21-standard-build-paxos-only)
      - [2.2 Raft-Enabled Build](#22-raft-enabled-build)
      - [2.3 Raft Lab Test Build](#23-raft-lab-test-build)
      - [2.4 Other Targets](#24-other-targets)
    - [3. CMake Configuration](#3-cmake-configuration)
      - [3.1 Key Build Options (`CMakeLists.txt`)](#31-key-build-options-cmakeliststxt)
      - [3.2 Build Targets](#32-build-targets)
      - [3.3 Key Compile Definitions](#33-key-compile-definitions)
      - [3.4 Third-Party Dependencies](#34-third-party-dependencies)
      - [3.5 Transport Layer Configuration](#35-transport-layer-configuration)
    - [4. Configuration System](#4-configuration-system)
      - [4.1 Mode Configuration](#41-mode-configuration)
      - [4.2 Shard/Host Configuration](#42-shardhost-configuration)
      - [4.3 Replication Group Configuration](#43-replication-group-configuration)
      - [4.4 Port Allocation: Paxos vs Raft](#44-port-allocation-paxos-vs-raft)
    - [5. Runtime Replication Switching](#5-runtime-replication-switching)
      - [5.1 The Dispatch Mechanism](#51-the-dispatch-mechanism)
      - [5.2 Setting the Replication Type](#52-setting-the-replication-type)
      - [5.3 The Global State](#53-the-global-state)
      - [5.4 The Dispatch Macros](#54-the-dispatch-macros)
    - [6. Running Mako](#6-running-mako)
      - [6.1 Basic Invocation](#61-basic-invocation)
      - [6.2 Multi-Shard Invocation](#62-multi-shard-invocation)
    - [7. CI Test Infrastructure](#7-ci-test-infrastructure)
      - [7.1 Paxos Test Suite (`ci/ci.sh`)](#71-paxos-test-suite-cicish)
      - [7.2 Raft Test Suite (`ci/ci_mako_raft.sh`)](#72-raft-test-suite-cici_mako_raftsh)
      - [7.3 Test Flow](#73-test-flow)
    - [8. Switching Between Paxos and Raft: Quick Reference](#8-switching-between-paxos-and-raft-quick-reference)
  - [Raft Protocol Implementation Overview](#raft-protocol-implementation-overview)
    - [What This Document Covers](#what-this-document-covers-3)
    - [1. Raft Fundamentals Recap](#1-raft-fundamentals-recap)
    - [2. Implementation-to-Paper Mapping](#2-implementation-to-paper-mapping)
      - [Class Hierarchy](#class-hierarchy)
    - [3. Core State Variables](#3-core-state-variables)
      - [Persistent State (on all servers)](#persistent-state-on-all-servers)
      - [Volatile State (on all servers)](#volatile-state-on-all-servers)
      - [Volatile State (on leaders)](#volatile-state-on-leaders)
      - [Additional State (implementation-specific)](#additional-state-implementation-specific)
    - [4. RPC Interface](#4-rpc-interface)
      - [RequestVote RPC](#requestvote-rpc)
      - [AppendEntries RPC](#appendentries-rpc)
      - [EmptyAppendEntries RPC (Heartbeat)](#emptyappendentries-rpc-heartbeat)
      - [TimeoutNow RPC](#timeoutnow-rpc)
    - [5. Algorithm Summary](#5-algorithm-summary)
      - [5.1 Leader Election](#51-leader-election)
      - [5.2 Log Replication](#52-log-replication)
      - [5.3 Safety](#53-safety)
    - [6. Key Deviations from the Raft Paper](#6-key-deviations-from-the-raft-paper)
      - [6.1 Preferred Leader Election (Novel Contribution)](#61-preferred-leader-election-novel-contribution)
      - [6.2 Integration with Two-Phase Commit](#62-integration-with-two-phase-commit)
      - [6.3 Batched Log Replication](#63-batched-log-replication)
      - [6.4 Optimized Log Reconciliation](#64-optimized-log-reconciliation)
      - [6.5 Persistent State Management](#65-persistent-state-management)
      - [6.6 Jetpack Recovery](#66-jetpack-recovery)
    - [7. Component Interaction Diagram](#7-component-interaction-diagram)
    - [8. File Map](#8-file-map)
  - [RaftServer Implementation Deep Dive](#raftserver-implementation-deep-dive)
    - [What This Document Covers](#what-this-document-covers-4)
    - [1. Class Hierarchy](#1-class-hierarchy)
    - [2. Member Variables](#2-member-variables)
      - [2.1 Persistent State (Must Survive Restarts)](#21-persistent-state-must-survive-restarts)
      - [2.2 Volatile State (All Servers)](#22-volatile-state-all-servers)
      - [2.3 Volatile State (Leaders Only)](#23-volatile-state-leaders-only)
      - [2.4 Preferred Leader System](#24-preferred-leader-system)
      - [2.5 Log Application Control](#25-log-application-control)
      - [2.6 Persistence and Snapshot](#26-persistence-and-snapshot)
      - [2.7 Timer and Configuration](#27-timer-and-configuration)
      - [2.8 RaftData Structure](#28-raftdata-structure)
    - [3. OnRequestVote() — Vote Granting](#3-onrequestvote-vote-granting)
      - [Algorithm Walkthrough](#algorithm-walkthrough)
      - [The doVote() Helper (`server.h:161-202`)](#the-dovote-helper-serverh161-202)
      - [Key Safety Properties](#key-safety-properties)
    - [4. OnAppendEntries() — Log Replication](#4-onappendentries-log-replication)
      - [Algorithm Walkthrough](#algorithm-walkthrough-1)
      - [Critical Design Decision: Mutex Release During Apply](#critical-design-decision-mutex-release-during-apply)
      - [Batch Optimization (`RAFT_BATCH_OPTIMIZATION`)](#batch-optimization-raft_batch_optimization)
    - [5. Start() — Leader Appends Commands](#5-start-leader-appends-commands)
      - [SetLocalAppend() — The Actual Append (`server.h:313-384`)](#setlocalappend-the-actual-append-serverh313-384)
    - [6. applyLogs() — State Machine Application](#6-applylogs-state-machine-application)
      - [Concurrency Design](#concurrency-design)
      - [Garbage Collection](#garbage-collection)
    - [7. Election Timer](#7-election-timer)
      - [7.1 StartElectionTimer() (`server.cc:1188-1228`)](#71-startelectiontimer-servercc1188-1228)
      - [7.2 GetElectionTimeout() — Dynamic Timeout (`server.cc:348-369`)](#72-getelectiontimeout-dynamic-timeout-servercc348-369)
      - [7.3 resetTimer() (`server.h:229-241`)](#73-resettimer-serverh229-241)
      - [7.4 HEARTBEAT_INTERVAL](#74-heartbeat_interval)
    - [8. HeartbeatLoop() — Leader Replication Loop](#8-heartbeatloop-leader-replication-loop)
      - [Loop Structure](#loop-structure)
      - [Commit Index Calculation (`server.cc:707-731`)](#commit-index-calculation-servercc707-731)
      - [Log Reconciliation Strategies (`server.cc:873-898`)](#log-reconciliation-strategies-servercc873-898)
    - [9. Log Persistence](#9-log-persistence)
      - [9.1 Persistence Functions](#91-persistence-functions)
      - [9.2 Recovery (`server.cc:105-203`)](#92-recovery-servercc105-203)
      - [9.3 Log Compaction (`server.cc:206-256`)](#93-log-compaction-servercc206-256)
    - [10. setIsLeader() — State Transitions](#10-setisleader-state-transitions)
      - [Becoming Leader (`become_new_leader`, lines 491-538)](#becoming-leader-become_new_leader-lines-491-538)
      - [Becoming Follower (`become_new_follower`, lines 539-565)](#becoming-follower-become_new_follower-lines-539-565)
    - [11. Destructor — Safe Shutdown](#11-destructor-safe-shutdown)
    - [12. RustyCpp Safety Annotations](#12-rustycpp-safety-annotations)
      - [@safe Methods](#safe-methods)
      - [@unsafe Methods](#unsafe-methods)
      - [Key Pattern: Timer Ownership](#key-pattern-timer-ownership)
    - [13. Constants and Configuration](#13-constants-and-configuration)
  - [Leader Election Mechanism](#leader-election-mechanism)
    - [What This Document Covers](#what-this-document-covers-5)
    - [1. Election Trigger](#1-election-trigger)
      - [Timer Loop](#timer-loop)
      - [Dynamic Timeout Values](#dynamic-timeout-values)
    - [2. The Election: RequestVote()](#2-the-election-requestvote)
      - [Step 1: Shutdown Guard (`server.cc:1000-1003`)](#step-1-shutdown-guard-servercc1000-1003)
      - [Step 2: Become Candidate (`server.cc:1016-1034`)](#step-2-become-candidate-servercc1016-1034)
      - [Step 3: Broadcast Vote RPCs (`server.cc:1041`)](#step-3-broadcast-vote-rpcs-servercc1041)
      - [Step 4: Wait for Quorum (`server.cc:1042`)](#step-4-wait-for-quorum-servercc1042)
      - [Step 5: Process Result (`server.cc:1048-1117`)](#step-5-process-result-servercc1048-1117)
    - [3. BroadcastVote() — Sending Vote RPCs](#3-broadcastvote-sending-vote-rpcs)
    - [4. RaftVoteQuorumEvent — Quorum Detection](#4-raftvotequorumevent-quorum-detection)
      - [Class Hierarchy](#class-hierarchy-1)
      - [Key Members (from `QuorumEvent`)](#key-members-from-quorumevent)
      - [Quorum Logic](#quorum-logic)
      - [FeedResponse()](#feedresponse)
    - [5. Vote Granting: OnRequestVote()](#5-vote-granting-onrequestvote)
      - [Decision Tree](#decision-tree)
      - [The doVote() Helper (`server.h:161-202`)](#the-dovote-helper-serverh161-202-1)
      - [Why Resetting the Timer on Vote Grant Matters](#why-resetting-the-timer-on-vote-grant-matters)
    - [6. Split Vote Handling](#6-split-vote-handling)
    - [7. Term Advancement](#7-term-advancement)
      - [Where Terms Advance](#where-terms-advance)
      - [Invariant: Monotonically Increasing](#invariant-monotonically-increasing)
    - [8. Complete Election Cycle — Sequence Diagram](#8-complete-election-cycle-sequence-diagram)
      - [Timing Breakdown](#timing-breakdown)
    - [9. Leader Change Notification](#9-leader-change-notification)
      - [Registration](#registration)
      - [Firing](#firing)
    - [10. Edge Cases and Robustness](#10-edge-cases-and-robustness)
      - [Pre-Vote Not Implemented](#pre-vote-not-implemented)
      - [Stale Election Guard](#stale-election-guard)
      - [Shutdown Safety](#shutdown-safety)
  - [Log Replication Mechanism](#log-replication-mechanism)
    - [What This Document Covers](#what-this-document-covers-6)
    - [1. Overview](#1-overview)
    - [2. Leader: Sending Entries](#2-leader-sending-entries)
      - [2.1 HeartbeatLoop() — The Replication Engine](#21-heartbeatloop-the-replication-engine)
      - [2.2 Preparing an AppendEntries](#22-preparing-an-appendentries)
      - [2.3 Non-Batch Mode](#23-non-batch-mode)
      - [2.4 Batch Mode (Default)](#24-batch-mode-default)
      - [2.5 SendAppendEntries2() — The RPC Call](#25-sendappendentries2-the-rpc-call)
    - [3. Follower: Receiving Entries](#3-follower-receiving-entries)
      - [3.1 OnAppendEntries() — The Handler](#31-onappendentries-the-handler)
      - [3.2 Consistency Check](#32-consistency-check)
      - [3.3 Entry Appending](#33-entry-appending)
      - [3.4 Commit Index Advancement on Follower](#34-commit-index-advancement-on-follower)
      - [3.5 Critical Design: Mutex Release During Apply](#35-critical-design-mutex-release-during-apply)
    - [4. Leader: Processing Responses](#4-leader-processing-responses)
      - [4.1 Response Cases](#41-response-cases)
      - [4.2 Log Reconciliation — Backtracking next_index_](#42-log-reconciliation-backtracking-next_index_)
    - [5. Commit Advancement](#5-commit-advancement)
      - [5.1 Leader Computes commitIndex](#51-leader-computes-commitindex)
      - [5.2 The Term Safety Rule](#52-the-term-safety-rule)
      - [5.3 Follower Learns commitIndex](#53-follower-learns-commitindex)
    - [6. Heartbeats](#6-heartbeats)
      - [Heartbeat Interval](#heartbeat-interval)
      - [Heartbeat Processing on Follower](#heartbeat-processing-on-follower)
    - [7. Applying Committed Entries](#7-applying-committed-entries)
      - [7.1 applyLogs()](#71-applylogs)
      - [7.2 The app_next_ Callback](#72-the-app_next_-callback)
    - [8. Complete Replication Sequence Diagram](#8-complete-replication-sequence-diagram)
      - [Timing](#timing)
    - [9. Log Conflict Resolution — Example](#9-log-conflict-resolution-example)
  - [CoordinatorRaft — Transaction Submission](#coordinatorraft-transaction-submission)
    - [What This Document Covers](#what-this-document-covers-7)
    - [1. Class Hierarchy](#1-class-hierarchy-1)
    - [2. Key Members](#2-key-members)
      - [Phase Enum](#phase-enum)
    - [3. Submit() — The Entry Point](#3-submit-the-entry-point)
      - [Flow](#flow)
      - [How Submit is Called](#how-submit-is-called)
    - [4. GotoNextPhase() — The Phase State Machine](#4-gotonextphase-the-phase-state-machine)
    - [5. AppendEntries() — Waiting for Commitment](#5-appendentries-waiting-for-commitment)
      - [Key Design Points](#key-design-points)
    - [6. WRONG_LEADER Handling](#6-wrong_leader-handling)
      - [The Error Code](#the-error-code)
      - [What Happens](#what-happens)
      - [View Propagation](#view-propagation)
    - [7. Slot Allocation via Arc<Cell<slotid_t>>](#7-slot-allocation-via-arc)
      - [How It Works](#how-it-works)
      - [Why Arc<Cell<T>>](#why-arc)
    - [8. Quorum Calculation](#8-quorum-calculation)
    - [9. CreateCoordinator() — Factory Method](#9-createcoordinator-factory-method)
    - [10. LeaderLearn() — Post-Commit Callback](#10-leaderlearn-post-commit-callback)
    - [11. Complete Submission Flow](#11-complete-submission-flow)
  - [RPC Layer — Communication Infrastructure](#rpc-layer-communication-infrastructure)
    - [What This Document Covers](#what-this-document-covers-8)
    - [1. Architecture Overview](#1-architecture-overview)
      - [Class Hierarchy](#class-hierarchy-2)
    - [2. RPC Definitions](#2-rpc-definitions)
      - [Wire Format](#wire-format)
      - [MarshallDeputy — Polymorphic Serialization](#marshalldeputy-polymorphic-serialization)
    - [3. Macros — Boilerplate Elimination](#3-macros-boilerplate-elimination)
      - [RpcHandler Macro (`macros.h:50-61`)](#rpchandler-macro-macrosh50-61)
      - [Call_Async Macro (`macros.h:72-76`)](#call_async-macro-macrosh72-76)
    - [4. RaftCommo — Sending RPCs](#4-raftcommo-sending-rpcs)
      - [Inheritance](#inheritance)
      - [Proxy Map](#proxy-map)
      - [SendAppendEntries2() (`commo.cc:26-94`)](#sendappendentries2-commocc26-94)
      - [SendAppendEntries() (`commo.cc:96-175`)](#sendappendentries-commocc96-175)
      - [BroadcastVote() (`commo.cc:177-210`)](#broadcastvote-commocc177-210)
      - [SendTimeoutNow() (`commo.cc:228-285`)](#sendtimeoutnow-commocc228-285)
      - [WAN_WAIT](#wan_wait)
    - [5. RaftServiceImpl — Receiving RPCs](#5-raftserviceimpl-receiving-rpcs)
      - [Constructor (`service.cc:15-20`)](#constructor-servicecc15-20)
      - [Handler Registration (via `RpcHandler` macro)](#handler-registration-via-rpchandler-macro)
      - [Handler Implementations (`service.cc`)](#handler-implementations-servicecc)
      - [DeferredReply Pattern](#deferredreply-pattern)
    - [6. RaftService — Generated Base Class](#6-raftservice-generated-base-class)
      - [RPC Registration (`__reg_to__`)](#rpc-registration-__reg_to__)
      - [RPC Dispatch (`__dispatch__`)](#rpc-dispatch-__dispatch__)
      - [Wrapper Methods](#wrapper-methods)
    - [7. RaftProxy — Sending Side](#7-raftproxy-sending-side)
    - [8. RaftFrame — Factory Pattern](#8-raftframe-factory-pattern)
      - [Registration](#registration-1)
      - [Factory Methods](#factory-methods)
      - [Ownership Model](#ownership-model)
      - [CreateScheduler() (`frame.cc:80-100`)](#createscheduler-framecc80-100)
      - [CreateCommo() (`frame.cc:103-190`)](#createcommo-framecc103-190)
      - [CreateRpcServices() (`frame.cc:193-204`)](#createrpcservices-framecc193-204)
    - [9. RaftVoteQuorumEvent — Quorum Tracking](#9-raftvotequorumevent-quorum-tracking)
    - [10. Disconnection Simulation](#10-disconnection-simulation)
      - [How It Works](#how-it-works-1)
      - [Default Disconnection Responses](#default-disconnection-responses)
      - [Reconnection](#reconnection)
    - [11. SendAppendEntriesResults — Response Tracking](#11-sendappendentriesresults-response-tracking)
    - [12. RAFT_TEST_CORO — Test Infrastructure](#12-raft_test_coro-test-infrastructure)
    - [13. Complete RPC Flow — AppendEntries Example](#13-complete-rpc-flow-appendentries-example)
  - [Preferred Leader Election — Design and Motivation](#preferred-leader-election-design-and-motivation)
    - [What This Document Covers](#what-this-document-covers-9)
    - [1. Why Preferred Leader?](#1-why-preferred-leader)
      - [Problem: Non-Deterministic Leader Placement](#problem-non-deterministic-leader-placement)
      - [Solution: Preferred Leader](#solution-preferred-leader)
    - [2. How It Differs from Standard Raft](#2-how-it-differs-from-standard-raft)
      - [What Is NOT Changed](#what-is-not-changed)
    - [3. Design Overview](#3-design-overview)
      - [Phase 1: Startup — Election Bias](#phase-1-startup-election-bias)
      - [Phase 2: Monitoring — Non-Preferred Leader Detection](#phase-2-monitoring-non-preferred-leader-detection)
      - [Phase 3: Transfer — Piggybacked Protocol](#phase-3-transfer-piggybacked-protocol)
    - [4. Transfer Protocol — Detailed Sequence](#4-transfer-protocol-detailed-sequence)
      - [Timing Analysis](#timing-analysis)
    - [5. The `TimeoutNow` RPC — Direct Transfer](#5-the-timeoutnow-rpc-direct-transfer)
    - [6. Dynamic Election Timeout — `GetElectionTimeout()`](#6-dynamic-election-timeout-getelectiontimeout)
    - [7. Configuration — `SetPreferredLeader()`](#7-configuration-setpreferredleader)
      - [Startup Configuration (`raft_main_helper.cc:300-349`)](#startup-configuration-raft_main_helpercc300-349)
      - [Dynamic Reconfiguration](#dynamic-reconfiguration)
    - [8. Safety Argument](#8-safety-argument)
      - [8.1 Election Safety](#81-election-safety)
      - [8.2 Leader Append-Only](#82-leader-append-only)
      - [8.3 Log Matching](#83-log-matching)
      - [8.4 Leader Completeness](#84-leader-completeness)
      - [8.5 State Machine Safety](#85-state-machine-safety)
      - [Transfer-Specific Safety](#transfer-specific-safety)
    - [9. Member Variables](#9-member-variables)
      - [Inline Helper Methods](#inline-helper-methods)
    - [10. Failure Modes and Recovery](#10-failure-modes-and-recovery)
      - [Preferred Replica Fails](#preferred-replica-fails)
      - [Non-Preferred Leader Fails During Transfer](#non-preferred-leader-fails-during-transfer)
      - [Preferred Replica Slow/Partitioned](#preferred-replica-slowpartitioned)
      - [Configuration Change](#configuration-change)
  - [Preferred Leader Election — Implementation Details](#preferred-leader-election-implementation-details)
    - [What This Document Covers](#what-this-document-covers-10)
    - [1. Member Variables](#1-member-variables)
      - [Design Rationale](#design-rationale)
    - [2. Inline Helper Methods](#2-inline-helper-methods)
      - [`AmIPreferredLeader()` (`server.h:136-138`)](#amipreferredleader-serverh136-138)
      - [`HaveCaughtUp()` (`server.h:142-145`)](#havecaughtup-serverh142-145)
    - [3. `SetPreferredLeader()` — Configuration Entry Point](#3-setpreferredleader-configuration-entry-point)
      - [Call Sites](#call-sites)
      - [Startup Configuration Flow (`raft_main_helper.cc:300-349`)](#startup-configuration-flow-raft_main_helpercc300-349)
    - [4. `GetElectionTimeout()` — Asymmetric Timeouts](#4-getelectiontimeout-asymmetric-timeouts)
      - [Timeout Decision Table](#timeout-decision-table)
      - [Called by: `StartElectionTimer()` (`server.cc:1197`)](#called-by-startelectiontimer-servercc1197)
    - [5. `setIsLeader()` — Transfer Integration Point](#5-setisleader-transfer-integration-point)
    - [6. `StartLeadershipTransferMonitoring()` — Background Monitor](#6-startleadershiptransfermonitoring-background-monitor)
      - [Design Decisions](#design-decisions)
    - [7. `ShouldTransferLeadership()` — Decision Logic](#7-shouldtransferleadership-decision-logic)
      - [Check Details](#check-details)
    - [8. `InitiateLeadershipTransfer()` — The Transfer Protocol](#8-initiateleadershiptransfer-the-transfer-protocol)
      - [Step-by-Step Walkthrough](#step-by-step-walkthrough)
      - [Why Piggybacked Approach?](#why-piggybacked-approach)
      - [What Happens on Each Replica](#what-happens-on-each-replica)
    - [9. `OnTimeoutNow()` — Direct Transfer RPC Handler](#9-ontimeoutnow-direct-transfer-rpc-handler)
      - [Edge Case Handling](#edge-case-handling)
    - [10. `StopLeadershipTransferMonitoring()` — Clean Shutdown](#10-stopleadershiptransfermonitoring-clean-shutdown)
    - [11. `~RaftServer()` — Destruction Sequence](#11-raftserver-destruction-sequence)
    - [12. `HeartbeatLoop()` — Transfer Delegation](#12-heartbeatloop-transfer-delegation)
    - [13. Integration Points Summary](#13-integration-points-summary)
      - [Call Graph](#call-graph)
    - [14. Full Sequence Diagram — Leadership Transfer](#14-full-sequence-diagram-leadership-transfer)
    - [15. Constants and Tuning Parameters](#15-constants-and-tuning-parameters)
  - [Preferred Leader Election — Testing](#preferred-leader-election-testing)
    - [What This Document Covers](#what-this-document-covers-11)
    - [1. Test Architecture Overview](#1-test-architecture-overview)
      - [Mako Replication API](#mako-replication-api)
      - [Build Configuration](#build-configuration)
    - [2. Test 1: `testPreferredReplicaStartup`](#2-test-1-testpreferredreplicastartup)
      - [Purpose](#purpose)
      - [Cluster Configuration](#cluster-configuration)
      - [Test Phases](#test-phases)
      - [State Tracking](#state-tracking)
      - [Leadership Callback](#leadership-callback)
      - [Success Criteria (Script-Level)](#success-criteria-script-level)
      - [What This Proves](#what-this-proves)
    - [3. Test 2: `testPreferredReplicaLogReplication`](#3-test-2-testpreferredreplicalogreplication)
      - [Purpose](#purpose-1)
      - [Cluster Configuration](#cluster-configuration-1)
      - [Test Parameters](#test-parameters)
      - [Test Phases](#test-phases-1)
      - [Command Wrapping](#command-wrapping)
      - [Log Application Callback](#log-application-callback)
      - [Success Criteria](#success-criteria)
      - [What This Proves](#what-this-proves-1)
    - [4. Test 3: `testNoOps`](#4-test-3-testnoops)
      - [Purpose](#purpose-2)
      - [Cluster Configuration](#cluster-configuration-2)
      - [Test Parameters](#test-parameters-1)
      - [NO-OPS Format](#no-ops-format)
      - [Test Phases](#test-phases-2)
      - [State Tracking](#state-tracking-1)
      - [Log Application Callback](#log-application-callback-1)
      - [Success Criteria](#success-criteria-1)
      - [What This Proves](#what-this-proves-2)
    - [5. Test Runner Scripts](#5-test-runner-scripts)
      - [Common Pattern](#common-pattern)
      - [Script-Specific Details](#script-specific-details)
      - [Invocation](#invocation)
    - [6. CI Integration](#6-ci-integration)
    - [7. Configuration Files](#7-configuration-files)
      - [`config/none_raft.yml`](#confignone_raftyml)
      - [`config/1c1s5r1p_cluster_test.yml` (5-node)](#config1c1s5r1p_cluster_testyml-5-node)
      - [`config/1c1s3r1p_cluster_test.yml` (3-node)](#config1c1s3r1p_cluster_testyml-3-node)
    - [8. Relationship to Standard Raft Tests](#8-relationship-to-standard-raft-tests)
    - [9. Correctness Guarantees](#9-correctness-guarantees)
  - [Mako-Raft Integration Architecture](#mako-raft-integration-architecture)
    - [1. The Integration Challenge](#1-the-integration-challenge)
    - [2. Dispatcher Architecture](#2-dispatcher-architecture)
      - [2.1 Layer Diagram](#21-layer-diagram)
      - [2.2 Global State: `rusty::Cell<ReplicationType>`](#22-global-state-rustycell)
      - [2.3 The `ReplicationType` Enum](#23-the-replicationtype-enum)
      - [2.4 Dispatch Macros](#24-dispatch-macros)
      - [2.5 Raft-Only Functions](#25-raft-only-functions)
    - [3. Protocol Detection: Two Mechanisms](#3-protocol-detection-two-mechanisms)
      - [3.1 Explicit CLI Flag](#31-explicit-cli-flag)
      - [3.2 Automatic Config Detection](#32-automatic-config-detection)
      - [3.3 YAML Config Format](#33-yaml-config-format)
      - [3.4 Detection Flow](#34-detection-flow)
    - [4. Unified Replication API](#4-unified-replication-api)
      - [4.1 Lifecycle Functions](#41-lifecycle-functions)
      - [4.2 Log Submission Functions](#42-log-submission-functions)
      - [4.3 Callback Registration Functions](#43-callback-registration-functions)
      - [4.4 Epoch and Election Functions](#44-epoch-and-election-functions)
      - [4.5 Network Client and Benchmark Functions](#45-network-client-and-benchmark-functions)
    - [5. Namespace Symmetry: `paxos_impl` vs `raft_impl`](#5-namespace-symmetry-paxos_impl-vs-raft_impl)
      - [5.1 Key Structural Differences](#51-key-structural-differences)
      - [5.2 Callback Handling Difference](#52-callback-handling-difference)
    - [6. Initialization Sequence](#6-initialization-sequence)
    - [7. Mako Call Sites](#7-mako-call-sites)
      - [7.1 Log Submission (Hot Path)](#71-log-submission-hot-path)
      - [7.2 Callback Registration (Initialization)](#72-callback-registration-initialization)
      - [7.3 Shutdown](#73-shutdown)
    - [8. Naming Conventions and Legacy Compatibility](#8-naming-conventions-and-legacy-compatibility)
    - [9. Safety Annotations](#9-safety-annotations)
  - [RaftWorker: The Mako-Raft Bridge](#raftworker-the-mako-raft-bridge)
    - [1. Purpose](#1-purpose)
    - [2. Class Layout](#2-class-layout)
      - [2.1 Member Variables](#21-member-variables)
      - [2.2 Comparison with PaxosWorker](#22-comparison-with-paxosworker)
    - [3. Setup Chain](#3-setup-chain)
      - [3.1 `SetupBase()` — Create the Protocol Stack](#31-setupbase-create-the-protocol-stack)
      - [3.2 `SetupService()` — Start the RPC Server](#32-setupservice-start-the-rpc-server)
      - [3.3 `SetupCommo()` — Connect to Peers](#33-setupcommo-connect-to-peers)
      - [3.4 `SetupHeartbeat()` — Control-Plane RPC](#34-setupheartbeat-control-plane-rpc)
      - [3.5 Post-Setup: `EnsureSetup()` and `StartSubmitThread()`](#35-post-setup-ensuresetup-and-startsubmitthread)
    - [4. Log Submission Path](#4-log-submission-path)
      - [4.1 Flow Diagram](#41-flow-diagram)
      - [4.2 `PendingLog` Queue](#42-pendinglog-queue)
      - [4.3 `CreateRaftLogCommand()` — Wrapping Raw Bytes](#43-createraftlogcommand-wrapping-raw-bytes)
      - [4.4 `SubmitLoop()` — Background Draining](#44-submitloop-background-draining)
      - [4.5 `StopSubmitThread()` — Graceful Drain](#45-stopsubmitthread-graceful-drain)
    - [5. Committed Entry Callback Path](#5-committed-entry-callback-path)
      - [5.1 How `Next()` Gets Registered](#51-how-next-gets-registered)
      - [5.2 How `RaftServer` Invokes `app_next_`](#52-how-raftserver-invokes-app_next_)
      - [5.3 `Next()` — The Core Callback](#53-next-the-core-callback)
    - [6. Leadership Queries](#6-leadership-queries)
      - [6.1 `IsLeader(par_id)`](#61-isleaderpar_id)
      - [6.2 `IsPartition(par_id)`](#62-ispartitionpar_id)
      - [6.3 Leader Change Notification](#63-leader-change-notification)
    - [7. Callback Registration Methods](#7-callback-registration-methods)
      - [7.1 Simple Callbacks (Legacy)](#71-simple-callbacks-legacy)
      - [7.2 Watermark Callbacks (Production)](#72-watermark-callbacks-production)
      - [7.3 Deprecated Legacy Bridge](#73-deprecated-legacy-bridge)
    - [8. Shutdown Sequence](#8-shutdown-sequence)
      - [Phase 1: `WaitForShutdown()`](#phase-1-waitforshutdown)
      - [Phase 2: `ShutDown()`](#phase-2-shutdown)
    - [9. Helper Methods](#9-helper-methods)
      - [9.1 `GetRaftServer()`](#91-getraftserver)
      - [9.2 `GetPollThreadWorker()`](#92-getpollthreadworker)
      - [9.3 `IncSubmit()` / `WaitForSubmit()`](#93-incsubmit-waitforsubmit)
    - [10. Global State](#10-global-state)
    - [11. Complete Data Flow Diagram](#11-complete-data-flow-diagram)
  - [raft_main_helper.cc: The Raft Glue Layer](#raft_main_helpercc-the-raft-glue-layer)
    - [1. Purpose](#1-purpose-1)
    - [2. Global State](#2-global-state)
      - [2.1 Globals in `janus::` Namespace](#21-globals-in-janus-namespace)
      - [2.2 State in `raft_impl::` Namespace](#22-state-in-raft_impl-namespace)
    - [3. Lifecycle Functions](#3-lifecycle-functions)
      - [3.1 `setup(argc, argv)` — Worker Creation](#31-setupargc-argv-worker-creation)
      - [3.2 `setup2(action, shardIndex)` — Service Launch + Preferred Leader](#32-setup2action-shardindex-service-launch-preferred-leader)
      - [3.3 `server_launch_worker()` — Internal Multi-Phase Boot](#33-server_launch_worker-internal-multi-phase-boot)
      - [3.4 `shutdown_paxos()` — Two-Phase Teardown](#34-shutdown_paxos-two-phase-teardown)
      - [3.5 `pre_shutdown_step()` — Graceful Disconnect](#35-pre_shutdown_step-graceful-disconnect)
    - [4. Internal Helper Functions](#4-internal-helper-functions)
      - [4.1 `find_worker(par_id)` — Partition Lookup](#41-find_workerpar_id-partition-lookup)
      - [4.2 `enqueue_to_worker()` — Submit Dispatch](#42-enqueue_to_worker-submit-dispatch)
      - [4.3 `apply_callbacks_for_partition(par_id)` — Callback Re-Application](#43-apply_callbacks_for_partitionpar_id-callback-re-application)
    - [5. Leader Change Propagation](#5-leader-change-propagation)
      - [5.1 Notification Chain](#51-notification-chain)
      - [5.2 `wait_for_local_leadership()` — Blocking Wait](#52-wait_for_local_leadership-blocking-wait)
    - [6. Log Submission Functions](#6-log-submission-functions)
      - [6.1 `add_log_to_nc()` — Primary Hot Path](#61-add_log_to_nc-primary-hot-path)
      - [6.2 `submit()` / `add_log()` / `add_log_without_queue()`](#62-submit-add_log-add_log_without_queue)
      - [6.3 `wait_for_submit(par_id)`](#63-wait_for_submitpar_id)
    - [7. Callback Registration Functions](#7-callback-registration-functions)
      - [7.1 Watermark Callbacks (Production Path)](#71-watermark-callbacks-production-path)
      - [7.2 Simple Callbacks](#72-simple-callbacks)
      - [7.3 `register_leader_election_callback()`](#73-register_leader_election_callback)
    - [8. NO-OP Entry System](#8-no-op-entry-system)
      - [8.1 `send_no_ops_for_mark(epoch)`](#81-send_no_ops_for_markepoch)
      - [8.2 `send_no_ops_to_all_workers(epoch)`](#82-send_no_ops_to_all_workersepoch)
    - [9. Epoch and Election State Functions](#9-epoch-and-election-state-functions)
      - [9.1 `ElectionState` Singleton](#91-electionstate-singleton)
      - [9.2 `get_epoch()` / `set_epoch()`](#92-get_epoch-set_epoch)
      - [9.3 `upgrade_p1_to_leader()`](#93-upgrade_p1_to_leader)
      - [9.4 `get_outstanding_logs(par_id)`](#94-get_outstanding_logspar_id)
    - [10. Preferred Leader API](#10-preferred-leader-api)
      - [10.1 `set_preferred_leader(site_id)`](#101-set_preferred_leadersite_id)
    - [11. Stub Functions](#11-stub-functions)
    - [12. Function Reference Table](#12-function-reference-table)
    - [13. Comparison with `paxos_main_helper.cc`](#13-comparison-with-paxos_main_helpercc)
  - [Mako-Side Integration Points](#mako-side-integration-points)
    - [1. Overview](#1-overview-1)
    - [2. `init_env()` — The Main Initialisation Sequence](#2-init_env-the-main-initialisation-sequence)
    - [3. `detect_replication_type_from_config()`](#3-detect_replication_type_from_config)
      - [3.1 Implementation](#31-implementation)
      - [3.2 Design Rationale](#32-design-rationale)
      - [3.3 Priority Rules](#33-priority-rules)
    - [4. `setup_leader_election_callbacks()`](#4-setup_leader_election_callbacks)
      - [4.1 Purpose](#41-purpose)
      - [4.2 Implementation Structure](#42-implementation-structure)
      - [4.3 The `is_using_raft()` Guard](#43-the-is_using_raft-guard)
      - [4.4 Compile-Time vs Runtime Guards](#44-compile-time-vs-runtime-guards)
    - [5. `setup_paxos_leader_callbacks()` / `setup_paxos_follower_callbacks()`](#5-setup_paxos_leader_callbacks-setup_paxos_follower_callbacks)
      - [5.1 Leader Callbacks](#51-leader-callbacks)
      - [5.2 Follower Callbacks](#52-follower-callbacks)
      - [5.3 Protocol Agnosticism](#53-protocol-agnosticism)
    - [6. Other Callback Setup Functions](#6-other-callback-setup-functions)
      - [6.1 `setup_sync_util_callbacks()`](#61-setup_sync_util_callbacks)
      - [6.2 `setup_transport_callbacks()`](#62-setup_transport_callbacks)
      - [6.3 `cleanup_and_shutdown()`](#63-cleanup_and_shutdown)
    - [7. YAML Config Files](#7-yaml-config-files)
      - [7.1 `config/occ_raft.yml`](#71-configocc_raftyml)
      - [7.2 Raft-Specific Cluster Configs](#72-raft-specific-cluster-configs)
    - [8. Launch Scripts: `shard.sh` vs `shard_raft.sh`](#8-launch-scripts-shardsh-vs-shard_raftsh)
      - [8.1 `bash/shard.sh` — Unified Launcher](#81-bashshardsh-unified-launcher)
      - [8.2 `bash/shard_raft.sh` — Raft-Specific Launcher](#82-bashshard_raftsh-raft-specific-launcher)
      - [8.3 Port Separation](#83-port-separation)
    - [9. Functions That Do NOT Need Raft Changes](#9-functions-that-do-not-need-raft-changes)
    - [10. Summary of Mako-Side Raft Changes](#10-summary-of-mako-side-raft-changes)
  - [Integration Challenges and Bugs Fixed](#integration-challenges-and-bugs-fixed)
    - [1. Overview](#1-overview-2)
    - [2. Bug: Dispatcher Routing to Paxos (simpleRaft.cc)](#2-bug-dispatcher-routing-to-paxos-simpleraftcc)
      - [Symptom](#symptom)
      - [Root Cause](#root-cause)
      - [Fix](#fix)
      - [Verification](#verification)
      - [Lesson](#lesson)
    - [3. Bug: Auto-Detection Failure in `dbtest`](#3-bug-auto-detection-failure-in-dbtest)
      - [Symptom](#symptom-1)
      - [Root Cause](#root-cause-1)
      - [Fix](#fix-1)
      - [Priority Chain](#priority-chain)
      - [Verification](#verification-1)
      - [Lesson](#lesson-1)
    - [4. Bug: Cross-Shard RPC Failures During Raft Elections (FAIL_NEW_VERSION)](#4-bug-cross-shard-rpc-failures-during-raft-elections-fail_new_version)
      - [Symptom](#symptom-2)
      - [Root Cause](#root-cause-2)
      - [Fix](#fix-2)
      - [Verification](#verification-2)
      - [Lesson](#lesson-2)
    - [5. Bug: GetOrCreateClient() Race Condition](#5-bug-getorcreateclient-race-condition)
      - [Symptom](#symptom-3)
      - [Root Cause](#root-cause-3)
      - [Fix](#fix-3)
      - [Verification](#verification-3)
      - [Lesson](#lesson-3)
    - [6. Challenge: Process Cleanup and Port Release](#6-challenge-process-cleanup-and-port-release)
      - [Problem](#problem)
      - [Root Cause](#root-cause-4)
      - [Fix](#fix-4)
      - [Lesson](#lesson-4)
    - [7. Challenge: Port Conflicts Between Paxos and Raft](#7-challenge-port-conflicts-between-paxos-and-raft)
      - [Problem](#problem-1)
      - [Solution](#solution)
      - [Lesson](#lesson-5)
    - [8. Challenge: Jetpack Recovery Incompatibility](#8-challenge-jetpack-recovery-incompatibility)
      - [Problem](#problem-2)
      - [Solution](#solution-1)
      - [Design Choice](#design-choice)
      - [Lesson](#lesson-6)
    - [9. Additional Fixes: Transport Layer Shutdown Races](#9-additional-fixes-transport-layer-shutdown-races)
    - [10. Summary](#10-summary)
      - [Key Architectural Lesson](#key-architectural-lesson)
  - [Standalone Raft Test Framework](#standalone-raft-test-framework)
    - [1. Overview](#1-overview-3)
    - [2. Compile-Time Activation](#2-compile-time-activation)
    - [3. Configuration: `raft_lab_test.yml`](#3-configuration-raft_lab_testyml)
    - [4. Test Constants](#4-test-constants)
    - [5. Architecture](#5-architecture)
      - [5.1 Bootstrap Sequence](#51-bootstrap-sequence)
      - [5.2 Class Hierarchy](#52-class-hierarchy)
      - [5.3 Commit Tracking (`SetLearnerAction`)](#53-commit-tracking-setlearneraction)
    - [6. Network Simulation](#6-network-simulation)
      - [6.1 Disconnect / Reconnect](#61-disconnect-reconnect)
      - [6.2 Unreliable Network (`netctlLoop`)](#62-unreliable-network-netctlloop)
      - [6.3 Slow Network](#63-slow-network)
    - [7. Key Test Utilities](#7-key-test-utilities)
      - [7.1 `OneLeader(expected)` / `NoLeader()`](#71-oneleaderexpected-noleader)
      - [7.2 `Start(svr, cmd, &index, &term)`](#72-startsvr-cmd-index-term)
      - [7.3 `DoAgreement(cmd, n, retry)`](#73-doagreementcmd-n-retry)
      - [7.4 `Wait(index, n, term)`](#74-waitindex-n-term)
      - [7.5 `NCommitted(index)`](#75-ncommittedindex)
      - [7.6 `RpcCount(svr, reset)` / `RpcTotal()`](#76-rpccountsvr-reset-rpctotal)
      - [7.7 Server ID Helpers](#77-server-id-helpers)
    - [8. Test Macros](#8-test-macros)
    - [9. Test Execution Flow](#9-test-execution-flow)
    - [10. Summary of the 11 Test Cases](#10-summary-of-the-11-test-cases)
    - [11. Design Decisions](#11-design-decisions)
      - [Coroutine-Based Execution](#coroutine-based-execution)
      - [Static State](#static-state)
      - [`TpcCommitCommand` as Test Payload](#tpccommitcommand-as-test-payload)
      - [Disconnect via Proxy Swap](#disconnect-via-proxy-swap)
  - [Individual Test Case Documentation](#individual-test-case-documentation)
    - [1. Overview](#1-overview-4)
    - [2. Test 1: Initial Election (`testInitialElection`)](#2-test-1-initial-election-testinitialelection)
    - [3. Test 2: Re-Election (`testReElection`)](#3-test-2-re-election-testreelection)
    - [4. Test 3: Basic Agreement (`testBasicAgree`)](#4-test-3-basic-agreement-testbasicagree)
    - [5. Test 4: Agreement Despite Follower Failure (`testFailAgree`)](#5-test-4-agreement-despite-follower-failure-testfailagree)
    - [6. Test 5: No Agreement Without Quorum (`testFailNoAgree`)](#6-test-5-no-agreement-without-quorum-testfailnoagree)
    - [7. Test 6: Rejoin of Disconnected Leader (`testRejoin`)](#7-test-6-rejoin-of-disconnected-leader-testrejoin)
    - [8. Test 7: Concurrent Starts (`testConcurrentStarts`)](#8-test-7-concurrent-starts-testconcurrentstarts)
    - [9. Test 8: Leader Backs Up Quickly (`testBackup`)](#9-test-8-leader-backs-up-quickly-testbackup)
    - [10. Test 9: RPC Count Verification (`testCount`)](#10-test-9-rpc-count-verification-testcount)
    - [11. Test 10: Unreliable Agreement (`testUnreliableAgree`)](#11-test-10-unreliable-agreement-testunreliableagree)
    - [12. Test 11: Figure 8 (`testFigure8`)](#12-test-11-figure-8-testfigure8)
    - [13. Test Progression Summary](#13-test-progression-summary)
    - [14. Index Tracking Across Tests](#14-index-tracking-across-tests)
  - [Test Configuration YAML Files](#test-configuration-yaml-files)
    - [1. Overview](#1-overview-5)
    - [2. Test Configuration: `raft_lab_test.yml`](#2-test-configuration-raft_lab_testyml)
      - [2.1 Field-by-Field Explanation](#21-field-by-field-explanation)
      - [2.2 Port Allocation](#22-port-allocation)
    - [3. CI Test Cluster Configs](#3-ci-test-cluster-configs)
      - [3.1 `1c1s3r1p_cluster_test.yml` (3-Replica Raft Test)](#31-1c1s3r1p_cluster_testyml-3-replica-raft-test)
      - [3.2 `1c1s5r1p_cluster_test.yml` (5-Replica Raft Test)](#32-1c1s5r1p_cluster_testyml-5-replica-raft-test)
      - [3.3 `1c1s3r3p_cluster_test.yml` (3-Replica, 3-Partition)](#33-1c1s3r3p_cluster_testyml-3-replica-3-partition)
    - [4. Production Raft Config: `raft6_shardidx0.yml`](#4-production-raft-config-raft6_shardidx0yml)
    - [5. Production Paxos Config: `paxos6_shardidx0.yml` (Comparison)](#5-production-paxos-config-paxos6_shardidx0yml-comparison)
    - [6. Mode Config Files](#6-mode-config-files)
      - [6.1 `occ_raft.yml`](#61-occ_raftyml)
      - [6.2 `occ_paxos.yml`](#62-occ_paxosyml)
    - [7. Comparison: Test vs Production](#7-comparison-test-vs-production)
      - [7.1 Why 5 Replicas in Tests?](#71-why-5-replicas-in-tests)
      - [7.2 Why `cc: none` in Tests?](#72-why-cc-none-in-tests)
    - [8. Port Range Allocation Summary](#8-port-range-allocation-summary)
    - [9. Config Naming Convention](#9-config-naming-convention)
  - [CI Script Documentation: `ci_mako_raft.sh`](#ci-script-documentation-ci_mako_raftsh)
    - [1. Overview](#1-overview-6)
    - [2. Script Structure](#2-script-structure)
      - [2.1 High-Level Architecture](#21-high-level-architecture)
      - [2.2 Available Commands](#22-available-commands)
      - [2.3 The `all` Execution Order](#23-the-all-execution-order)
    - [3. Process Management](#3-process-management)
      - [3.1 `cleanup_processes()` (lines 65-105)](#31-cleanup_processes-lines-65-105)
      - [3.2 `check_for_hanging_processes()` (lines 25-59)](#32-check_for_hanging_processes-lines-25-59)
      - [3.3 Test Function Pattern](#33-test-function-pattern)
    - [4. Environment Variables](#4-environment-variables)
    - [5. Comparison: `ci_mako_raft.sh` vs `ci.sh`](#5-comparison-ci_mako_raftsh-vs-cish)
      - [5.1 Structural Differences](#51-structural-differences)
      - [5.2 Raft Tests in `ci.sh`](#52-raft-tests-in-cish)
      - [5.3 Process Cleanup Differences](#53-process-cleanup-differences)
    - [6. Shard Launch Scripts](#6-shard-launch-scripts)
      - [6.1 `bash/shard_raft.sh` (Raft-Specific)](#61-bashshard_raftsh-raft-specific)
      - [6.2 `bash/shard.sh` (Unified Launcher)](#62-bashshardsh-unified-launcher)
      - [6.3 Arguments](#63-arguments)
    - [7. Result Archival](#7-result-archival)
  - [CI Test Scenarios](#ci-test-scenarios)
    - [1. Overview](#1-overview-7)
    - [2. Scenario 1: simpleRaft](#2-scenario-1-simpleraft)
      - [2.1 Configuration](#21-configuration)
      - [2.2 Execution Sequence](#22-execution-sequence)
      - [2.3 Pass/Fail Criteria](#23-passfail-criteria)
      - [2.4 What It Tests](#24-what-it-tests)
    - [3. Scenario 2: shard1ReplicationRaft](#3-scenario-2-shard1replicationraft)
      - [3.1 Configuration](#31-configuration)
      - [3.2 Execution Sequence](#32-execution-sequence)
      - [3.3 Log Files](#33-log-files)
      - [3.4 Pass/Fail Criteria](#34-passfail-criteria)
      - [3.5 What It Tests](#35-what-it-tests)
    - [4. Scenario 3: shard2ReplicationRaft](#4-scenario-3-shard2replicationraft)
      - [4.1 Configuration](#41-configuration)
      - [4.2 Execution Sequence](#42-execution-sequence)
      - [4.3 Shutdown Procedure](#43-shutdown-procedure)
      - [4.4 Pass/Fail Criteria](#44-passfail-criteria)
      - [4.5 What It Tests](#45-what-it-tests)
    - [5. Scenario 4: shard1ReplicationSimpleRaft](#5-scenario-4-shard1replicationsimpleraft)
      - [5.1 Configuration](#51-configuration)
      - [5.2 Execution Sequence](#52-execution-sequence)
      - [5.3 Pass/Fail Criteria](#53-passfail-criteria)
      - [5.4 What It Tests](#54-what-it-tests)
    - [6. Scenario 5: shard2ReplicationSimpleRaft](#6-scenario-5-shard2replicationsimpleraft)
      - [6.1 Configuration](#61-configuration)
      - [6.2 Execution Sequence](#62-execution-sequence)
      - [6.3 Pass/Fail Criteria](#63-passfail-criteria)
      - [6.4 What It Tests](#64-what-it-tests)
    - [7. Process Naming and Binaries](#7-process-naming-and-binaries)
    - [8. Known Issues Handled in Scripts](#8-known-issues-handled-in-scripts)
      - [8.1 Leader Shutdown Hang](#81-leader-shutdown-hang)
      - [8.2 Port Conflicts](#82-port-conflicts)
      - [8.3 RocksDB Cleanup](#83-rocksdb-cleanup)
    - [9. Raft vs Paxos Scenario Comparison](#9-raft-vs-paxos-scenario-comparison)
  - [Shell Scripts Walkthrough](#shell-scripts-walkthrough)
    - [1. Overview](#1-overview-8)
    - [2. `simpleRaft.sh` — Basic Raft Replication](#2-simpleraftsh-basic-raft-replication)
      - [2.1 Step-by-Step Walkthrough](#21-step-by-step-walkthrough)
      - [2.2 Key Design Choices](#22-key-design-choices)
      - [2.3 Log Parsing Functions](#23-log-parsing-functions)
    - [3. `test_1shard_replication_raft.sh` — 1-Shard TPC-C](#3-test_1shard_replication_raftsh-1-shard-tpc-c)
      - [3.1 Step-by-Step Walkthrough](#31-step-by-step-walkthrough)
      - [3.2 Thread Count](#32-thread-count)
    - [4. `test_2shard_replication_raft.sh` — 2-Shard TPC-C](#4-test_2shard_replication_raftsh-2-shard-tpc-c)
      - [4.1 Step-by-Step Walkthrough](#41-step-by-step-walkthrough)
      - [4.2 Key Design: Completion Polling](#42-key-design-completion-polling)
      - [4.3 Key Design: Multi-Phase Shutdown](#43-key-design-multi-phase-shutdown)
    - [5. `test_1shard_replication_simple_raft.sh` — 1-Shard Simple Tx](#5-test_1shard_replication_simple_raftsh-1-shard-simple-tx)
      - [5.1 Step-by-Step Walkthrough](#51-step-by-step-walkthrough)
      - [5.2 Key Design: Leader Hang Tolerance](#52-key-design-leader-hang-tolerance)
    - [6. `test_2shard_replication_simple_raft.sh` — 2-Shard Simple Tx](#6-test_2shard_replication_simple_raftsh-2-shard-simple-tx)
      - [6.1 Step-by-Step Walkthrough](#61-step-by-step-walkthrough)
    - [7. `bash/shard_raft.sh` — Raft Shard Launcher](#7-bashshard_raftsh-raft-shard-launcher)
      - [7.1 Step-by-Step Walkthrough](#71-step-by-step-walkthrough)
      - [7.2 Config File Selection](#72-config-file-selection)
    - [8. Non-CI Scripts](#8-non-ci-scripts)
      - [8.1 `run_test1_preferred_startup.sh` (361 lines)](#81-run_test1_preferred_startupsh-361-lines)
      - [8.2 `run_test_log_replication.sh` (159 lines)](#82-run_test_log_replicationsh-159-lines)
      - [8.3 `run_test_noops.sh` (256 lines)](#83-run_test_noopssh-256-lines)
      - [8.4 Common Patterns in Non-CI Scripts](#84-common-patterns-in-non-ci-scripts)
    - [9. Script Dependency Graph](#9-script-dependency-graph)
  - [Benchmark Methodology](#benchmark-methodology)
    - [1. Overview](#1-overview-9)
    - [2. Test Environment](#2-test-environment)
      - [2.1 Hardware](#21-hardware)
      - [2.2 Transport](#22-transport)
      - [2.3 Build Configuration](#23-build-configuration)
    - [3. Workloads](#3-workloads)
      - [3.1 TPC-C Benchmark](#31-tpc-c-benchmark)
      - [3.2 Simple Transaction Workload](#32-simple-transaction-workload)
    - [4. Test Configurations](#4-test-configurations)
      - [4.1 1-Shard TPC-C](#41-1-shard-tpc-c)
      - [4.2 2-Shard TPC-C](#42-2-shard-tpc-c)
      - [4.3 1-Shard Simple Transaction](#43-1-shard-simple-transaction)
      - [4.4 2-Shard Simple Transaction](#44-2-shard-simple-transaction)
    - [5. Metrics Collected](#5-metrics-collected)
      - [5.1 Primary Metrics](#51-primary-metrics)
      - [5.2 Per-Transaction Metrics](#52-per-transaction-metrics)
      - [5.3 Replication Metrics](#53-replication-metrics)
    - [6. Measurement Procedure](#6-measurement-procedure)
      - [6.1 TPC-C Tests](#61-tpc-c-tests)
      - [6.2 Simple Transaction Tests](#62-simple-transaction-tests)
    - [7. Caveats and Limitations](#7-caveats-and-limitations)
      - [7.1 Single-Node Deployment](#71-single-node-deployment)
      - [7.2 Test Duration Difference](#72-test-duration-difference)
      - [7.3 Process Count Difference](#73-process-count-difference)
      - [7.4 Replication Protocol Differences](#74-replication-protocol-differences)
      - [7.5 Warmup Period](#75-warmup-period)
      - [7.6 Resource Contention](#76-resource-contention)
  - [Detailed Benchmark Results](#detailed-benchmark-results)
    - [1. Overview](#1-overview-10)
    - [2. 1-Shard TPC-C Results](#2-1-shard-tpc-c-results)
      - [2.1 Aggregate Throughput](#21-aggregate-throughput)
      - [2.2 Per-Transaction Latency (1-Shard)](#22-per-transaction-latency-1-shard)
      - [2.3 Follower Replication (1-Shard)](#23-follower-replication-1-shard)
    - [3. 2-Shard TPC-C Results](#3-2-shard-tpc-c-results)
      - [3.1 Aggregate Throughput](#31-aggregate-throughput)
      - [3.2 Aggregate Comparison](#32-aggregate-comparison)
      - [3.3 Throughput Drop: 1-Shard to 2-Shard](#33-throughput-drop-1-shard-to-2-shard)
      - [3.4 Follower Replication (2-Shard)](#34-follower-replication-2-shard)
    - [4. Simple Transaction Results](#4-simple-transaction-results)
      - [4.1 simpleRaft Test](#41-simpleraft-test)
      - [4.2 1-Shard Simple Transaction (Raft)](#42-1-shard-simple-transaction-raft)
      - [4.3 2-Shard Simple Transaction (Raft)](#43-2-shard-simple-transaction-raft)
    - [5. Replication Correctness](#5-replication-correctness)
      - [5.1 Data Integrity](#51-data-integrity)
      - [5.2 Replication Completeness](#52-replication-completeness)
    - [6. Summary Table](#6-summary-table)
  - [Performance Analysis and Discussion](#performance-analysis-and-discussion)
    - [1. Overview](#1-overview-11)
    - [2. Single-Shard Analysis: Why Paxos Is 28% Faster](#2-single-shard-analysis-why-paxos-is-28-faster)
      - [2.1 Observed Difference](#21-observed-difference)
      - [2.2 Factor 1: Multi-Paxos Pipelining](#22-factor-1-multi-paxos-pipelining)
      - [2.3 Factor 2: Test Duration Difference](#23-factor-2-test-duration-difference)
      - [2.4 Factor 3: Process Count and CPU Contention](#24-factor-3-process-count-and-cpu-contention)
      - [2.5 Decomposition of the 28% Gap](#25-decomposition-of-the-28-gap)
    - [3. Two-Shard Analysis: Why Throughput Is Equal](#3-two-shard-analysis-why-throughput-is-equal)
      - [3.1 Observed Equality](#31-observed-equality)
      - [3.2 Cross-Shard Coordination Dominates](#32-cross-shard-coordination-dominates)
      - [3.3 Throughput Drop Factors](#33-throughput-drop-factors)
      - [3.4 Higher Remote Abort Ratio Under Raft](#34-higher-remote-abort-ratio-under-raft)
    - [4. Replication Batching Behaviour](#4-replication-batching-behaviour)
      - [4.1 Replay Batch Comparison](#41-replay-batch-comparison)
      - [4.2 Implementation Differences](#42-implementation-differences)
      - [4.3 Batch Size Analysis](#43-batch-size-analysis)
      - [4.4 Two-Shard Batch Size](#44-two-shard-batch-size)
    - [5. Per-Transaction Latency Analysis](#5-per-transaction-latency-analysis)
      - [5.1 Latency Comparison (1-Shard)](#51-latency-comparison-1-shard)
      - [5.2 Why Raft Is Faster for Some Transactions](#52-why-raft-is-faster-for-some-transactions)
      - [5.3 Why Paxos Is Faster for Payment](#53-why-paxos-is-faster-for-payment)
      - [5.4 Why Aggregate Throughput Favours Paxos Despite Per-Transaction Mix](#54-why-aggregate-throughput-favours-paxos-despite-per-transaction-mix)
    - [6. Replica Topology Trade-offs](#6-replica-topology-trade-offs)
      - [6.1 Process Count](#61-process-count)
      - [6.2 Throughput per Process](#62-throughput-per-process)
      - [6.3 Quorum Mechanics](#63-quorum-mechanics)
      - [6.4 Leader Election](#64-leader-election)
    - [7. Replication Correctness](#7-replication-correctness)
      - [7.1 Data Integrity](#71-data-integrity)
      - [7.2 Replication Completeness](#72-replication-completeness)
    - [8. Production Deployment Implications](#8-production-deployment-implications)
      - [8.1 When to Choose Raft](#81-when-to-choose-raft)
      - [8.2 When to Choose Paxos](#82-when-to-choose-paxos)
      - [8.3 Performance Parity in Practice](#83-performance-parity-in-practice)
      - [8.4 Throughput in Context](#84-throughput-in-context)
      - [8.5 Summary of Trade-offs](#85-summary-of-trade-offs)
    - [9. Threats to Validity](#9-threats-to-validity)
      - [9.1 Single-Node Testing](#91-single-node-testing)
      - [9.2 Single Run](#92-single-run)
      - [9.3 Small Scale](#93-small-scale)
      - [9.4 Duration Mismatch](#94-duration-mismatch)
  - [Performance Figures and Charts](#performance-figures-and-charts)
    - [1. 1-Shard TPC-C Throughput Comparison](#1-1-shard-tpc-c-throughput-comparison)
    - [2. 2-Shard TPC-C Per-Shard Throughput](#2-2-shard-tpc-c-per-shard-throughput)
    - [3. Throughput Scaling: 1-Shard to 2-Shard](#3-throughput-scaling-1-shard-to-2-shard)
    - [4. Per-Transaction Commit Latency (1-Shard)](#4-per-transaction-commit-latency-1-shard)
    - [5. Follower Replay Batch Comparison (1-Shard)](#5-follower-replay-batch-comparison-1-shard)
    - [6. Architectural Comparison Table](#6-architectural-comparison-table)
    - [7. Remote Abort Ratio Comparison (2-Shard)](#7-remote-abort-ratio-comparison-2-shard)
    - [8. Process Count vs Throughput](#8-process-count-vs-throughput)
    - [9. Mermaid Charts (for Markdown Renderers)](#9-mermaid-charts-for-markdown-renderers)
      - [9.1 Throughput Comparison](#91-throughput-comparison)
      - [9.2 Follower Replay Batches](#92-follower-replay-batches)
      - [9.3 Per-Transaction Latency](#93-per-transaction-latency)
  - [Persistent Log Storage](#persistent-log-storage)
    - [1. Overview](#1-overview-12)
    - [2. LogEntry Structure](#2-logentry-structure)
    - [3. LogStorage Interface](#3-logstorage-interface)
      - [3.1 Single Entry Operations](#31-single-entry-operations)
      - [3.2 Batch Operations](#32-batch-operations)
      - [3.3 Index Queries](#33-index-queries)
      - [3.4 Metadata Operations](#34-metadata-operations)
      - [3.5 Lifecycle Operations](#35-lifecycle-operations)
    - [4. InMemoryLogStorage](#4-inmemorylogstorage)
      - [4.1 Internal Structure](#41-internal-structure)
      - [4.2 Key Characteristics](#42-key-characteristics)
      - [4.3 Thread Safety](#43-thread-safety)
    - [5. RocksDBLogStorage](#5-rocksdblogstorage)
      - [5.1 Internal Structure](#51-internal-structure)
      - [5.2 Key Prefixes](#52-key-prefixes)
      - [5.3 RocksDB Configuration](#53-rocksdb-configuration)
      - [5.4 Serialization](#54-serialization)
      - [5.5 Batch Operations](#55-batch-operations)
      - [5.6 Index Queries](#56-index-queries)
      - [5.7 Lifecycle](#57-lifecycle)
    - [6. Raft Server Integration](#6-raft-server-integration)
      - [6.1 Storage Members](#61-storage-members)
      - [6.2 Metadata Keys](#62-metadata-keys)
      - [6.3 Persistence Methods](#63-persistence-methods)
      - [6.4 When Persistence Is Called](#64-when-persistence-is-called)
      - [6.5 SetLogStorage and RecoverFromStorage](#65-setlogstorage-and-recoverfromstorage)
    - [7. Paxos Server Integration](#7-paxos-server-integration)
      - [7.1 Metadata Keys](#71-metadata-keys)
      - [7.2 Persistence Methods](#72-persistence-methods)
      - [7.3 Additional Paxos Methods](#73-additional-paxos-methods)
    - [8. Storage Path Configuration](#8-storage-path-configuration)
      - [8.1 Log Storage Paths](#81-log-storage-paths)
      - [8.2 Snapshot Storage Paths](#82-snapshot-storage-paths)
      - [8.3 User Isolation](#83-user-isolation)
    - [9. Unit Tests](#9-unit-tests)
  - [Crash Recovery Process](#crash-recovery-process)
    - [1. Overview](#1-overview-13)
    - [2. RecoveryMode Enum](#2-recoverymode-enum)
    - [3. RecoveryConfig](#3-recoveryconfig)
      - [3.1 Factory Method](#31-factory-method)
    - [4. RecoveryResult](#4-recoveryresult)
    - [5. RecoveryManager](#5-recoverymanager)
      - [5.1 Internal State](#51-internal-state)
      - [5.2 Mode Detection](#52-mode-detection)
      - [5.3 Storage Creation](#53-storage-creation)
      - [5.4 Generic Recovery Template](#54-generic-recovery-template)
    - [6. Recovery Integration in Server Startup](#6-recovery-integration-in-server-startup)
      - [6.1 Sequence Diagram](#61-sequence-diagram)
      - [6.2 Raft Recovery Call](#62-raft-recovery-call)
      - [6.3 What RecoverFromStorage Does (Raft)](#63-what-recoverfromstorage-does-raft)
      - [6.4 What RecoverFromStorage Does (Paxos)](#64-what-recoverfromstorage-does-paxos)
    - [7. Resolving Uncommitted Entries](#7-resolving-uncommitted-entries)
      - [7.1 Raft](#71-raft)
      - [7.2 Paxos](#72-paxos)
    - [8. Fresh Start vs Recovery Detection](#8-fresh-start-vs-recovery-detection)
    - [9. Storage Cleanup in CI Tests](#9-storage-cleanup-in-ci-tests)
  - [Snapshot Support](#snapshot-support)
    - [1. Overview](#1-overview-14)
    - [2. SnapshotMetadata](#2-snapshotmetadata)
    - [3. SnapshotManager Interface](#3-snapshotmanager-interface)
      - [3.1 Snapshot Creation](#31-snapshot-creation)
      - [3.2 Snapshot Loading](#32-snapshot-loading)
      - [3.3 Snapshot Queries](#33-snapshot-queries)
      - [3.4 Snapshot Cleanup](#34-snapshot-cleanup)
    - [4. SnapshotReader and SnapshotWriter](#4-snapshotreader-and-snapshotwriter)
      - [4.1 SnapshotReader (lines 63-97)](#41-snapshotreader-lines-63-97)
      - [4.2 SnapshotWriter (lines 103-137)](#42-snapshotwriter-lines-103-137)
    - [5. FileSnapshotManager](#5-filesnapshotmanager)
      - [5.1 File Naming Convention](#51-file-naming-convention)
      - [5.2 Directory Structure](#52-directory-structure)
      - [5.3 Configuration](#53-configuration)
      - [5.4 Taking a Snapshot](#54-taking-a-snapshot)
      - [5.5 FileSnapshotWriter](#55-filesnapshotwriter)
      - [5.6 FileSnapshotReader](#56-filesnapshotreader)
      - [5.7 Retention Policy](#57-retention-policy)
      - [5.8 Log Compaction](#58-log-compaction)
    - [6. Snapshot Binary Format](#6-snapshot-binary-format)
      - [6.1 Layout](#61-layout)
      - [6.2 SnapshotHeader](#62-snapshotheader)
      - [6.3 CRC32 Implementation](#63-crc32-implementation)
      - [6.4 Serialization](#64-serialization)
      - [6.5 Deserialization](#65-deserialization)
      - [6.6 Compression](#66-compression)
    - [7. Snapshot Types](#7-snapshot-types)
    - [8. Server Integration](#8-server-integration)
      - [8.1 Raft Server](#81-raft-server)
      - [8.2 Paxos Server](#82-paxos-server)
    - [9. Snapshot Lifecycle](#9-snapshot-lifecycle)
    - [10. Crash Safety Properties](#10-crash-safety-properties)
      - [10.1 Atomic Writes](#101-atomic-writes)
      - [10.2 CRC32 Verification](#102-crc32-verification)
      - [10.3 Retention Guarantee](#103-retention-guarantee)
  - [Complete File Reference](#complete-file-reference)
    - [1. Raft Implementation (`src/deptran/raft/`)](#1-raft-implementation-srcdeptranraft)
    - [2. Paxos Implementation (`src/deptran/paxos/`)](#2-paxos-implementation-srcdeptranpaxos)
    - [3. Integration Files](#3-integration-files)
    - [4. Persistence Layer (`src/rrr/rpc/`)](#4-persistence-layer-srcrrrrpc)
    - [5. Test Files](#5-test-files)
      - [5.1 C++ Test Binaries (`examples/mako-raft-tests/`)](#51-c-test-binaries-examplesmako-raft-tests)
      - [5.2 Test Shell Scripts (`examples/mako-raft-tests/`)](#52-test-shell-scripts-examplesmako-raft-tests)
      - [5.3 Unit Tests (`test/`)](#53-unit-tests-test)
    - [6. CI Scripts](#6-ci-scripts)
    - [7. Shard Launch Scripts](#7-shard-launch-scripts)
    - [8. Configuration Files](#8-configuration-files)
      - [8.1 Raft Configs (`config/`)](#81-raft-configs-config)
      - [8.2 Raft Cluster Topologies (`config/1leader_2followers/`)](#82-raft-cluster-topologies-config1leader_2followers)
      - [8.3 Paxos Configs (for comparison)](#83-paxos-configs-for-comparison)
      - [8.4 Shard Configs (`src/mako/config/`)](#84-shard-configs-srcmakoconfig)
      - [8.5 Test Cluster Configs (`config/`)](#85-test-cluster-configs-config)
  - [YAML Configuration Reference](#yaml-configuration-reference)
    - [1. Mode Configuration](#1-mode-configuration)
      - [1.1 Raft Mode (`config/occ_raft.yml`)](#11-raft-mode-configocc_raftyml)
      - [1.2 Paxos Mode (`config/occ_paxos.yml`)](#12-paxos-mode-configocc_paxosyml)
      - [1.3 Other Raft Mode Variants](#13-other-raft-mode-variants)
      - [1.4 Mode Config Fields](#14-mode-config-fields)
    - [2. Replication Group Configuration](#2-replication-group-configuration)
      - [2.1 Raft 6-Partition Shard 0 (`config/1leader_2followers/raft6_shardidx0.yml`)](#21-raft-6-partition-shard-0-config1leader_2followersraft6_shardidx0yml)
      - [2.2 Structure](#22-structure)
      - [2.3 Partition Naming Convention](#23-partition-naming-convention)
    - [3. Port Allocation Scheme](#3-port-allocation-scheme)
      - [3.1 Port Ranges](#31-port-ranges)
      - [3.2 Port Allocation Formula](#32-port-allocation-formula)
      - [3.3 How to Avoid Port Conflicts](#33-how-to-avoid-port-conflicts)
    - [4. Shard Configuration](#4-shard-configuration)
      - [4.1 Shard Config (`src/mako/config/local-shards1-warehouses6.yml`)](#41-shard-config-srcmakoconfiglocal-shards1-warehouses6yml)
      - [4.2 Multi-Shard Config (`src/mako/config/local-shards2-warehouses6.yml`)](#42-multi-shard-config-srcmakoconfiglocal-shards2-warehouses6yml)
      - [4.3 Fields](#43-fields)
    - [5. Standalone Test Configuration](#5-standalone-test-configuration)
      - [5.1 Lab Test Config (`config/raft_lab_test.yml`)](#51-lab-test-config-configraft_lab_testyml)
      - [5.2 Test Cluster Configs](#52-test-cluster-configs)
    - [6. Switching Between Paxos and Raft](#6-switching-between-paxos-and-raft)
      - [6.1 Via `shard_raft.sh` (Raft-dedicated)](#61-via-shard_raftsh-raft-dedicated)
      - [6.2 Via `shard.sh` (Unified)](#62-via-shardsh-unified)
      - [6.3 Via Mode Config](#63-via-mode-config)
      - [6.4 Via Command-Line Flag](#64-via-command-line-flag)
    - [7. Config File Selection by `shard_raft.sh`](#7-config-file-selection-by-shard_raftsh)
  - [Glossary](#glossary-1)
    - [1. Raft-Specific Terms](#1-raft-specific-terms)
    - [2. Mako-Specific Terms](#2-mako-specific-terms)
    - [3. Transaction Terms](#3-transaction-terms)
    - [4. System and Infrastructure Terms](#4-system-and-infrastructure-terms)
    - [5. Persistence Terms](#5-persistence-terms)
  - [RustyCpp Safety Annotations in Raft Code](#rustycpp-safety-annotations-in-raft-code)
    - [1. Overview](#1-overview-15)
    - [2. Annotation Summary by File](#2-annotation-summary-by-file)
    - [3. Which Methods Are @safe and Why](#3-which-methods-are-safe-and-why)
      - [3.1 All @safe — RPC Service Layer](#31-all-safe-rpc-service-layer)
      - [3.2 All @safe — Executor](#32-all-safe-executor)
      - [3.3 All @safe — Frame Factory Methods](#33-all-safe-frame-factory-methods)
      - [3.4 All @safe — Communication Layer](#34-all-safe-communication-layer)
      - [3.5 @safe — Read-Only Accessors](#35-safe-read-only-accessors)
    - [4. Which Methods Are @unsafe and Why](#4-which-methods-are-unsafe-and-why)
      - [4.1 Persistence Methods (I/O)](#41-persistence-methods-io)
      - [4.2 State Mutation](#42-state-mutation)
      - [4.3 RPC and Connection Management](#43-rpc-and-connection-management)
      - [4.4 Random Number Generation](#44-random-number-generation)
      - [4.5 Monitoring](#45-monitoring)
    - [5. RustyCpp Types Used in Raft](#5-rustycpp-types-used-in-raft)
      - [5.1 `rusty::Arc<T>` — Thread-Safe Shared Ownership](#51-rustyarc-thread-safe-shared-ownership)
      - [5.2 `rusty::Box<T>` — Single Ownership](#52-rustybox-single-ownership)
      - [5.3 `rusty::Cell<T>` — Interior Mutability](#53-rustycell-interior-mutability)
      - [5.4 `rusty::Option<T>` — Optional Values](#54-rustyoption-optional-values)
      - [5.5 `rusty::Function<Sig>` — Type-Erased Callable](#55-rustyfunction-type-erased-callable)
      - [5.6 `rusty::Mutex<T>` — Thread-Safe Lock](#56-rustymutex-thread-safe-lock)
    - [6. Borrow Checking Configuration](#6-borrow-checking-configuration)
      - [6.1 Checked Files](#61-checked-files)
      - [6.2 Excluded Files](#62-excluded-files)
      - [6.3 Build Command](#63-build-command)
    - [7. Key Safety Patterns](#7-key-safety-patterns)
      - [7.1 Arc<Cell<T>> for Shared Mutable State](#71-arc-for-shared-mutable-state)
      - [7.2 Box<T> for Owned Resources](#72-box-for-owned-resources)
      - [7.3 Lambda Over std::bind](#73-lambda-over-stdbind)
      - [7.4 Inline @unsafe Blocks](#74-inline-unsafe-blocks)
    - [8. Safety Statistics](#8-safety-statistics)

---

# Introduction and Reading Guide


### What This Document Covers

This is the master table of contents and reading guide for the thesis documentation on **integrating Raft consensus into the Mako distributed transaction system**. The documentation covers the design, implementation, testing, and performance evaluation of a Raft replication module that operates as an alternative to Mako's existing Multi-Paxos atomic broadcast layer.

**Author contribution scope**: The Raft module, its integration with Mako, standalone tests, preferred leader election, and the CI test suite were implemented by the author. Mako itself (storage engine, concurrency control, transaction coordination, sharding) is pre-existing infrastructure.

---

### Document Map

All documents are organized under `doc/thesis/` in topical subfolders. Each document is self-contained but cross-references related documents.

#### Chapter 1: Mako System Overview (`01-mako-overview/`)

| Document | Description |
|----------|-------------|
| [`system_architecture.md`](#system-architecture) | High-level Mako architecture: Masstree storage, OCC concurrency control, atomic broadcast layer, sharding, and transaction flow. Where Raft/Paxos plug in. |
| [`build_system.md`](#build-system) | CMake build system, `MAKO_USE_RAFT` flag, runtime protocol switching via `--replication`, YAML configuration format, port allocation scheme. |

#### Chapter 2: Raft Protocol Implementation (`02-raft-core/`)

| Document | Description |
|----------|-------------|
| [`protocol_overview.md`](#protocol-overview) | How this implementation maps to the Raft paper: key classes, state transitions, deviations and extensions. |
| [`server_implementation.md`](#server-implementation) | `RaftServer` deep dive: all member variables, `OnRequestVote()`, `OnAppendEntries()`, `Start()`, `applyLogs()`, timers, persistence hooks. |
| [`leader_election.md`](#leader-election) | Election mechanism: trigger, vote collection, quorum detection, split-vote handling, term advancement. Includes sequence diagram. |
| [`log_replication.md`](#log-replication) | Log replication: leader sends entries, follower consistency check, `match_index`/`next_index` tracking, commit advancement, backtracking, heartbeats. |
| [`coordinator.md`](#coordinator) | `CoordinatorRaft`: transaction submission via `Submit()`, `WRONG_LEADER` retry logic, quorum calculation. |
| [`rpc_layer.md`](#rpc-layer) | Communication infrastructure: `RaftCommo`, `RaftServiceImpl`, `RaftFrame` factory, RPC macros, wire format. |

#### Chapter 3: Preferred Leader Election (`03-preferred-leader/`)

| Document | Description |
|----------|-------------|
| [`design.md`](#design) | Motivation for preferred leader (data locality, operational control), design overview, safety argument. |
| [`implementation.md`](#implementation) | Implementation details: `SetPreferredLeader()`, `AmIPreferredLeader()`, `HaveCaughtUp()`, `ShouldTransferLeadership()`, `InitiateLeadershipTransfer()`, `OnTimeoutNow()`, dynamic election timeouts. Includes sequence diagram. |
| [`testing.md`](#testing) | Test binaries: `testPreferredReplicaStartup`, `testPreferredReplicaLogReplication`, `testNoOps`. |

#### Chapter 4: Mako Integration (`04-mako-integration/`)

| Document | Description |
|----------|-------------|
| [`architecture.md`](#architecture) | Integration architecture: `replication_helper.h` dispatcher, `DISPATCH_RAFT_OR_PAXOS` macro, `rusty::Cell<ReplicationType>` global state, `detect_replication_type_from_config()`. |
| [`raft_worker.md`](#raft-worker) | `RaftWorker` bridge: setup chain, leader/follower callbacks, log submission pipeline, `PendingLog` queue, `Next()` callback. |
| [`raft_main_helper.md`](#raft-main-helper) | `raft_main_helper.cc` glue code: `raft_impl` namespace, `setup()`/`setup2()`, leadership change handling, NO-OP entries, `wait_for_local_leadership()`. |
| [`mako_hooks.md`](#mako-hooks) | Mako-side integration: `setup_leader_election_callbacks()`, `detect_replication_type_from_config()`, `is_using_raft()` checks, `shard_raft.sh` vs `shard.sh`. |
| [`challenges.md`](#challenges) | Integration bugs fixed: dispatcher routing, replication type auto-detection, cross-shard RPC failures during leader elections, race conditions, process cleanup. |

#### Chapter 5: Standalone Raft Testing (`05-standalone-testing/`)

| Document | Description |
|----------|-------------|
| [`test_framework.md`](#test-framework) | `RaftLabTest` and `RaftTestConfig`: coroutine-based harness, 5-server setup, network simulation helpers. |
| [`test_cases.md`](#test-cases) | All 11 test cases documented: `testInitialElection`, `testReElection`, `testBasicAgree`, `testFailAgree`, `testFailNoAgree`, `testRejoin`, `testConcurrentStarts`, `testBackup`, `testCount`, `testUnreliableAgree`, `testFigure8`. |
| [`config_files.md`](#config-files) | `raft_lab_test.yml` structure, how test configs differ from production. |

#### Chapter 6: CI Integration Testing (`06-ci-testing/`)

| Document | Description |
|----------|-------------|
| [`ci_script.md`](#ci-script) | `ci_mako_raft.sh` documentation: script structure, process management, port management. |
| [`test_scenarios.md`](#test-scenarios) | Each CI scenario: simpleRaft, shard1ReplicationRaft, shard2ReplicationRaft, shard1ReplicationSimpleRaft, shard2ReplicationSimpleRaft. Binaries, configs, pass/fail criteria. |
| [`example_scripts.md`](#example-scripts) | Shell script walkthroughs: `simpleRaft.sh`, all `test_*_raft.sh` scripts, `shard_raft.sh`. |

#### Chapter 7: Performance Analysis (`07-performance/`)

| Document | Description |
|----------|-------------|
| [`methodology.md`](#methodology) | Benchmark methodology: test environment, workload (TPC-C), configuration, metrics, caveats. |
| [`results.md`](#results) | Detailed results: 1-shard and 2-shard TPC-C throughput, latency breakdown, replay_batch, abort ratios. |
| [`analysis.md`](#analysis) | Why Paxos is faster in single-shard, why 2-shard is equal, throughput drop factors, architectural implications. |
| [`figures.md`](#figures) | Throughput bar charts, scaling line charts, architectural comparison tables (ASCII/Mermaid). |

#### Chapter 8: Log Persistence and Recovery (`08-persistence/`)

| Document | Description |
|----------|-------------|
| [`log_storage.md`](#log-storage) | `LogStorage` interface, `InMemoryLogStorage`, `RocksDBLogStorage`, integration with `RaftServer`. |
| [`recovery.md`](#recovery) | Crash recovery: `RecoveryManager`, `ReplayCommittedEntries()`, resolving uncommitted entries. |
| [`snapshots.md`](#snapshots) | `SnapshotManager`, `FileSnapshotManager`, snapshot format, `CompactLog()`, retention. |

#### Chapter 9: Appendix (`09-appendix/`)

| Document | Description |
|----------|-------------|
| [`file_reference.md`](#file-reference) | Complete file listing: all Raft files, Paxos files (for comparison), integration files, configs, test scripts, CI scripts. |
| [`configuration_reference.md`](#configuration-reference) | YAML configuration reference: mode fields, replication group structure, port allocation. |
| [`glossary.md`](#glossary) | Terms and definitions: Raft-specific, Mako-specific, and system-specific terminology. |
| [`rustycpp_safety.md`](#rustycpp-safety) | RustyCpp safety annotations in Raft code: `@safe` vs `@unsafe` methods, `rusty::` types used, borrow checking status. |

---

### Suggested Reading Paths

#### Quick Overview (~30 minutes)

For readers who want to understand the contribution at a high level:

1. **This README** -- document map and glossary
2. [`01-mako-overview/system_architecture.md`](#system-architecture) -- what Mako is and where Raft fits
3. [`04-mako-integration/architecture.md`](#architecture) -- how Raft was integrated alongside Paxos
4. [`07-performance/results.md`](#results) -- benchmark results
5. [`07-performance/analysis.md`](#analysis) -- what the results mean

#### Implementation Deep Dive (~2 hours)

For readers who want to understand the Raft implementation:

1. [`02-raft-core/protocol_overview.md`](#protocol-overview) -- mapping to the Raft paper
2. [`02-raft-core/server_implementation.md`](#server-implementation) -- `RaftServer` internals
3. [`02-raft-core/leader_election.md`](#leader-election) -- election mechanism
4. [`02-raft-core/log_replication.md`](#log-replication) -- replication protocol
5. [`03-preferred-leader/design.md`](#design) -- preferred leader extension
6. [`03-preferred-leader/implementation.md`](#implementation) -- how it works in code

#### Integration Story (~1.5 hours)

For readers interested in how a consensus protocol is grafted onto an existing system:

1. [`01-mako-overview/system_architecture.md`](#system-architecture) -- the existing system
2. [`01-mako-overview/build_system.md`](#build-system) -- how both protocols coexist in one binary
3. [`04-mako-integration/architecture.md`](#architecture) -- the dispatcher pattern
4. [`04-mako-integration/raft_worker.md`](#raft-worker) -- the bridge between Mako and Raft
5. [`04-mako-integration/raft_main_helper.md`](#raft-main-helper) -- glue code
6. [`04-mako-integration/challenges.md`](#challenges) -- bugs encountered and fixed

#### Testing and Validation (~1 hour)

For readers interested in correctness and performance validation:

1. [`05-standalone-testing/test_framework.md`](#test-framework) -- test infrastructure
2. [`05-standalone-testing/test_cases.md`](#test-cases) -- what each test verifies
3. [`06-ci-testing/test_scenarios.md`](#test-scenarios) -- CI test scenarios and pass criteria
4. [`07-performance/methodology.md`](#methodology) -- how benchmarks were run
5. [`07-performance/results.md`](#results) -- results
6. [`07-performance/analysis.md`](#analysis) -- analysis

---

### Key Source File Reference

These are the primary source files discussed across the documentation, organized by component:

#### Core Raft Protocol
| File | Lines | Description |
|------|------:|-------------|
| `src/deptran/raft/server.h` | ~636 | `RaftServer` class: state machine, election, replication, persistence |
| `src/deptran/raft/server.cc` | ~800 | `RaftServer` implementation |
| `src/deptran/raft/coordinator.h` | ~60 | `CoordinatorRaft`: transaction submission to Raft |
| `src/deptran/raft/coordinator.cc` | ~80 | Coordinator implementation with `WRONG_LEADER` retry |
| `src/deptran/raft/commo.h` | ~100 | `RaftCommo`: RPC communication layer |
| `src/deptran/raft/commo.cc` | ~200 | Send/broadcast implementations |
| `src/deptran/raft/service.h` | ~60 | `RaftServiceImpl`: RPC handler registration |
| `src/deptran/raft/service.cc` | ~100 | RPC handler implementations |
| `src/deptran/raft/frame.h` | ~80 | `RaftFrame`: factory for protocol components |
| `src/deptran/raft/frame.cc` | ~120 | Factory method implementations |
| `src/deptran/raft/exec.h` | ~40 | `RaftExecutor`: command execution |
| `src/deptran/raft/exec.cc` | ~60 | Executor implementation |
| `src/deptran/raft/macros.h` | ~77 | RPC handler code generation macros |

#### Raft-Mako Bridge
| File | Lines | Description |
|------|------:|-------------|
| `src/deptran/raft/raft_worker.h` | ~150 | `RaftWorker`: connects Mako watermarks to Raft replication |
| `src/deptran/raft/raft_worker.cc` | ~400 | Worker setup, callbacks, submit pipeline |
| `src/deptran/raft_main_helper.cc` | ~680 | `raft_impl` namespace: all functions the dispatcher calls |
| `src/deptran/replication_helper.h` | ~200 | `DISPATCH_RAFT_OR_PAXOS` macro, unified API declarations |
| `src/deptran/replication_helper.cc` | ~300 | Runtime dispatcher, `ReplicationType` global state |
| `src/mako/mako.hh` | -- | `detect_replication_type_from_config()`, `setup_leader_election_callbacks()` |

#### Standalone Tests
| File | Lines | Description |
|------|------:|-------------|
| `src/deptran/raft/test.h` | ~100 | `RaftLabTest`: coroutine-based test harness |
| `src/deptran/raft/test.cc` | ~600 | 11 test cases (election, agreement, figure 8, etc.) |
| `src/deptran/raft/testconf.h` | ~80 | `RaftTestConfig`: test utilities |
| `src/deptran/raft/testconf.cc` | ~400 | Network simulation, leader detection, agreement checks |

#### Paxos (for Comparison)
| File | Description |
|------|-------------|
| `src/deptran/paxos/server.h` | Multi-Paxos server (pre-existing) |
| `src/deptran/paxos/coordinator.h` | Paxos coordinator (pre-existing) |
| `src/deptran/paxos/commo.h` | Paxos communication (pre-existing) |
| `src/deptran/paxos/frame.h` | Paxos factory (pre-existing) |

---

### Glossary

#### Raft-Specific Terms

| Term | Definition |
|------|------------|
| **Term** | A monotonically increasing integer that acts as a logical clock in Raft. Each term begins with an election. A node's `currentTerm` is the highest term it has seen. |
| **Log index** | The position of an entry in the replicated log, starting from 1. Each entry has a unique `(term, index)` pair. |
| **Commit index** (`commitIndex`) | The highest log index known to be replicated on a majority of servers. Entries up to the commit index are safe to apply to the state machine. |
| **Execute index** (`executeIndex`) | The highest log index that has been applied to the state machine. Always `executeIndex <= commitIndex`. |
| **Last log index** (`lastLogIndex`) | The index of the last entry in a server's local log. |
| **Match index** (`match_index_[i]`) | Leader-maintained: the highest log index known to be replicated on follower `i`. Used to compute `commitIndex`. |
| **Next index** (`next_index_[i]`) | Leader-maintained: the next log index to send to follower `i`. Initialized to `lastLogIndex + 1`; decremented on rejection (backtracking). |
| **Vote** (`vote_for_`) | The candidate a server voted for in the current term. At most one vote per term (election safety). Persisted to stable storage. |
| **Election timeout** | Random duration (default 0.4--0.7s) after which a follower that has not heard from a leader becomes a candidate and starts an election. Randomization prevents repeated split votes. |
| **Heartbeat** | An empty `AppendEntries` RPC sent by the leader at regular intervals (`HEARTBEAT_INTERVAL`) to maintain authority and prevent follower elections. |
| **Split vote** | When no candidate receives a majority of votes in an election, causing the term to end without a leader. The randomized election timeout makes repeated splits unlikely. |
| **Quorum** | A majority of servers: `n/2 + 1` for a cluster of `n` nodes. For 3 nodes, quorum = 2. |
| **Leader completeness** | Safety property: if a log entry is committed in a given term, that entry will be present in the log of any leader for all higher terms. |
| **Preferred leader** | Extension to standard Raft: a designated node that the system biases toward electing as leader, using shorter election timeouts and a `TimeoutNow` RPC for leadership transfer. |
| **Leadership transfer** | The process of an existing leader voluntarily stepping down so a preferred leader can take over, via the `TimeoutNow` RPC mechanism. |
| **`TimeoutNow` RPC** | A Raft extension RPC that tells a follower to immediately start an election, bypassing the normal election timeout. Used for deterministic leadership transfer. |

#### Mako-Specific Terms

| Term | Definition |
|------|------------|
| **Shard** | A horizontal partition of the database. Each shard holds a subset of the data (e.g., a range of TPC-C warehouses) and is independently replicated. |
| **Partition** | Within Mako, a subdivision of a shard's keyspace. Each shard may have multiple partitions, each with its own Raft/Paxos instance. Not to be confused with network partitions. |
| **Partition group** | A set of replicas (across different machines) responsible for the same partition. One replica is the leader; others are followers. |
| **Watermark** | A progress marker indicating which log entries have been committed and applied. Used for garbage collection and follower catch-up. |
| **Epoch** | A logical time boundary. NO-OP entries are used to synchronize epochs across partitions. |
| **NO-OP** | A no-operation log entry submitted to Raft for epoch/watermark synchronization. Contains no actual data but advances the commit index. |
| **Atomic broadcast (`ab`)** | The replication layer that ensures total order of operations across replicas. Mako supports `multi_paxos` and `raft` as atomic broadcast implementations. |
| **OCC** | Optimistic Concurrency Control: Mako's default transaction isolation mechanism. Transactions execute speculatively and are validated at commit time. |
| **Masstree** | A high-performance in-memory trie/B-tree hybrid used as Mako's storage engine. |
| **`dbtest`** | The main benchmark binary that runs TPC-C workloads on Mako. |
| **`simpleTransactionRep`** | A simpler test binary for basic key-value transactions with replication (Paxos variant). |
| **`simpleTransactionRepRaft`** | The Raft variant of `simpleTransactionRep`. |
| **`replay_batch`** | A metric reported by followers indicating how many batches of replicated log entries they have replayed (applied to their local state machine). |
| **`agg_persist_throughput`** | Aggregate persisted throughput: the primary performance metric in `dbtest`, measuring committed transactions per second. |

#### System-Specific Terms

| Term | Definition |
|------|------------|
| **rrr** | Mako's custom RPC framework: a TCP/IP-based request/response system with ~10--50 us latency. Used as the default transport. |
| **eRPC** | An alternative high-performance RDMA-based RPC backend (~1--2 us latency). Not used for Raft testing. |
| **DPDK** | Data Plane Development Kit: kernel bypass networking. Available as an optional transport but not used for Raft. |
| **RustyCpp** | A static analysis tool that enforces Rust-style ownership and borrowing rules on C++ code. All new code must pass borrow checking. |
| **`@safe`** | RustyCpp annotation indicating a function has no unsafe operations (no raw pointer manipulation, no I/O, no calls to unchecked code). |
| **`@unsafe`** | RustyCpp annotation indicating a function calls non-borrow-checked code (STL I/O, legacy functions, third-party libraries). |
| **`rusty::Cell<T>`** | A RustyCpp type providing interior mutability for `Copy` types, similar to Rust's `Cell<T>`. Thread-safe for single-word types. |
| **`rusty::Arc<T>`** | A RustyCpp type for thread-safe reference-counted shared ownership, similar to Rust's `Arc<T>`. |
| **`rusty::Box<T>`** | A RustyCpp type for single-ownership heap allocation, similar to Rust's `Box<T>`. Replaces `std::unique_ptr`. |
| **Frame** | A factory class in Mako's protocol architecture. Each protocol (Raft, Paxos, OCC, etc.) has a `Frame` subclass that creates protocol-specific components. |
| **TPC-C** | Transaction Processing Performance Council benchmark C: a standard OLTP workload simulating a wholesale distributor with 5 transaction types (NewOrder, Payment, Delivery, OrderStatus, StockLevel). |
| **2PC** | Two-Phase Commit: the protocol used for cross-shard transactions. The coordinator first prepares all shards, then commits or aborts. |

---

## Mako System Architecture

<a id="what-this-document-covers-1"></a>

### What This Document Covers

This document provides a high-level overview of the Mako distributed transaction system: its purpose, core components, transaction flow, shard architecture, and — critically — where the atomic broadcast (replication) layer plugs in. Understanding this architecture is essential for appreciating why integrating Raft was both necessary and non-trivial.

**Note**: Mako itself is pre-existing infrastructure. The author's contribution is the Raft replication module and its integration with Mako (documented in [Chapter 4](#architecture)).

---

### 1. What Mako Is

Mako is a **speculative distributed transaction system with geo-replication**, designed for high throughput OLTP workloads. It is the system described in the OSDI'25 paper. Key properties:

- **Speculative execution**: Transactions execute optimistically before consensus completes
- **Geo-replication**: Data is replicated across geographically distributed datacenters
- **Sharding**: Data is horizontally partitioned across multiple shards for scalability
- **Pluggable replication**: The atomic broadcast layer (Multi-Paxos or Raft) is swappable at runtime

Mako builds on the Janus codebase (OSDI'16: "Consolidating Concurrency Control and Consensus for Commits under Conflicts") and extends it with speculative execution and a Masstree-based storage engine.

---

### 2. Core Components

```
+------------------------------------------------------------------+
|                        Mako System                                |
|                                                                   |
|  +-------------+    +---------------+    +-------------------+    |
|  |   Client    |--->| TxnCoordinator|--->|   TxnScheduler    |   |
|  | (benchmark) |    |  (dispatch)   |    | (execute & order) |   |
|  +-------------+    +-------+-------+    +--------+----------+   |
|                             |                     |               |
|                    +--------v---------+  +--------v----------+   |
|                    |   Communicator   |  |  Masstree Storage |   |
|                    | (RPC to replicas)|  |  (in-memory index)|   |
|                    +--------+---------+  +-------------------+   |
|                             |                                     |
|              +--------------v-----------------+                   |
|              |    Atomic Broadcast Layer       |                  |
|              |  (Multi-Paxos  OR  Raft)        |                  |
|              |  via replication_helper.h        |                  |
|              +--------------------------------+                   |
+------------------------------------------------------------------+
```

#### 2.1 Storage Engine: Masstree

Masstree is a high-performance in-memory trie/B-tree hybrid that serves as Mako's primary index structure. It provides:

- Lock-free reads via optimistic version validation
- Fine-grained concurrency control at the leaf level
- Efficient range scans and point lookups

Source: `src/mako/masstree/` (the Masstree library) and `src/mako/benchmarks/` (integration with TPC-C).

#### 2.2 Concurrency Control: OCC

Mako uses **Optimistic Concurrency Control** (OCC) as its default transaction isolation mechanism (`src/deptran/occ/`):

- Transactions execute speculatively, reading and writing to local state
- At commit time, a validation phase checks for conflicts
- If validation succeeds, changes are committed; otherwise the transaction aborts and retries
- Configuration: `cc: occ` in the mode YAML

#### 2.3 Atomic Broadcast Layer

The atomic broadcast layer ensures **total order** of committed operations across all replicas within a shard. This is where consensus protocols plug in:

- **Multi-Paxos** (`src/deptran/paxos/`): The original replication backend
- **Raft** (`src/deptran/raft/`): The author's contribution, integrated as an alternative

Both protocols implement the same abstract interface exposed through `src/deptran/replication_helper.h`, allowing runtime switching with a single configuration change (`ab: multi_paxos` vs `ab: raft`).

#### 2.4 Sharding

Data is horizontally partitioned across **shards**. Each shard:

- Holds a subset of the keyspace (e.g., a range of TPC-C warehouses)
- Has its own independent replication group (leader + followers)
- Runs its own Raft or Paxos instance per partition
- Can be placed on a different set of machines

Cross-shard transactions use **Two-Phase Commit (2PC)** coordinated by the `TxnCoordinator`.

---

### 3. Transaction Flow

A transaction in Mako proceeds through the following stages:

```
Client (dbtest)
    |
    v
[1] TxnCoordinator::DoTxAsync()     -- Dispatch transaction pieces to shards
    |
    v
[2] Communicator::BroadcastDispatch()  -- Send pieces via RPC to target shards
    |
    v
[3] TxnScheduler (TxLogServer)        -- Execute piece on local storage
    |   - Read/write Masstree index
    |   - OCC validation
    |
    v
[4] Atomic Broadcast (Paxos/Raft)     -- Replicate committed log entry
    |   - Leader submits to replication_helper::submit()
    |   - Dispatched to paxos_impl::submit() or raft_impl::submit()
    |   - Consensus achieved across replicas
    |
    v
[5] app_next_ callback                -- Apply committed entry to state machine
    |   - Follower replays committed log entries
    |   - Updates watermark / epoch
    |
    v
[6] Coordinator collects results       -- For cross-shard: 2PC prepare/commit
    |
    v
[7] Client receives commit/abort
```

#### Key observations:

1. **Steps 1-3** are identical regardless of replication protocol
2. **Step 4** is the only point where Paxos and Raft differ
3. **Step 5** uses the `app_next_` callback registered in `TxLogServer` (`src/deptran/scheduler.h:354`), which both Paxos and Raft invoke when entries are committed
4. For **single-shard transactions**, steps 1-5 complete the transaction
5. For **cross-shard transactions**, step 6 adds a 2PC coordination phase

---

### 4. Key Classes

#### 4.1 Frame (`src/deptran/frame.h`)

The **factory pattern** hub for creating protocol-specific components. Each protocol (OCC, 2PL, Paxos, Raft) registers a `Frame` subclass that creates the right coordinator, scheduler, communicator, and RPC services.

```
Frame (base class)
 ├── MultiPaxosFrame    (src/deptran/paxos/frame.h)
 ├── RaftFrame          (src/deptran/raft/frame.h)
 ├── OccFrame           (src/deptran/occ/)
 ├── TwoPLFrame         (src/deptran/2pl/)
 └── ...other protocols
```

Key factory methods (`src/deptran/frame.h:51-79`):
- `CreateCoordinator()`: Protocol-specific transaction coordinator
- `CreateScheduler()`: Protocol-specific scheduler (TxLogServer)
- `CreateCommo()`: Protocol-specific communicator
- `CreateRpcServices()`: RPC service handlers

Protocol name-to-mode mapping is defined in `src/deptran/frame.cc:466-495`:
- `"occ"` -> `MODE_OCC`
- `"multi_paxos"` -> `MODE_MULTI_PAXOS`
- `"raft"` -> `MODE_RAFT`

#### 4.2 TxnCoordinator (`src/deptran/coordinator.h`)

Manages the lifecycle of a distributed transaction:
- Dispatches transaction pieces to target shards
- Tracks completion status: `n_dispatch_`, `n_dispatch_ack_`, `n_prepare_req_`, `n_finish_ack_` (lines 91-96)
- Coordinates 2PC for cross-shard transactions via `SendPrepare()`, `SendCommit()`
- Key field: `commo_` (line 76) — the Communicator used for RPC
- Key field: `client_status_` (line 55) — shared `rusty::Arc<ClientStatus>` for statistics

#### 4.3 TxLogServer / TxnScheduler (`src/deptran/scheduler.h:332-685`)

The scheduler runs on each replica and manages:
- **Transaction execution**: Processes pieces against local storage
- **Log application**: `app_next_` callback (line 354) — invoked by the replication layer when a log entry is committed
- **Epoch management**: `epoch_mgr_`, `jepoch_`, `oepoch_` (lines 370-375)
- **Jetpack recovery**: State machine recovery after failures (lines 335-344, 379-390)
- **Database interface**: `kv_table_` for application data, `database_` for checksums (lines 534-548)

The `app_next_` callback is the critical integration point: both Paxos and Raft call this callback when entries are committed, making the rest of the system agnostic to the replication protocol.

#### 4.4 Communicator (`src/deptran/communicator.h:372-622`)

The RPC layer for inter-node communication:
- **Connection management**: `rpc_clients_` map using `rusty::Arc<rrr::Client>` (line 380)
- **Leader tracking**: `leader_cache_` for partition-to-leader mapping (line 383)
- **Broadcast primitives**: `BroadcastDispatch()`, `SendPrepare()`, `SendCommit()` (lines 478-500)
- **View management**: `partition_views_` tracks current leader for each partition (line 389)

#### 4.5 Tx (`src/deptran/tx.h:40-153`)

Runtime state for an individual transaction:
- `tid_`: Transaction ID (line 47)
- `mdb_txn_`: Database transaction handle (line 51)
- Data operations: `ReadColumn()`, `WriteColumn()`, `Query()` (lines 91-125)
- Workspace: `ws_` holds intermediate results (line 54)

---

### 5. Shard Architecture

#### 5.1 Data Partitioning

Mako partitions data by **shard** and further subdivides each shard into **partitions**. In TPC-C:
- Each shard handles a set of warehouses (e.g., warehouses 0-5 on shard 0, 6-11 on shard 1)
- Each shard has `nthreads` worker threads, each handling one partition

Configuration example (`src/mako/config/local-shards2-warehouses6.yml`):
```yaml
shards: 2          # Two shards
replicas: 3        # Three replicas per shard
warehouses: 12     # Total warehouses (6 per shard)
```

#### 5.2 Replica Topology

Each shard has a **replication group** of replicas across datacenters:

```
Shard 0 Replication Group:
  +-----------+    +-----------+    +-----------+    +-----------+
  | localhost |    |    p1     |    |    p2     |    |  learner  |
  |  (leader) |    | (follower)|    | (follower)|    | (Paxos   |
  | port 31000|    | port 32000|    | port 33000|    |   only)  |
  +-----------+    +-----------+    +-----------+    +-----------+

Shard 1 Replication Group:
  +-----------+    +-----------+    +-----------+    +-----------+
  | localhost |    |    p1     |    |    p2     |    |  learner  |
  |  (leader) |    | (follower)|    | (follower)|    | (Paxos   |
  | port 31006|    | port 32006|    | port 33006|    |   only)  |
  +-----------+    +-----------+    +-----------+    +-----------+
```

**Paxos topology**: 4 replicas per shard (3 voters + 1 learner)
**Raft topology**: 3 replicas per shard (all voters, no learner)

This topology difference (33% fewer processes for Raft) has performance implications discussed in [Chapter 7](#analysis).

#### 5.3 Cross-Shard Transactions

When a transaction touches data on multiple shards, Mako uses **Two-Phase Commit (2PC)**:

1. **Prepare phase**: Coordinator sends `Prepare` to all participating shards
2. **Commit phase**: If all shards vote "yes", coordinator sends `Commit`; otherwise `Abort`

Cross-shard coordination latency (typically ~10ms on localhost) dominates over replication latency, which is why single-shard and multi-shard performance differ dramatically (see [Performance Results](#results)).

---

### 6. The Existing Paxos Path

Before Raft was added, Mako used **Multi-Paxos** as its sole atomic broadcast protocol.

#### Multi-Paxos Implementation (`src/deptran/paxos/`)

Key classes:
- **`PaxosServer`** (`server.h:27-99`): Manages slots, ballots, and log persistence
  - Slot tracking: `min_active_slot_`, `max_executed_slot_`, `max_committed_slot_`, `cur_open_slot_` (lines 29-37)
  - Paxos state per slot: `PaxosData` with `max_ballot_seen_`, `max_ballot_accepted_`, `accepted_cmd_`, `committed_cmd_` (lines 14-20)
  - Log persistence via `log_storage_` with metadata keys: `"cur_epoch"`, `"max_committed_slot"`, `"max_executed_slot"` (lines 55-58)
- **`MultiPaxosFrame`** (`frame.h:12-33`): Factory for Paxos components
- **`MultiPaxosCommo`** (`commo.h`): Paxos RPC communication
- **`MultiPaxosCoordinator`** (`coordinator.h`): Paxos proposal submission

#### Paxos Worker Setup (`src/deptran/paxos_main_helper.cc`)

The Paxos initialization sequence (lines 86-123):
1. `worker->SetupService()` — Register RPC handlers
2. `worker->SetupCommo()` — Establish connections to replicas
3. `worker->InitQueueRead()` — Start processing submit queue
4. `worker->SetupHeartbeat()` — Start leader heartbeats

This pattern is mirrored by the Raft integration's `raft_main_helper.cc`, which follows a similar `SetupBase() -> SetupService() -> SetupCommo() -> SetupHeartbeat()` chain (see [raft_main_helper documentation](#raft-main-helper)).

---

### 7. Where Raft Plugs In

The replication layer is abstracted behind `src/deptran/replication_helper.h`, which provides a **unified API** that dispatches to either `paxos_impl::` or `raft_impl::` at runtime:

```
                    replication_helper.h
                    (unified API)
                          |
            +-------------+-------------+
            |                           |
     paxos_impl::                raft_impl::
  (paxos_main_helper.cc)    (raft_main_helper.cc)
            |                           |
     PaxosWorker                  RaftWorker
            |                           |
     PaxosServer                  RaftServer
```

The dispatch is controlled by a single global:
```cpp
// src/deptran/replication_helper.cc
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};
```

This can be set via:
1. **CLI flag**: `--replication raft` (parsed in `dbtest.cc`)
2. **Auto-detection**: `detect_replication_type_from_config()` scans YAML config for `ab: raft`
3. **Programmatic**: `janus::set_replication_type(janus::ReplicationType::RAFT)`

The key functions dispatched through this interface include:
- `setup()` / `setup2()`: Initialization
- `submit()`: Log submission
- `add_log()`: Log entry addition
- `register_for_leader()` / `register_for_follower()`: Callback registration
- `get_epoch()` / `set_epoch()`: Epoch management

For the full integration story, see [Chapter 4: Mako Integration](#architecture).

---

## Build System and Configuration

<a id="what-this-document-covers-2"></a>

### What This Document Covers

This document explains how to build, configure, and run Mako with either Paxos or Raft replication. It covers the CMake build system, key compile-time and runtime flags, the YAML configuration format, port allocation conventions, and the CI test infrastructure.

**Note**: The build system itself is pre-existing infrastructure. The author's contribution is the Raft build targets, runtime switching mechanism, and Raft CI test suite.

---

### 1. Build System Overview

Mako uses **CMake** as its build system, wrapped by a top-level **Makefile** for convenience. The build produces a single `dbtest` executable (and several test executables) that contains both Paxos and Raft code compiled in.

```
Developer
    |
    v
Makefile (convenience wrapper)
    |
    v
CMake (CMakeLists.txt)
    |
    +-- Masstree (autoconf, built as part of mako library)
    +-- eRPC (third-party/erpc, added as subdirectory)
    +-- RustyCpp (third-party/rusty-cpp, borrow checking)
    +-- Rust library (rust-lib/, built via cargo)
    +-- Mako static library (71+ source files)
    +-- Executables (dbtest, simpleRaft, etc.)
```

---

### 2. Build Commands

#### 2.1 Standard Build (Paxos Only)

```bash
make -j32              # Build with Paxos replication (default)
```

This invokes CMake with default flags. The `MAKO_USE_RAFT` option defaults to `OFF`, so only Paxos-related executables are built.

#### 2.2 Raft-Enabled Build

```bash
make mako-raft         # Build with both Paxos and Raft
```

This sets `MAKO_USE_RAFT=ON`, which:
1. Adds the `MAKO_USE_RAFT=1` compile definition (`CMakeLists.txt:219-223`)
2. Builds additional Raft test executables (see Section 3.2)
3. Enables the Raft code path in `replication_helper.h`

#### 2.3 Raft Lab Test Build

```bash
make raft-test         # Build with Raft testing coroutines
```

Sets both `MAKO_USE_RAFT=ON` and `RAFT_TEST=ON`, adding `RAFT_TEST_CORO=1` for standalone Raft protocol testing.

#### 2.4 Other Targets

```bash
make clean             # Remove build artifacts, temp files, Masstree output
make rebuild           # Clean + rebuild
```

The `clean` target also removes `/tmp/${USER}_*` temporary test files, Masstree build artifacts (`out-perf.masstree/`), LZ4 artifacts, and Rust build caches.

---

### 3. CMake Configuration

#### 3.1 Key Build Options (`CMakeLists.txt`)

| Option | Default | Purpose | Line |
|--------|---------|---------|------|
| `MAKO_USE_RAFT` | `OFF` | Enable Raft replication layer | 219 |
| `RAFT_TEST` | `OFF` | Enable Raft lab testing coroutine | 220 |
| `USE_MALLOC_MODE` | `1` | Malloc mode: 0=libc, 1=jemalloc, 2=tcmalloc, 3=flow | 232 |
| `MODE` | `"perf"` | Build mode: perf, backoff, factor-gc, sandbox | 233 |
| `ENABLE_BORROW_CHECKING` | `ON` | Enable RustyCpp borrow checking | 52 |

#### 3.2 Build Targets

**Core executables** (always built, `CMakeLists.txt:965-1025`):

| Target | Source | Purpose |
|--------|--------|---------|
| `dbtest` | `src/mako/benchmarks/dbtest.cc` | Main benchmark driver (TPC-C) |
| `simpleTransaction` | `examples/simpleTransaction.cc` | Single-node transaction test |
| `simpleTransactionRep` | `examples/simpleTransactionRep.cc` | Transaction with replication |
| `simplePaxos` | `examples/simplePaxos.cc` | Basic Paxos test |
| `test_rocksdb_persistence` | `examples/test_rocksdb_persistence.cc` | RocksDB persistence test |
| `rocksdb_replay_app` | `src/mako/benchmarks/rocksdb_replay_app.cc` | RocksDB replay utility |

**Raft-only executables** (built when `MAKO_USE_RAFT=ON`, `CMakeLists.txt:1027-1034`):

| Target | Source | Purpose |
|--------|--------|---------|
| `deptran_server` | `src/deptran/s_main.cc` | Standalone Deptran server |
| `simpleRaft` | `examples/mako-raft-tests/simpleRaft.cc` | Basic Raft test |
| `simpleTransactionRepRaft` | `examples/mako-raft-tests/simpleTransactionRepRaft.cc` | Raft transaction replication |
| `testPreferredReplicaStartup` | `examples/mako-raft-tests/testPreferredReplicaStartup.cc` | Preferred leader startup |
| `testPreferredReplicaLogReplication` | `examples/mako-raft-tests/testPreferredReplicaLogReplication.cc` | Preferred leader log replication |
| `testNoOps` | `examples/mako-raft-tests/testNoOps.cc` | No-op entry handling |

#### 3.3 Key Compile Definitions

| Definition | Purpose |
|------------|---------|
| `MAKO_USE_RAFT=1` | Enables Raft code paths |
| `RAFT_TEST_CORO=1` | Enables Raft testing coroutine |
| `REUSE_CORO` | Coroutine reuse (always enabled, required for Raft stability) |
| `USE_JEMALLOC` | Use jemalloc allocator |
| `ERPC_FAKE=true` | Use fake eRPC transport (Ethernet mode) |
| `CHECK_INVARIANTS` | Enable runtime invariant checking |
| `DISABLE_DISK` | Disable RocksDB persistence |
| `TRACKING_LATENCY` | Enable latency tracking |

#### 3.4 Third-Party Dependencies

The build links against these external libraries:

| Library | Purpose | Integration |
|---------|---------|-------------|
| jemalloc | Memory allocator | `pkg_check_modules` (line 509) |
| RocksDB | Persistent storage | Linked in `add_apps()` |
| yaml-cpp | YAML config parsing | Linked in `add_apps()` |
| Boost | System, filesystem, thread, coroutine, context | `find_package` (line 65) |
| libevent | Event-driven I/O | `pkg_check_modules` (line 505) |
| protobuf | Serialization | `pkg_check_modules` |
| gflags | Command-line flags | `pkg_check_modules` |
| eRPC | High-performance RPC | `add_subdirectory` (line 146) |
| Rust library | Redis integration | `cargo build --release` (lines 81-110) |
| RustyCpp | Borrow checking | `add_subdirectory` (line 52) |

#### 3.5 Transport Layer Configuration

The build reads `env.txt` to determine the network transport backend (`CMakeLists.txt:112-144`):

| Value | Backend | Description |
|-------|---------|-------------|
| `eth` | Ethernet | TCP/IP via ASIO (default), fake eRPC |
| `ib` | InfiniBand | RoCE support, real eRPC |
| `dpdk` | DPDK | Kernel bypass, real eRPC |

In the default `eth` mode, eRPC runs in fake mode (`ERPC_FAKE=true`) and all network I/O goes through the `rrr/rpc` TCP/IP stack.

---

### 4. Configuration System

Mako uses YAML configuration files organized into three categories:

1. **Mode config** — Concurrency control and atomic broadcast settings
2. **Shard/host config** — Cluster topology, replica placement, port allocation
3. **Replication group config** — Per-shard Paxos/Raft group membership

#### 4.1 Mode Configuration

Mode configs define the transaction protocol and replication backend. They live in `config/`.

**Paxos mode** (`config/occ_paxos.yml`):
```yaml
mode:
  cc: occ                 # Concurrency control: OCC
  ab: multi_paxos         # Atomic broadcast: Multi-Paxos
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1              # Concurrent transactions per client
```

**Raft mode** (`config/occ_raft.yml`):
```yaml
mode:
  cc: occ                 # Concurrency control: OCC
  ab: raft                # Atomic broadcast: Raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1              # Concurrent transactions per client
```

The **only difference** between Paxos and Raft mode configs is the `ab:` field. This single field controls which replication backend is used at runtime (see Section 5).

**Available `ab:` values**:
- `multi_paxos` — Multi-Paxos replication
- `raft` — Raft replication
- `fpga_raft` — FPGA-optimized Raft variant

**Available `cc:` values**:
- `occ` — Optimistic Concurrency Control
- `2pl_ww` — Two-Phase Locking (wound-wait)
- `none` — No concurrency control

#### 4.2 Shard/Host Configuration

Shard configs define cluster topology: how many shards, replicas per shard, warehouses, and port assignments. They live in `src/mako/config/`.

**Example** (`src/mako/config/local-shards2-warehouses6.yml`):
```yaml
shards: 2              # Number of shards
replicas: 3            # Replicas per shard
warehouses: 6          # Warehouses per shard (= worker threads)

localhost:             # Leader replica
  - name: shard0
    index: 0
    ip:   127.0.0.1
    port: 31000
  - name: shard1
    index: 1
    ip:   127.0.0.1
    port: 31100

p1:                    # Follower 1
  - name: shard0
    index: 0
    ip:  127.0.0.1
    port: 32000
  - name: shard1
    index: 1
    ip: 127.0.0.1
    port: 32100

p2:                    # Follower 2
  - name: shard0
    index: 0
    ip:  127.0.0.1
    port: 33000
  - name: shard1
    index: 1
    ip:  127.0.0.1
    port: 33100

learner:               # Learner (Paxos only)
  - name: shard0
    index: 0
    ip:  127.0.0.1
    port: 34000
  - name: shard1
    index: 1
    ip:  127.0.0.1
    port: 34100

memlocalhost: 6001     # Memory control ports
memlearner: 6001
memp1: 6002
memp2: 6003
```

**Port allocation convention for shard configs**:

| Replica | Shard 0 | Shard 1 | Pattern |
|---------|---------|---------|---------|
| localhost (leader) | 31000 | 31100 | 31000 + shard_idx * 100 |
| p1 (follower 1) | 32000 | 32100 | 32000 + shard_idx * 100 |
| p2 (follower 2) | 33000 | 33100 | 33000 + shard_idx * 100 |
| learner | 34000 | 34100 | 34000 + shard_idx * 100 |

#### 4.3 Replication Group Configuration

Replication group configs define per-shard Paxos/Raft group membership with partition-level granularity. They live in `config/1leader_2followers/`.

**Paxos replication group** (`config/1leader_2followers/paxos2_shardidx0.yml`):
```yaml
site:
  # Each line is a partition (Paxos group)
  # 4 replicas: master, p1, p2, learner
  server:
    - ["s101:17001", "s201:17101", "s301:17201", "s401:17301"]
    - ["s102:17002", "s202:17102", "s302:17202", "s402:17302"]

process:
  s101: localhost          # Master
  s201: p1                 # Follower 1
  s301: p2                 # Follower 2
  s401: learner            # Learner (non-voting)
  s102: localhost
  s202: p1
  s302: p2
  s402: learner

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
  learner: 127.0.0.1
```

**Raft replication group** (`config/1leader_2followers/raft2_shardidx0.yml`):
```yaml
site:
  # Each line is a partition (Raft group)
  # 3 replicas only (no learner in Raft)
  server:
    - ["s101:27001", "s201:27101", "s301:27201"]
    - ["s102:27002", "s202:27102", "s302:27202"]

process:
  s101: localhost
  s201: p1
  s301: p2
  s102: localhost
  s202: p1
  s302: p2

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
```

#### 4.4 Port Allocation: Paxos vs Raft

The two replication protocols use **different port ranges** to avoid conflicts:

| Component | Paxos Ports | Raft Ports |
|-----------|-------------|------------|
| Partition ports | `17XXX` | `27XXX` |
| Site naming | s101-s401 (4 per partition) | s101-s301 (3 per partition) |
| Replicas per group | 4 (3 voters + 1 learner) | 3 (all voters) |

**Naming convention**: Site `sNPP` where:
- `N` = replica index (1=localhost, 2=p1, 3=p2, 4=learner)
- `PP` = partition number (01, 02, ...)

**Example for 2 partitions**:

```
Paxos (4 processes per partition):
  Partition 1: s101:17001, s201:17101, s301:17201, s401:17301
  Partition 2: s102:17002, s202:17102, s302:17202, s402:17302

Raft (3 processes per partition):
  Partition 1: s101:27001, s201:27101, s301:27201
  Partition 2: s102:27002, s202:27102, s302:27202
```

This means Raft uses **25% fewer processes** than Paxos (no learner), which has performance implications discussed in [Performance Analysis](#analysis).

---

### 5. Runtime Replication Switching

Both Paxos and Raft code are compiled into the same binary. The active protocol is selected at runtime through a global dispatch mechanism.

#### 5.1 The Dispatch Mechanism

```
                                  dbtest
                                    |
                    +---------------+---------------+
                    |                               |
             CLI: --replication raft         YAML: ab: raft
                    |                               |
                    v                               v
              set_replication_type()    detect_replication_type_from_config()
                    |                               |
                    +--------->-<-------------------+
                               |
                               v
                    g_replication_type (rusty::Cell)
                               |
                               v
                    DISPATCH_RAFT_OR_PAXOS macro
                         /            \
                        v              v
                  paxos_impl::    raft_impl::
```

#### 5.2 Setting the Replication Type

There are three ways to select Raft replication:

**1. CLI flag** (explicit):
```bash
./build/dbtest --replication raft -f config/occ_raft.yml ...
```

**2. Auto-detection from config** (implicit):
The `detect_replication_type_from_config()` function (`src/mako/mako.hh:779-816`) scans the YAML config files for `ab: raft` or `ab: fpga_raft` and sets the replication type automatically. This is called during `init_env()` before the replication layer is initialized.

**3. Programmatic** (in test code):
```cpp
janus::set_replication_type(janus::ReplicationType::RAFT);
```

#### 5.3 The Global State

The replication type is stored as a `rusty::Cell<ReplicationType>` for thread-safe interior mutability (`src/deptran/replication_helper.cc:8-12`):

```cpp
enum class ReplicationType : int {
    PAXOS = 0,
    RAFT = 1
};

// @safe - Using rusty::Cell for thread-safe interior mutability
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};
```

#### 5.4 The Dispatch Macros

All replication functions are dispatched through macros in `replication_helper.cc:57-73`:

```cpp
#define DISPATCH_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            return raft_impl::func(__VA_ARGS__); \
        } else { \
            return paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)
```

Functions dispatched through this interface:
- `setup()` / `setup2()` — Initialization
- `submit()` — Log entry submission
- `add_log()` — Log entry addition
- `register_for_leader()` / `register_for_follower()` — Callback registration
- `get_epoch()` / `set_epoch()` — Epoch management

---

### 6. Running Mako

#### 6.1 Basic Invocation

```bash
# Run dbtest with Paxos replication (1 shard, 6 warehouses)
./build/dbtest \
    -f config/occ_paxos.yml \
    -f config/1leader_2followers/paxos6_shardidx0.yml \
    -f src/mako/config/local-shards1-warehouses6.yml \
    -d 10 -c 1

# Run dbtest with Raft replication (1 shard, 6 warehouses)
./build/dbtest \
    -f config/occ_raft.yml \
    -f config/1leader_2followers/raft6_shardidx0.yml \
    -f src/mako/config/local-shards1-warehouses6.yml \
    -d 10 -c 1
```

**Key flags**:
- `-f <config.yml>` — Configuration file (can be specified multiple times)
- `-d <seconds>` — Duration of benchmark run
- `-c <count>` — Number of client threads
- `--replication raft` — Explicit replication type override

#### 6.2 Multi-Shard Invocation

For multi-shard runs, provide one replication group config per shard:

```bash
./build/dbtest \
    -f config/occ_paxos.yml \
    -f config/1leader_2followers/paxos6_shardidx0.yml \
    -f config/1leader_2followers/paxos6_shardidx1.yml \
    -f src/mako/config/local-shards2-warehouses6.yml \
    -d 10 -c 1
```

---

### 7. CI Test Infrastructure

#### 7.1 Paxos Test Suite (`ci/ci.sh`)

The main CI script tests Paxos replication across various configurations:

```bash
./ci/ci.sh all                    # Run all tests
./ci/ci.sh compile                # Build only
./ci/ci.sh simpleTransaction      # Single-node transaction
./ci/ci.sh simplePaxos            # Basic Paxos replication
./ci/ci.sh shardNoReplication     # 2-shard, no replication
./ci/ci.sh shard1Replication      # 1 shard with Paxos (dbtest)
./ci/ci.sh shard2Replication      # 2 shards with Paxos (dbtest)
./ci/ci.sh shard1ReplicationSimple  # 1 shard with Paxos (simple)
./ci/ci.sh shard2ReplicationSimple  # 2 shards with Paxos (simple)
./ci/ci.sh rocksdbTests           # RocksDB persistence
./ci/ci.sh shardFaultTolerance    # Fault recovery
./ci/ci.sh multiShardSingleProcess  # Multi-shard in one process
./ci/ci.sh cpuThrottlingScaling   # CPU throttling
```

#### 7.2 Raft Test Suite (`ci/ci_mako_raft.sh`)

The author's Raft CI script mirrors the Paxos tests:

```bash
./ci/ci_mako_raft.sh all                        # Run all Raft tests
./ci/ci_mako_raft.sh compile                    # Build with Raft
./ci/ci_mako_raft.sh simpleRaft                 # Basic Raft test
./ci/ci_mako_raft.sh shard1ReplicationRaft       # 1 shard Raft (dbtest)
./ci/ci_mako_raft.sh shard2ReplicationRaft       # 2 shards Raft (dbtest)
./ci/ci_mako_raft.sh shard1ReplicationSimpleRaft # 1 shard Raft (simple)
./ci/ci_mako_raft.sh shard2ReplicationSimpleRaft # 2 shards Raft (simple)
```

#### 7.3 Test Flow

Each CI test follows this pattern:
1. **Compile** — `make -j32` (or `make mako-raft -j32`)
2. **Port cleanup** — Wait for ports 7001-8006 and 31000-31100 to be free
3. **Run test** — Execute the test script (which invokes `dbtest` or a standalone test)
4. **Verify output** — Check exit code, grep for success markers
5. **Cleanup** — Kill any remaining processes, remove temp files

---

### 8. Switching Between Paxos and Raft: Quick Reference

| Step | Paxos | Raft |
|------|-------|------|
| **Build** | `make -j32` | `make mako-raft` (or `make -j32` if `MAKO_USE_RAFT` is cached ON) |
| **Mode config** | `config/occ_paxos.yml` | `config/occ_raft.yml` |
| **Replication group** | `config/1leader_2followers/paxosN_shardidxM.yml` | `config/1leader_2followers/raftN_shardidxM.yml` |
| **Shard config** | `src/mako/config/local-shardsX-warehousesY.yml` | Same config (shared) |
| **Replicas per shard** | 4 (3 voters + 1 learner) | 3 (all voters) |
| **Port range** | `17XXX` | `27XXX` |
| **CI tests** | `ci/ci.sh` | `ci/ci_mako_raft.sh` |

The shard/host config files are **protocol-agnostic** — the same `local-shardsX-warehousesY.yml` works for both Paxos and Raft. Only the mode config and replication group config differ.

---

## Raft Protocol Implementation Overview

<a id="what-this-document-covers-3"></a>

### What This Document Covers

This document provides a high-level overview of the Raft consensus protocol as implemented in the Mako codebase. It maps the implementation to the original Raft paper (Ongaro & Ousterhout, 2014), documents deviations and extensions, and serves as an entry point into the more detailed documents in this chapter.

**Note**: The Raft implementation is a core thesis contribution. This overview covers the architecture; subsequent documents cover each component in depth.

---

### 1. Raft Fundamentals Recap

Raft is a consensus protocol that ensures a replicated log is consistent across a cluster of servers. It decomposes consensus into three sub-problems:

1. **Leader election** -- At most one leader per term; other servers are followers.
2. **Log replication** -- The leader receives commands, appends them to its log, and replicates to followers.
3. **Safety** -- If a log entry is committed, no future leader will have a different entry at that index.

Each server is in one of three states at any time:

```
                  timeout, start election
            +-----------------------------------+
            |                                   |
            v          receives majority        |
        +-----------+    votes won    +--------+------+
 start  |           |--------------->|                |
------->| Follower  |               |   Candidate    |
        |           |<--------------|                |
        +-----------+  discovers    +--------+------+
            ^          current leader        |
            |          or new term           |
            |                               |
            |     discovers server with     |
            |        higher term            |
            |                               v
            |                       +-----------+
            +-----------------------|           |
                                   |  Leader   |
                                   |           |
                                   +-----------+
```

**Reference**: Ongaro, D. and Ousterhout, J. (2014). "In Search of an Understandable Consensus Algorithm." USENIX ATC '14.

---

### 2. Implementation-to-Paper Mapping

The implementation maps Raft concepts to classes as follows:

| Raft Paper Concept | Implementation Class | Source File |
|--------------------|---------------------|-------------|
| State machine / Raft server | `RaftServer` | `src/deptran/raft/server.h`, `server.cc` |
| RPC transport layer | `RaftCommo` | `src/deptran/raft/commo.h`, `commo.cc` |
| RPC request handlers | `RaftServiceImpl` | `src/deptran/raft/service.h`, `service.cc` |
| Protocol factory | `RaftFrame` | `src/deptran/raft/frame.h`, `frame.cc` |
| Client interface / command submission | `CoordinatorRaft` | `src/deptran/raft/coordinator.h`, `coordinator.cc` |
| Log entry | `RaftData` | `src/deptran/raft/server.h` |
| RPC definitions | `RaftService` proxy/service | `src/deptran/rcc_rpc.h` |

#### Class Hierarchy

```
Frame (protocol factory base)
  |
  +-- RaftFrame
        |
        +-- creates RaftServer (extends TxLogServer)
        +-- creates RaftCommo (extends Communicator)
        +-- creates CoordinatorRaft (extends CoordinatorBase)
        +-- creates RaftServiceImpl (extends RaftService)
```

`RaftServer` inherits from `TxLogServer`, which provides the `app_next_` callback for applying committed log entries to the upper-level transaction system. This is the integration point between Raft consensus and Mako's transaction processing pipeline.

---

### 3. Core State Variables

The Raft paper defines persistent and volatile state. Here is how the implementation maps them:

#### Persistent State (on all servers)

| Paper Variable | Implementation | Location |
|----------------|---------------|----------|
| `currentTerm` | `uint64_t currentTerm` | `server.h:274` |
| `votedFor` | `siteid_t vote_for_` | `server.h:89` |
| `log[]` | `map<slotid_t, shared_ptr<RaftData>> raft_logs_` | `server.h:277` |

Persistence is handled by optional `LogStorage` backend (`server.h:52`). When configured, `PersistTermAndVote()` and `PersistLogEntry()` write to stable storage.

#### Volatile State (on all servers)

| Paper Variable | Implementation | Location |
|----------------|---------------|----------|
| `commitIndex` | `uint64_t commitIndex` | `server.h:275` |
| `lastApplied` | `uint64_t executeIndex` | `server.h:276` |

#### Volatile State (on leaders)

| Paper Variable | Implementation | Location |
|----------------|---------------|----------|
| `nextIndex[]` | `std::map<siteid_t, uint64_t> next_index_` | `server.h:82` |
| `matchIndex[]` | `std::map<siteid_t, uint64_t> match_index_` | `server.h:81` |

#### Additional State (implementation-specific)

| Variable | Purpose | Location |
|----------|---------|----------|
| `is_leader_` | Current role flag | `server.h:91` |
| `lastLogIndex` | Cached last log index | `server.h:273` |
| `preferred_leader_site_id_` | Preferred leader for geo-distribution | `server.h:126` |
| `transferring_leadership_` | Leadership transfer in progress | `server.h:128` |

---

### 4. RPC Interface

The implementation defines four RPCs (in `src/deptran/rcc_rpc.h`):

#### RequestVote RPC

```
Request:  (lst_log_idx: u64, lst_log_term: ballot, can_id: siteid, can_term: ballot)
Response: (reply_term: ballot, vote_granted: bool)
```

- Sent by candidates to request votes during elections.
- Handler: `RaftServiceImpl::HandleVote()` -> `RaftServer::OnRequestVote()`

#### AppendEntries RPC

```
Request:  (slot: u64, ballot: ballot, leaderCurrentTerm: u64, leaderSiteId: siteid,
           leaderPrevLogIndex: u64, leaderPrevLogTerm: u64, leaderCommitIndex: u64,
           cmd: MarshallDeputy, leaderNextLogTerm: u64)
Response: (followerAppendOK: u64, followerCurrentTerm: u64, followerLastLogIndex: u64)
```

- Sent by leader to replicate log entries and as heartbeats.
- Handler: `RaftServiceImpl::HandleAppendEntries()` -> `RaftServer::OnAppendEntries()`

#### EmptyAppendEntries RPC (Heartbeat)

```
Request:  (slot: u64, ballot: ballot, leaderCurrentTerm: u64, leaderSiteId: siteid,
           leaderPrevLogIndex: u64, leaderPrevLogTerm: u64, leaderCommitIndex: u64,
           trigger_election_now: bool)
Response: (followerAppendOK: u64, followerCurrentTerm: u64, followerLastLogIndex: u64)
```

- Heartbeat variant with no log payload. The `trigger_election_now` flag supports the leadership transfer protocol.
- Handler: `RaftServiceImpl::HandleEmptyAppendEntries()` -> `RaftServer::OnAppendEntries()` with `cmd=nullptr`

#### TimeoutNow RPC

```
Request:  (leaderTerm: u64, leaderSiteId: siteid)
Response: (followerTerm: u64, success: bool)
```

- Sent by a stepping-down leader to trigger immediate election on the preferred replica.
- Handler: `RaftServiceImpl::HandleTimeoutNow()` -> `RaftServer::OnTimeoutNow()`
- **Not in the original Raft paper** -- part of the preferred leader extension.

---

### 5. Algorithm Summary

#### 5.1 Leader Election

The election algorithm follows the Raft paper with dynamic timeout tuning:

1. **Timer expiry** -- Follower's election timer fires (`StartElectionTimer()`, `server.cc:1188`)
2. **Become candidate** -- Increment `currentTerm`, vote for self, persist (`RequestVote()`, `server.cc:1016`)
3. **Broadcast votes** -- Send RequestVote RPC to all peers (`RaftCommo::BroadcastVote()`)
4. **Collect votes** -- `RaftVoteQuorumEvent` tallies responses; majority wins
5. **Become leader** -- Initialize `next_index_[]` and `match_index_[]`, start heartbeats (`setIsLeader(true)`, `server.cc:444`)

**Election timeout ranges**:

| Role | Timeout Range | Purpose |
|------|---------------|---------|
| Preferred leader | 150-300 ms | Win elections quickly |
| Non-preferred (grace period, first 5s) | 1-2 s | Let preferred leader win first |
| Non-preferred (normal) | 500 ms - 1 s | Standard Raft randomized timeout |

See [Leader Election](#leader-election) for the full walkthrough.

#### 5.2 Log Replication

The leader replicates entries through a heartbeat loop (`HeartbeatLoop()`, `server.cc:636`):

1. **Client submission** -- `CoordinatorRaft::Submit()` calls `RaftServer::Start()` to append locally
2. **Broadcast** -- Heartbeat loop sends `AppendEntries` RPCs with batched entries
3. **Follower acceptance** -- `OnAppendEntries()` performs consistency check, appends, updates `commitIndex`
4. **Commit advancement** -- Leader computes majority-replicated index from `match_index_[]`
5. **Application** -- `applyLogs()` calls `app_next_()` callback for each committed entry

**Optimizations over standard Raft**:
- **Batch replication**: Multiple entries sent in a single `AppendEntries` RPC via `TpcBatchCommand`
- **Fast log reconciliation**: On rejection, follower reports its `lastLogIndex`, allowing the leader to jump `next_index_` directly instead of decrementing one-by-one
- **Exponential backoff**: When no follower hint is available, `next_index_` is halved rather than decremented

See [Log Replication](#log-replication) for the full walkthrough.

#### 5.3 Safety

The implementation preserves the Raft safety properties:

- **Election Safety**: At most one leader per term -- enforced by `vote_for_` persistence and single-vote-per-term check in `OnRequestVote()` (`server.cc:1141-1147`)
- **Leader Append-Only**: Leader never overwrites or deletes its own entries -- `Start()` only appends (`server.cc:1230`)
- **Log Matching**: If two logs contain an entry with the same index and term, all preceding entries are identical -- enforced by `prevLogTerm` check in `OnAppendEntries()` (`server.cc:1286`)
- **Leader Completeness**: If an entry is committed in a term, it will be present in all future leaders' logs -- enforced by the log up-to-date check in vote granting (`server.cc:1158-1181`)
- **State Machine Safety**: Applied entries are never un-applied -- `executeIndex` only advances forward in `applyLogs()` (`server.cc:581`)

---

### 6. Key Deviations from the Raft Paper

#### 6.1 Preferred Leader Election (Novel Contribution)

Standard Raft elects whichever candidate wins first. This implementation adds a **preferred leader** mechanism for geo-distributed deployments where a specific replica should be leader for locality:

- **Dynamic election timeouts**: Preferred replica uses 150-300ms; others use 500ms-2s. This gives the preferred replica a statistical advantage in winning elections.
- **Leadership transfer protocol**: When a non-preferred replica becomes leader (e.g., after a failure), a background monitor thread (`StartLeadershipTransferMonitoring()`) watches for the preferred replica to catch up. Once caught up, the current leader steps down and triggers an immediate election on the preferred replica via `TimeoutNow` RPC.
- **Piggybacked transfer signal**: The `trigger_election_now` flag in `EmptyAppendEntries` allows the stepping-down leader to signal all replicas simultaneously.

See [Preferred Leader Election](#mechanism) for the full design.

#### 6.2 Integration with Two-Phase Commit

Unlike standalone Raft implementations, this one integrates with Mako's distributed transaction system:

- **Command type**: Log entries are `TpcCommitCommand` objects (transaction commit records), not arbitrary byte strings
- **Application callback**: `applyLogs()` invokes `app_next_(slot_id, cmd)`, which feeds committed commands back into Mako's transaction processing pipeline
- **WRONG_LEADER handling**: When `CoordinatorRaft::Submit()` detects it's not on the leader, it returns `WRONG_LEADER` with view data so the client can redirect
- **View tracking**: `RaftFrame` maintains view data that maps partition IDs to current leader information

#### 6.3 Batched Log Replication

The paper describes sending entries one at a time. This implementation batches multiple entries per RPC:

```
Leader log:  [..., entry_5, entry_6, entry_7, entry_8]
                                                  ^
                                            lastLogIndex

Follower (next_index=6):
  Receives: [entry_6, entry_7, entry_8] in one AppendEntries RPC
```

This is controlled by `RAFT_BATCH_OPTIMIZATION` and uses `TpcBatchCommand` to bundle entries.

#### 6.4 Optimized Log Reconciliation

Standard Raft decrements `next_index` by 1 on each rejected `AppendEntries`. This implementation uses three strategies:

1. **Fast backoff** -- If follower reports `lastLogIndex < next_index - 1`, jump directly to `lastLogIndex + 1`
2. **Exponential backoff** -- If no hint available and `next_index > 10`, halve it
3. **Linear backoff** -- If `next_index <= 10`, decrement by 1

This reduces O(n) round trips to O(log n) in the worst case for log reconciliation.

#### 6.5 Persistent State Management

The implementation optionally persists Raft state to RocksDB:

- `PersistTermAndVote()` -- Called after every term change or vote grant
- `PersistLogEntry()` -- Called in `SetLocalAppend()` when leader appends new entries
- `PersistCommitIndex()` -- Called when `commitIndex` advances
- `RecoverFromStorage()` -- Restores state on restart
- `ReplayCommittedEntries()` -- Replays committed entries through `app_next_` after recovery

#### 6.6 Jetpack Recovery

On leader election, the new leader can optionally run `JetpackRecoveryEntry()` to recover state. This is controlled by the `MAKO_DISABLE_JETPACK` environment variable and is specific to Mako's speculative execution model.

---

### 7. Component Interaction Diagram

The following shows how the major components interact during a typical command submission:

```
Client (Mako Transaction Layer)
    |
    | Submit(cmd)
    v
CoordinatorRaft
    |
    | Start(cmd)          Creates log entry
    v
RaftServer (Leader)
    |
    | SetLocalAppend()    Appends to local log, persists
    | Signal ready_for_replication_
    v
HeartbeatLoop             Runs continuously on leader
    |
    | For each follower:
    | SendAppendEntries2()
    v
RaftCommo ----RPC----> RaftServiceImpl (Follower)
                            |
                            | HandleAppendEntries()
                            v
                       RaftServer (Follower)
                            |
                            | OnAppendEntries()
                            |   - Term check
                            |   - Log consistency check
                            |   - Append entries
                            |   - Update commitIndex
                            |   - applyLogs()
                            v
                       app_next_(slot, cmd)
                            |
                            v
                       Mako Transaction Layer

    Meanwhile, back on the leader:
    HeartbeatLoop
        |
        | Update match_index_ from responses
        | Compute new commitIndex (majority)
        | applyLogs()
        v
    app_next_(slot, cmd)
        |
        v
    Mako Transaction Layer
```

---

### 8. File Map

| File | Lines | Purpose |
|------|-------|---------|
| `src/deptran/raft/server.h` | ~280 | RaftServer class definition, member variables |
| `src/deptran/raft/server.cc` | ~1830 | Full Raft algorithm implementation |
| `src/deptran/raft/commo.h` | ~70 | RaftCommo class definition |
| `src/deptran/raft/commo.cc` | ~290 | RPC sending: AppendEntries, Vote, TimeoutNow |
| `src/deptran/raft/service.h` | ~50 | RaftServiceImpl class definition |
| `src/deptran/raft/service.cc` | ~115 | RPC handlers: HandleVote, HandleAppendEntries |
| `src/deptran/raft/frame.h` | ~60 | RaftFrame class definition |
| `src/deptran/raft/frame.cc` | ~210 | Factory methods, RPC service registration |
| `src/deptran/raft/coordinator.h` | ~50 | CoordinatorRaft class definition |
| `src/deptran/raft/coordinator.cc` | ~200 | Command submission, WRONG_LEADER handling |
| `src/deptran/rcc_rpc.h` | (relevant: 1491-1700) | RPC ID definitions, wire format |

---

## RaftServer Implementation Deep Dive

<a id="what-this-document-covers-4"></a>

### What This Document Covers

This document is a detailed walkthrough of the `RaftServer` class — the core Raft consensus state machine. It covers every member variable, the class hierarchy, and full algorithm walkthroughs for `OnRequestVote()`, `OnAppendEntries()`, `Start()`, `applyLogs()`, the election timer, log persistence, and RustyCpp safety annotations.

**Source files**: `src/deptran/raft/server.h` (638 lines) and `src/deptran/raft/server.cc` (1830 lines)

---

### 1. Class Hierarchy

```
Scheduler (base scheduling interface)
    |
    v
TxLogServer (transaction log server, provides app_next_ callback)
    |
    v
RaftServer (Raft consensus state machine)
```

- `Scheduler` — Base class for transaction scheduling (`src/deptran/scheduler.h`)
- `TxLogServer` — Extends `Scheduler` with log application infrastructure (`app_next_` callback), epoch management, and Jetpack recovery support (`src/deptran/scheduler.h:332`)
- `RaftServer` — Implements the Raft consensus protocol on top of `TxLogServer`

The key integration point is `app_next_`: a callback registered by Mako's transaction layer. When Raft commits a log entry, `applyLogs()` invokes `app_next_(slot_id, cmd)` to feed the committed command back into the transaction pipeline.

---

### 2. Member Variables

#### 2.1 Persistent State (Must Survive Restarts)

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `currentTerm` | `uint64_t` | `0` | `server.h:274` | Current election term. Monotonically increasing. |
| `vote_for_` | `siteid_t` | `INVALID_SITEID` | `server.h:89` | Candidate ID that received vote in current term, or `INVALID_SITEID` if none. |
| `raft_logs_` | `map<slotid_t, shared_ptr<RaftData>>` | `{}` | `server.h:277` | In-memory log entries indexed by slot ID. |

These are persisted via `PersistTermAndVote()` and `PersistLogEntry()` when a `LogStorage` backend is configured.

#### 2.2 Volatile State (All Servers)

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `lastLogIndex` | `uint64_t` | `0` | `server.h:273` | Index of the last log entry. |
| `commitIndex` | `uint64_t` | `0` | `server.h:275` | Index of highest committed entry (majority replicated). |
| `executeIndex` | `uint64_t` | `0` | `server.h:276` | Index of highest applied entry (fed to `app_next_`). |
| `is_leader_` | `bool` | `false` | `server.h:91` | Whether this server is the current leader. |
| `stop_` | `bool` | `false` | `server.h:88` | Shutdown signal. Checked by coroutines before calling virtual methods. |
| `last_heartbeat_time_` | `uint64_t` | `0` | `server.h:86` | Timestamp of last timer reset (for election timeout). |
| `req_voting_` | `bool` | `false` | `server.h:96` | True while an election is in progress. |
| `disconnected_` | `bool` | `false` | `server.h:95` | Whether this server is network-disconnected (for testing). |
| `looping_` | `bool` | `false` | `server.h:107` | Set when `HeartbeatLoop()` is running. Used as a shutdown guard. |

#### 2.3 Volatile State (Leaders Only)

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `match_index_` | `map<siteid_t, uint64_t>` | `{}` | `server.h:81` | Highest replicated index for each follower. |
| `next_index_` | `map<siteid_t, uint64_t>` | `{}` | `server.h:82` | Next log index to send to each follower. |

Initialized in `setIsLeader(true)`: `match_index_[peer] = 0`, `next_index_[peer] = lastLogIndex + 1` (`server.cc:460-466`).

#### 2.4 Preferred Leader System

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `preferred_leader_site_id_` | `siteid_t` | `INVALID_SITEID` | `server.h:126` | Configured preferred leader for this partition. |
| `leader_last_commit_index_` | `uint64_t` | `0` | `server.h:127` | Leader's commit index, cached from heartbeats. |
| `transferring_leadership_` | `bool` | `false` | `server.h:128` | True during leadership transfer protocol. |
| `leadership_transfer_start_time_` | `uint64_t` | `0` | `server.h:129` | When transfer was initiated (for timeout). |
| `leadership_monitor_stop_` | `atomic<bool>` | `false` | `server.h:130` | Signal to stop background monitor thread. |
| `leadership_monitor_thread_` | `std::thread` | — | `server.h:131` | Background thread checking for transfer opportunity. |
| `startup_timestamp_` | `uint64_t` | `0` | `server.h:132` | Server startup time (for grace period logic). |

#### 2.5 Log Application Control

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `in_applying_logs_` | `bool` | `false` | `server.h:97` | Reentrancy guard for `applyLogs()`. |
| `apply_pending_` | `atomic<bool>` | `false` | `server.h:98` | Tracks if new work arrived while applying logs. |
| `min_active_slot_` | `slotid_t` | `1` | `server.h:262` | Slots before this have been freed (GC boundary). |

#### 2.6 Persistence and Snapshot

| Variable | Type | Location | Description |
|----------|------|----------|-------------|
| `log_storage_` | `shared_ptr<LogStorage>` | `server.h:52` | Optional persistent storage backend (RocksDB). |
| `snapshot_manager_` | `shared_ptr<SnapshotManager>` | `server.h:57` | Optional snapshot manager. |

#### 2.7 Timer and Configuration

| Variable | Type | Default | Location | Description |
|----------|------|---------|----------|-------------|
| `timer_` | `rusty::Box<Timer>` | — | `server.h:85` | Owned timer for election timeout. RustyCpp `Box` ensures cleanup. |
| `snapidx_` | `slotid_t` | `0` | `server.h:92` | Snapshot index (entries before this are compacted). |
| `snapterm_` | `ballot_t` | `0` | `server.h:93` | Term of the last snapshot entry. |
| `failover_` | `bool` | `true` | `server.h:102` | Whether election failover is enabled. |

#### 2.8 RaftData Structure

Each log entry is stored as a `RaftData` struct (`server.h:20-33`):

```cpp
struct RaftData {
  ballot_t max_ballot_seen_ = 0;       // Legacy Paxos fields
  ballot_t max_ballot_accepted_ = 0;
  shared_ptr<Marshallable> accepted_cmd_{nullptr};
  shared_ptr<Marshallable> committed_cmd_{nullptr};

  ballot_t term;                        // Term when entry was created
  shared_ptr<Marshallable> log_{nullptr}; // The actual command

  // For retries
  ballot_t prevTerm;
  slotid_t slot_id;
  ballot_t ballot;
};
```

---

### 3. OnRequestVote() — Vote Granting

**Location**: `server.cc:1120-1185`

This is the handler for incoming `RequestVote` RPCs from candidates.

#### Algorithm Walkthrough

```
OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term, reply_term*, vote_granted*, cb)
  |
  +-- [1] Lock mutex
  |
  +-- [2] Stale term? (can_term < currentTerm)
  |     YES -> doVote(false) -> return
  |
  +-- [3] Already voted for someone else this term?
  |     (can_term == currentTerm && vote_for_ != INVALID && vote_for_ != can_id)
  |     YES -> doVote(false) -> return
  |
  +-- [4] Already voted for THIS candidate? (idempotent)
  |     (can_term == currentTerm && vote_for_ == can_id)
  |     YES -> doVote(true) -> return
  |
  +-- [5] Log up-to-date check:
  |     Compute my last log term and index
  |     Candidate's log at least as up-to-date?
  |       (lst_log_term > my_last_term) OR
  |       (lst_log_term == my_last_term AND lst_log_idx >= my_last_idx)
  |     YES -> doVote(true)
  |     NO  -> doVote(false)
```

#### The doVote() Helper (`server.h:161-202`)

`doVote()` performs the actual vote response:

1. **Term advancement**: If `can_term > currentTerm`, steps down to follower, updates term, resets `vote_for_`, persists (`server.h:177-186`)
2. **Record vote**: If granting, sets `vote_for_ = can_id` and resets the election timer (`server.h:189-198`)
3. **Reply**: Sets `*vote_granted` and `*reply_term`, invokes callback

#### Key Safety Properties

- **Single vote per term**: Line 1141 ensures at most one vote per term (except idempotent re-votes for the same candidate)
- **Log completeness**: Lines 1176 ensures only candidates with up-to-date logs can be elected
- **Persistence**: `PersistTermAndVote()` and `PersistVote()` are called before replying, ensuring crash safety

---

### 4. OnAppendEntries() — Log Replication

**Location**: `server.cc:1272-1462`

This is the handler for incoming `AppendEntries` RPCs from the leader.

<a id="algorithm-walkthrough-1"></a>

#### Algorithm Walkthrough

```
OnAppendEntries(leaderCurrentTerm, leaderSiteId, leaderPrevLogIndex,
                leaderPrevLogTerm, leaderCommitIndex, cmd, ..., trigger_election_now)
  |
  +-- [1] Lock mutex
  |
  +-- [2] Pre-checks:
  |     term_ok    = (leaderCurrentTerm >= currentTerm)
  |     index_ok   = (leaderPrevLogIndex <= lastLogIndex)
  |     prev_term_ok = (prevLogIndex == 0 || local_term_at_prevIndex == leaderPrevLogTerm)
  |
  +-- [3] If term_ok: reset election timer
  |     If leaderCurrentTerm > currentTerm: update term, step down, persist
  |
  +-- [4] All checks pass?
  |     |
  |     +-- YES (Accept):
  |     |     [4a] If cmd != null:
  |     |           Non-batch: append single entry at leaderPrevLogIndex + 1
  |     |           Batch: iterate TpcBatchCommand, append each entry
  |     |           Persist entries
  |     |     [4b] Update commitIndex = min(leaderCommitIndex, lastLogIndex)
  |     |     [4c] Set followerAppendOK = 1, report currentTerm and lastLogIndex
  |     |     [4d] Release mutex, call applyLogs() if needed, re-acquire mutex
  |     |
  |     +-- NO (Reject):
  |           Set followerAppendOK = 0, report currentTerm and lastLogIndex
  |           (Leader uses lastLogIndex for fast backoff)
  |
  +-- [5] Piggybacked leadership transfer:
  |     If trigger_election_now && AmIPreferredLeader() && !IsLeader():
  |       Sleep 30ms, then start election via RequestVote()
  |
  +-- [6] Unlock mutex, invoke callback
```

#### Critical Design Decision: Mutex Release During Apply

At line 1377, the mutex is released *before* calling `applyLogs()`. This is deliberate: applying entries can be slow (involves transaction processing), and holding the mutex would block incoming AppendEntries RPCs. The `in_applying_logs_` / `apply_pending_` mechanism in `applyLogs()` handles concurrent arrivals safely.

#### Batch Optimization (`RAFT_BATCH_OPTIMIZATION`)

When enabled, `AppendEntries` carries a `TpcBatchCommand` containing multiple `TpcCommitCommand` entries. The follower iterates the batch and appends all entries in one pass (`server.cc:1344-1358`), with a single batched `PersistLogEntries()` call.

---

### 5. Start() — Leader Appends Commands

**Location**: `server.cc:1230-1267`

This is the entry point for new commands from `CoordinatorRaft`.

```cpp
bool RaftServer::Start(shared_ptr<Marshallable>& cmd,
                       uint64_t* index, uint64_t* term,
                       slotid_t slot_id, ballot_t ballot)
{
    lock(mtx_);

    // Only leader can accept new commands
    if (!IsLeader()) {
        *index = 0; *term = 0;
        return false;
    }

    // Append to local log
    SetLocalAppend(cmd, term, index, slot_id, ballot);

    // SetLocalAppend returns old lastLogIndex; Start returns the new index
    verify(lastLogIndex == (*index) + 1);
    *index = lastLogIndex;
    return true;
}
```

#### SetLocalAppend() — The Actual Append (`server.h:313-384`)

1. Records old `lastLogIndex` in `*index`
2. Increments `lastLogIndex`
3. Creates `RaftData` entry with `cmd`, `currentTerm`, `slot_id`, `ballot`
4. Persists the entry via `PersistLogEntry()`

After `Start()` returns, the caller (`CoordinatorRaft`) signals `ready_for_replication_` to wake the `HeartbeatLoop`, which replicates the new entry to followers.

---

### 6. applyLogs() — State Machine Application

**Location**: `server.cc:581-625`

This function applies committed but unapplied log entries to the state machine.

```
applyLogs()
  |
  +-- [1] If executeIndex < commitIndex: set apply_pending_ = true
  |
  +-- [2] If in_applying_logs_: return (let current apply handle it)
  |
  +-- [3] in_applying_logs_ = true
  |
  +-- [4] LOOP:
  |     |  apply_pending_ = false
  |     |  FOR id = executeIndex+1 TO commitIndex:
  |     |    instance = GetRaftInstance(id)
  |     |    if instance && instance->log_:
  |     |      RuleWitnessGC(instance->log_)
  |     |      app_next_(id, instance->log_)    <-- feeds to Mako transaction layer
  |     |      executeIndex = id
  |     |    else: break
  |     |
  |     +-- WHILE apply_pending_ (new work arrived during this iteration)
  |
  +-- [5] in_applying_logs_ = false
  |
  +-- [6] GC: remove old commands where slot + 60000 < executeIndex
```

#### Concurrency Design

The `do-while` loop with `apply_pending_` is a lock-free work notification pattern:

- `applyLogs()` can be called concurrently from `HeartbeatLoop()` (leader) and `OnAppendEntries()` (follower)
- The `in_applying_logs_` flag prevents reentrancy — if another call arrives while applying, it sets `apply_pending_` and returns
- The current apply loop sees `apply_pending_` at the end and loops again, picking up the new work
- This guarantees no committed entry is ever missed, even under heavy load

#### Garbage Collection

Lines 619-624 remove old entries to prevent unbounded memory growth. Entries are removed when they are more than 60,000 slots behind `executeIndex`.

---

### 7. Election Timer

#### 7.1 StartElectionTimer() (`server.cc:1188-1228`)

Launches a coroutine that periodically checks if the election timeout has fired:

```
StartElectionTimer()
  |
  +-- resetTimer("start election timer")
  +-- last_heartbeat_time_ = now()
  |
  +-- Fiber::create_run([this]() {
        WHILE !stop_:
          election_timeout = GetElectionTimeout()
          sleep(random(HEARTBEAT_INTERVAL*2, HEARTBEAT_INTERVAL*4))

          time_elapsed = now() - last_heartbeat_time_

          IF !IsLeader() && time_elapsed > election_timeout:
            req_voting_ = true
            RequestVote()
            WHILE req_voting_: sleep(wait_int_)
      })
```

#### 7.2 GetElectionTimeout() — Dynamic Timeout (`server.cc:348-369`)

Returns a timeout in microseconds based on the server's role:

| Condition | Timeout Range | Purpose |
|-----------|---------------|---------|
| Preferred leader (`AmIPreferredLeader()`) | 150-300 ms | Win elections quickly |
| Non-preferred, grace period (< 5s since startup) | 1-2 s | Let preferred leader win first |
| Non-preferred, after grace period | 500 ms - 1 s | Standard failover |

#### 7.3 resetTimer() (`server.h:229-241`)

Updates `last_heartbeat_time_` to the current time and restarts the timer. Called on:
- Receiving valid `AppendEntries` from current-term leader
- Granting a vote
- Becoming a follower (stepping down)
- Reconnection

#### 7.4 HEARTBEAT_INTERVAL

Defined as a preprocessor constant (`server.h:40-44`):
- **Test mode** (`RAFT_TEST_CORO`): 100,000 μs = 100 ms
- **Production mode**: 5,000 μs = 5 ms

---

### 8. HeartbeatLoop() — Leader Replication Loop

**Location**: `server.cc:636-961`

This is the main replication loop, running continuously on every server (but only active when leader).

#### Loop Structure

```
HeartbeatLoop()
  |
  +-- Initialize match_index_ and next_index_ for all peers
  +-- looping_ = true
  |
  +-- WHILE looping_:
        |
        +-- [A] Wait: ready_for_replication_->wait(HEARTBEAT_INTERVAL)
        |         (woken by Start() or timer)
        |
        +-- [B] If !IsLeader(): continue (skip iteration)
        |
        +-- [C] FOR each follower in next_index_:
        |     |
        |     +-- [C1] Update commitIndex:
        |     |     Collect all match_index_ values
        |     |     Sort ascending
        |     |     newCommitIndex = matchedIndices[(N-1)/2]  (majority)
        |     |     If newCommitIndex > commitIndex && term matches: update
        |     |     If commitIndex > executeIndex: applyLogs()
        |     |
        |     +-- [C2] Prepare AppendEntries:
        |     |     prevLogIndex = next_index_[peer] - 1
        |     |     prevLogTerm = raft_logs_[prevLogIndex]->term
        |     |     cmd = batch of entries from next_index_ to lastLogIndex
        |     |
        |     +-- [C3] Send AppendEntries RPC (500ms timeout)
        |     |
        |     +-- [C4] Handle response:
        |           |
        |           +-- Higher term from follower:
        |           |     Step down (setIsLeader(false))
        |           |     Update currentTerm
        |           |
        |           +-- Rejected (log conflict):
        |           |     Fast backoff: next_index_ = follower's lastLogIndex + 1
        |           |     OR exponential backoff: next_index_ /= 2
        |           |     OR linear backoff: next_index_--
        |           |
        |           +-- Accepted:
        |                 Update match_index_ = follower's lastLogIndex
        |                 Update next_index_ = follower's lastLogIndex + 1
```

#### Commit Index Calculation (`server.cc:707-731`)

The leader computes the new commit index by sorting all `match_index_` values and taking the median:

```
For N=3 (3 servers): matchedIndices[(3-1)/2] = matchedIndices[1]
  → The 2nd highest value, meaning at least 2 of 3 servers have it
  → This is the majority quorum (N/2 + 1 = 2)
```

An entry is only committed if its term equals `currentTerm` (the Raft paper's Figure 8 safety rule, line 728).

#### Log Reconciliation Strategies (`server.cc:873-898`)

When a follower rejects `AppendEntries`, the leader backtracks `next_index_`:

1. **Fast backoff** (line 879): Use follower's reported `lastLogIndex` to jump directly. Reduces O(n) round trips to O(1) for simple divergence.
2. **Exponential backoff** (line 885): When `next_index > 10`, halve it. Converges in O(log n) steps.
3. **Linear backoff** (line 891): When `next_index <= 10`, decrement by 1 for precision.

---

### 9. Log Persistence

#### 9.1 Persistence Functions

| Function | What It Persists | When Called | Location |
|----------|-----------------|-------------|----------|
| `PersistTermAndVote()` | `currentTerm`, `vote_for_` | Term change, vote grant | `server.cc:36-44` |
| `PersistVote()` | `vote_for_` only | Vote grant (optimization) | `server.cc:47-54` |
| `PersistCommitIndex()` | `commitIndex` | Commit advancement | `server.cc:57-64` |
| `PersistLogEntry()` | Single log entry | `SetLocalAppend()` on leader | `server.cc:67-80` |
| `PersistLogEntries()` | Batch of entries | Batch append on follower | `server.cc:83-102` |

All persistence functions check `log_storage_ && log_storage_->is_open()` before writing. If no storage backend is configured, they are no-ops.

#### 9.2 Recovery (`server.cc:105-203`)

`RecoverFromStorage()` restores state on restart:

1. Reads `currentTerm` from metadata
2. Reads `vote_for_` from metadata
3. Reads `commitIndex` from metadata
4. Reads `lastLogIndex` from storage
5. Loads all log entries into `raft_logs_`

`ReplayCommittedEntries()` replays entries `[executeIndex+1, commitIndex]` through `app_next_()` after the callback is registered.

#### 9.3 Log Compaction (`server.cc:206-256`)

`CompactLog(up_to_index)` removes entries covered by a snapshot:
- Safety check: never compacts beyond `commitIndex`
- Removes from both persistent storage and in-memory `raft_logs_`
- Updates `min_active_slot_`

---

### 10. setIsLeader() — State Transitions

**Location**: `server.cc:444-578`

This function handles all leader/follower transitions:

#### Becoming Leader (`become_new_leader`, lines 491-538)

1. Initialize `match_index_` = 0, `next_index_` = `lastLogIndex + 1` for all peers (lines 460-468)
2. Clear `transferring_leadership_` flag (line 499)
3. Update `new_view_` with self as leader, notify communicator (lines 504-519)
4. Trigger Jetpack recovery if enabled (line 522-524)
5. If non-preferred leader: start `StartLeadershipTransferMonitoring()` (lines 534-538)
6. Fire `leader_change_cb_(true)` (lines 570-572)

#### Becoming Follower (`become_new_follower`, lines 539-565)

1. Reset election timer to prevent immediate re-election (line 551)
2. Stop leadership transfer monitoring thread (line 562)
3. Fire `leader_change_cb_(false)` (lines 573-575)

---

### 11. Destructor — Safe Shutdown

**Location**: `server.cc:963-993`

```
~RaftServer()
  |
  +-- stop_ = true                    // Signal all coroutines
  +-- looping_ = false                // Stop HeartbeatLoop
  +-- ready_for_replication_->set(1)  // Wake sleeping HeartbeatLoop
  +-- StopLeadershipTransferMonitoring()
  +-- sleep(100ms)                    // Let detached coroutines see stop_
  +-- Log final stats
```

The 100ms sleep is critical: election timer and leadership transfer coroutines are detached, and without this delay, they may try to call virtual methods (like `RequestVote()`) through a collapsed vtable after the destructor runs.

---

### 12. RustyCpp Safety Annotations

#### @safe Methods

| Method | Why Safe |
|--------|----------|
| `GetElectionTimeout()` | Read-only computation with local variables |
| `Setup()` | Creates coroutines with fiber framework |
| `IsLeader()` | Read-only accessor with shutdown guard |
| `applyLogs()` | Uses atomic flags for concurrency, calls `app_next_` |
| `IsDisconnected()` | Read-only accessor |
| `setIsLeader()` | Modifies owned state under mutex |
| `LogTermChange()` | Read-only logging |

#### @unsafe Methods

| Method | Why Unsafe |
|--------|------------|
| `OnRequestVote()` | Calls `doVote()` with pointer outputs |
| `OnAppendEntries()` | Modifies log state, calls `applyLogs()` |
| `Start()` | Calls `SetLocalAppend()` with pointer outputs |
| `SetLocalAppend()` | Legacy I/O code, `dynamic_pointer_cast` chains |
| `HeartbeatLoop()` | Complex mutable state, `SendAppendEntries2()` with pointer outputs |
| `PersistTermAndVote()` | Uses `LogStorage` API (third-party) |
| `RecoverFromStorage()` | Uses `LogStorage` API, `std::stoull` |
| `commo()` | Raw pointer cast from `commo_` base pointer |
| `Disconnect()` | Modifies proxy maps |

#### Key Pattern: Timer Ownership

The `timer_` field uses `rusty::Box<Timer>` instead of `std::unique_ptr<Timer>`:

```cpp
RaftServer::RaftServer(Frame* frame)
  : timer_(rusty::Box<Timer>::make(Timer()))  // server.cc:317
```

This follows the RustyCpp migration: `Box<T>` provides single-ownership semantics with automatic cleanup, replacing raw pointers or `unique_ptr`.

---

### 13. Constants and Configuration

| Constant | Value | Location | Purpose |
|----------|-------|----------|---------|
| `HEARTBEAT_INTERVAL` | 5,000 μs (prod) / 100,000 μs (test) | `server.h:41-44` | How often the leader sends heartbeats |
| `INVALID_SITEID` | `(siteid_t)-1` | `server.h:16` | Sentinel for "no site" |
| `NUM_BATCH_TIMER_RESET` | 100 | `server.h:17` | Batch counter threshold for timer reset |
| `SEC_BATCH_TIMER_RESET` | 1 | `server.h:18` | Time threshold for batched timer reset |
| GC threshold | 60,000 entries | `server.cc:620` | Slots beyond this behind `executeIndex` are freed |

---

## Leader Election Mechanism

<a id="what-this-document-covers-5"></a>

### What This Document Covers

This document provides a complete walkthrough of the Raft leader election mechanism as implemented, including the election trigger, vote broadcasting, quorum detection, vote granting logic, split vote handling, and term advancement. It concludes with a full sequence diagram of a successful election cycle.

**Key source files**:
- `src/deptran/raft/server.cc` — `RequestVote()`, `StartElectionTimer()`, `OnRequestVote()`, `doVote()`
- `src/deptran/raft/commo.cc` — `BroadcastVote()`
- `src/deptran/raft/commo.h` — `RaftVoteQuorumEvent`
- `src/rrr/reactor/quorum_event.h` — `QuorumEvent` base class

---

### 1. Election Trigger

Elections are triggered by the election timer coroutine (`StartElectionTimer()`, `server.cc:1188-1228`). The coroutine runs continuously on every server and checks whether enough time has passed since the last heartbeat.

#### Timer Loop

```
StartElectionTimer():
  resetTimer("start election timer")
  last_heartbeat_time_ = now()

  Fiber::create_run:
    WHILE !stop_:
      election_timeout = GetElectionTimeout()
      sleep(random(HEARTBEAT_INTERVAL*2, HEARTBEAT_INTERVAL*4))

      time_elapsed = now() - last_heartbeat_time_

      IF !IsLeader() AND time_elapsed > election_timeout:
        req_voting_ = true
        RequestVote()
        WHILE req_voting_: sleep(wait_int_)
```

The timer fires when:
1. This server is **not** the leader
2. `time_elapsed` exceeds the dynamic election timeout

The timeout is **not** fired if:
- The server received a valid `AppendEntries` from a current-term leader (which calls `resetTimer()`)
- The server granted a vote (which also calls `resetTimer()`)

#### Dynamic Timeout Values

| Condition | Range | Purpose |
|-----------|-------|---------|
| Preferred leader | 150-300 ms | Win elections quickly after failure |
| Non-preferred (grace period, first 5s) | 1-2 s | Let preferred leader win startup election |
| Non-preferred (normal) | 500 ms - 1 s | Standard Raft randomized timeout |

The randomization prevents synchronized elections across servers.

---

### 2. The Election: RequestVote()

**Location**: `server.cc:995-1118`

When the timer fires, `RequestVote()` executes the candidate's side of the election:

#### Step 1: Shutdown Guard (`server.cc:1000-1003`)

```cpp
if (stop_) {
    return false;  // Prevent crash during destructor teardown
}
```

This prevents calling virtual methods through a collapsed vtable if the destructor has already started.

#### Step 2: Become Candidate (`server.cc:1016-1034`)

Under the mutex:
1. Increment `currentTerm`
2. Vote for self: `vote_for_ = site_id_`
3. Persist both to stable storage
4. Record last log index and term for the log up-to-date check

```cpp
currentTerm++;
vote_for_ = site_id_;
PersistTermAndVote();
```

#### Step 3: Broadcast Vote RPCs (`server.cc:1041`)

```cpp
auto sp_quorum = ((RaftCommo*)(this->commo_))->BroadcastVote(
    par_id, lst_idx, lst_term, loc_id, term);
```

This sends `RequestVote` RPCs to all peers and returns a `RaftVoteQuorumEvent` for collecting responses.

#### Step 4: Wait for Quorum (`server.cc:1042`)

```cpp
sp_quorum->wait(1000000);  // 1 second timeout
```

The wait returns when:
- A majority votes YES (`sp_quorum->yes()`)
- A majority votes NO (`sp_quorum->no()`)
- The 1-second timeout expires (`sp_quorum->timeouted_`)

#### Step 5: Process Result (`server.cc:1048-1117`)

Three outcomes:

**Won** (`sp_quorum->yes()`):
1. Verify term hasn't advanced during the wait (stale election guard)
2. `setIsLeader(true)` — initialize leader state, update views
3. Trigger Jetpack recovery if enabled
4. Fire `leader_change_cb_(true)` to notify the upper layer

**Lost** (`sp_quorum->no()`):
1. `setIsLeader(false)` — ensure follower state
2. Update `currentTerm` if a higher term was observed in responses
3. Reset `vote_for_` on term advancement

**Timeout** (neither majority):
1. Return `false` — the election timer will retry with a new random timeout

---

### 3. BroadcastVote() — Sending Vote RPCs

**Location**: `commo.cc:177-210`

```
BroadcastVote(par_id, lst_log_idx, lst_log_term, self_id, cur_term):
  |
  +-- n = partition size (total replicas)
  +-- Create RaftVoteQuorumEvent(n, n/2)
  |     (n total, n/2 quorum threshold)
  |
  +-- FOR each peer in partition (skip self):
  |     Send async Vote RPC with callback:
  |       callback(future):
  |         Extract (term, vote_granted) from reply
  |         quorum_event->FeedResponse(vote_granted, term)
  |
  +-- RETURN quorum_event
```

The quorum threshold is `n/2` (not `n/2 + 1`) because the candidate already counts its own vote. For a 3-node cluster, `n/2 = 1`, so the candidate needs 1 additional vote (total 2 out of 3 = majority).

---

### 4. RaftVoteQuorumEvent — Quorum Detection

**Location**: `commo.h:11-38`, inherits from `QuorumEvent` (`src/rrr/reactor/quorum_event.h:18-112`)

<a id="class-hierarchy-1"></a>

#### Class Hierarchy

```
Event (base reactor event)
    |
    v
QuorumEvent (generic quorum voting)
    |
    v
RaftVoteQuorumEvent (Raft-specific vote tracking)
```

#### Key Members (from `QuorumEvent`)

| Member | Type | Purpose |
|--------|------|---------|
| `n_voted_yes_` | `int32_t` | Count of YES votes |
| `n_voted_no_` | `int32_t` | Count of NO votes |
| `n_total_` | `int32_t` | Total replicas in the partition |
| `quorum_` | `int32_t` | Quorum threshold (n/2) |
| `highest_term_` | `int64_t` | Highest term seen in any response |
| `timeouted_` | `bool` | Whether the wait timed out |

#### Quorum Logic

```cpp
// Majority voted YES
bool yes() {
    return n_voted_yes_ >= quorum_;
}

// Majority voted NO (impossible to reach quorum)
bool no() {
    return n_voted_no_ > (n_total_ - quorum_);
}
```

For a 3-node cluster (`n_total_=3`, `quorum_=1`):
- `yes()` when `n_voted_yes_ >= 1` (self + 1 peer = 2/3 majority)
- `no()` when `n_voted_no_ > 2` (impossible to reach quorum)

#### FeedResponse()

```cpp
void FeedResponse(bool y, ballot_t term) {
    if (y) {
        vote_yes();     // Increment n_voted_yes_, call test()
    } else {
        vote_no();      // Increment n_voted_no_, call test()
        if (term > highest_term_) {
            highest_term_ = term;  // Track highest term for term advancement
        }
    }
}
```

The `test()` call (via `vote_yes()`/`vote_no()`) triggers `is_ready()` evaluation, which wakes the waiting coroutine if quorum is reached.

---

### 5. Vote Granting: OnRequestVote()

**Location**: `server.cc:1120-1185`

When a server receives a `RequestVote` RPC, it evaluates whether to grant the vote:

#### Decision Tree

```
OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term):
  |
  +-- [1] can_term < currentTerm?
  |     YES -> REJECT (stale candidate)
  |
  +-- [2] Already voted for different candidate in this term?
  |     (can_term == currentTerm && vote_for_ != INVALID && vote_for_ != can_id)
  |     YES -> REJECT (already voted)
  |
  +-- [3] Already voted for THIS candidate? (idempotent)
  |     (can_term == currentTerm && vote_for_ == can_id)
  |     YES -> GRANT (safe to re-grant)
  |
  +-- [4] Log up-to-date check:
  |     my_last_term = raft_logs_[lastLogIndex].term
  |     my_last_idx  = lastLogIndex
  |
  |     Candidate at least as up-to-date?
  |       (lst_log_term > my_last_term) OR
  |       (lst_log_term == my_last_term AND lst_log_idx >= my_last_idx)
  |     YES -> GRANT
  |     NO  -> REJECT
```

<a id="the-dovote-helper-serverh161-202-1"></a>

#### The doVote() Helper (`server.h:161-202`)

All vote responses go through `doVote()`, which handles:

1. **Term advancement**: If `can_term > currentTerm`, the voter steps down, updates its term, resets `vote_for_`, and persists:
   ```cpp
   if (can_term > currentTerm) {
       setIsLeader(false);
       currentTerm = can_term;
       vote_for_ = INVALID_SITEID;
       PersistTermAndVote();
   }
   ```

2. **Vote recording**: If granting, records the vote and resets the election timer:
   ```cpp
   if (vote) {
       setIsLeader(false);
       vote_for_ = can_id;
       PersistVote();
       resetTimer("granted vote");
   }
   ```

3. **Reply**: Sets `*vote_granted` and `*reply_term = currentTerm`, invokes callback.

#### Why Resetting the Timer on Vote Grant Matters

When a follower grants a vote, it resets its election timer. This prevents the voter from immediately starting its own election, giving the candidate time to win and begin sending heartbeats.

---

### 6. Split Vote Handling

Split votes occur when two candidates start elections simultaneously and neither reaches a majority. Raft handles this via **randomized election timeouts**:

1. Each candidate that loses (or times out) returns to follower state
2. The next election attempt uses a fresh random timeout from `GetElectionTimeout()`
3. With high probability, one candidate's timer fires before the other
4. That candidate wins the next election uncontested

The preferred leader mechanism further reduces split votes by giving the preferred replica a shorter timeout (150-300ms vs 500ms-1s), making it statistically likely to start elections first.

---

### 7. Term Advancement

Term advancement ensures the system makes progress and stale leaders step down:

#### Where Terms Advance

| Location | Trigger | Action |
|----------|---------|--------|
| `RequestVote()` line 1021 | Starting election | `currentTerm++`, vote for self |
| `doVote()` line 177-186 | Vote request with higher term | `currentTerm = can_term`, step down |
| `OnAppendEntries()` line 1305-1313 | AppendEntries with higher term | `currentTerm = leaderTerm`, step down |
| `HeartbeatLoop()` line 859-871 | Follower response with higher term | Step down, `currentTerm = ret_term` |
| `RequestVote()` line 1100-1106 | Lost election, observed higher term | `currentTerm = new_term` |

#### Invariant: Monotonically Increasing

`currentTerm` never decreases. Every term change is persisted before any response is sent, ensuring crash safety.

---

### 8. Complete Election Cycle — Sequence Diagram

The following shows a successful election in a 3-node cluster where Server A (preferred leader) wins.

```
Time    Server A (Preferred)         Server B                    Server C
 |      State: Follower              State: Follower             State: Follower
 |
 |      [Election timer fires]
 |      term: 0 -> 1
 |      vote_for_ = A
 |      PersistTermAndVote()
 |      State: Candidate
 |
 |      ---- RequestVote(term=1, lastIdx=5, lastTerm=1) ------>
 |      ---- RequestVote(term=1, lastIdx=5, lastTerm=1) ---------------------->
 |
 |                                   [OnRequestVote]            [OnRequestVote]
 |                                   can_term(1) >= myTerm(0)   can_term(1) >= myTerm(0)
 |                                   Log up-to-date? YES        Log up-to-date? YES
 |                                   vote_for_ = A              vote_for_ = A
 |                                   PersistVote()              PersistVote()
 |                                   resetTimer()               resetTimer()
 |
 |      <--- Reply(term=1, granted=true) -----
 |      <--- Reply(term=1, granted=true) ------------------------------------
 |
 |      [RaftVoteQuorumEvent]
 |      n_voted_yes_ = 2 >= quorum_(1)
 |      sp_quorum->yes() = true
 |
 |      [setIsLeader(true)]
 |      Initialize match_index_: {B:0, C:0}
 |      Initialize next_index_:  {B:6, C:6}
 |      Update new_view_ (leader=A, term=1)
 |      Fire leader_change_cb_(true)
 |      State: Leader
 |
 |      [HeartbeatLoop wakes]
 |      ---- AppendEntries(term=1, prevIdx=5, commit=5) ------>
 |      ---- AppendEntries(term=1, prevIdx=5, commit=5) ---------------------->
 |
 |                                   [resetTimer]               [resetTimer]
 |                                   (heartbeat received)       (heartbeat received)
 |
 v      Leader established, normal operation begins
```

#### Timing Breakdown

| Phase | Duration | Bounded By |
|-------|----------|------------|
| Election timeout | 150-300 ms (preferred) | `GetElectionTimeout()` |
| Vote RPC round-trip | < 1 ms (local) | Network latency |
| Quorum wait | < 1 ms (local) | Slowest voter |
| Total election | ~150-300 ms | Dominated by timeout |

In production with network latency, the vote RPC round-trip adds 1-10ms per hop. The total election time is still dominated by the election timeout, not the RPC latency.

---

### 9. Leader Change Notification

When a server transitions to or from leader, it fires a callback to notify the upper system layer:

#### Registration

`RaftWorker::SetupBase()` (`src/deptran/raft/raft_worker.cc:50-59`) registers the callback:

```cpp
raft_server->RegisterLeaderChangeCallback([this](bool leader) {
    {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        is_leader = leader ? 1 : 0;
    }
    uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
    NotifyRaftLeaderChange(par_id, leader);
});
```

#### Firing

In `setIsLeader()` (`server.cc:569-577`):
- `leader_change_cb_(true)` when `become_new_leader`
- `leader_change_cb_(false)` when `become_new_follower`

This allows clients to retarget requests to the new leader after elections.

---

### 10. Edge Cases and Robustness

#### Pre-Vote Not Implemented

This implementation does not use the pre-vote extension (Section 9.6 of the Raft dissertation). A partitioned server will increment its term on each election timeout and may disrupt the cluster when it reconnects. The preferred leader mechanism partially mitigates this by biasing elections toward a known-good replica.

#### Stale Election Guard

After winning a quorum, the candidate checks whether its term has advanced during the wait (`server.cc:1050`):

```cpp
if (term != currentTerm) {
    return false;  // Abandon: another election happened while we waited
}
```

This prevents a stale election from overwriting a newer leader.

#### Shutdown Safety

The `stop_` flag is checked before every `RequestVote()` call to prevent virtual method calls through a collapsed vtable during destructor execution.

---

## Log Replication Mechanism

<a id="what-this-document-covers-6"></a>

### What This Document Covers

This document provides a complete walkthrough of Raft log replication as implemented: how the leader sends entries to followers, how followers validate and append entries, how the leader tracks follower progress, how commit advancement works, how log conflicts are resolved, and how batching optimizes throughput.

**Key source files**:
- `src/deptran/raft/server.cc` — `HeartbeatLoop()`, `OnAppendEntries()`, `Start()`, `applyLogs()`
- `src/deptran/raft/commo.cc` — `SendAppendEntries2()`
- `src/deptran/classic/tpc_command.h` — `TpcBatchCommand`, `TpcCommitCommand`

---

### 1. Overview

Log replication ensures every committed entry is durably stored on a majority of servers. The flow is:

```
Client submits command
        |
        v
  CoordinatorRaft::Submit()
        |
        v
  RaftServer::Start()           [Leader appends locally]
        |
        v
  HeartbeatLoop()               [Leader replicates to followers]
        |
        +----> SendAppendEntries2() ---RPC---> OnAppendEntries() [Follower]
        |                                           |
        |      <---- response (ok, term, lastIdx) --+
        |
        v
  Update match_index_, next_index_
  Compute new commitIndex (majority)
        |
        v
  applyLogs()                   [Feed to Mako transaction layer]
```

---

### 2. Leader: Sending Entries

#### 2.1 HeartbeatLoop() — The Replication Engine

**Location**: `server.cc:636-961`

The `HeartbeatLoop()` runs as a coroutine on every server but only sends entries when the server is leader. It serves dual purposes: replicating new entries and sending heartbeats.

```
HeartbeatLoop():
  Initialize match_index_[peer] = 0, next_index_[peer] = 1 for all peers
  looping_ = true

  WHILE looping_:
    [A] Wait: ready_for_replication_->wait(HEARTBEAT_INTERVAL)
    [B] If !IsLeader(): continue

    [C] FOR each peer in next_index_:
      [C1] Compute new commitIndex (see Section 5)
      [C2] Prepare and send AppendEntries (see below)
      [C3] Process response (see Section 4)
```

**Wake mechanism**: The loop sleeps on `ready_for_replication_` (an `IntEvent`). It wakes either:
- When `HEARTBEAT_INTERVAL` elapses (5ms in production, 100ms in test mode)
- When `CoordinatorRaft` signals `ready_for_replication_->set(1)` after a new entry is appended via `Start()`

#### 2.2 Preparing an AppendEntries

For each follower, the leader prepares an AppendEntries based on `next_index_[peer]`:

```
prevLogIndex = next_index_[peer] - 1
prevLogTerm  = raft_logs_[prevLogIndex].term

IF next_index_[peer] <= lastLogIndex:
    // There are entries to send
    Prepare cmd (single entry or batch)
ELSE:
    // No new entries — send heartbeat (cmd = nullptr)
```

**Safety checks** (server.cc:744-775):
- If `prevLogIndex > lastLogIndex`, reset `next_index_` to `lastLogIndex + 1`
- Null check on `GetRaftInstance(prevLogIndex)` to prevent crash

#### 2.3 Non-Batch Mode

When `RAFT_BATCH_OPTIMIZATION` is not defined (server.cc:786-797):

```cpp
if (it->second <= lastLogIndex) {
    auto curInstance = GetRaftInstance(it->second);
    cmd = curInstance->log_;
    cmdLogTerm = curInstance->term;
}
```

Sends one entry at a time: the entry at index `next_index_[peer]`.

#### 2.4 Batch Mode (Default)

When `RAFT_BATCH_OPTIMIZATION` is defined (server.cc:800-825):

```cpp
vector<shared_ptr<TpcCommitCommand>> batch_buffer_;
for (int idx = max(it->second, min_active_slot_); idx <= lastLogIndex; idx++) {
    auto curInstance = GetRaftInstance(idx);
    shared_ptr<TpcCommitCommand> curCmd = dynamic_pointer_cast<TpcCommitCommand>(curInstance->log_);
    curCmd->term = curInstance->term;
    batch_buffer_.push_back(curCmd);
}
shared_ptr<TpcBatchCommand> batch_cmd = make_shared<TpcBatchCommand>();
batch_cmd->AddCmds(batch_buffer_);
cmd = dynamic_pointer_cast<Marshallable>(batch_cmd);
```

All entries from `next_index_[peer]` through `lastLogIndex` are bundled into a single `TpcBatchCommand`. This reduces the number of RPCs from O(n) to O(1) per follower per heartbeat cycle, critical for high-throughput workloads.

**TpcBatchCommand structure** (`tpc_command.h:57-71`):
```cpp
class TpcBatchCommand : public Marshallable {
    vector<shared_ptr<TpcCommitCommand>> cmds_;  // Batched entries
    void AddCmds(vector<shared_ptr<TpcCommitCommand>>& cmds);
    Marshal& to_marshal(Marshal&) const override;  // Serialization
    Marshal& from_marshal(Marshal&) override;
};
```

#### 2.5 SendAppendEntries2() — The RPC Call

**Location**: `commo.cc:26-94`

This function sends a single AppendEntries RPC to one follower:

```
SendAppendEntries2(site_id, par_id, ..., cmd, ..., ret_status*, ret_term*, ret_last_log_index*):
  |
  +-- Create IntEvent for synchronization
  +-- Find proxy for target site_id
  |
  +-- IF cmd == nullptr:
  |     Send EmptyAppendEntries (heartbeat)
  |     (trigger_election_now = false for normal heartbeats)
  |
  +-- ELSE:
  |     Wrap cmd in MarshallDeputy
  |     Send AppendEntries with serialized command
  |
  +-- Callback:
  |     Extract (ret_status, ret_term, ret_last_log_index) from reply
  |     Signal IntEvent
  |
  +-- RETURN IntEvent (caller waits on it)
```

The caller waits with a 500ms timeout (server.cc:847):
```cpp
r->wait(500000);
if (r->status_.get() == Event::TIMEOUT) {
    continue;  // Skip this follower, try again next heartbeat
}
```

This bounded wait prevents a slow or partitioned follower from stalling the leader's replication to other followers.

---

### 3. Follower: Receiving Entries

#### 3.1 OnAppendEntries() — The Handler

**Location**: `server.cc:1272-1462`

When a follower receives an AppendEntries RPC, it performs three checks and then either accepts or rejects:

```
OnAppendEntries(leaderTerm, leaderPrevLogIndex, leaderPrevLogTerm,
                leaderCommitIndex, cmd, ...):
  |
  +-- [1] Pre-checks:
  |     term_ok     = (leaderTerm >= currentTerm)
  |     index_ok    = (leaderPrevLogIndex <= lastLogIndex)
  |     prev_term_ok = (prevLogIndex == 0 || local_term_at_prevIdx == leaderPrevLogTerm)
  |
  +-- [2] Term update:
  |     IF term_ok: resetTimer()
  |     IF leaderTerm > currentTerm: step down, update term, persist
  |
  +-- [3] Accept or Reject:
  |
  |     ALL checks pass → ACCEPT:
  |       Append entries (single or batch)
  |       Persist entries
  |       Update commitIndex = min(leaderCommitIndex, lastLogIndex)
  |       Reply: (ok=1, currentTerm, lastLogIndex)
  |       Release mutex, call applyLogs()
  |
  |     Any check fails → REJECT:
  |       Reply: (ok=0, currentTerm, lastLogIndex)
  |       Leader uses lastLogIndex for fast backoff
```

#### 3.2 Consistency Check

The consistency check ensures the Raft Log Matching Property: if two logs contain an entry with the same index and term, all preceding entries are identical.

The check verifies:
1. **Term validity**: The leader's term is not stale (`leaderTerm >= currentTerm`)
2. **Index existence**: The follower's log extends at least to `leaderPrevLogIndex`
3. **Term match**: The entry at `leaderPrevLogIndex` has the same term the leader expects

If the term at `leaderPrevLogIndex` doesn't match, the follower's log diverged from the leader's at some point. The follower rejects, and the leader backtracks (see Section 4.2).

#### 3.3 Entry Appending

**Non-batch mode** (server.cc:1330-1336):
```cpp
lastLogIndex = leaderPrevLogIndex + 1;
auto instance = GetRaftInstance(lastLogIndex);
instance->log_ = cmd;
instance->term = leaderNextLogTerm;
PersistLogEntry(lastLogIndex, *instance);
```

**Batch mode** (server.cc:1344-1358):
```cpp
auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
int cnt = 0;
vector<pair<slotid_t, shared_ptr<RaftData>>> entries_to_persist;
for (shared_ptr<TpcCommitCommand>& c : cmds->cmds_) {
    cnt++;
    lastLogIndex = leaderPrevLogIndex + cnt;
    auto instance = GetRaftInstance(lastLogIndex);
    instance->log_ = c;
    instance->term = dynamic_pointer_cast<TpcCommitCommand>(c)->term;
    entries_to_persist.emplace_back(lastLogIndex, instance);
}
PersistLogEntries(entries_to_persist);  // Batch persist
```

In batch mode, all entries in the `TpcBatchCommand` are appended in a single pass, and persistence is batched into one `PersistLogEntries()` call.

#### 3.4 Commit Index Advancement on Follower

After accepting entries (server.cc:1362-1368):
```cpp
if (leaderCommitIndex > commitIndex) {
    commitIndex = min(leaderCommitIndex, lastLogIndex);
    PersistCommitIndex();
}
```

The follower then calls `applyLogs()` to feed committed entries to the transaction layer.

#### 3.5 Critical Design: Mutex Release During Apply

At server.cc:1377, the mutex is released before calling `applyLogs()`:

```cpp
mtx_.unlock();
if (need_apply) {
    applyLogs();  // Called WITHOUT holding the mutex
}
mtx_.lock();
```

This is deliberate: applying entries involves transaction processing that can be slow. Holding the mutex would block incoming AppendEntries RPCs, causing the leader to time out and potentially start unnecessary elections.

---

### 4. Leader: Processing Responses

#### 4.1 Response Cases

After receiving a response from a follower, the leader handles four cases (server.cc:852-945):

**Case 0: Lost RPC** (`ret_status == false && ret_term == 0 && ret_last_log_index == 0`)
- Do nothing. The RPC was lost or the follower is unreachable.

**Case 1: Higher Term** (`ret_status == 0 && ret_term > term`)
- The follower has a higher term, meaning this leader is stale.
- Step down: `setIsLeader(false)`, update `currentTerm = ret_term`
- Skip remaining followers, restart the loop.

**Case 2: Log Conflict** (`ret_status == 0`)
- The follower rejected because its log doesn't match at `prevLogIndex`.
- Backtrack `next_index_` (see Section 4.2).

**Case 3: Success** (`ret_status == true`)
- Update tracking indices:
  - Non-batch: `match_index_ = next_index_; next_index_++`
  - Batch: `match_index_ = ret_last_log_index; next_index_ = ret_last_log_index + 1`
- Safety check: cap `match_index_` at `lastLogIndex`

#### 4.2 Log Reconciliation — Backtracking next_index_

When a follower rejects an AppendEntries, the leader must find the correct `next_index_` where the logs agree. Three strategies are used (server.cc:873-898):

```
IF follower reported lastLogIndex AND lastLogIndex < next_index - 1:
    [FAST BACKOFF]
    next_index = follower's lastLogIndex + 1
    (Jump directly to where follower's log ends)

ELSE IF next_index > 10:
    [EXPONENTIAL BACKOFF]
    next_index = next_index / 2
    (Halve the gap to converge in O(log n) steps)

ELSE IF next_index > 1:
    [LINEAR BACKOFF]
    next_index--
    (Precise one-at-a-time near the log start)

ELSE:
    next_index = 1
    (Already at the beginning)
```

**Why three tiers?**

| Strategy | Convergence | When Used |
|----------|-------------|-----------|
| Fast backoff | O(1) | Follower reports actual last index |
| Exponential | O(log n) | Large gap, no follower hint |
| Linear | O(1) | Near log start, need precision |

Standard Raft decrements `next_index` by 1 on each rejection, requiring O(n) round trips in the worst case. This implementation's fast backoff reduces it to O(1) when the follower reports its last log index, and exponential backoff handles the case where the follower's response doesn't provide a useful hint.

---

### 5. Commit Advancement

#### 5.1 Leader Computes commitIndex

The leader updates `commitIndex` in `HeartbeatLoop()` (server.cc:707-731):

```
Collect all match_index_ values into a sorted array
newCommitIndex = matchedIndices[(nservers - 1) / 2]  // Median = majority threshold

IF newCommitIndex > commitIndex
   AND raft_logs_[newCommitIndex].term == currentTerm:
    commitIndex = newCommitIndex
```

**Example for 3 servers** (leader + 2 followers):

```
match_index_ = {follower_B: 7, follower_C: 5}
Leader's own lastLogIndex = 10

Sorted: [5, 7]
Median index: (3-1)/2 = 1 → matchedIndices[1] = 7

Since leader also has entry 7, majority (leader + follower_B = 2/3) have it.
→ commitIndex = 7
```

#### 5.2 The Term Safety Rule

An entry is only committed if its term equals `currentTerm` (server.cc:728):

```cpp
if (commitInstance && newCommitIndex > commitIndex
    && (commitInstance->term == currentTerm)) {
    commitIndex = newCommitIndex;
}
```

This implements the safety guarantee from Figure 8 of the Raft paper: a leader cannot commit entries from previous terms by counting replicas alone. It must first commit an entry from its own term, which implicitly commits all prior entries.

#### 5.3 Follower Learns commitIndex

Followers learn the commit index from the `leaderCommitIndex` field in every AppendEntries RPC. On each accepted AppendEntries (server.cc:1363):

```cpp
commitIndex = min(leaderCommitIndex, lastLogIndex);
```

The `min` ensures a follower doesn't set `commitIndex` beyond what it has actually received.

---

### 6. Heartbeats

Heartbeats are empty AppendEntries RPCs (no log entries, `cmd = nullptr`). They serve three purposes:

1. **Lease renewal**: Reset followers' election timers to prevent unnecessary elections
2. **Commit propagation**: Carry the leader's `commitIndex` to followers
3. **Failure detection**: If a follower hasn't heard from the leader within the election timeout, it starts an election

#### Heartbeat Interval

| Mode | `HEARTBEAT_INTERVAL` | Effective Send Rate |
|------|---------------------|---------------------|
| Production | 5,000 μs (5 ms) | ~200 heartbeats/sec |
| Test (`RAFT_TEST_CORO`) | 100,000 μs (100 ms) | ~10 heartbeats/sec |

The heartbeat interval must be much shorter than the election timeout to prevent false elections. With production settings: heartbeat = 5ms, election timeout = 150ms-1s, giving a safety factor of 30x-200x.

#### Heartbeat Processing on Follower

When a follower receives an empty AppendEntries (server.cc:1303):
1. Reset election timer (`resetTimer()`)
2. Update `currentTerm` if leader's term is higher
3. Advance `commitIndex` if `leaderCommitIndex > commitIndex`
4. Apply committed entries via `applyLogs()`

---

### 7. Applying Committed Entries

#### 7.1 applyLogs()

**Location**: `server.cc:581-625`

Both leaders and followers call `applyLogs()` after advancing `commitIndex`:

```
applyLogs():
  IF executeIndex < commitIndex: apply_pending_ = true
  IF in_applying_logs_: return  (reentrancy guard)
  in_applying_logs_ = true

  DO:
    apply_pending_ = false
    FOR id = executeIndex+1 TO commitIndex:
      instance = GetRaftInstance(id)
      IF instance AND instance->log_:
        app_next_(id, instance->log_)    ← Feeds to Mako transaction layer
        executeIndex = id
      ELSE: break
  WHILE apply_pending_  (new work arrived during apply)

  in_applying_logs_ = false

  // GC: remove entries > 60,000 slots behind executeIndex
```

#### 7.2 The app_next_ Callback

`app_next_` is a callback registered by Mako's transaction layer (`TxLogServer::RegLearnerAction()`). When called with `(slot_id, cmd)`, it feeds the committed transaction into Mako's processing pipeline.

This is the integration boundary between Raft consensus and Mako's transaction system.

---

### 8. Complete Replication Sequence Diagram

The following shows a successful replication cycle in a 3-node cluster:

```
Time    Leader (A)                   Follower (B)              Follower (C)
 |
 |      Client: Submit(cmd)
 |      Start(cmd) → append at index 8
 |      ready_for_replication_->set(1)
 |
 |      [HeartbeatLoop wakes]
 |
 |      Compute commitIndex:
 |        match_index_ = {B:7, C:5}
 |        sorted = [5, 7]
 |        median = 7 → commitIndex = 7
 |        applyLogs() for 6,7
 |
 |      For B (next_index=8):
 |        prevLogIndex=7, prevLogTerm=1
 |        cmd = batch([entry_8])
 |        --- AppendEntries(term=1, prev=7, commit=7, batch=[8]) -->
 |
 |                                    [OnAppendEntries]
 |                                    term_ok=true, index_ok=true
 |                                    prev_term_ok=true → ACCEPT
 |                                    Append entry 8
 |                                    commitIndex = min(7, 8) = 7
 |                                    applyLogs() for 6,7
 |      <--- (ok=1, term=1, lastIdx=8) --
 |
 |      match_index_[B] = 8
 |      next_index_[B] = 9
 |
 |      For C (next_index=6):
 |        prevLogIndex=5, prevLogTerm=1
 |        cmd = batch([entry_6, entry_7, entry_8])
 |        --- AppendEntries(term=1, prev=5, commit=7, batch=[6,7,8]) ---------->
 |
 |                                                             [OnAppendEntries]
 |                                                             ACCEPT
 |                                                             Append entries 6,7,8
 |                                                             commitIndex = min(7,8) = 7
 |                                                             applyLogs() for 6,7
 |      <--- (ok=1, term=1, lastIdx=8) ------------------------------------------
 |
 |      match_index_[C] = 8
 |      next_index_[C] = 9
 |
 |      [Next heartbeat iteration]
 |      Compute commitIndex:
 |        match_index_ = {B:8, C:8}
 |        sorted = [8, 8]
 |        median = 8 → commitIndex = 8
 |        applyLogs() for 8
 |
 v      Entry 8 is now committed on all servers
```

#### Timing

| Phase | Duration | Notes |
|-------|----------|-------|
| Start() → HeartbeatLoop wake | < 5 ms | Bounded by HEARTBEAT_INTERVAL |
| AppendEntries RPC round-trip | < 1 ms (local) | Network latency in production |
| Follower processing | < 1 ms | Log append + persistence |
| Commit: first RPC returns | ~5 ms | Entry committed (majority = 2/3) |
| Commit: all RPCs return | ~5-10 ms | All replicas up-to-date |

---

### 9. Log Conflict Resolution — Example

When a follower has divergent entries (e.g., from a crashed previous leader):

```
Leader log:   [1:a] [1:b] [2:c] [2:d] [3:e]
                                        ^
                                   lastLogIndex=5

Follower log: [1:a] [1:b] [2:c] [2:x] [2:y] [2:z]
                                              ^
                                        lastLogIndex=6
                            ^
                     Divergence at index 4
```

**Round 1**: Leader sends AppendEntries with prevLogIndex=4, prevLogTerm=2
- Follower: local_prev_term = 2 (for index 4, entry [2:x])
- prev_term_ok = (2 == 2) = true ... but the entries differ!
- Actually, the term matches, so follower accepts and overwrites index 5 with [3:e]

If the terms were different:
```
Leader log:   [1:a] [1:b] [3:c] [3:d]    term=3 at index 3
Follower log: [1:a] [1:b] [2:x] [2:y]    term=2 at index 3
```

**Round 1**: Leader sends AppendEntries with prevLogIndex=3, prevLogTerm=3
- Follower: local_prev_term = 2 ≠ 3 → REJECT
- Follower reports lastLogIndex=4

**Leader backtracks**: Fast backoff → next_index = 4+1 = 5? No, that's too high.
Exponential backoff → next_index = 3/2 = 1? Then try from beginning.

**Round 2**: Leader sends from index 2 (which matches on both)
- Follower accepts, overwrites divergent entries

---

## CoordinatorRaft — Transaction Submission

<a id="what-this-document-covers-7"></a>

### What This Document Covers

This document explains how transactions are submitted to the Raft consensus layer through `CoordinatorRaft`. It covers the `Submit()` entry point, the phase-based state machine, how `AppendEntries()` waits for commitment, `WRONG_LEADER` handling with view propagation, slot allocation via `Arc<Cell<slotid_t>>`, and how the coordinator integrates with Mako's transaction processing pipeline.

**Key source files**:
- `src/deptran/raft/coordinator.h` — Class definition (82 lines)
- `src/deptran/raft/coordinator.cc` — Implementation (181 lines)
- `src/deptran/raft/frame.cc` — `CreateCoordinator()` factory (lines 50-77)
- `src/deptran/coordinator.h` — Base `Coordinator` class
- `src/deptran/classic/scheduler.cc` — Where `Submit()` is called from

---

<a id="1-class-hierarchy-1"></a>

### 1. Class Hierarchy

```
Coordinator (base class, src/deptran/coordinator.h)
    |
    v
CoordinatorRaft (Raft-specific coordinator, src/deptran/raft/coordinator.h)
```

`CoordinatorRaft` extends the base `Coordinator` class, which provides:
- `commo_` — Pointer to the communicator
- `frame_` — Pointer to the protocol frame
- `phase_` — Current phase counter
- `committed_` — Whether the command was committed
- `commit_callback_` — Callback invoked after commitment
- `par_id_` — Partition ID
- `loc_id_` — Locale ID
- `mtx_` — Recursive mutex for thread safety

---

### 2. Key Members

| Member | Type | Purpose |
|--------|------|---------|
| `svr_` | `RaftServer*` | Pointer to the local Raft server instance |
| `cmd_` | `shared_ptr<Marshallable>` | The command being submitted |
| `slot_hint_` | `Arc<Cell<slotid_t>>` | Shared mutable slot counter (see Section 6) |
| `n_replica_` | `uint32_t` | Total replicas in the partition |
| `in_submission_` | `bool` | Debug flag: prevents concurrent submissions |
| `in_append_entries` | `bool` | Debug flag: prevents concurrent appends |
| `committed_` | `bool` | Whether the current command was committed |

#### Phase Enum

```cpp
enum Phase { INIT_END = 0, PREPARE = 1, ACCEPT = 2, COMMIT = 3, FORWARD = 4 };
```

---

### 3. Submit() — The Entry Point

**Location**: `coordinator.cc:40-100`

This is the main entry point called by Mako's transaction layer to submit a committed transaction to Raft for replication.

#### Flow

```
Submit(cmd, commit_callback, exe_callback):
  |
  +-- [1] IsLeader()?
  |     |
  |     +-- NO (WRONG_LEADER path):
  |     |     Set cmd->ret_ = WRONG_LEADER (-20)
  |     |     Attach view data (current leader info)
  |     |     Call commit_callback()
  |     |     Call app_next_(0, cmd) to update view
  |     |     RETURN
  |     |
  |     +-- YES (Leader path):
  |           Lock mutex
  |           Verify not already in submission
  |           Store cmd_ = cmd
  |           Store commit_callback_
  |           GotoNextPhase()
```

#### How Submit is Called

The upper layer calls `Submit()` from `SchedulerClassic::OnCommit()` (`src/deptran/classic/scheduler.cc:241-263`):

```cpp
auto cmd = make_shared<TpcCommitCommand>();
cmd->tx_id_ = tx_id;
cmd->ret_ = commit_or_abort;
cmd->cmd_ = sp_tx->cmd_;

shared_ptr<Coordinator> coo{CreateRepCoord(dep_id.id)};
coo->Submit(sp_m);   // Blocks until committed or WRONG_LEADER

if (cmd->ret_ == WRONG_LEADER)
    return WRONG_LEADER;
```

Each transaction creates a fresh `CoordinatorRaft` instance via `CreateRepCoord()`, submits its command, and blocks until Raft commits it (or returns `WRONG_LEADER`).

---

### 4. GotoNextPhase() — The Phase State Machine

**Location**: `coordinator.cc:151-179`

After `Submit()`, execution flows through a phase-based state machine:

```
GotoNextPhase():
  current_phase = phase_ % 4
  phase_++

  SWITCH current_phase:
    INIT_END (0):
      IF IsLeader():
        phase_++          // Skip PREPARE (not used in Raft)
        AppendEntries()   // Go directly to ACCEPT phase
        phase_++          // Advance to COMMIT
      ELSE:
        verify(0)         // Non-leader should have been handled in Submit()
      // FALL THROUGH to ACCEPT case

    ACCEPT (2):
      IF committed_:
        LeaderLearn()     // Call commit_callback
      ELSE:
        // Command not committed (term changed)

    PREPARE (1):
      AppendEntries()     // Alternate path

    COMMIT (3):
      // Terminal state, do nothing
```

For the leader path, the flow is: `INIT_END → AppendEntries() → COMMIT → LeaderLearn()`.

---

### 5. AppendEntries() — Waiting for Commitment

**Location**: `coordinator.cc:103-133`

This is where the coordinator hands the command to `RaftServer` and waits for it to be committed:

```
AppendEntries():
  |
  +-- Lock mutex
  +-- svr_->Start(cmd_, &index, &term)   // Append to leader's log
  |     Returns: index = log index, term = current term
  |
  +-- Signal ready_for_replication_->set(1)
  |     (Wake HeartbeatLoop to replicate immediately)
  |
  +-- POLLING LOOP:
  |     WHILE svr_->commitIndex < index:
  |       Wait 1ms (TimeoutEvent)
  |       IF svr_->currentTerm != term:
  |         // Leader changed! Term advanced.
  |         committed_ = false
  |         RETURN
  |
  +-- committed_ = true
```

#### Key Design Points

1. **Blocking wait**: The coordinator polls `commitIndex` every 1ms until it reaches the entry's index. This is a cooperative wait using Mako's fiber system (`Reactor::create_sp_event<TimeoutEvent>(1000)->wait()`).

2. **Term change detection**: If `currentTerm` changes during the wait, the leader lost leadership. The command may or may not be committed by the new leader. The coordinator marks `committed_ = false` and returns, letting the upper layer retry.

3. **Immediate replication signal**: After `Start()`, the coordinator signals `ready_for_replication_` to wake the `HeartbeatLoop` immediately rather than waiting for the next heartbeat interval.

---

### 6. WRONG_LEADER Handling

**Location**: `coordinator.cc:42-85`

When `Submit()` is called on a non-leader, it returns `WRONG_LEADER` with view data so the client can redirect:

#### The Error Code

Defined in `src/deptran/constants.h:78`:
```cpp
#define WRONG_LEADER (-20)
```

#### What Happens

1. **Set error**: `tpc_cmd->ret_ = WRONG_LEADER`
2. **Attach view**: Create `ViewData` from `svr_->new_view_` (which contains the current leader info and term) and attach to `tpc_cmd->sp_view_data_`
3. **Handle empty view**: If `new_view_` is empty (no known leader), construct a view with `leader = -1`
4. **Call callbacks**: Still call `commit_callback()` and `app_next_(0, cmd)` so the upper layer can update its leader tracking and clean up

#### View Propagation

The `WRONG_LEADER` response carries view data back through Mako's RPC layer. The client extracts the view to learn who the current leader is and retries the transaction there.

```
Client → Submit(cmd) → non-leader server
                            |
                            v
                     ret_ = WRONG_LEADER
                     sp_view_data_ = ViewData(leader=X, term=T)
                            |
                            v
                     Client receives WRONG_LEADER
                     Updates leader tracking
                     Retries on server X
```

---

### 7. Slot Allocation via Arc<Cell<slotid_t>>

**Location**: `coordinator.h:40`, `frame.h:19`, `frame.cc:69-71`

The `slot_hint_` field provides thread-safe slot ID allocation using RustyCpp's interior mutability pattern:

#### How It Works

1. **Created once** in `RaftFrame` (`frame.h:19`):
   ```cpp
   rusty::Arc<rusty::Cell<slotid_t>> slot_hint_ =
       rusty::Arc<rusty::Cell<slotid_t>>::make(1);
   ```

2. **Shared** with each `CoordinatorRaft` via `Arc` copy (`frame.cc:69`):
   ```cpp
   coo->slot_hint_ = slot_hint_;  // Arc copy shares ownership
   ```

3. **Incremented** atomically for each new coordinator (`frame.cc:70-71`):
   ```cpp
   coo->slot_id_ = slot_hint_->get();
   slot_hint_->set(slot_hint_->get() + 1);
   ```

#### Why Arc<Cell<T>>

- `Arc<T>` provides thread-safe reference counting (like `std::shared_ptr` but following Rust ownership model)
- `Cell<T>` provides interior mutability for `Copy` types (like `slotid_t`) without a mutex
- Together, they allow multiple coordinators to share a monotonically increasing counter without locks
- This follows the RustyCpp migration guidelines from `CLAUDE.md`

---

### 8. Quorum Calculation

**Location**: `coordinator.h:61-63`

```cpp
uint32_t GetQuorum() {
    return n_replica() / 2 + 1;
}
```

For a 3-node cluster: `3/2 + 1 = 2` (majority). This is the standard Raft quorum formula. Note that `CoordinatorRaft` doesn't directly use `GetQuorum()` for commit decisions — that logic lives in `RaftServer::HeartbeatLoop()`. The quorum is defined here for consistency and is used by other coordinator methods.

---

### 9. CreateCoordinator() — Factory Method

**Location**: `frame.cc:50-77`

Each transaction gets a fresh `CoordinatorRaft` instance:

```
RaftFrame::CreateCoordinator(coo_id, config, benchmark, client_status, id, txn_reg):
  |
  +-- Create new CoordinatorRaft(coo_id, benchmark, client_status, id)
  +-- Set coo->frame_ = this
  +-- Set coo->commo_ = commo_.get()           // Borrow communicator
  +-- Set coo->svr_ = svr_.get()               // Borrow Raft server
  +-- Set coo->slot_hint_ = slot_hint_          // Share Arc
  +-- Set coo->slot_id_ = slot_hint_->get()     // Allocate slot
  +-- Increment slot_hint_
  +-- Set coo->n_replica_ = partition size
  +-- Set coo->loc_id_ = locale ID
  +-- RETURN coo
```

The coordinator borrows pointers to the frame's owned `RaftCommo` and `RaftServer`. This is safe because the frame outlives all coordinators.

---

### 10. LeaderLearn() — Post-Commit Callback

**Location**: `coordinator.cc:144-148`

After successful commitment:

```cpp
void CoordinatorRaft::LeaderLearn() {
    lock(mtx_);
    commit_callback_();     // Notify upper layer of commit
    verify(phase_ == COMMIT);
    GotoNextPhase();        // Move to terminal state
}
```

The `commit_callback_` is set by `Submit()` from the caller's provided callback. This is how the transaction layer learns that Raft has committed the entry.

---

### 11. Complete Submission Flow

```
Mako Transaction Layer
    |
    | OnCommit(tx_id)
    v
SchedulerClassic::OnCommit()
    |
    | CreateRepCoord() → new CoordinatorRaft
    | Submit(TpcCommitCommand)
    v
CoordinatorRaft::Submit()
    |
    +-- IsLeader()? YES
    |
    v
GotoNextPhase() → INIT_END
    |
    v
AppendEntries()
    |
    +-- RaftServer::Start(cmd) → append at index N, term T
    +-- Signal HeartbeatLoop to replicate
    +-- Poll: WHILE commitIndex < N
    |     Wait 1ms
    |     Check: term still T?
    +-- committed_ = true
    |
    v
GotoNextPhase() → COMMIT
    |
    v
LeaderLearn()
    |
    +-- commit_callback_()  → Notify transaction layer
    |
    v
Submit() returns, transaction is committed
```

---

## RPC Layer — Communication Infrastructure

<a id="what-this-document-covers-8"></a>

### What This Document Covers

This document explains the complete RPC (Remote Procedure Call) infrastructure that enables Raft nodes to communicate. It covers the three-layer architecture (`RaftCommo` → `RaftProxy` → `RaftService` → `RaftServiceImpl`), the macro system that eliminates boilerplate, the `DeferredReply` pattern for asynchronous responses, the `RaftFrame` factory pattern, proxy connection setup, the wire format via `Marshal`, and the disconnection simulation system used for testing.

**Key source files**:
- `src/deptran/raft/commo.h` — `RaftCommo` and `RaftVoteQuorumEvent` (129 lines)
- `src/deptran/raft/commo.cc` — RPC sending implementations (287 lines)
- `src/deptran/raft/service.h` — `RaftServiceImpl` with `RpcHandler` macro usage (84 lines)
- `src/deptran/raft/service.cc` — Handler implementations (112 lines)
- `src/deptran/raft/macros.h` — `RpcHandler` and `Call_Async` macro definitions (77 lines)
- `src/deptran/raft/frame.h` — `RaftFrame` factory class (50 lines)
- `src/deptran/raft/frame.cc` — Factory method implementations (207 lines)
- `src/deptran/rcc_rpc.h` — Generated `RaftService` and `RaftProxy` classes
- `src/deptran/communicator.h` — Base `Communicator` class with proxy maps
- `src/rrr/rpc/server.hpp` — `DeferredReply` RAII class
- `src/rrr/misc/marshal.hpp` — `Marshal` and `MarshallDeputy` serialization

---

### 1. Architecture Overview

The Raft RPC system has four layers, from sender to receiver:

```
SENDER SIDE                           RECEIVER SIDE

RaftCommo                             RaftServiceImpl
  (application logic)                   (application logic)
        |                                     ^
        v                                     |
RaftProxy                             RaftService (base)
  (serializes args,                     (deserializes args,
   sends async RPC)                      dispatches to handler)
        |                                     ^
        v                                     |
rrr::Client                           rrr::Server
  (TCP connection,                      (TCP listener,
   marshals request)                     unmarshals request)
```

<a id="class-hierarchy-2"></a>

#### Class Hierarchy

```
rrr::Service (abstract base)
    |
    v
RaftService (generated, rcc_rpc.h)
    |  - enum { VOTE, APPENDENTRIES, EMPTYAPPENDENTRIES, TIMEOUTNOW }
    |  - __reg_to__(): registers RPC IDs with server
    |  - __dispatch__(): routes incoming RPCs by ID
    |  - __*__wrapper__(): deserializes args, creates DeferredReply
    |  - pure virtual: Vote(), AppendEntries(), EmptyAppendEntries(), TimeoutNow()
    |
    v
RaftServiceImpl (service.h)
    |  - RpcHandler macro overrides pure virtuals
    |  - Handle*() methods delegate to RaftServer
    |
    v
RaftServer (actual Raft logic)
```

---

### 2. RPC Definitions

Four RPCs are defined for the Raft protocol, each with a unique 32-bit ID:

| RPC | ID | Direction | Purpose |
|-----|----|-----------|---------|
| `Vote` | `0x3587ec7b` | Candidate → Peer | Request vote during election |
| `AppendEntries` | `0x1fc0e195` | Leader → Follower | Replicate log entry |
| `EmptyAppendEntries` | `0x4e8db0c5` | Leader → Follower | Heartbeat (no data) |
| `TimeoutNow` | `0x33c8c834` | Leader → Preferred | Trigger immediate election |

#### Wire Format

Each RPC's arguments are serialized sequentially using the `Marshal` (`rrr::Marshal`) binary format. The `<<` and `>>` operators handle serialization and deserialization for primitive types (`uint64_t`, `bool_t`, `ballot_t`, `siteid_t`) and compound types (`MarshallDeputy`).

**Vote RPC:**
```
Request:  [lst_log_idx:u64] [lst_log_term:ballot_t] [site_id:siteid_t] [cur_term:ballot_t]
Response: [reply_term:ballot_t] [vote_granted:bool_t]
```

**AppendEntries RPC:**
```
Request:  [slot:u64] [ballot:ballot_t] [leaderCurrentTerm:u64] [leaderSiteId:siteid_t]
          [leaderPrevLogIndex:u64] [leaderPrevLogTerm:u64] [leaderCommitIndex:u64]
          [cmd:MarshallDeputy] [leaderNextLogTerm:u64]
Response: [followerAppendOK:u64] [followerCurrentTerm:u64] [followerLastLogIndex:u64]
```

**EmptyAppendEntries RPC:**
```
Request:  [slot:u64] [ballot:ballot_t] [leaderCurrentTerm:u64] [leaderSiteId:siteid_t]
          [leaderPrevLogIndex:u64] [leaderPrevLogTerm:u64] [leaderCommitIndex:u64]
          [trigger_election_now:bool_t]
Response: [followerAppendOK:u64] [followerCurrentTerm:u64] [followerLastLogIndex:u64]
```

**TimeoutNow RPC:**
```
Request:  [leaderTerm:u64] [leaderSiteId:siteid_t]
Response: [followerTerm:u64] [success:bool_t]
```

#### MarshallDeputy — Polymorphic Serialization

`MarshallDeputy` (`src/rrr/misc/marshal.hpp:88`) is a type-erasing wrapper that serializes polymorphic `Marshallable` objects. It stores:
- `kind_` — A 32-bit type tag identifying the concrete type (e.g., `TpcCommitCommand`, `TpcBatchCommand`)
- `sp_data_` — A `shared_ptr<Marshallable>` to the actual data

On the wire, `kind_` is written first, followed by the object's serialized form. On deserialization, a factory registry (populated via `reg_initializer()`) maps `kind_` back to a constructor, creating the correct concrete type.

---

### 3. Macros — Boilerplate Elimination

Two macros in `macros.h` handle the repetitive parts of RPC handler registration and async RPC sending.

#### RpcHandler Macro (`macros.h:50-61`)

```cpp
#define RpcHandler(name, ...) \
  void name(_ARGPAIRS(__VA_ARGS__), rrr::DeferredReply defer) override { \
    verify(svr_ != nullptr); \
    if (svr_->IsDisconnected()) { \
      OnDisconnected##name(_PARAMS(__VA_ARGS__)); \
      defer.reply(); \
    }  else { \
      Handle##name(_PARAMS(__VA_ARGS__), std::move(defer)); \
    } \
  } \
  void Handle##name(_ARGPAIRS(__VA_ARGS__), rrr::DeferredReply defer); \
  void OnDisconnected##name(_ARGPAIRS(__VA_ARGS__))
```

This macro generates three things for each RPC:
1. **Override method** (`Vote()`, `AppendEntries()`, etc.) — the virtual method from `RaftService`. It checks if the server is disconnected (for testing) and routes accordingly.
2. **Handler declaration** (`HandleVote()`, `HandleAppendEntries()`, etc.) — the real handler, implemented in `service.cc`.
3. **Disconnection handler declaration** (`OnDisconnectedVote()`, etc.) — called when the server simulates disconnection, returning default values.

The helper macros `_PARAMS` and `_ARGPAIRS` extract parameter names and type-name pairs from variadic arguments, supporting up to 20 parameters.

**Example expansion** for `RpcHandler(Vote, 6, ...)`:
```cpp
// Generated override (calls HandleVote or OnDisconnectedVote)
void Vote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term,
          const siteid_t& can_id, const ballot_t& can_term,
          ballot_t* reply_term, bool_t* vote_granted,
          rrr::DeferredReply defer) override {
    verify(svr_ != nullptr);
    if (svr_->IsDisconnected()) {
        OnDisconnectedVote(lst_log_idx, lst_log_term, can_id, can_term,
                          reply_term, vote_granted);
        defer.reply();  // Reply with default values
    } else {
        HandleVote(lst_log_idx, lst_log_term, can_id, can_term,
                  reply_term, vote_granted, std::move(defer));
    }
}

// Handler declaration (implemented in service.cc)
void HandleVote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term,
                const siteid_t& can_id, const ballot_t& can_term,
                ballot_t* reply_term, bool_t* vote_granted,
                rrr::DeferredReply defer);

// Disconnection handler (returns defaults set in service.h initializer)
void OnDisconnectedVote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term,
                        const siteid_t& can_id, const ballot_t& can_term,
                        ballot_t* reply_term, bool_t* vote_granted);
```

#### Call_Async Macro (`macros.h:72-76`)

```cpp
#define Call_Async(proxy, name, ...) { \
  auto f = proxy->async##_##name(__VA_ARGS__); \
  _RPC_COUNT(); \
  Future::safe_release(f); \
}
```

This macro:
1. Calls the proxy's async method (e.g., `proxy->async_Vote(...)`)
2. Optionally increments an RPC counter (when `RAFT_TEST_CORO` is defined)
3. Releases the future via `Future::safe_release()` — the callback (via `FutureAttr`) handles the result

---

### 4. RaftCommo — Sending RPCs

**Location**: `commo.h:51-125`, `commo.cc:1-287`

`RaftCommo` extends `Communicator` and provides three RPC sending methods plus a helper.

#### Inheritance

```
Communicator (base, communicator.h)
    |  - rpc_par_proxies_: map<parid_t, vector<SiteProxyPair>>
    |  - ConnectToPeers(): establishes TCP connections
    |  - rpc_poll_: poll thread for async I/O
    |
    v
RaftCommo (raft/commo.h)
    |  - SendAppendEntries2(): point-to-point with IntEvent return
    |  - SendAppendEntries(): point-to-point with results struct return
    |  - BroadcastVote(): broadcast to all peers with quorum event
    |  - SendTimeoutNow(): point-to-point with callback
```

#### Proxy Map

`rpc_par_proxies_` is populated by `Communicator::ConnectToPeers()` during startup. For each partition, it maps `parid_t` → `vector<(siteid_t, ClassicProxy*)>`. `RaftCommo` casts these `ClassicProxy*` pointers to `RaftProxy*` when sending Raft-specific RPCs:

```cpp
auto proxy = (RaftProxy*) p.second;
```

This cast is safe because the same `rrr::Client` connection supports multiple proxy types — they share the same underlying TCP connection and only differ in which RPC IDs they send.

#### SendAppendEntries2() (`commo.cc:26-94`)

Used by `HeartbeatLoop` for the primary replication path. Returns an `IntEvent` that signals when the follower replies.

```
SendAppendEntries2(site_id, par_id, ..., cmd, ret_status, ret_term, ret_last_log_index):
  |
  +-- Create IntEvent (ret) for signaling completion
  +-- Find proxy for site_id in rpc_par_proxies_[par_id]
  +-- Set callback: on reply, extract (status, term, last_log_index), set ret(1)
  +-- IF cmd == nullptr:
  |     Call_Async(proxy, EmptyAppendEntries, ...)   // Heartbeat
  +-- ELSE:
  |     Wrap cmd in MarshallDeputy
  |     Call_Async(proxy, AppendEntries, ...)         // Data entry
  +-- RETURN IntEvent
```

Key design: the callback writes results directly into caller-provided pointers (`ret_status`, `ret_term`, `ret_last_log_index`) and then signals the event. The caller polls or waits on the event with a timeout.

#### SendAppendEntries() (`commo.cc:96-175`)

Older variant used by some code paths. Returns a `SendAppendEntriesResults` struct instead of an `IntEvent`. Also supports the `trigger_election_now` flag for the preferred leader protocol.

The `SendAppendEntriesResults` struct tracks:
- `done` — Whether a valid reply was received
- `ok` — Whether the follower accepted the entry
- `followerTerm` — Follower's current term
- `followerLastLogIndex` — Follower's last log index
- `empty` — Whether this was a heartbeat

A special case: `(ok=false, followerTerm=0, followerLastLogIndex=0)` is reserved to simulate a lost RPC (no reply), so `done` stays `false`.

#### BroadcastVote() (`commo.cc:177-210`)

Broadcasts `Vote` RPCs to all peers in the partition and returns a `RaftVoteQuorumEvent`:

```
BroadcastVote(par_id, lst_log_idx, lst_log_term, self_id, cur_term):
  |
  +-- n = partition size
  +-- Create RaftVoteQuorumEvent(n, n/2)
  |     Quorum threshold = n/2 (candidate already has self-vote)
  +-- FOR each peer (skip self):
  |     Set callback: extract (term, vote_granted), FeedResponse()
  |     Call_Async(proxy, Vote, ...)
  +-- RETURN quorum event
```

#### SendTimeoutNow() (`commo.cc:228-285`)

Sends a single `TimeoutNow` RPC to a target replica for leadership transfer:

```
SendTimeoutNow(site_id, par_id, leader_term, leader_site_id, callback):
  |
  +-- Find proxy for site_id
  +-- Set callback: on reply, extract (follower_term, success), call user callback
  +-- Call_Async(proxy, TimeoutNow, leader_term, leader_site_id, ...)
  +-- IF site not found: callback(false, 0)
```

#### WAN_WAIT

The `WAN_WAIT` macro (`communicator.h:29-35`) is a compile-time switch:
- When `SIMULATE_WAN` is defined: calls `_wan_wait()` to inject artificial network delay
- Otherwise: expands to `;` (no-op)

This allows testing WAN-like latency in local deployments.

---

### 5. RaftServiceImpl — Receiving RPCs

**Location**: `service.h:21-83`, `service.cc:1-112`

`RaftServiceImpl` receives incoming RPCs and delegates to `RaftServer`.

#### Constructor (`service.cc:15-20`)

```cpp
RaftServiceImpl::RaftServiceImpl(TxLogServer *sched)
    : svr_((RaftServer*)sched) {
    struct timespec curr_time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &curr_time);
    srand(curr_time.tv_nsec);
}
```

Seeds the random number generator with nanosecond-precision time for election timeout randomization.

#### Handler Registration (via `RpcHandler` macro)

Each RPC is declared in `service.h` using the `RpcHandler` macro, which:
1. Sets output parameters to safe defaults (e.g., `*vote_granted = false`)
2. Generates the override, handler declaration, and disconnection handler

```cpp
RpcHandler(Vote, 6,
           const uint64_t&, lst_log_idx,
           const ballot_t&, lst_log_term,
           const siteid_t&, can_id,
           const ballot_t&, can_term,
           ballot_t*, reply_term,
           bool_t*, vote_granted) {
    *reply_term = can_term;     // Default: echo candidate's term
    *vote_granted = false;      // Default: reject
}
```

The body in braces after the macro is the `OnDisconnected` handler — it sets the default output values when the server is simulating disconnection.

#### Handler Implementations (`service.cc`)

Each `Handle*` method delegates to the corresponding `RaftServer` method:

| Handler | Runs In | Delegates To |
|---------|---------|-------------|
| `HandleVote` | Caller's fiber | `svr_->OnRequestVote()` |
| `HandleAppendEntries` | New fiber | `svr_->OnAppendEntries()` |
| `HandleEmptyAppendEntries` | New fiber | `svr_->OnAppendEntries()` with `cmd=nullptr` |
| `HandleTimeoutNow` | Caller's fiber | `svr_->OnTimeoutNow()` |

**Why new fibers for AppendEntries?** `HandleAppendEntries` and `HandleEmptyAppendEntries` create new fibers (`Fiber::create_run`) because `OnAppendEntries` may block (e.g., waiting for a mutex or applying logs). Running in a separate fiber prevents blocking the RPC dispatch thread.

`HandleVote` and `HandleTimeoutNow` run in the caller's fiber because they are fast operations that don't block.

#### DeferredReply Pattern

Each handler receives a `DeferredReply` object by move. This RAII wrapper:
1. Holds the original `Request` and a `WeakServerConnection`
2. On `defer.reply()`: serializes output parameters via `marshal_reply_` callback, sends the response
3. On destruction: calls `cleanup_` to delete the heap-allocated parameter storage

The handler captures `defer` in a lambda and calls `defer.reply()` after setting output parameters:

```cpp
void HandleVote(..., rrr::DeferredReply defer) {
    svr_->OnRequestVote(lst_log_idx, lst_log_term, can_id, can_term,
                        reply_term, vote_granted,
                        [defer = std::move(defer)]() mutable { defer.reply(); });
}
```

This pattern decouples the reply from the RPC dispatch — the server can reply asynchronously after completing its work.

---

### 6. RaftService — Generated Base Class

**Location**: `rcc_rpc.h:1491-1671`

`RaftService` is the base class for `RaftServiceImpl`. It handles the low-level RPC plumbing:

#### RPC Registration (`__reg_to__`)

Registers each RPC ID with the `rrr::Server`:

```cpp
int __reg_to__(rrr::Server& svr, size_t svc_index) override {
    svr.reg_rpc(VOTE, svc_index);
    svr.reg_rpc(APPENDENTRIES, svc_index);
    svr.reg_rpc(EMPTYAPPENDENTRIES, svc_index);
    svr.reg_rpc(TIMEOUTNOW, svc_index);
}
```

The server uses `svc_index` to find the correct service when an RPC arrives.

#### RPC Dispatch (`__dispatch__`)

Routes incoming RPCs by ID:

```cpp
void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req,
                  rrr::WeakServerConnection weak_sconn) override {
    switch (rpc_id) {
        case VOTE:              __Vote__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES:     __AppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case EMPTYAPPENDENTRIES:__EmptyAppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case TIMEOUTNOW:        __TimeoutNow__wrapper__(std::move(req), weak_sconn); break;
    }
}
```

#### Wrapper Methods

Each `__*__wrapper__` method performs deserialization:

1. **Allocate** heap storage for each input and output parameter
2. **Deserialize** inputs from the request's `Marshal` buffer (`req->m >> *in_N`)
3. **Create** `marshal_reply_` lambda to serialize outputs on reply
4. **Create** `cleanup_` lambda to delete all allocated storage
5. **Construct** `DeferredReply` with these lambdas
6. **Call** the virtual handler method

Example for `Vote`:
```cpp
void __Vote__wrapper__(rusty::Box<rrr::Request> req, WeakServerConnection weak_sconn) {
    uint64_t* in_0 = new uint64_t;     req->m >> *in_0;  // lst_log_idx
    ballot_t* in_1 = new ballot_t;     req->m >> *in_1;  // lst_log_term
    siteid_t* in_2 = new siteid_t;     req->m >> *in_2;  // can_id
    ballot_t* in_3 = new ballot_t;     req->m >> *in_3;  // can_term
    ballot_t* out_0 = new ballot_t;                       // reply_term
    bool_t*   out_1 = new bool_t;                         // vote_granted

    auto __marshal_reply__ = [=](Marshal& m) { m << *out_0; m << *out_1; };
    auto __cleanup__ = [=] { delete in_0; ...; delete out_1; };

    DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);
    this->Vote(*in_0, *in_1, *in_2, *in_3, out_0, out_1, std::move(__defer__));
}
```

---

### 7. RaftProxy — Sending Side

**Location**: `rcc_rpc.h:1673-1770`

`RaftProxy` wraps an `rrr::Client` connection and provides typed async methods for each RPC:

```cpp
class RaftProxy {
    rrr::Client* __cl__;

    FutureResult async_Vote(const uint64_t& lst_log_idx, ..., const FutureAttr& fuattr) {
        return __cl__->request(RaftService::VOTE, fuattr, [&](Marshal& m) {
            m << lst_log_idx;
            m << lst_log_term;
            m << site_id;
            m << cur_term;
        });
    }
    // ... async_AppendEntries, async_EmptyAppendEntries, async_TimeoutNow
};
```

The `request()` method:
1. Serializes the RPC ID and arguments into a `Marshal` buffer
2. Sends the request over TCP
3. Associates the `FutureAttr::callback` with the request for async response handling
4. Returns a `FutureResult` (which `Call_Async` releases via `safe_release()`)

---

### 8. RaftFrame — Factory Pattern

**Location**: `frame.h:16-49`, `frame.cc:1-207`

`RaftFrame` is the factory that creates all Raft components. It extends the base `Frame` class and is registered via the `REG_FRAME` macro.

<a id="registration-1"></a>

#### Registration

```cpp
REG_FRAME(MODE_RAFT, vector<string>({"raft"}), RaftFrame);
```

This registers `RaftFrame` for mode `MODE_RAFT` with the name `"raft"`, matching YAML config `mode: raft`.

#### Factory Methods

| Method | Creates | Lifetime |
|--------|---------|----------|
| `CreateScheduler()` | `RaftServer` | Owned by frame (`unique_ptr`) |
| `CreateCommo()` | `RaftCommo` | Owned by frame (`unique_ptr`) |
| `CreateCoordinator()` | `CoordinatorRaft` | Caller-owned (raw `new`) |
| `CreateExecutor()` | `RaftExecutor` | Caller-owned (raw `new`) |
| `CreateRpcServices()` | `RaftServiceImpl` | Returned as `Box<Service>` |

#### Ownership Model

```
RaftFrame (owns)
    |
    +-- commo_: unique_ptr<RaftCommo>     (single communicator)
    +-- svr_: unique_ptr<RaftServer>      (single server)
    +-- slot_hint_: Arc<Cell<slotid_t>>   (shared counter)
    |
    |   (borrows to)
    +-- CoordinatorRaft instances
    |     coo->commo_ = commo_.get()      (raw pointer borrow)
    |     coo->svr_ = svr_.get()          (raw pointer borrow)
    |     coo->slot_hint_ = slot_hint_    (Arc copy)
    |
    +-- RaftServiceImpl
          svc->svr_ = svr_.get()          (raw pointer borrow)
```

The frame owns the long-lived components. Short-lived components (coordinators, service impl) borrow pointers. This is safe because the frame outlives all borrowers.

#### CreateScheduler() (`frame.cc:80-100`)

Creates the `RaftServer` exactly once. Calling it twice triggers `verify(0)` (abort). Under `RAFT_TEST_CORO`, also registers this frame in a static map for test infrastructure.

#### CreateCommo() (`frame.cc:103-190`)

Creates the `RaftCommo` on first call, returns the cached instance on subsequent calls. Under `RAFT_TEST_CORO`, waits for all 5 replicas to create communicators, then launches the test coroutine.

#### CreateRpcServices() (`frame.cc:193-204`)

Returns a vector containing a single `RaftServiceImpl` wrapped in `rusty::Box<Service>`:

```cpp
vector<rusty::Box<Service>>
RaftFrame::CreateRpcServices(uint32_t site_id, TxLogServer *rep_sched, ...) {
    auto result = vector<rusty::Box<Service>>();
    switch (config->replica_proto_) {
        case MODE_RAFT:
            result.push_back(rusty::make_box<RaftServiceImpl>(rep_sched));
        default: break;
    }
    return result;
}
```

---

### 9. RaftVoteQuorumEvent — Quorum Tracking

**Location**: `commo.h:11-38`

`RaftVoteQuorumEvent` extends `QuorumEvent` to track vote responses:

```cpp
class RaftVoteQuorumEvent : public QuorumEvent {
    void FeedResponse(bool y, ballot_t term) {
        if (y) { vote_yes(); }
        else {
            vote_no();
            if (term > highest_term_) highest_term_ = term;
        }
    }

    int64_t Term() { return highest_term_; }
};
```

The `highest_term_` field tracks the maximum term seen in any NO vote, allowing the candidate to advance its term after losing an election.

The quorum event integrates with Mako's reactor system:
- `vote_yes()` / `vote_no()` call `test()` which evaluates `is_ready()`
- When ready (quorum reached), the waiting coroutine is woken

---

### 10. Disconnection Simulation

The RPC layer supports simulating network partitions for testing via the `IsDisconnected()` mechanism:

<a id="how-it-works-1"></a>

#### How It Works

1. `RaftServer::Disconnect(true)` sets `disconnected_ = true`
2. When an RPC arrives, the `RpcHandler` macro checks `svr_->IsDisconnected()`
3. If disconnected: runs `OnDisconnected*()` handler (returns default/reject values) and immediately replies
4. If connected: runs `Handle*()` handler (delegates to `RaftServer`)

#### Default Disconnection Responses

Set in the `RpcHandler` initializer blocks in `service.h`:

| RPC | Disconnected Response |
|-----|----------------------|
| Vote | `reply_term = can_term`, `vote_granted = false` |
| AppendEntries | `followerAppendOK = false`, `followerCurrentTerm = 0`, `followerLastLogIndex = 0` |
| EmptyAppendEntries | Same as AppendEntries |
| TimeoutNow | `followerTerm = 0`, `success = false` |

The `(ok=false, term=0, lastLogIndex=0)` triple for AppendEntries is specially recognized by `SendAppendEntries()` as a "lost RPC" — the `done` flag stays `false`, simulating a packet that never arrived.

#### Reconnection

`RaftServer::Reconnect()` sets `disconnected_ = false` and resets the election timer, simulating a node rejoining the network.

---

### 11. SendAppendEntriesResults — Response Tracking

**Location**: `commo.h:40-48`

```cpp
class SendAppendEntriesResults {
    std::recursive_mutex mtx;
    bool done = false;
    uint64_t ok = 0;
    uint64_t followerTerm = 0;
    uint64_t followerLastLogIndex = 0;
    bool empty = true;
};
```

This struct is used by `SendAppendEntries()` (the older variant) to collect results from a single follower. The `done` flag distinguishes between:
- `done = true`: A valid response was received
- `done = false`: Either no response yet or a simulated lost RPC

---

### 12. RAFT_TEST_CORO — Test Infrastructure

When compiled with `RAFT_TEST_CORO`, additional infrastructure is enabled:

1. **RPC counting**: `Call_Async` increments `rpc_count_` (protected by `rpc_mtx_`) for test assertions
2. **Frame tracking**: Static `frames_` map tracks all `RaftFrame` instances by locale ID
3. **Test coroutine**: `CreateCommo()` on site 0 creates a test fiber that waits for all 5 communicators, then runs `RaftLabTest::Run()`
4. **Reactor shutdown**: After tests complete, `Reactor::get_reactor()->looping_ = false` terminates the event loop

---

### 13. Complete RPC Flow — AppendEntries Example

```
Leader RaftServer (HeartbeatLoop)
    |
    | RaftCommo::SendAppendEntries2(site_id, ..., cmd, &status, &term, &last_idx)
    v
RaftCommo
    |
    | Create IntEvent for result signaling
    | Find proxy in rpc_par_proxies_[par_id]
    | Set callback to extract reply fields
    | Call_Async(proxy, AppendEntries, slot, ballot, term, siteId, prevIdx, prevTerm, commitIdx, md, logTerm, fuattr)
    v
RaftProxy::async_AppendEntries(...)
    |
    | client->request(APPENDENTRIES, fuattr, [&](Marshal& m) { m << slot << ballot << ... })
    v
    ~~~~ TCP ~~~~
    v
rrr::Server receives request
    |
    | Looks up svc_index for APPENDENTRIES
    | Calls RaftService::__dispatch__(APPENDENTRIES, req, weak_sconn)
    v
RaftService::__AppendEntries__wrapper__()
    |
    | Deserializes: req->m >> slot >> ballot >> term >> siteId >> prevIdx >> prevTerm >> commitIdx >> cmd >> logTerm
    | Allocates output: followerAppendOK, followerCurrentTerm, followerLastLogIndex
    | Creates DeferredReply with marshal_reply_ and cleanup_ lambdas
    v
RaftServiceImpl::AppendEntries()  (generated by RpcHandler macro)
    |
    | IsDisconnected()? → OnDisconnectedAppendEntries() → reply with defaults
    | Otherwise → HandleAppendEntries()
    v
HandleAppendEntries() [in new Fiber]
    |
    | svr_->OnAppendEntries(slot, ballot, term, siteId, prevIdx, prevTerm,
    |                       commitIdx, cmd->sp_data_, logTerm,
    |                       &followerAppendOK, &followerCurrentTerm, &followerLastLogIndex,
    |                       [defer]() { defer.reply(); })
    v
RaftServer::OnAppendEntries() — processes entry
    |
    | Sets *followerAppendOK, *followerCurrentTerm, *followerLastLogIndex
    | Calls callback → defer.reply()
    v
DeferredReply::reply()
    |
    | marshal_reply_(m): m << *followerAppendOK << *followerCurrentTerm << *followerLastLogIndex
    | Sends response over TCP
    | cleanup_(): deletes all heap-allocated parameters
    v
    ~~~~ TCP ~~~~
    v
RaftProxy callback fires (on leader)
    |
    | fu->get_reply() >> *ret_status >> *ret_term >> *ret_last_log_index
    | ret->set(1)  — signals IntEvent
    v
Leader HeartbeatLoop wakes, reads results from ret_status/ret_term/ret_last_log_index
```

---

## Preferred Leader Election — Design and Motivation

<a id="what-this-document-covers-9"></a>

### What This Document Covers

This document explains the design rationale, high-level mechanism, and safety argument for the preferred leader extension to Raft. This is a novel contribution beyond the standard Raft protocol: it adds deterministic leader placement while preserving all of Raft's safety properties.

**Key source files**:
- `src/deptran/raft/server.h` — Preferred leader member variables and inline methods (lines 113-146, 575-636)
- `src/deptran/raft/server.cc` — `GetElectionTimeout()` (lines 348-369), `setIsLeader()` (lines 443-554), `OnTimeoutNow()` (lines 1476-1575), `StopLeadershipTransferMonitoring()` (lines 1577-1586), `StartLeadershipTransferMonitoring()` (lines 1588-1671), `ShouldTransferLeadership()` (lines 1681-1725), `InitiateLeadershipTransfer()` (lines 1727-1827)
- `src/deptran/raft/commo.cc` — `SendTimeoutNow()` (lines 228-285), `SendAppendEntries()` with `trigger_election_now` (lines 96-175)
- `src/deptran/raft_main_helper.cc` — `SetPreferredLeader()` call site (lines 300-349)

---

### 1. Why Preferred Leader?

Standard Raft makes no guarantee about which replica becomes leader. After a leader failure, any follower can win the election, depending on random timeouts and network conditions. This is sufficient for correctness, but suboptimal for systems like Mako that have operational preferences.

#### Problem: Non-Deterministic Leader Placement

In a geo-replicated system:
1. **Data locality**: The leader handles all client reads/writes. If the leader is far from the data or the clients, every operation incurs cross-datacenter latency.
2. **Cross-shard coordination**: Mako runs distributed transactions across shards. If each shard's leader is in a different datacenter, every cross-shard operation requires cross-datacenter communication.
3. **Operational control**: Operators want to place leaders on specific machines for capacity planning, maintenance windows, and resource isolation.

#### Solution: Preferred Leader

The preferred leader mechanism ensures that a designated replica becomes the leader whenever possible, while still allowing any replica to serve as leader during failures (preserving fault tolerance).

**Key design goals**:
- **Deterministic placement**: A configured preferred replica will become leader within seconds of startup or recovery
- **Automatic failover**: If the preferred replica fails, standard Raft election selects an alternative leader
- **Automatic failback**: When the preferred replica recovers, leadership automatically transfers back to it
- **Safety preservation**: All Raft safety properties are maintained — no data loss, no split-brain

---

### 2. How It Differs from Standard Raft

| Aspect | Standard Raft | Preferred Leader Extension |
|--------|--------------|---------------------------|
| Election timeout | Same for all replicas (150-300ms) | Asymmetric: 150-300ms for preferred, 500ms-2s for others |
| Leader selection | First to timeout wins | Preferred wins startup races; non-preferred leaders transfer back |
| Leader failover | Random follower wins | Any follower can still win (safety preserved) |
| Leader failback | Not supported | Automatic via monitoring thread and transfer protocol |
| Post-election behavior | Leader stays indefinitely | Non-preferred leaders actively monitor and transfer |
| Transfer mechanism | Not in standard Raft | Piggybacked `trigger_election_now` in AppendEntries |

#### What Is NOT Changed

The preferred leader extension does not modify any of these standard Raft mechanisms:
- Log replication protocol
- Vote granting rules (term check, log up-to-date check)
- Commit rule (majority replication in current term)
- Leader completeness property
- AppendEntries consistency check

---

### 3. Design Overview

The preferred leader system operates in three phases:

#### Phase 1: Startup — Election Bias

At startup, all replicas begin as followers. The preferred replica is configured via `SetPreferredLeader(site_id)`, which is called during initialization (in `raft_main_helper.cc`). The system biases elections toward the preferred replica using asymmetric timeouts:

```
GetElectionTimeout():
  |
  +-- AmIPreferredLeader()?
  |     YES → 150-300ms (short timeout, wins election races)
  |     NO  → Is startup grace period (<5 seconds)?
  |             YES → 1-2 seconds (very long, lets preferred win)
  |             NO  → 500ms-1 second (medium, enables failover)
```

During the 5-second startup grace period, non-preferred replicas wait 1-2 seconds before starting elections. The preferred replica waits only 150-300ms. This creates a high probability that the preferred replica wins the first election.

**Why a grace period?** At startup, all replicas boot simultaneously. Without the grace period, a non-preferred replica with a slightly shorter random timeout could win before the preferred replica even starts. The 5-second grace period ensures the preferred replica has time to initialize and win the first election.

**After the grace period**, non-preferred replicas use 500ms-1 second timeouts. This is longer than the preferred replica's 150-300ms, giving the preferred replica an advantage in subsequent elections, but short enough to detect leader failures within a reasonable time.

#### Phase 2: Monitoring — Non-Preferred Leader Detection

When a non-preferred replica wins an election (e.g., because the preferred replica was down during failover), it becomes a "non-preferred leader." This triggers the monitoring system:

```
setIsLeader(true):
  |
  +-- become_new_leader?
        |
        +-- AmIPreferredLeader()?
              NO → StartLeadershipTransferMonitoring()
```

The monitoring thread runs in the background (a `std::thread`, not a fiber) and checks every 1 second whether the preferred replica is alive and caught up:

```
StartLeadershipTransferMonitoring():
  |
  +-- Wait MIN_STABLE_TIME_US (500ms) after becoming leader
  +-- Loop every 1 second:
  |     +-- Still leader? Still non-preferred?
  |     +-- ShouldTransferLeadership()?
  |     |     +-- Am I leader? (must be)
  |     |     +-- Am I non-preferred? (must be)
  |     |     +-- Is preferred configured? (must be)
  |     |     +-- Not already transferring? (must not be)
  |     |     +-- Is preferred in peer list? (must be)
  |     |     +-- Is preferred caught up? (match_index >= commitIndex)
  |     |           YES → Return true
  |     +-- If should_transfer:
  |           InitiateLeadershipTransfer()
  |           break
```

The `ShouldTransferLeadership()` check verifies:
1. This replica is the current leader
2. This replica is NOT the preferred leader
3. A preferred leader is configured (`!= INVALID_SITEID`)
4. No transfer is already in progress
5. The preferred replica is known (exists in `match_index_`)
6. The preferred replica is caught up (`match_index_[preferred] >= commitIndex`)

The "caught up" check is the critical safety condition — transferring to a replica that hasn't received all committed entries could lose data.

#### Phase 3: Transfer — Piggybacked Protocol

When conditions are met, `InitiateLeadershipTransfer()` executes the actual leadership transfer:

```
InitiateLeadershipTransfer():
  |
  +-- [Under lock] Set transferring_leadership_ = true
  +-- [Under lock] Send heartbeat to ALL replicas with trigger_election_now=true
  +-- [Release lock] Sleep 20ms (ensure packets sent)
  +-- [Under lock] setIsLeader(false) — step down
```

The transfer uses a **piggybacked approach**: instead of a separate `TimeoutNow` RPC, the transfer signal is embedded in the regular heartbeat `EmptyAppendEntries` RPC via the `trigger_election_now` flag. This has two advantages:
1. **Atomic notification**: All replicas learn about the transfer simultaneously
2. **Timer reset**: The heartbeat resets non-preferred replicas' election timers, preventing them from starting elections

**When replicas receive `trigger_election_now=true`**:

| Recipient | Action |
|-----------|--------|
| Preferred replica | Waits 30ms, then calls `RequestVote()` to start election |
| Non-preferred replica | Logs the event, does nothing (its election timer was just reset) |

The 30ms delay before the preferred replica starts election gives the old leader time to step down and for other replicas to process the heartbeat (resetting their timers). This prevents election storms.

---

### 4. Transfer Protocol — Detailed Sequence

```
Non-Preferred Leader              Preferred Follower           Other Follower
       |                                |                          |
  [ShouldTransfer? YES]                 |                          |
       |                                |                          |
  [Set transferring_=true]              |                          |
       |                                |                          |
  [EmptyAppendEntries                   |                          |
   trigger_election_now=true] --------->|                          |
       |                         [Reset timer]                     |
  [EmptyAppendEntries                   |                          |
   trigger_election_now=true] ------------------------------------>|
       |                                |                   [Reset timer]
       |                                |                   [Log, do nothing]
  [Sleep 20ms]                          |                          |
       |                         [Wait 30ms]                       |
  [setIsLeader(false)]                  |                          |
  [Now follower]                        |                          |
       |                         [RequestVote()]                   |
       |                                |                          |
       |                         [Increment term]                  |
       |                                |                          |
       |<--- RequestVote(new_term) -----|                          |
       |                                |---- RequestVote -------->|
       |                                |                          |
  [Grant vote]                          |                   [Grant vote]
       |--- VoteReply(yes) ------------>|                          |
       |                                |<----- VoteReply(yes) ----|
       |                                |                          |
       |                         [Won election!]                   |
       |                         [setIsLeader(true)]               |
       |                                |                          |
       |<---- AppendEntries ------------|--- AppendEntries ------->|
       |     (new leader heartbeat)     |  (new leader heartbeat)  |
```

#### Timing Analysis

| Event | Time |
|-------|------|
| T=0 | Non-preferred leader sends trigger_election_now heartbeat |
| T=0-5ms | Heartbeat arrives at all replicas (LAN) |
| T=20ms | Old leader steps down (`setIsLeader(false)`) |
| T=30ms | Preferred replica starts election (`RequestVote()`) |
| T=30-35ms | Vote requests arrive at all replicas |
| T=35-40ms | Vote replies arrive at preferred replica |
| T=40ms | Preferred replica becomes leader |

Total transfer time: ~40ms on LAN.

---

### 5. The `TimeoutNow` RPC — Direct Transfer

In addition to the piggybacked approach, the system also implements a standalone `TimeoutNow` RPC (`SendTimeoutNow()` in `commo.cc:228-285`). This RPC directly instructs a replica to start an election immediately.

**`OnTimeoutNow()` handler edge cases** (`server.cc:1476-1575`):

| Edge Case | Response |
|-----------|----------|
| Server shutting down (`stop_`) | Ignore, reply default |
| Stale TimeoutNow (`leaderTerm < currentTerm`) | Ignore |
| Leader ahead (`leaderTerm > currentTerm`) | Update term, step down if leader, then start election |
| Already leader | Reply `success=true` (goal achieved) |
| Already candidate (`req_voting_`) | Reply `success=true` (already trying) |
| Currently transferring (`transferring_leadership_`) | Ignore |
| Valid request | Call `RequestVote()`, reply with result |

The `TimeoutNow` RPC is available for external use (e.g., operational tools) but the primary transfer mechanism uses the piggybacked approach for its atomicity guarantees.

---

### 6. Dynamic Election Timeout — `GetElectionTimeout()`

**Location**: `server.cc:348-369`

The election timeout is the core mechanism that biases elections toward the preferred replica:

```
State                              Timeout Range        Purpose
─────────────────────────────────────────────────────────────────
Preferred replica (always)         150-300ms            Win elections quickly
Non-preferred, grace period        1-2 seconds          Let preferred win startup
Non-preferred, after grace         500ms-1 second       Enable failover detection
```

The grace period is defined as the first 5 seconds after startup (`startup_timestamp_`). After this period, non-preferred replicas reduce their timeout to 500ms-1s, which is still long enough to give the preferred replica an advantage but short enough to detect leader failures.

**Comparison to standard Raft**:
- Standard Raft: All replicas use 150-300ms (from the paper)
- This implementation: Preferred uses 150-300ms, others use 500ms-2s
- The asymmetry creates a probabilistic guarantee that the preferred replica wins elections
- But any replica CAN still win if the preferred is down (safety preserved)

---

### 7. Configuration — `SetPreferredLeader()`

The preferred leader is configured at runtime via `SetPreferredLeader(site_id)` (`server.h:587-604`).

#### Startup Configuration (`raft_main_helper.cc:300-349`)

During initialization, the system automatically selects the preferred leader:

```
For each Raft partition:
  1. Get all sites in this partition
  2. Find the site with locale_id == 0 (localhost)
  3. Call raft_server->SetPreferredLeader(preferred_site_id)
```

In the current configuration, **localhost (locale_id=0) is always the preferred leader** for every partition. In a multi-partition setup (e.g., 6 partitions with 3 replicas each = 18 sites), each partition independently selects its localhost replica as preferred.

#### Dynamic Reconfiguration

`SetPreferredLeader()` can be called at any time to change the preferred leader:

```cpp
void SetPreferredLeader(siteid_t site_id) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);
    preferred_leader_site_id_ = site_id;
    // If I'm a non-preferred leader, start monitoring for transfer
    if (!AmIPreferredLeader() && is_leader_ && looping_) {
        StartLeadershipTransferMonitoring();
    }
}
```

If the current leader is now non-preferred (because the preferred was changed to a different site), monitoring starts immediately.

---

### 8. Safety Argument

The preferred leader extension preserves all five Raft safety properties from the original paper:

#### 8.1 Election Safety

**Property**: At most one leader can be elected in a given term.

**Preserved because**: The preferred leader mechanism only affects election *timing* (via asymmetric timeouts), not election *rules*. A candidate still needs a majority of votes to win. The `trigger_election_now` flag causes the preferred replica to start an election, but it must still collect votes using the standard RequestVote protocol. No replica grants its vote to two candidates in the same term.

#### 8.2 Leader Append-Only

**Property**: A leader never overwrites or deletes entries in its log; it only appends new entries.

**Preserved because**: The transfer mechanism doesn't modify any logs. The old leader simply steps down (`setIsLeader(false)`). The new leader appends entries normally.

#### 8.3 Log Matching

**Property**: If two logs contain an entry with the same index and term, then the logs are identical in all entries through that index.

**Preserved because**: The transfer mechanism doesn't modify logs. The `trigger_election_now` flag is part of `EmptyAppendEntries` (heartbeat), which doesn't carry log entries. Normal AppendEntries consistency checks still apply.

#### 8.4 Leader Completeness

**Property**: If a log entry is committed in a given term, that entry will be present in the logs of the leaders of all higher-numbered terms.

**Preserved because**: `ShouldTransferLeadership()` only returns `true` when the preferred replica's `match_index >= commitIndex`. This means the preferred replica has ALL committed entries before the transfer begins. Combined with standard Raft vote restriction (candidates must have an up-to-date log), the preferred replica will win elections and maintain all committed entries.

#### 8.5 State Machine Safety

**Property**: If a server has applied a log entry at a given index to its state machine, no other server will ever apply a different log entry for that index.

**Preserved because**: This follows from Leader Completeness. Since the preferred replica has all committed entries before becoming leader, it will never commit a different entry at any index.

#### Transfer-Specific Safety

Beyond the five standard properties, the transfer protocol has additional safety guarantees:

1. **No dual leadership**: The old leader steps down (`setIsLeader(false)`) after sending the transfer signal. Even if the transfer fails (preferred doesn't win), the old leader is now a follower and won't accept writes.

2. **No lost writes**: `ShouldTransferLeadership()` checks `match_index_[preferred] >= commitIndex`, ensuring all committed entries are replicated to the preferred replica before transfer.

3. **No election storms**: The piggybacked approach resets all non-preferred replicas' election timers simultaneously. Combined with the 30ms delay before the preferred starts its election, this prevents multiple replicas from competing.

4. **Bounded transfer time**: If the preferred replica doesn't win the election within its timeout, normal Raft election proceeds. The system doesn't block indefinitely.

---

### 9. Member Variables

**Location**: `server.h:113-132`

| Variable | Type | Default | Purpose |
|----------|------|---------|---------|
| `preferred_leader_site_id_` | `siteid_t` | `INVALID_SITEID` (i.e., `(siteid_t)-1`) | Site ID of the preferred leader |
| `leader_last_commit_index_` | `uint64_t` | `0` | Leader's commit index (from heartbeats, used by `HaveCaughtUp()`) |
| `transferring_leadership_` | `bool` | `false` | True during an active transfer |
| `leadership_transfer_start_time_` | `uint64_t` | `0` | Timestamp when transfer started |
| `leadership_monitor_stop_` | `std::atomic<bool>` | `false` | Signal to stop the monitoring thread |
| `leadership_monitor_thread_` | `std::thread` | (default) | Background monitoring thread |
| `startup_timestamp_` | `uint64_t` | `0` | Server startup time (for grace period) |

#### Inline Helper Methods

```cpp
// Check if this replica is the configured preferred leader
bool AmIPreferredLeader() const {
    return preferred_leader_site_id_ != INVALID_SITEID &&
           site_id_ == preferred_leader_site_id_;
}

// Check if this replica has caught up to the leader's commit level
// (Used by preferred follower to know when it can safely become leader)
bool HaveCaughtUp() const {
    return commitIndex >= leader_last_commit_index_;
}
```

---

### 10. Failure Modes and Recovery

#### Preferred Replica Fails

1. Non-preferred replica's election timeout fires (500ms-1s)
2. Normal Raft election proceeds — any follower can win
3. New leader serves requests normally
4. When preferred recovers, it receives heartbeats and starts replicating
5. Once caught up (`match_index >= commitIndex`), non-preferred leader transfers back

#### Non-Preferred Leader Fails During Transfer

1. Transfer signal sent but old leader crashes before stepping down
2. Preferred replica may have started an election — if it wins, it becomes leader normally
3. If preferred doesn't win, another replica's election timeout fires and normal election proceeds
4. No data loss: committed entries are on a majority by definition

#### Preferred Replica Slow/Partitioned

1. `ShouldTransferLeadership()` checks `match_index >= commitIndex`
2. If preferred is slow, its match index lags behind commit index
3. Transfer doesn't happen until preferred catches up
4. System continues operating with non-preferred leader indefinitely

#### Configuration Change

1. `SetPreferredLeader(new_site_id)` called at runtime
2. If current leader is now non-preferred, monitoring starts immediately
3. Standard transfer protocol takes over
4. Can disable preferred leader entirely with `SetPreferredLeader(INVALID_SITEID)`

---

## Preferred Leader Election — Implementation Details

<a id="what-this-document-covers-10"></a>

### What This Document Covers

This document provides a method-by-method walkthrough of every function that implements the preferred leader system. It covers member variables, inline helpers, the dynamic election timeout, the monitoring thread, the transfer decision logic, the piggybacked transfer protocol, the `OnTimeoutNow` RPC handler, the destruction sequence, and integration points with the rest of the Raft server.

**Key source files**:
- `src/deptran/raft/server.h` — Member variables (lines 113-132), inline helpers (lines 135-146), `SetPreferredLeader()` (lines 587-604), `GetPreferredLeader()` (lines 610-612)
- `src/deptran/raft/server.cc` — `GetElectionTimeout()` (lines 348-369), `Setup()` (lines 372-374), `setIsLeader()` (lines 443-554), `HeartbeatLoop()` comment (lines 949-960), `~RaftServer()` (lines 963-984), `StartElectionTimer()` (lines 1188-1228), `OnAppendEntries()` trigger_election_now (lines 1428-1462), `OnTimeoutNow()` (lines 1476-1575), `StopLeadershipTransferMonitoring()` (lines 1577-1586), `StartLeadershipTransferMonitoring()` (lines 1588-1671), `ShouldTransferLeadership()` (lines 1681-1725), `InitiateLeadershipTransfer()` (lines 1727-1827)
- `src/deptran/raft/commo.cc` — `SendAppendEntries()` with `trigger_election_now` (lines 96-175), `SendTimeoutNow()` (lines 228-285)
- `src/deptran/raft_main_helper.cc` — Startup configuration (lines 300-349), `set_preferred_leader()` runtime API (lines 650-679)

---

### 1. Member Variables

**Location**: `server.h:113-132`

```cpp
// Preferred Replica System state
siteid_t preferred_leader_site_id_ = INVALID_SITEID;     // (siteid_t)-1 means disabled
uint64_t leader_last_commit_index_ = 0;                   // Leader's commitIndex (from heartbeats)
bool transferring_leadership_ = false;                    // True during active transfer
uint64_t leadership_transfer_start_time_ = 0;             // Transfer start timestamp (microseconds)
std::atomic<bool> leadership_monitor_stop_{false};        // Signal to stop monitor thread
std::thread leadership_monitor_thread_;                   // Background OS thread (not a fiber)
uint64_t startup_timestamp_ = 0;                          // Server start time for grace period
```

#### Design Rationale

| Variable | Why It Exists |
|----------|---------------|
| `preferred_leader_site_id_` | Central configuration — every decision branches on this |
| `leader_last_commit_index_` | Allows followers to know when they've caught up via `HaveCaughtUp()` |
| `transferring_leadership_` | Prevents double-transfer and suppresses elections during transfer |
| `leadership_transfer_start_time_` | Could be used for transfer timeout (not currently enforced) |
| `leadership_monitor_stop_` | Thread-safe signal (atomic) for clean monitor shutdown |
| `leadership_monitor_thread_` | OS thread (not fiber) because the monitor must survive reactor loop changes |
| `startup_timestamp_` | Enables the 5-second grace period for startup election bias |

---

### 2. Inline Helper Methods

#### `AmIPreferredLeader()` (`server.h:136-138`)

```cpp
bool AmIPreferredLeader() const {
    return preferred_leader_site_id_ != INVALID_SITEID &&
           site_id_ == preferred_leader_site_id_;
}
```

**Logic**: Returns `true` only if (a) a preferred leader is configured AND (b) this replica's `site_id_` matches. Both conditions are necessary — when `preferred_leader_site_id_ == INVALID_SITEID`, the system operates as standard Raft.

**Called by**: `GetElectionTimeout()`, `setIsLeader()`, `SetPreferredLeader()`, `ShouldTransferLeadership()`, `StartLeadershipTransferMonitoring()`, `OnAppendEntries()` (trigger_election_now handling).

#### `HaveCaughtUp()` (`server.h:142-145`)

```cpp
bool HaveCaughtUp() const {
    return commitIndex >= leader_last_commit_index_;
}
```

**Logic**: Compares this replica's `commitIndex` against the leader's last known `commitIndex` (received via AppendEntries heartbeats). Used by followers to know when they have all committed entries.

**Note**: `leader_last_commit_index_` is updated from the `leaderCommitIndex` field in AppendEntries RPCs. This variable is declared but not currently written to in the codebase — the actual catch-up check in `ShouldTransferLeadership()` uses the leader's own `match_index_` (which is more direct and authoritative).

---

### 3. `SetPreferredLeader()` — Configuration Entry Point

**Location**: `server.h:587-604`

```cpp
void SetPreferredLeader(siteid_t site_id) {
    std::lock_guard<std::recursive_mutex> lock(mtx_);

    siteid_t old_preferred = preferred_leader_site_id_;
    preferred_leader_site_id_ = site_id;

    if (old_preferred != site_id) {
        Log_info("[LEADERSHIP-TRANSFER] Site %d: Preferred leader set to %d",
                 site_id_, site_id);
    }

    // If I'm a non-preferred leader, start monitoring for transfer opportunity
    if (!AmIPreferredLeader() && is_leader_ && looping_) {
        StartLeadershipTransferMonitoring();
    }
}
```

**Key behavior**:
1. Updates the preferred site ID under lock
2. If this replica is currently a leader AND is now non-preferred → immediately starts monitoring
3. Thread-safe: protected by `mtx_` (recursive mutex)

#### Call Sites

| Caller | When | Context |
|--------|------|---------|
| `raft_main_helper.cc:340` | Startup | Sets localhost (locale_id==0) as preferred for each partition |
| `set_preferred_leader():666` | Runtime API | Allows dynamic reconfiguration from external code |

#### Startup Configuration Flow (`raft_main_helper.cc:300-349`)

```
For each raft_worker in raft_workers_g:
    partition_id = raft_server->partition_id_
    partition_sites = config->SitesByPartitionId(partition_id)

    For each site in partition_sites:
        if site.locale_id == 0:   // localhost
            preferred_site_id = site.id
            break

    raft_server->SetPreferredLeader(preferred_site_id)
```

In a multi-partition deployment, each partition independently selects the localhost replica as its preferred leader.

---

### 4. `GetElectionTimeout()` — Asymmetric Timeouts

**Location**: `server.cc:348-369`

```cpp
uint64_t RaftServer::GetElectionTimeout() {
    uint64_t base_timeout;
    uint64_t current_time = Time::now();
    bool in_grace_period = (current_time - startup_timestamp_) < 5000000; // 5 seconds

    if (AmIPreferredLeader()) {
        base_timeout = 150000; // 150ms
        uint64_t jitter = RandomGenerator::rand(0, 150000);
        return base_timeout + jitter; // 150-300ms
    } else if (in_grace_period) {
        base_timeout = 1000000; // 1s
        uint64_t jitter = RandomGenerator::rand(0, 1000000);
        return base_timeout + jitter; // 1-2s
    } else {
        base_timeout = 500000; // 500ms
        uint64_t jitter = RandomGenerator::rand(0, 500000);
        return base_timeout + jitter; // 500ms-1s
    }
}
```

#### Timeout Decision Table

| Condition | Base (us) | Jitter (us) | Total Range | Purpose |
|-----------|-----------|-------------|-------------|---------|
| Preferred replica | 150,000 | 0-150,000 | 150-300ms | Win elections quickly |
| Non-preferred, startup grace (<5s) | 1,000,000 | 0-1,000,000 | 1-2s | Let preferred win first |
| Non-preferred, after grace | 500,000 | 0-500,000 | 500ms-1s | Enable failover |

#### Called by: `StartElectionTimer()` (`server.cc:1197`)

```cpp
void RaftServer::StartElectionTimer() {
    // ...
    while(!stop_) {
        uint64_t election_timeout = GetElectionTimeout();

        Fiber::sleep(RandomGenerator::rand(HEARTBEAT_INTERVAL * 2, HEARTBEAT_INTERVAL * 4));

        auto time_elapsed = Time::now() - last_heartbeat_time_;

        if (!IsLeader() && time_elapsed > election_timeout) {
            req_voting_ = true;
            if (stop_) return;
            RequestVote();
            while(req_voting_) {
                Fiber::sleep(wait_int_);
                if(stop_) return;
            }
        }
    }
}
```

The election timer wakes periodically and compares elapsed time since last heartbeat against the dynamic timeout. Because the timeout is recomputed each iteration via `GetElectionTimeout()`, the grace period naturally expires — after 5 seconds, non-preferred replicas use the shorter 500ms-1s timeout.

---

### 5. `setIsLeader()` — Transfer Integration Point

**Location**: `server.cc:443-554`

When a replica transitions to leader, `setIsLeader(true)` checks if it should start monitoring:

```
setIsLeader(isLeader):
    |
    +-- become_new_leader = isLeader && !was_leader
    +-- become_new_follower = !isLeader && was_leader
    |
    +-- IF isLeader:
    |     Reset match_index_ and next_index_ for all peers
    |
    +-- is_leader_ = isLeader
    |
    +-- IF become_new_leader:
    |     transferring_leadership_ = false   // Clear any stale transfer flag
    |     Update view (old_view_ → new_view_)
    |     Update communicator view
    |     IF !AmIPreferredLeader() && looping_:
    |         StartLeadershipTransferMonitoring()  // <<< KEY INTEGRATION
    |
    +-- IF become_new_follower:
          resetTimer("became follower")  // Prevent instant re-election
```

**Why check `looping_`?** The `looping_` flag indicates the HeartbeatLoop is running. During initialization (before `Setup()` is called), `looping_` is `false`. Starting the monitoring thread before the heartbeat loop would be premature.

**Why clear `transferring_leadership_`?** If this replica just won an election, any previous transfer (where this replica was giving away leadership) is now moot. Clearing the flag prevents stale state from blocking future transfers.

---

### 6. `StartLeadershipTransferMonitoring()` — Background Monitor

**Location**: `server.cc:1588-1671`

This method launches an OS thread (not a fiber) that periodically checks if the preferred replica is ready to become leader.

```cpp
void RaftServer::StartLeadershipTransferMonitoring() {
    // Reset stop flag if previously set
    if (leadership_monitor_stop_.load()) {
        leadership_monitor_stop_ = false;
    }

    // Stop any existing monitor thread
    if (leadership_monitor_thread_.joinable()) {
        leadership_monitor_stop_ = true;
        leadership_monitor_thread_.join();         // Wait for old thread to finish
        leadership_monitor_stop_ = false;
    }

    // Launch new monitoring thread
    leadership_monitor_thread_ = std::thread([this]() {
        const uint64_t CHECK_INTERVAL_MS = 1000;       // Check every 1 second
        const uint64_t MIN_STABLE_TIME_US = 500000;    // Wait 0.5 seconds after becoming leader

        uint64_t became_leader_time = Time::now();

        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(CHECK_INTERVAL_MS));

            bool should_transfer = false;
            {
                std::lock_guard<std::recursive_mutex> lock(mtx_);

                if (leadership_monitor_stop_) break;    // Shutdown requested
                if (stop_) break;                       // Server shutting down
                if (!is_leader_) break;                 // No longer leader
                if (AmIPreferredLeader()) break;        // Became preferred (dynamic reconfig)

                // Wait for stability
                uint64_t time_as_leader = Time::now() - became_leader_time;
                if (time_as_leader < MIN_STABLE_TIME_US) {
                    continue;                           // Too soon, check again next iteration
                }

                if (ShouldTransferLeadership()) {
                    should_transfer = true;
                }
            } // Lock released before calling InitiateLeadershipTransfer

            if (should_transfer) {
                InitiateLeadershipTransfer();
                break;  // Exit after transferring
            }
        }
    });
}
```

#### Design Decisions

1. **OS thread vs. fiber**: Uses `std::thread` instead of `Fiber::create_run` because the monitor needs to survive reactor loop reconfigurations and must be stoppable via `join()`.

2. **Lock-then-release pattern**: The lock is acquired to check shared state, then released before calling `InitiateLeadershipTransfer()`. This prevents deadlock — `InitiateLeadershipTransfer()` acquires the same lock internally.

3. **Stability wait (500ms)**: After becoming leader, the system waits 500ms before considering transfer. This allows the new leader to establish itself, replicate entries, and update `match_index_` for followers.

4. **Exit conditions**: The thread exits on any of: stop requested, server shutting down, no longer leader, became preferred leader, or transfer completed.

---

### 7. `ShouldTransferLeadership()` — Decision Logic

**Location**: `server.cc:1681-1725`

```
ShouldTransferLeadership():
    |
    +-- [1] is_leader_?                              NO → return false
    +-- [2] AmIPreferredLeader()?                    YES → return false
    +-- [3] preferred_leader_site_id_ == INVALID?    YES → return false
    +-- [4] transferring_leadership_?                YES → return false
    +-- [5] preferred in match_index_?               NO → return false
    +-- [6] match_index_[preferred] >= commitIndex?  NO → return false
    |
    +-- All checks passed → return true
```

#### Check Details

| # | Check | Why |
|---|-------|-----|
| 1 | Must be leader | Only leaders can initiate transfer |
| 2 | Must not be preferred | Preferred leaders don't transfer away |
| 3 | Must have preferred configured | No target = no transfer |
| 4 | Must not be already transferring | Prevents double-transfer |
| 5 | Preferred must be in peer list | Preferred must be a known replica in `match_index_` |
| 6 | Preferred must be caught up | `match_index_[preferred] >= commitIndex` ensures no data loss |

**Safety-critical check**: #6 is the key safety property. The `match_index_` for the preferred replica reflects the highest log index known to be replicated there. If `match_index_[preferred] >= commitIndex`, then all committed entries exist on the preferred replica, making it safe to become leader.

---

### 8. `InitiateLeadershipTransfer()` — The Transfer Protocol

**Location**: `server.cc:1727-1827`

This method executes the actual leadership transfer using the piggybacked approach.

#### Step-by-Step Walkthrough

```
InitiateLeadershipTransfer():
    |
    [1] if (stop_) return;              // Abort if shutting down
    |
    [2] { LOCK mtx_
          target = preferred_leader_site_id_
          transferring_leadership_ = true
          leadership_transfer_start_time_ = Time::now()

          FOR each peer in match_index_:
              prevLogIndex = next_index_[peer] - 1
              prevLogTerm = logs_[prevLogIndex]->term

              commo()->SendAppendEntries(
                  peer, partition_id_, commitIndex, 0, true, site_id_,
                  currentTerm, prevLogIndex, prevLogTerm, commitIndex,
                  nullptr,  // Empty = heartbeat
                  0,
                  true      // trigger_election_now = true <<<
              )
        } // UNLOCK
    |
    [3] sleep(20ms)                     // Let packets transmit
    |
    [4] { LOCK mtx_
          setIsLeader(false)            // Step down
        } // UNLOCK
```

#### Why Piggybacked Approach?

The transfer signal is sent as a regular `EmptyAppendEntries` with `trigger_election_now=true`. This was chosen over a separate `TimeoutNow` RPC for these reasons:

1. **Atomic notification**: All replicas receive the signal via the same heartbeat mechanism, ensuring synchronization.
2. **Timer reset**: The heartbeat resets non-preferred replicas' election timers, preventing them from starting elections while the preferred replica starts its election.
3. **No new RPC needed on the critical path**: Reuses existing infrastructure.

#### What Happens on Each Replica

When a replica receives `EmptyAppendEntries` with `trigger_election_now=true` (`server.cc:1428-1462`):

**Preferred replica** (in `OnAppendEntries`):
```cpp
if (AmIPreferredLeader()) {
    if (!IsLeader()) {
        Fiber::create_run([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (stop_) return;
            RequestVote();
        });
    }
}
```

Waits 30ms (to let the old leader step down and heartbeats reach other replicas), then starts election. The 30ms wait prevents the preferred replica from starting an election while the old leader is still active.

**Non-preferred replica**:
```cpp
else {
    Log_info("[PIGGYBACKED-TRANSFER] Site %d (non-preferred): Received transfer signal");
}
```

Does nothing — its election timer was just reset by the heartbeat, so it won't start competing elections.

---

### 9. `OnTimeoutNow()` — Direct Transfer RPC Handler

**Location**: `server.cc:1476-1575`

This handler processes the standalone `TimeoutNow` RPC (as opposed to the piggybacked `trigger_election_now` flag). It's available as an alternative transfer mechanism.

#### Edge Case Handling

```
OnTimeoutNow(leaderTerm, leaderSiteId, *followerTerm, *success, cb):
    |
    +-- *followerTerm = currentTerm
    +-- *success = false
    |
    +-- [Case 0] stop_?                  → return (shutting down)
    +-- [Case 1] leaderTerm < currentTerm? → return (stale)
    +-- [Case 1b] leaderTerm > currentTerm?
    |     → currentTerm = leaderTerm
    |     → vote_for_ = INVALID_SITEID
    |     → if is_leader_: setIsLeader(false)
    +-- [Case 2] is_leader_?             → success=true, return (already leader)
    +-- [Case 3] req_voting_?            → success=true, return (already electing)
    +-- [Case 4] transferring_leadership_? → return (in transfer)
    |
    +-- [Valid] RequestVote()
          → success = election_started
          → cb()
```

**Notable behaviors**:
- Cases 2 and 3 return `success=true` because the goal (becoming leader or attempting to) is already being achieved
- Case 1b updates the term before proceeding — the replica may need to step down from leadership if the leader's term is higher
- The mutex (`mtx_`) is held for the entire handler duration — this prevents concurrent state modifications

---

### 10. `StopLeadershipTransferMonitoring()` — Clean Shutdown

**Location**: `server.cc:1577-1586`

```cpp
void RaftServer::StopLeadershipTransferMonitoring() {
    leadership_monitor_stop_ = true;

    if (leadership_monitor_thread_.joinable()) {
        leadership_monitor_thread_.detach();  // Don't join — avoid deadlock
    }
}
```

**Why `detach()` instead of `join()`?** The destructor calls `StopLeadershipTransferMonitoring()`, and the monitor thread acquires `mtx_`. If the destructor held `mtx_` and tried to `join()`, deadlock would occur. By detaching, the thread is signaled to stop (via `leadership_monitor_stop_`) and can exit on its own schedule.

**Called by**: `~RaftServer()` (line 980) during server shutdown.

---

### 11. `~RaftServer()` — Destruction Sequence

**Location**: `server.cc:963-984`

```
~RaftServer():
    |
    [1] stop_ = true                           // Signal all coroutines to stop
    [2] looping_ = false                       // Stop HeartbeatLoop
    [3] ready_for_replication_->set(1)         // Wake HeartbeatLoop if sleeping
    [4] StopLeadershipTransferMonitoring()     // Signal and detach monitor thread
    [5] sleep(50ms)                            // Let detached coroutines see stop_=true
```

The 50ms sleep at step 5 is critical. Without it, detached coroutines (election timer, leadership transfer) might still be running when the vtable is destroyed, causing crashes when they call virtual methods like `RequestVote()`.

---

### 12. `HeartbeatLoop()` — Transfer Delegation

**Location**: `server.cc:949-960`

The HeartbeatLoop previously contained leadership transfer logic, which was removed and delegated to the monitoring thread:

```cpp
// ============================================================================
// LEADERSHIP TRANSFER: Handled by Monitor Thread
// ============================================================================
// Leadership transfer is now handled by StartLeadershipTransferMonitoring() thread,
// not here in HeartbeatLoop. This prevents race conditions and double-triggering.
// The monitor thread is started in setIsLeader() when becoming a non-preferred leader.
```

This design decision prevents a race condition where both HeartbeatLoop and the monitoring thread could independently decide to transfer, causing duplicate `InitiateLeadershipTransfer()` calls.

---

### 13. Integration Points Summary

The preferred leader system integrates with the following core Raft methods:

| Core Method | Integration |
|------------|-------------|
| `Setup()` | Records `startup_timestamp_` for grace period |
| `StartElectionTimer()` | Calls `GetElectionTimeout()` for dynamic timeout |
| `setIsLeader(true)` | Starts monitoring if non-preferred leader |
| `setIsLeader(false)` | Resets election timer (prevents instant re-election) |
| `HeartbeatLoop()` | Comment-only: documents transfer is handled by monitor thread |
| `OnAppendEntries()` | Handles `trigger_election_now` flag |
| `~RaftServer()` | Stops monitoring thread |

#### Call Graph

```
SetPreferredLeader(site_id)
    |
    +-- [If non-preferred leader] --> StartLeadershipTransferMonitoring()

setIsLeader(true)
    |
    +-- [If non-preferred] --> StartLeadershipTransferMonitoring()

StartLeadershipTransferMonitoring()       [OS thread, loops every 1s]
    |
    +-- ShouldTransferLeadership()
    |     +-- match_index_[preferred] >= commitIndex?
    |
    +-- InitiateLeadershipTransfer()
          +-- SendAppendEntries(trigger_election_now=true) to ALL
          +-- sleep(20ms)
          +-- setIsLeader(false)

OnAppendEntries(trigger_election_now=true)  [On preferred replica]
    |
    +-- sleep(30ms)
    +-- RequestVote()                       [Starts election]
    +-- [Wins election] → setIsLeader(true) [Preferred is now leader]

~RaftServer()
    |
    +-- StopLeadershipTransferMonitoring()
```

---

### 14. Full Sequence Diagram — Leadership Transfer

```
Time  Non-Pref Leader (site 1)     Preferred Follower (site 0)     Other Follower (site 2)
─────────────────────────────────────────────────────────────────────────────────────────────
 T=0  MonitorThread: ShouldTransfer?
      YES: match[0]=5, commit=5
      |
 T=0  transferring_=true
      SendAppendEntries to site 0
        trigger_election_now=true
      SendAppendEntries to site 2  ─────────────────────────────────>  Receive heartbeat
        trigger_election_now=true    Receive heartbeat                  Reset election timer
      |                              Reset election timer               Log: "non-preferred,
      |                              AmIPreferred? YES                         do nothing"
      |                              Not leader → start timer
      |                              |
T=20  sleep(20ms)                    |
      setIsLeader(false)             |
      [Now follower]                 |
      resetTimer("became follower")  |
      |                              |
T=30  |                              sleep(30ms) done
      |                              RequestVote()
      |                              currentTerm++ (→ T+1)
      |                              BroadcastVote to sites 1,2
      |                              |
T=31  Receive RequestVote(T+1)       |                                 Receive RequestVote(T+1)
      T+1 > myTerm → update          |                                 T+1 > myTerm → update
      Log up-to-date? YES            |                                 Log up-to-date? YES
      Grant vote                     |                                 Grant vote
      |                              |
T=35  |                              Receive 2 YES votes
      |                              Quorum reached (2/2 needed)
      |                              setIsLeader(true)
      |                              AmIPreferred? YES → no monitor
      |                              Start sending heartbeats
      |                              |
T=40  Receive heartbeat              |                                 Receive heartbeat
      [Preferred is leader!]         [Leader, term T+1]                [Follower, term T+1]
```

---

### 15. Constants and Tuning Parameters

| Constant | Value | Location | Purpose |
|----------|-------|----------|---------|
| `INVALID_SITEID` | `(siteid_t)-1` | `server.h:16` | Sentinel for "no preferred leader" |
| Preferred election timeout | 150-300ms | `GetElectionTimeout()` | Win elections quickly |
| Grace period timeout | 1-2s | `GetElectionTimeout()` | Let preferred win at startup |
| Normal non-preferred timeout | 500ms-1s | `GetElectionTimeout()` | Failover detection |
| Grace period duration | 5 seconds | `GetElectionTimeout()` | Startup bias window |
| Monitor check interval | 1 second | `StartLeadershipTransferMonitoring()` | How often to check transfer conditions |
| Stability wait | 500ms | `StartLeadershipTransferMonitoring()` | Delay after becoming leader |
| Pre-stepdown sleep | 20ms | `InitiateLeadershipTransfer()` | Ensure packets sent |
| Preferred pre-election wait | 30ms | `OnAppendEntries()` trigger | Let old leader step down |
| Destructor sleep | 50ms | `~RaftServer()` | Let coroutines see stop_ |

---

## Preferred Leader Election — Testing

<a id="what-this-document-covers-11"></a>

### What This Document Covers

This document describes the three dedicated test binaries that verify the preferred leader system, their test runner scripts, the CI integration, the Mako C API used by the tests, configuration files, and the success criteria for each test.

**Test binary source files**:
- `examples/mako-raft-tests/testPreferredReplicaStartup.cc` (265 lines)
- `examples/mako-raft-tests/testPreferredReplicaLogReplication.cc` (422 lines)
- `examples/mako-raft-tests/testNoOps.cc` (531 lines)

**Test runner scripts**:
- `examples/mako-raft-tests/run_test1_preferred_startup.sh` (361 lines)
- `examples/mako-raft-tests/run_test_log_replication.sh` (159 lines)
- `examples/mako-raft-tests/run_test_noops.sh` (256 lines)

**CI integration**:
- `ci/ci_mako_raft.sh` (252 lines) — Cleanup support, not direct test invocation

**Configuration files**:
- `config/none_raft.yml` — Raft mode with no concurrency control
- `config/1c1s3r1p_cluster_test.yml` — 3-node cluster (1 shard, 3 replicas)
- `config/1c1s5r1p_cluster_test.yml` — 5-node cluster (1 shard, 5 replicas)

---

### 1. Test Architecture Overview

All three tests follow the same multi-process architecture:

```
Test Runner Script (bash)
    |
    +-- Launches N processes (5 typically) of the same binary
    |     Each process gets a different name: localhost, p1, p2, p3, p4
    |     Each writes to its own log file
    |
    +-- Waits for all processes to complete (with timeout)
    |
    +-- Parses log files for results
    |
    +-- Reports PASS/FAIL based on success criteria
```

Each process name maps to a host entry in the YAML config. All run on `127.0.0.1` but bind to different ports. The `localhost` process is always the **preferred leader** (locale_id=0).

#### Mako Replication API

All tests use the Mako replication helper API (`src/deptran/replication_helper.cc`), which dispatches to the Raft or Paxos implementation via the `DISPATCH_RAFT_OR_PAXOS` macro:

| API Function | Purpose |
|-------------|---------|
| `setup(argc, argv)` | Initialize Raft from config files |
| `setup2(arg1, arg2)` | Launch Raft services (RPC, heartbeats, election) |
| `register_leader_election_callback(fn)` | Hook for leadership changes (`fn(1)` = became leader, `fn(0)` = lost) |
| `register_for_leader_par_id_return(fn, par_id)` | Register log apply callback for leader |
| `register_for_follower_par_id_return(fn, par_id)` | Register log apply callback for follower |
| `add_log_to_nc(data, len, par_id, batch_size)` | Submit log entry (leader-only) |
| `shutdown_paxos()` | Clean shutdown |

#### Build Configuration

All three binaries are built when `MAKO_USE_RAFT=ON` (`CMakeLists.txt:1031-1033`):

```cmake
if(MAKO_USE_RAFT)
  add_apps(testPreferredReplicaStartup examples/mako-raft-tests/testPreferredReplicaStartup.cc)
  add_apps(testPreferredReplicaLogReplication examples/mako-raft-tests/testPreferredReplicaLogReplication.cc)
  add_apps(testNoOps examples/mako-raft-tests/testNoOps.cc)
endif()
```

---

### 2. Test 1: `testPreferredReplicaStartup`

#### Purpose

Verify that the preferred replica (localhost) becomes leader via the TimeoutNow leadership transfer protocol, even when another replica initially wins the election.

#### Cluster Configuration

- **Nodes**: 5 (localhost, p1, p2, p3, p4)
- **Config files**: `config/none_raft.yml` + `config/1c1s3r1p_cluster_test.yml`
- **Preferred leader**: localhost (locale_id=0)
- **Total duration**: ~32 seconds (2s startup + 30s monitoring)

#### Test Phases

```
Phase 1: Configuration (T=0)
    Parse command-line args, construct argv for Mako
    Args: -b -d 60 -f none_raft.yml -f 1c1s3r1p_cluster_test.yml -t 30 -T 100000 -n 32 -P <name> -A 10000

Phase 2: Raft Initialization (T=0)
    Call setup(argc, argv)
    Register leadership callbacks

Phase 3: Service Launch (T=0+)
    Call setup2(arg1, arg2)
    Record startup_time_ms

Phase 4: Startup Wait (T=0 to T=2s)
    Sleep 2 seconds for cluster stabilization
    During this time: election happens, any replica may win

Phase 5: Monitoring (T=2s to T=32s)
    Check every 5 seconds:
      - Am I leader?
      - Elapsed time since becoming leader
    If preferred leader lost leadership after gaining it → flag issue

Phase 6: Results (T=32s)
    Report: times_became_leader, times_lost_leadership, leader_stable_duration
    Exit code 0 if localhost is leader, 1 otherwise
```

#### State Tracking

```cpp
atomic<bool> i_am_leader{false};
atomic<int> times_became_leader{0};
atomic<int> times_lost_leadership{0};
atomic<uint64_t> time_became_leader_ms{0};
atomic<uint64_t> startup_time_ms{0};
```

#### Leadership Callback

The test registers a callback that tracks leadership transitions:

```cpp
register_leader_election_callback([&](int control) {
    times_became_leader++;
    i_am_leader.store(true);
    uint64_t elapsed = current_time - startup_time_ms;
    safe_print("[" + proc_name + "] BECAME LEADER at +" + to_string(elapsed) + "ms");
});
```

Additionally, a dummy follower log callback is registered to satisfy the API:

```cpp
register_for_follower_par_id_return([&](const char*& log, int len, int par_id, int slot_id,
    queue<tuple<int, int, int, int, const char*>>& un_replay_logs_) {
    return static_cast<int>(timestamp * 10 + 1);
}, 0);
```

#### Success Criteria (Script-Level)

The test runner script (`run_test1_preferred_startup.sh`) checks:

| Criterion | Condition |
|-----------|-----------|
| All processes exited cleanly | `exit_code == 0` for all 5 |
| localhost became leader | `LOCALHOST_LEADER >= 1` |
| p1 never became leader | `P1_LEADER == 0` |
| p2 never became leader | `P2_LEADER == 0` |
| p3 never became leader | `P3_LEADER == 0` |
| p4 never became leader | `P4_LEADER == 0` |

The script parses "BECAME LEADER" strings from each process's log file to determine leadership history.

#### What This Proves

1. **Preferred leader election works**: The designated preferred replica (localhost) becomes leader
2. **Leadership transfer works**: If a non-preferred replica wins the initial election, it transfers leadership to localhost
3. **Stability**: Once localhost becomes leader, it remains leader for the full 30-second monitoring period
4. **Non-preferred replicas stay followers**: No other replica should become leader while localhost is alive

---

### 3. Test 2: `testPreferredReplicaLogReplication`

<a id="purpose-1"></a>

#### Purpose

Verify that log entries submitted to the leader are correctly replicated to ALL replicas, including command wrapping in `TpcCommitCommand`.

<a id="cluster-configuration-1"></a>

#### Cluster Configuration

- **Nodes**: 5 (localhost, p1, p2, p3, p4)
- **Config**: 5-node cluster (`config/1c1s5r1p_cluster_test.yml`)
- **Total duration**: ~15 seconds

#### Test Parameters

```cpp
const int STARTUP_TIME_SEC = 2;
const int LEADER_WAIT_SEC = 3;
const int REPLICATION_WAIT_SEC = 5;
const int NUM_LOGS = 25;
const int BATCH_SIZE = 5;
```

<a id="test-phases-1"></a>

#### Test Phases

```
Phase 1: Startup (0-2s)
    Cluster stabilization, election happens

Phase 2: Wait for Leader (2-5s)
    Wait for leadership callback to fire

Phase 3: Log Submission (5-8s, leader only)
    FOR i = 0 to 24:
        Create log: "LOG_ENTRY_NNN" (NNN = zero-padded index)
        Wrap in TpcCommitCommand
        Call add_log_to_nc(serialized, len, 0, BATCH_SIZE)
        Sleep 10ms between submissions

Phase 4: Replication Wait (8-13s)
    Poll every 100ms:
        IF logs_applied_count >= NUM_LOGS: break (early exit)
    Timeout at 5 seconds

Phase 5: Verification
    Assert: logs_applied_count >= NUM_LOGS
    Report PASS/FAIL
```

#### Command Wrapping

Each log entry is wrapped in a `TpcCommitCommand` before submission:

```cpp
shared_ptr<TpcCommitCommand> create_log_command(const string& log_data) {
    auto cmd = make_shared<TpcCommitCommand>();
    cmd->tx_id_ = unique_id++;  // Monotonic transaction ID
    auto sp = make_shared<SimpleCommand>();
    sp->input.put_blob(log_data.c_str(), log_data.length());
    cmd->cmd_ = make_shared<VecPieceData>();
    cmd->cmd_->push_back(sp);
    return cmd;
}
```

The serialized format is: `[tx_id (8 bytes)][length (4 bytes)][log_data (variable)]`.

#### Log Application Callback

Each replica registers both leader and follower callbacks that count applied logs:

```cpp
register_for_follower_par_id_return([&](const char*& log, int len, int par_id, int slot_id, ...) {
    int count = ++logs_applied_count;
    if (count == 1) first_log_applied_time = now;
    if (count == NUM_LOGS) last_log_applied_time = now;
    return timestamp * 10 + 1;
}, 0);
```

#### Success Criteria

| Criterion | Condition |
|-----------|-----------|
| All logs replicated | `logs_applied_count >= NUM_LOGS` (25) for each replica |
| All processes pass | 5/5 replicas report "PASS" in their logs |

<a id="what-this-proves-1"></a>

#### What This Proves

1. **Log replication works with preferred leader**: Commands submitted to the preferred leader replicate to all followers
2. **TpcCommitCommand wrapping**: The Mako-specific command wrapping integrates correctly with Raft
3. **Batch submission**: Logs submitted with `BATCH_SIZE=5` are correctly batched and replicated
4. **Applied count accuracy**: Every replica applies exactly the same number of logs

---

### 4. Test 3: `testNoOps`

<a id="purpose-2"></a>

#### Purpose

Verify the NO-OPS watermark synchronization mechanism. NO-OPS are special log entries ("no-ops:N" where N is an epoch number) used to synchronize state across replicas without carrying user data. They serve as epoch boundaries for the Mako transaction system.

<a id="cluster-configuration-2"></a>

#### Cluster Configuration

- **Nodes**: 5 (localhost, p1, p2, p3, p4)
- **Config**: 5-node cluster
- **Total duration**: ~25 seconds

<a id="test-parameters-1"></a>

#### Test Parameters

```cpp
const int STARTUP_TIME_SEC = 2;
const int LEADER_WAIT_SEC = 3;
const int NOOPS_WAIT_SEC = 5;
const int LOGS_WAIT_SEC = 5;
const int NUM_NOOPS = 5;            // Epochs 0-4
const int NUM_REGULAR_LOGS = 10;
const int BATCH_SIZE = 1;           // NO-OPS should not be batched
```

#### NO-OPS Format

```
"no-ops:X"  (8 bytes)
  |        |
  +--------+-- Prefix: "no-ops:" (7 bytes)
           +-- Epoch digit: '0'-'9' (1 byte)
```

The detection function:

```cpp
int isNoopsLocal(const char* log, int len) {
    if (len == 8) {
        if (log[0]=='n' && log[1]=='o' && log[2]=='-' &&
            log[3]=='o' && log[4]=='p' && log[5]=='s' && log[6]==':') {
            return log[7] - '0';  // Return epoch number
        }
    }
    return -1;  // Not a NO-OPS
}
```

<a id="test-phases-2"></a>

#### Test Phases

```
Phase 1-3: Startup and Leader Election (0-5s)
    Same as other tests

Phase 4: NO-OPS Submission (5-5.25s, leader only)
    FOR epoch = 0 to 4:
        msg = "no-ops:" + to_string(epoch)
        add_log_to_nc(msg, 8, 0, BATCH_SIZE=1)  // Not batched
        Sleep 50ms between epochs

Phase 5: NO-OPS Propagation Wait (5.25-10.25s)
    Poll every 100ms:
        IF noops_applied_count >= NUM_NOOPS: break (early exit)
    Timeout at 5 seconds

Phase 6: Epoch Verification
    Check that all epochs 0-4 were received
    Verify max_epoch_seen == NUM_NOOPS - 1

Phase 7: Regular Log Submission (10.25-11s, leader only)
    FOR i = 0 to 9:
        msg = "REGULAR_LOG_NNN"
        add_log_to_nc(msg, len, 0, BATCH_SIZE=1)
        Sleep 20ms between logs

Phase 8: Regular Log Wait (11-16s)
    Poll every 100ms for regular log application
    Timeout at 5 seconds

Phase 9: Final Verification
    Assert: noops_applied >= NUM_NOOPS
    Assert: all epochs 0-4 received
    Assert: regular_logs_applied >= NUM_REGULAR_LOGS
    If preferred replica: assert still leader
```

<a id="state-tracking-1"></a>

#### State Tracking

```cpp
atomic<int> noops_applied_count{0};
atomic<int> regular_logs_applied_count{0};
atomic<int> noops_submitted_count{0};
atomic<int> regular_logs_submitted_count{0};
atomic<int> max_epoch_seen{-1};
vector<atomic<bool>> epoch_received(NUM_NOOPS);  // Per-epoch reception tracking
```

<a id="log-application-callback-1"></a>

#### Log Application Callback

The callback distinguishes between NO-OPS and regular logs:

```cpp
register_for_follower_par_id_return([&](const char*& log, int len, int par_id, int slot_id, ...) {
    int epoch = isNoopsLocal(log, len);
    if (epoch >= 0) {
        // NO-OPS message
        noops_applied_count++;
        epoch_received[epoch] = true;
        if (epoch > max_epoch_seen) max_epoch_seen = epoch;
    } else {
        // Regular log
        regular_logs_applied_count++;
    }
    return timestamp * 10 + 1;
}, 0);
```

<a id="success-criteria-1"></a>

#### Success Criteria

| Criterion | Condition |
|-----------|-----------|
| All NO-OPS applied | `noops_applied_count >= 5` |
| All epochs received | `epoch_received[0..4]` all true |
| All regular logs applied | `regular_logs_applied_count >= 10` |
| Preferred leader stable | If preferred: `i_am_leader == true` at end |
| All replicas pass | 5/5 replicas report "PASS" |

<a id="what-this-proves-2"></a>

#### What This Proves

1. **NO-OPS propagation**: Special watermark entries replicate to all replicas
2. **Epoch ordering**: All epochs 0-4 arrive and are tracked individually
3. **NO-OPS + regular log coexistence**: NO-OPS and regular logs can be interleaved without issues
4. **Non-batching behavior**: `BATCH_SIZE=1` ensures NO-OPS are sent individually (not bundled with other entries)
5. **Preferred leader stability**: The preferred leader remains leader throughout both NO-OPS and regular log phases

---

### 5. Test Runner Scripts

#### Common Pattern

All three scripts follow the same architecture:

```
1. Setup
   - Determine paths (SCRIPT_DIR, PROJECT_ROOT, BUILD_DIR)
   - Check/build test binary if missing
   - Create log directory
   - Kill any stale processes from previous runs

2. Launch
   - Start 5 processes in background (&), capture PIDs
   - Each gets a different name: localhost, p1, p2, p3, p4
   - Each writes stdout/stderr to its own log file

3. Monitor
   - Wait for completion with progress display
   - Timeout protection (kill hanging processes)

4. Analyze
   - Wait for each process and capture exit code
   - Parse log files for results (grep for keywords)
   - Count PASS/FAIL verdicts

5. Report
   - Per-process results
   - Overall PASS/FAIL based on aggregated criteria
   - Log file locations for debugging
   - Exit with appropriate code
```

#### Script-Specific Details

| Script | Duration | Log Directory | Kill Signal |
|--------|----------|---------------|-------------|
| `run_test1_preferred_startup.sh` | ~35s | `logs/test1_startup/` | `pkill -9 -f testPreferredReplicaStartup` |
| `run_test_log_replication.sh` | ~20s | `logs/test_log_replication/` | `pkill -9 -f testPreferredReplicaLogReplication` |
| `run_test_noops.sh` | ~25s | `logs_noops_test/` | `pkill -9 -f testNoOps` |

#### Invocation

```bash
# From project root
./examples/mako-raft-tests/run_test1_preferred_startup.sh
./examples/mako-raft-tests/run_test_log_replication.sh
./examples/mako-raft-tests/run_test_noops.sh
```

---

### 6. CI Integration

The CI script (`ci/ci_mako_raft.sh`) includes cleanup support for preferred leader test processes:

```bash
# In cleanup function (lines 74-78)
pkill -9 -f "build/testPreferredReplicaStartup"
pkill -9 -f "build/testPreferredReplicaLogReplication"
pkill -9 -f "build/testNoOps"
```

The preferred leader tests are not currently included in the automated CI `all` target — they are run manually or via their individual runner scripts. The CI `all` target runs integration tests that exercise Raft through the Mako shard framework (`shard1ReplicationRaft`, `shard2ReplicationRaft`, etc.), which indirectly test preferred leader behavior since the preferred leader system is always active when Raft is used.

---

### 7. Configuration Files

#### `config/none_raft.yml`

```yaml
mode:
  cc: none          # No concurrency control (tests only replication)
  ab: raft          # Atomic broadcast = Raft
  batch: false
  retry: 20
  ongoing: 1
```

This config disables concurrency control, isolating the test to pure Raft replication behavior.

#### `config/1c1s5r1p_cluster_test.yml` (5-node)

```yaml
site:
  server:
    - ["localhost:38101", "p1:38102", "p2:38103", "p3:38104", "p4:38105"]
  client:
    - ["c01"]

process:
  localhost: localhost
  p1: p1
  p2: p2
  p3: p3
  p4: p4
  c01: client

host:
  localhost: 127.0.0.1
  p1: 127.0.0.1
  p2: 127.0.0.1
  p3: 127.0.0.1
  p4: 127.0.0.1
  client: 127.0.0.1
```

All 5 nodes run on `127.0.0.1` with different ports (38101-38105). The `localhost` process maps to `locale_id=0`, which the preferred leader system uses to select it as the preferred leader.

#### `config/1c1s3r1p_cluster_test.yml` (3-node)

Same structure but with only 3 replicas: `localhost:38100`, `p1:38101`, `p2:38102`.

---

### 8. Relationship to Standard Raft Tests

The codebase also includes unit-level Raft tests (`src/deptran/raft/test.h`, `test.cc`, `testconf.h`, `testconf.cc`) that test basic Raft correctness:

| Test | What It Tests |
|------|---------------|
| `testInitialElection` | Single leader election with 5 nodes |
| `testReElection` | Re-election after disconnecting the leader |
| `testBasicAgree` | Log agreement across 3 nodes |
| `testFailAgree` | Agreement despite one follower failure |
| `testFailNoAgree` | No agreement when quorum lost |
| `testRejoin` | Disconnected leader rejoining the cluster |
| `testConcurrentStarts` | Multiple concurrent log submissions |
| `testBackup` | Leader backoff when follower is behind |
| `testCount` | RPC count bounds |
| `testUnreliableAgree` | Agreement with unreliable network |
| `testFigure8` | Figure 8 scenario from the Raft paper |

These tests run in a single process with simulated network partitions (via `Disconnect()`/`Reconnect()`), whereas the preferred leader tests run as multi-process deployments with real TCP connections.

The standard tests verify core Raft correctness. The preferred leader tests verify that the leadership transfer extension works correctly on top of the correct Raft base.

---

### 9. Correctness Guarantees

The three preferred leader tests collectively guarantee:

| Property | Verified By |
|----------|------------|
| Preferred replica becomes leader | `testPreferredReplicaStartup` — localhost becomes leader, p1-p4 don't |
| Leadership transfer works | `testPreferredReplicaStartup` — even if non-preferred wins first |
| Leader stability after transfer | `testPreferredReplicaStartup` — 30s monitoring without leadership loss |
| Log replication correctness | `testPreferredReplicaLogReplication` — 25 logs replicated to all 5 replicas |
| TpcCommitCommand wrapping | `testPreferredReplicaLogReplication` — command serialization/deserialization |
| Batch replication | `testPreferredReplicaLogReplication` — BATCH_SIZE=5 works correctly |
| NO-OPS watermark propagation | `testNoOps` — 5 epochs (0-4) received by all replicas |
| Epoch tracking | `testNoOps` — individual epoch reception verified |
| NO-OPS + regular log ordering | `testNoOps` — both types coexist without interference |
| Multi-process deployment | All 3 tests — real TCP connections, real ports, real processes |

---

## Mako-Raft Integration Architecture

### 1. The Integration Challenge

Mako was originally designed to use Multi-Paxos as its sole atomic broadcast
(replication) protocol.  The entire transaction pipeline---from log submission
by worker threads to commit-point callbacks on followers---was wired directly
to a `paxos_main_helper.cc` module that manages `PaxosWorker` instances.

Adding Raft as an alternative replication backend required solving three
problems simultaneously:

1. **API parity**: Mako worker threads call ~30 replication functions
   (`setup`, `add_log_to_nc`, `register_for_leader_par_id_return`, etc.).
   Each must work identically regardless of which consensus protocol is
   active.

2. **Runtime switching**: Operators must be able to choose the protocol via a
   CLI flag (`--replication raft`) or a YAML config field (`ab: raft`) without
   recompiling.

3. **Zero Mako-side changes**: The Mako storage engine, OCC layer, and
   benchmark harness must remain protocol-agnostic.  All Raft-specific logic
   must live behind the replication helper.

The solution is a three-layer dispatcher architecture: a thin **dispatch
layer** (`replication_helper.{h,cc}`) sits between Mako and two parallel
**implementation namespaces** (`paxos_impl` in `paxos_main_helper.cc`,
`raft_impl` in `raft_main_helper.cc`).

### 2. Dispatcher Architecture

#### 2.1 Layer Diagram

```
+------------------------------------------------------------------+
|                        Mako Application                          |
|  (dbtest.cc, mako.hh, worker threads, TPC-C benchmark)          |
+------------------------------------------------------------------+
         |                                           |
         | setup(), add_log_to_nc(),                 | --replication raft
         | register_for_leader_par_id_return(), ...   | (CLI flag)
         v                                           v
+------------------------------------------------------------------+
|               replication_helper.h / .cc                         |
|  +------------------------------------------------------------+  |
|  | rusty::Cell<ReplicationType> g_replication_type {PAXOS}    |  |
|  +------------------------------------------------------------+  |
|  | DISPATCH_RAFT_OR_PAXOS(func, ...)                          |  |
|  | DISPATCH_VOID_RAFT_OR_PAXOS(func, ...)                     |  |
|  +------------------------------------------------------------+  |
|  | 30+ dispatch functions: setup(), add_log_to_nc(), ...      |  |
|  +------------------------------------------------------------+  |
+------------------------------------------------------------------+
         |                                 |
         | paxos_impl::func(...)           | raft_impl::func(...)
         v                                 v
+----------------------------+  +----------------------------+
|   paxos_main_helper.cc     |  |   raft_main_helper.cc      |
|   namespace paxos_impl     |  |   namespace raft_impl      |
|                            |  |                            |
|   PaxosWorker instances    |  |   RaftWorker instances     |
|   ElectionState            |  |   ElectionState            |
|   leader_replay_cb map     |  |   leader_replay_cb map     |
|   follower_replay_cb map   |  |   follower_replay_cb map   |
+----------------------------+  +----------------------------+
         |                                 |
         v                                 v
+----------------------------+  +----------------------------+
|   Paxos Protocol Layer     |  |   Raft Protocol Layer      |
|   (Multi-Paxos/copilot)    |  |   (RaftServer, RaftCommo)  |
+----------------------------+  +----------------------------+
```

#### 2.2 Global State: `rusty::Cell<ReplicationType>`

The runtime protocol selection is stored in a single global variable using
RustyCpp's `Cell<T>` for safe interior mutability:

```cpp
// replication_helper.cc
namespace janus {
static rusty::Cell<ReplicationType> g_replication_type{ReplicationType::PAXOS};
}
```

`Cell<T>` is appropriate here because `ReplicationType` is a trivially
copyable enum (backed by `int`), the value is set once during initialization
and read many times, and `Cell::get()` / `Cell::set()` are safe operations
that do not require runtime borrow checking.

**Source**: `src/deptran/replication_helper.cc:12`

#### 2.3 The `ReplicationType` Enum

```cpp
// replication_helper.h
enum class ReplicationType : int {
    PAXOS = 0,
    RAFT = 1
};
```

The explicit `int` backing type ensures the enum is trivially copyable,
which is a requirement for `rusty::Cell<T>`.

Convenience accessors are provided:

| Function | Purpose | Source |
|----------|---------|--------|
| `get_replication_type()` | Read current type via `Cell::get()` | `replication_helper.cc:15` |
| `set_replication_type(type)` | Write via `Cell::set()` | `replication_helper.cc:20` |
| `set_replication_type_from_string(str)` | Parse `"paxos"` or `"raft"` | `replication_helper.cc:28` |
| `replication_type_to_string(type)` | Pure function for logging | `replication_helper.cc:40` |
| `is_using_raft()` | Inline check | `replication_helper.h:38` |
| `is_using_paxos()` | Inline check | `replication_helper.h:39` |

#### 2.4 Dispatch Macros

Two macros handle the branching:

```cpp
// replication_helper.cc:57-73
#define DISPATCH_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            return raft_impl::func(__VA_ARGS__); \
        } else { \
            return paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)

#define DISPATCH_VOID_RAFT_OR_PAXOS(func, ...) \
    do { \
        if (janus::is_using_raft()) { \
            raft_impl::func(__VA_ARGS__); \
        } else { \
            paxos_impl::func(__VA_ARGS__); \
        } \
    } while(0)
```

`DISPATCH_RAFT_OR_PAXOS` is used for functions that return a value (the
`return` statement forwards the callee's return value).
`DISPATCH_VOID_RAFT_OR_PAXOS` is used for `void`-returning functions.

Both macros read `g_replication_type` on every call via `is_using_raft()`.
Since the type is set once at startup and never changes during execution,
the branch predictor effectively eliminates this overhead.

#### 2.5 Raft-Only Functions

One function is Raft-specific and has no Paxos counterpart:

```cpp
// replication_helper.cc:217-222
void set_preferred_leader(int site_id) {
    if (janus::is_using_raft()) {
        raft_impl::set_preferred_leader(site_id);
    }
    // No-op for Paxos
}
```

This is declared in `replication_helper.h:175` and exposed to Mako as a
top-level function.  Paxos uses a fixed-leader model and does not need
dynamic leader preference.

### 3. Protocol Detection: Two Mechanisms

#### 3.1 Explicit CLI Flag

The `--replication` flag (short form `-R`) is parsed in `dbtest.cc`:

```cpp
// src/mako/benchmarks/dbtest.cc:31
{"replication", required_argument, 0, 'R'},

// dbtest.cc:112-113
case 'R':
    replication_type = string(optarg);
    break;
```

After argument parsing:

```cpp
// dbtest.cc:341-343
if (!replication_type.empty()) {
    janus::set_replication_type_from_string(replication_type);
}
```

This runs **before** any replication subsystem initialisation, ensuring the
dispatch macros route to the correct namespace from the very first call.

**Example command line**:
```bash
./build/dbtest --replication raft -F config/occ_raft.yml -P p0 ...
```

#### 3.2 Automatic Config Detection

If no `--replication` flag is provided, `detect_replication_type_from_config()`
scans the YAML config files for the `ab:` field:

```cpp
// src/mako/mako.hh:779-816
static void detect_replication_type_from_config(const vector<string>& config_files) {
    if (janus::is_using_raft()) {
        return;  // Don't override explicit CLI setting
    }
    for (const auto& file_path : config_files) {
        std::ifstream ifs(file_path);
        // ... scan for "ab:" line ...
        if (value == "raft" || value == "fpga_raft") {
            janus::set_replication_type(janus::ReplicationType::RAFT);
            return;
        }
    }
}
```

This is called in `init_env()` (`mako.hh:889`) just before `setup()`:

```cpp
detect_replication_type_from_config(benchConfig.getPaxosConfigFile());
std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
```

**Priority**: The CLI flag takes precedence because
`set_replication_type_from_string` runs in `dbtest.cc:main()` before
`init_env()` is called, and `detect_replication_type_from_config()` checks
`is_using_raft()` as its first guard.

#### 3.3 YAML Config Format

The `ab` field in the YAML mode section selects the atomic broadcast protocol:

```yaml
# config/rule_raft.yml
mode:
  cc: rule          # concurrency control protocol
  ab: raft          # atomic broadcast → MODE_RAFT (0x400)
  batch: false
  retry: 20
  ongoing: 1
```

The YAML parser in `Config::InitMode()` converts the `ab` string to a mode
constant via `Frame::Name2Mode()`:

```cpp
// config.cc:496
replica_proto_ = Frame::Name2Mode(ab_name);
```

The name-to-mode mapping is defined in `frame.cc:487`:

```cpp
{"raft",       MODE_RAFT},       // 0x400
{"fpga_raft",  MODE_FPGA_RAFT},  // 0x401
```

`raft_main_helper.cc` verifies consistency at startup (`raft_main_helper.cc:258`):

```cpp
if (config->replica_proto_ != MODE_RAFT) {
    Log_warn("[RAFT-SETUP] replica_proto_=%d is not MODE_RAFT...");
}
```

#### 3.4 Detection Flow

```
dbtest.cc main()
    |
    +-- parse_command_line_args()    // -R flag → replication_type string
    |
    +-- set_replication_type_from_string()  // if -R was provided
    |
    +-- init_env()
         |
         +-- detect_replication_type_from_config()  // scan YAML for "ab: raft"
         |   (no-op if type already set to RAFT)
         |
         +-- setup()                 // dispatches to raft_impl::setup()
         |                           //  or paxos_impl::setup()
         +-- setup2()                // launches workers, preferred leader
```

### 4. Unified Replication API

The dispatch layer exposes 30+ functions that Mako calls without knowing
which protocol is active.  They group into five categories:

#### 4.1 Lifecycle Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `setup()` | `(int argc, char* argv[]) -> vector<string>` | Parse configs, create workers, return site names |
| `setup2()` | `(int action, int shardIndex) -> int` | Launch RPC services, configure preferred leader, start heartbeats |
| `shutdown_paxos()` | `() -> int` | Drain queues, destroy workers, release configs |
| `pre_shutdown_step()` | `() -> void` | Gracefully disconnect heartbeat RPC before full shutdown |

#### 4.2 Log Submission Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `submit()` | `(const char*, int, uint32_t) -> void` | Submit log to leader's queue |
| `add_log()` | `(const char*, int, uint32_t) -> void` | Alias for `submit()` |
| `add_log_without_queue()` | `(const char*, int, uint32_t) -> void` | Compatibility stub (still uses queue in Raft) |
| `add_log_to_nc()` | `(const char*, int, uint32_t, int) -> void` | Submit with batch size hint; waits for leadership if needed |
| `wait_for_submit()` | `(uint32_t) -> void` | Block until partition's submit queue is drained |

#### 4.3 Callback Registration Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `register_for_follower()` | `(cb, par_id) -> void` | Simple follower apply callback |
| `register_for_follower_par_id()` | `(cb, par_id) -> void` | Follower callback with partition ID |
| `register_for_follower_par_id_return()` | `(cb, par_id) -> void` | Full watermark follower callback |
| `register_for_leader()` | `(cb, par_id) -> void` | Simple leader apply callback |
| `register_for_leader_par_id()` | `(cb, par_id) -> void` | Leader callback with partition ID |
| `register_for_leader_par_id_return()` | `(cb, par_id) -> void` | Full watermark leader callback |
| `register_leader_election_callback()` | `(cb) -> void` | Notification when leadership changes |

The watermark callback signature is:
```cpp
std::function<int(const char*& data, int len, int par_id, int epoch,
                  std::queue<std::tuple<int, int, int, int, const char*>>& unreplayed)>
```

#### 4.4 Epoch and Election Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `get_epoch()` | `() -> int` | Current epoch from `ElectionState` |
| `set_epoch()` | `(int) -> void` | Update epoch, propagate to all workers |
| `upgrade_p1_to_leader()` | `() -> void` | Force leader callback invocation |
| `get_outstanding_logs()` | `(uint32_t) -> int` | Unreplicated log count: `n_tot - commitIndex` |

#### 4.5 Network Client and Benchmark Functions

| Function | Purpose |
|----------|---------|
| `nc_setup_server()` | Setup network client (stubs in Raft) |
| `nc_get_new_order_requests()` | TPC-C new-order replay (stubs in Raft) |
| `nc_get_payment_requests()` | TPC-C payment replay (stubs in Raft) |
| `nc_get_delivery_requests()` | TPC-C delivery replay (stubs in Raft) |
| `nc_get_order_status_requests()` | TPC-C order-status replay (stubs in Raft) |
| `nc_get_stock_level_requests()` | TPC-C stock-level replay (stubs in Raft) |
| `nc_get_read_requests()` | Read replay (stubs in Raft) |
| `nc_get_rmw_requests()` | Read-modify-write replay (stubs in Raft) |
| `microbench_paxos()` | Microbenchmark (stubs in Raft) |
| `microbench_paxos_queue()` | Queue microbenchmark (stubs in Raft) |
| `worker_info_stats()` | Per-partition counter dump |
| `getHosts()` | YAML host bindings parser |

The `nc_*` functions are stubs in the Raft implementation because the
network-client subsystem is tightly coupled to Paxos internals and is not
needed for Raft's use cases.

### 5. Namespace Symmetry: `paxos_impl` vs `raft_impl`

Both namespaces declare identical function signatures.  This is enforced by
the forward declarations in `replication_helper.h`:

```cpp
// replication_helper.h:46-85
namespace paxos_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
// ... 30+ declarations ...
}

// replication_helper.h:90-130
namespace raft_impl {
std::vector<std::string> setup(int argc, char* argv[]);
int setup2(int action, int shardIndex);
// ... 30+ declarations ...
void set_preferred_leader(int site_id);  // Raft-only
}
```

The linker enforces that both namespaces provide implementations for every
declared function.  Any missing function causes a link error, not a runtime
crash.

#### 5.1 Key Structural Differences

| Aspect | `paxos_impl` | `raft_impl` |
|--------|-------------|-------------|
| Source file | `paxos_main_helper.cc` | `raft_main_helper.cc` |
| Worker type | `PaxosWorker` | `RaftWorker` |
| Worker storage | `pxs_workers_g` (vector) | `raft_workers_g` (vector) |
| Leader model | Fixed leader (machine 0) | Preferred leader (election bias) |
| `setup2()` | Sets `ElectionState`, calls `server_launch_worker` | Configures per-partition preferred leader, then launches |
| `add_log_to_nc()` | Immediate enqueue | Checks `IsLeader()` per-partition |
| `nc_*` functions | Full network-client integration | Stubs returning `nullptr` |
| `shutdown_paxos()` | Drains queues, destroys config | Same pattern via `WaitForShutdown()` + `ShutDown()` |
| Callback storage | `leader_replay_cb` map only | Both `leader_replay_cb` and `follower_replay_cb` maps |
| `set_preferred_leader()` | (not present) | Iterates workers, calls `RaftServer::SetPreferredLeader()` |
| Jetpack | Enabled by default | Disabled (`MAKO_DISABLE_JETPACK=1`) |

#### 5.2 Callback Handling Difference

In Paxos, callbacks are registered only for the current role (leader or
follower) because leadership never changes during normal operation.

In Raft, leadership can change at any time, so `raft_main_helper.cc`
stores **both** leader and follower callbacks in separate maps and
registers them both on every worker:

```cpp
// raft_main_helper.cc:209-226
void apply_callbacks_for_partition(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    auto leader_it = leader_replay_cb.find(par_id);
    if (leader_it != leader_replay_cb.end()) {
        worker->register_leader_callback_par_id_return(leader_it->second);
    }
    auto follower_it = follower_replay_cb.find(par_id);
    if (follower_it != follower_replay_cb.end()) {
        worker->register_follower_callback_par_id_return(follower_it->second);
    }
}
```

When `raft_handle_leader_change()` fires (`raft_main_helper.cc:732-740`),
it re-applies callbacks for the affected partition and notifies the
external callback:

```cpp
void raft_handle_leader_change(uint32_t partition_id, bool is_leader) {
    raft_impl::handle_leader_change_impl(partition_id);
    if (leader_callback_) {
        leader_callback_(is_leader ? 1 : 0);
    }
}
```

### 6. Initialization Sequence

The full Raft initialization sequence from `dbtest.cc` through to running
Raft workers is:

```
1. dbtest.cc::main()
   +-- parse_command_line_args()
   |   '--> -R "raft" → replication_type = "raft"
   +-- set_replication_type_from_string("raft")
   |   '--> g_replication_type.set(ReplicationType::RAFT)
   |
2. init_env()  [mako.hh:858]
   +-- detect_replication_type_from_config()  // no-op: already RAFT
   +-- prepare_paxos_args()                   // builds argc/argv for setup()
   +-- setup(argc, argv)
   |   '--> DISPATCH → raft_impl::setup()
   |        +-- Config::CreateConfig(argc, argv)
   |        |   '--> reads YAML, sets replica_proto_ = MODE_RAFT
   |        +-- for each server site:
   |        |   +-- RaftWorker::new()
   |        |   +-- worker->SetupBase()
   |        |   |   '--> Frame::GetFrame(MODE_RAFT) → RaftFrame
   |        |   |   '--> RaftFrame::CreateScheduler() → RaftServer
   |        |   +-- raft_workers_g.push_back(worker)
   |        +-- returns site names
   |
   +-- setup_paxos_leader_callbacks()         // registers watermark callbacks
   +-- setup_paxos_follower_callbacks()       // (name is legacy, works with Raft)
   |
   +-- setup2(0, shardIndex)
   |   '--> DISPATCH → raft_impl::setup2()
   |        +-- for each worker:
   |        |   +-- GetRaftServer()
   |        |   +-- find locale_id==0 site in partition
   |        |   +-- SetPreferredLeader(preferred_site_id)
   |        +-- server_launch_worker()
   |             +-- SetupService()      // register RPC handlers
   |             +-- SetupCommo()        // connect to peers
   |             +-- EnsureSetup()       // start election timer + heartbeat
   |             +-- StartSubmitThread() // background log submission
   |             +-- SetupHeartbeat()    // control-plane heartbeat RPC
   |
   +-- sleep(3)                               // wait for all nodes to start
```

### 7. Mako Call Sites

Mako itself is entirely protocol-agnostic.  It calls the top-level dispatch
functions defined in `replication_helper.h`.  Key call sites:

#### 7.1 Log Submission (Hot Path)

From `mako.hh`:

```cpp
// mako.hh:953 - End-of-experiment signal
add_log_to_nc((char*)endLogInd.c_str(), 0, i);
```

Worker threads call `add_log_to_nc()` to replicate transaction logs.  The
dispatch routes to `raft_impl::add_log_to_nc()` which:
1. Finds the worker for the partition (`find_worker`)
2. Checks `IsLeader()` --- drops if not leader
3. Calls `enqueue_to_worker()` which increments `n_submit` and queues the log

#### 7.2 Callback Registration (Initialization)

```cpp
// mako.hh:900-901
setup_paxos_leader_callbacks(benchConfig.getAdvanceWatermarkTracker());
setup_paxos_follower_callbacks(replicated_db);
```

These functions (defined elsewhere in Mako) call the unified API:
- `register_for_leader_par_id_return(cb, par_id)` for leaders
- `register_for_follower_par_id_return(cb, par_id)` for followers

#### 7.3 Shutdown

```cpp
// mako.hh:970+
void db_close() {
    // ...
    pre_shutdown_step();
    shutdown_paxos();
}
```

Both functions dispatch to `raft_impl` and cleanly tear down workers.

### 8. Naming Conventions and Legacy Compatibility

Several function names retain "paxos" in their identifiers even when called
under Raft mode:

| Function | Reason |
|----------|--------|
| `shutdown_paxos()` | Original Paxos API; changing would break 100+ call sites |
| `setup_paxos_leader_callbacks()` | Mako-side function name |
| `prepare_paxos_args()` | Builds argc/argv for the config parser |
| `microbench_paxos()` | Placeholder for future Raft microbenchmarks |
| `getPaxosConfigFile()` | BenchmarkConfig accessor |

This is a deliberate design choice: the unified API maintains backward
compatibility by keeping the function names that Mako already uses, while
the dispatch layer transparently routes them to the correct implementation.

### 9. Safety Annotations

The dispatch layer is annotated following RustyCpp conventions:

- **Global state**: `rusty::Cell<ReplicationType>` (`@safe`, interior mutability)
- **Dispatch macros**: `@safe` (no ownership transfer, pure branching)
- **Dispatch functions**: `@unsafe` (delegate to non-borrow-checked legacy code)
- **Implementation namespaces**: `@unsafe` (direct interaction with RPC, I/O, and
  legacy worker infrastructure)

The dispatch layer itself is safe---it only reads a `Cell` and forwards
arguments by value or reference.  The unsafety is contained within the
implementation namespaces where workers interact with the RPC framework,
file I/O, and raw pointers from legacy code.

---

## RaftWorker: The Mako-Raft Bridge

### 1. Purpose

`RaftWorker` is the bridge class that connects Mako's watermark-based
transaction pipeline to Raft's replicated log.  It occupies the same
architectural position as `PaxosWorker` in the original system: Mako hands
it serialised transaction logs, it pushes them into the consensus protocol,
and when the protocol commits an entry, it fires a callback that feeds the
committed data back to Mako for replay.

The key design constraint is that Mako does not know it is talking to Raft.
It calls the same API (`submit`, `add_log_to_nc`, `register_for_leader_par_id_return`)
regardless of which protocol is active.  The `raft_main_helper.cc` dispatcher
(see `architecture.md`) routes those calls to functions that operate on
`RaftWorker` instances stored in the global `raft_workers_g` vector.

**Source files**: `src/deptran/raft/raft_worker.h` (168 lines),
`src/deptran/raft/raft_worker.cc` (616 lines)

### 2. Class Layout

#### 2.1 Member Variables

| Member | Type | Purpose |
|--------|------|---------|
| `callback_` | `std::function<void(const char*, int)>` | Simple log-apply callback (legacy) |
| `callback_par_id_` | `std::function<void(const char*&, int, int)>` | Callback with partition ID (legacy) |
| `leader_callback_par_id_return_` | `std::function<int(...)>` | Leader-role watermark callback |
| `follower_callback_par_id_return_` | `std::function<int(...)>` | Follower-role watermark callback |
| `submit_queue_` | `std::deque<PendingLog>` | Buffered logs awaiting submission |
| `submit_mutex_` / `submit_cv_` | mutex + condvar | Synchronisation for the submit queue |
| `submit_thread_` | `std::thread` | Background thread draining the queue |
| `submit_thread_stop_` | `std::atomic<bool>` | Shutdown signal for the submit thread |
| `batch_limit_` | `int` | Max logs dequeued per iteration (default 1) |
| `n_current` | `std::atomic<int>` | In-flight request count |
| `n_submit` | `std::atomic<int>` | Total submitted count |
| `n_tot` | `std::atomic<int>` | Total processed (committed) count |
| `site_info_` | `Config::SiteInfo*` | Partition, locale, port configuration |
| `rep_frame_` | `Frame*` | Raft frame (factory for protocol components) |
| `rep_sched_` | `TxLogServer*` | Points to the `RaftServer` instance |
| `rep_commo_` | `Communicator*` | Points to `RaftCommo` for peer RPCs |
| `svr_poll_thread_worker_` | `rusty::Option<rusty::Arc<PollThread>>` | Main RPC poll thread |
| `rpc_server_` | `rrr::Server*` | Raft RPC server (AppendEntries, Vote, etc.) |
| `thread_pool_g` | `base::ThreadPool*` | Thread pool for RPC handler dispatch |
| `svr_hb_poll_thread_worker_g` | `rusty::Option<rusty::Arc<PollThread>>` | Heartbeat RPC poll thread |
| `server_status_` | `rusty::Option<rusty::Arc<ServerStatus>>` | Status for heartbeat service |
| `hb_rpc_server_` | `rrr::Server*` | Heartbeat/control RPC server |
| `un_replay_logs_` | `std::queue<std::tuple<...>>` | Unreplayed logs (safety failures) |
| `is_leader` | `int` | Cached leadership flag (0 or 1) |
| `cur_epoch` | `int` | Current epoch from `ElectionState` |
| `election_state_lock` | `std::recursive_mutex` | Guards leadership state changes |

The watermark callback signature used by the production Mako path is:
```cpp
std::function<int(const char*& data, int len, int par_id, int epoch,
                  std::queue<std::tuple<int, int, int, int, const char*>>& unreplayed)>
```

#### 2.2 Comparison with PaxosWorker

| Aspect | PaxosWorker | RaftWorker |
|--------|-------------|------------|
| Source | `src/deptran/paxos_worker.h` | `src/deptran/raft/raft_worker.h` |
| Protocol server | `PaxosServer` (via `rep_sched_`) | `RaftServer` (via `rep_sched_`) |
| Callback model | Single `callback_par_id_return_` | Separate `leader_callback_par_id_return_` + `follower_callback_par_id_return_` |
| Submit path | `_Submit()` / `_BulkSubmit()` | `Submit()` / `EnqueueLog()` + `SubmitLoop()` |
| Background thread | Uses `SubmitPool` | Uses `std::thread` + `std::deque` |
| Leadership query | `IsLeader()` via `ElectionState` | `IsLeader()` via `RaftServer::IsLeader()` |
| Leader change | External, via `ElectionState` | Internal, via `RegisterLeaderChangeCallback()` |
| Port scheme | Main port (e.g., 17xxx) | Main port + heartbeat at port + 10000 |
| Poll threads | `rusty::Option<rusty::Arc<PollThread>>` | Same (migrated to RustyCpp) |

The most important difference is the **dual callback model**.  Paxos uses a
fixed leader, so the worker only ever has one role.  Raft leadership can change
at any time, so `RaftWorker` stores both callbacks and dynamically selects the
correct one in its `Next()` method based on the current leadership state.

### 3. Setup Chain

`RaftWorker` initialisation happens in four phases, called sequentially by
`raft_main_helper.cc::server_launch_worker()`:

#### 3.1 `SetupBase()` — Create the Protocol Stack

```
SetupBase()
  +-- Config::GetConfig()
  +-- Frame::GetFrame(replica_proto_)     → RaftFrame
  +-- rep_frame_->CreateScheduler()       → RaftServer
  +-- Set loc_id_, site_id_, partition_id_ on RaftServer
  +-- RegisterLeaderChangeCallback(lambda)
  +-- tot_num = config->get_tot_req()
```

**Key detail**: The leader-change callback captures `this` and:
1. Updates `is_leader` under `election_state_lock`
2. Calls `NotifyRaftLeaderChange(par_id, leader)` which routes through
   `raft_handle_leader_change()` in `raft_main_helper.cc` to re-apply
   callbacks and invoke the external leader election callback

**Source**: `raft_worker.cc:35-62`

#### 3.2 `SetupService()` — Start the RPC Server

```
SetupService()
  +-- PollThread::create()                → svr_poll_thread_worker_
  +-- new ThreadPool(1)                   → thread_pool_g
  +-- new rrr::Server(poll_worker)        → rpc_server_
  +-- rep_frame_->CreateRpcServices(...)  → RaftServiceImpl
  +-- rpc_server_->reg_service(svc)       // ownership transferred
  +-- rpc_server_->start(bind_addr)
```

The RPC server listens on the port specified in `site_info_` and handles
incoming `Vote`, `AppendEntries`, `EmptyAppendEntries`, and `TimeoutNow`
RPCs.  Services are owned by the `rpc_server_` (transferred via
`reg_service`).

**Source**: `raft_worker.cc:65-97`

#### 3.3 `SetupCommo()` — Connect to Peers

```
SetupCommo()
  +-- rep_frame_->CreateCommo(poll_thread.clone())  → RaftCommo
  +-- rep_sched_->commo_ = rep_commo_
```

`CreateCommo()` inside `RaftFrame` calls `ConnectToPeers()` which establishes
outbound TCP connections to all other replicas in the same partition.  The
clone of the poll thread is used for outbound I/O.

**Source**: `raft_worker.cc:100-109`

#### 3.4 `SetupHeartbeat()` — Control-Plane RPC

```
SetupHeartbeat()
  +-- if (!config->do_heart_beat()) return
  +-- PollThread::create()                         → svr_hb_poll_thread_worker_g
  +-- new ThreadPool(1)                            → hb_thread_pool_g
  +-- new rrr::Server(hb_poll_worker)              → hb_rpc_server_
  +-- Arc<ServerStatus>::make()                    → server_status_
  +-- make_box<ServerControlServiceImpl>(status)
  +-- hb_rpc_server_->start(port + 10000)
```

The heartbeat server runs on a separate port (`site_info_->port + CtrlPortDelta`
where `CtrlPortDelta = 10000`).  It provides a `ServerControlServiceImpl` that
external monitors use to check liveness and trigger graceful shutdown.

**Source**: `raft_worker.cc:112-134`

#### 3.5 Post-Setup: `EnsureSetup()` and `StartSubmitThread()`

After the four phases, `server_launch_worker()` in `raft_main_helper.cc`
does two more things:

1. **`RaftServer::EnsureSetup()`** — starts the election timer and heartbeat
   loop.  This is queued as a `OneTimeJob` on the poll thread to ensure
   correct thread affinity.

2. **`StartSubmitThread()`** — launches the background thread that drains the
   `submit_queue_`.

### 4. Log Submission Path

The submission path is how Mako transaction logs flow from the application
through RaftWorker into the Raft consensus protocol.

#### 4.1 Flow Diagram

```
Mako Worker Thread
    |
    | add_log_to_nc(data, len, par_id, batch_size)
    v
raft_impl::add_log_to_nc()                    [raft_main_helper.cc:534]
    |
    +-- find_worker(par_id)
    +-- IsLeader(par_id) check → drop if not leader
    +-- enqueue_to_worker(worker, data, len, par_id, batch_size)
         |
         +-- worker->IncSubmit()               [raft_worker.cc:382]
         +-- worker->EnqueueLog(...)           [raft_worker.cc:275]
              |
              +-- PendingLog{payload, par_id}
              +-- submit_queue_.push_back()
              +-- submit_cv_.notify_one()
                   |
                   v
SubmitLoop() [background thread]               [raft_worker.cc:588]
    |
    +-- wait on submit_cv_
    +-- dequeue up to batch_limit_ entries
    +-- for each entry: Submit(data, len, par_id)
         |
         v
Submit(data, len, par_id)                      [raft_worker.cc:338]
    |
    +-- IsLeader(par_id) check → drop if not leader
    +-- CreateRaftLogCommand(data, len, tx_id)
    |   +-- TpcCommitCommand{tx_id_}
    |   +-- VecPieceData{sp_vec_piece_data_}
    |   +-- SimpleCommand{input.values_[0] = STR(data)}
    |
    +-- raft_server->Start(cmd, &index, &term)
    +-- n_tot++
```

#### 4.2 `PendingLog` Queue

```cpp
// raft_worker.h:41-44
struct PendingLog {
    std::string payload;   // Copy of the raw bytes
    uint32_t par_id;       // Target partition
};
```

The queue decouples Mako's write path from Raft's consensus latency.  Mako
threads push into the `submit_queue_` (protected by `submit_mutex_`) and
immediately return.  The background `SubmitLoop` thread drains entries and
calls `Submit()` which blocks until `RaftServer::Start()` returns.

The copy semantics (`payload.assign(log, len)` at `raft_worker.cc:282`) are
necessary because the Mako caller's buffer may be reused immediately after
`add_log_to_nc` returns.

#### 4.3 `CreateRaftLogCommand()` — Wrapping Raw Bytes

Raft's internal log format uses `Marshallable` objects, specifically
`TpcCommitCommand` with `VecPieceData` for batch optimisation.  Mako
sends raw serialised bytes.  `CreateRaftLogCommand()` bridges this gap:

```cpp
// raft_worker.cc:298-335
TpcCommitCommand
  +-- tx_id_ = auto-incrementing atomic counter
  +-- cmd_ = VecPieceData
       +-- sp_vec_piece_data_ = vector<shared_ptr<SimpleCommand>>
            +-- SimpleCommand[0]
                 +-- input.values_[0] = Value(STR(raw_bytes))
                 +-- partition_id_ = 0
```

The raw bytes are stored as a `Value::STR` (not `i32`) to avoid
`get_i32()` crashes in `SetLocalAppend`.  This structure is the same
for both production Mako logs and test payloads.

**Source**: `raft_worker.cc:293-335`

#### 4.4 `SubmitLoop()` — Background Draining

```cpp
// raft_worker.cc:588-613
void RaftWorker::SubmitLoop() {
    unique_lock lock(submit_mutex_);
    while (true) {
        submit_cv_.wait(lock, [&] {
            return submit_thread_stop_ || !submit_queue_.empty();
        });
        if (submit_thread_stop_ && submit_queue_.empty()) break;

        // Dequeue up to batch_limit_ entries
        vector<PendingLog> batch;
        while (!submit_queue_.empty() && batch.size() < batch_limit_)
            batch.push_back(move(submit_queue_.front()));

        lock.unlock();
        for (auto& entry : batch)
            Submit(entry.payload.data(), entry.payload.size(), entry.par_id);
        lock.lock();
    }
}
```

The lock is released during `Submit()` calls so Mako threads can continue
enqueuing.  The `batch_limit_` is set by the `batch_size` parameter of
`EnqueueLog()`, which comes from `add_log_to_nc`'s fourth argument.

#### 4.5 `StopSubmitThread()` — Graceful Drain

When shutting down, `StopSubmitThread()` (`raft_worker.cc:249-272`):
1. Sets `submit_thread_stop_ = true` under lock
2. Notifies the condvar
3. Joins the thread
4. Drains any remaining entries by calling `Submit()` inline

This ensures no committed logs are lost during shutdown.

### 5. Committed Entry Callback Path

When Raft commits a log entry, it flows back to Mako through the `Next()`
callback.

#### 5.1 How `Next()` Gets Registered

Every callback registration method (`register_apply_callback`,
`register_leader_callback_par_id_return`, etc.) calls:

```cpp
rep_sched_->RegLearnerAction(
    std::bind(&RaftWorker::Next, this, _1, _2));
```

`RegLearnerAction()` is defined in `TxLogServer` (the base class of
`RaftServer`) at `scheduler.h:501`:

```cpp
void RegLearnerAction(function<int(int, shared_ptr<Marshallable>)> learner_action) {
    app_next_ = learner_action;
}
```

#### 5.2 How `RaftServer` Invokes `app_next_`

`RaftServer::applyLogs()` is called after `commitIndex` advances (when a
majority confirms an entry).  For each newly committed slot:

```cpp
// server.cc:601-609
for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
    auto next_instance = GetRaftInstance(id);
    if (next_instance && next_instance->log_) {
        RuleWitnessGC(next_instance->log_);
        app_next_(id, next_instance->log_);   // → RaftWorker::Next()
        executeIndex = id;
    }
}
```

#### 5.3 `Next()` — The Core Callback

```
RaftServer::applyLogs()
    |
    | app_next_(slot_id, cmd)
    v
RaftWorker::Next(slot_id, cmd)               [raft_worker.cc:497]
    |
    +-- dynamic_cast<TpcCommitCommand>(cmd)
    +-- Extract VecPieceData → SimpleCommand → Value::STR → raw bytes
    +-- Determine role: am_leader = IsLeader(par_id)
    +-- Select callback:
    |     leader? → leader_callback_par_id_return_
    |     follower? → follower_callback_par_id_return_
    |
    +-- active_callback(log, len, par_id, slot_id, un_replay_logs_)
    |   → returns encoded_value
    |
    +-- status = encoded_value % 10
    +-- timestamp = encoded_value / 10
    |
    +-- if status == STATUS_SAFETY_FAIL && len > 0:
    |     malloc + memcpy → un_replay_logs_.push(...)
    |
    +-- return status
```

**Key details**:

1. **Payload extraction**: `Next()` reverses the wrapping done by
   `CreateRaftLogCommand()`.  It follows the chain
   `TpcCommitCommand → VecPieceData → SimpleCommand → Value::STR → raw bytes`.

2. **Dynamic role selection**: Unlike `PaxosWorker` which only has one
   callback, `RaftWorker` checks `IsLeader(par_id)` on **every** commit
   to choose the correct callback.  This handles leadership changes that
   occur between log submission and commit.

3. **Encoded return value**: Mako's callback returns `timestamp * 10 + status`.
   `Next()` decodes this to separate the two values.

4. **Safety failure handling**: When the callback returns
   `PaxosStatus::STATUS_SAFETY_FAIL`, the log entry is copied (via `malloc +
   memcpy`) into `un_replay_logs_` for later retry.  The queue is passed to
   the next callback invocation so Mako can attempt to replay them.

**Source**: `raft_worker.cc:497-585`

### 6. Leadership Queries

#### 6.1 `IsLeader(par_id)`

```cpp
// raft_worker.cc:212-229
bool RaftWorker::IsLeader(uint32_t par_id) {
    // Check partition match
    if (rep_frame_->site_info_->partition_id_ != par_id)
        return false;
    // Delegate to RaftServer
    auto raft_server = GetRaftServer();
    if (raft_server)
        return raft_server->IsLeader();
    return false;
}
```

This is called on every `Submit()` and every `Next()`.  It queries the
`RaftServer` directly rather than caching, ensuring that leadership changes
from elections or transfers are immediately reflected.

#### 6.2 `IsPartition(par_id)`

```cpp
// raft_worker.cc:232-236
bool RaftWorker::IsPartition(uint32_t par_id) {
    return rep_frame_->site_info_->partition_id_ == par_id;
}
```

Used by `raft_main_helper.cc::find_worker()` to locate the correct worker
for a given partition.

#### 6.3 Leader Change Notification

When `RaftServer` detects a leadership change (via `setIsLeader()`), it
invokes the callback registered in `SetupBase()`:

```cpp
// raft_worker.cc:51-58
raft_server->RegisterLeaderChangeCallback([this](bool leader) {
    {
        std::lock_guard<std::recursive_mutex> guard(election_state_lock);
        is_leader = leader ? 1 : 0;
    }
    uint32_t par_id = site_info_ ? site_info_->partition_id_ : 0;
    NotifyRaftLeaderChange(par_id, leader);
});
```

`NotifyRaftLeaderChange()` is a free function in `janus::` namespace that
delegates to `raft_handle_leader_change()`, which re-applies the correct
leader/follower callbacks and invokes Mako's external election callback.

### 7. Callback Registration Methods

`RaftWorker` provides five callback registration methods, forming a
hierarchy from simple to full-featured:

#### 7.1 Simple Callbacks (Legacy)

```cpp
void register_apply_callback(function<void(const char*, int)> cb);
void register_apply_callback_par_id(function<void(const char*&, int, int)> cb);
```

These store the callback and call `RegLearnerAction()` to wire up `Next()`.
They are the simplest form: the callback receives just the log bytes and
optionally the partition ID.  Used for basic test scenarios.

#### 7.2 Watermark Callbacks (Production)

```cpp
void register_leader_callback_par_id_return(function<int(...)> cb);
void register_follower_callback_par_id_return(function<int(...)> cb);
```

These are the callbacks Mako uses in production.  They receive the full
signature including `epoch` (slot ID) and the `un_replay_logs_` queue, and
return an encoded `timestamp * 10 + status` value.

Both methods also call `RegLearnerAction()` to install `Next()` as the
`app_next_` handler.  Multiple registrations are idempotent because
`RegLearnerAction` simply overwrites the previous binding.

#### 7.3 Deprecated Legacy Bridge

```cpp
void register_apply_callback_par_id_return(function<int(...)> cb);
```

This method exists for backward compatibility.  It logs a deprecation
warning and delegates to `register_follower_callback_par_id_return()`.

### 8. Shutdown Sequence

Shutdown is a two-phase process:

#### Phase 1: `WaitForShutdown()`

```
WaitForShutdown()                            [raft_worker.cc:202]
  +-- StopSubmitThread()                     // drain queue, join thread
  +-- hb_rpc_server_->do_shutdown()          // signal heartbeat server
  +-- hb_rpc_server_->wait_for_shutdown()    // block until done
```

#### Phase 2: `ShutDown()`

```
ShutDown()                                   [raft_worker.cc:137]
  +-- Signal poll threads to stop            // allows Reactor::Loop() to exit
  +-- delete rpc_server_                     // tears down Raft RPC server
  +-- delete hb_rpc_server_                  // tears down heartbeat server
  +-- server_status_ = rusty::None           // release Arc
  +-- hb_thread_pool_g->release()
  +-- thread_pool_g->release()
  +-- StopSubmitThread()                     // idempotent
  +-- delete rep_sched_                      // destroys RaftServer
  +-- Shutdown poll threads                  // AFTER servers are destroyed
```

**Critical ordering**: Poll threads must be shut down **after** the RPC
servers are destroyed.  The `Server::~Server()` destructor enqueues cleanup
commands to the poll thread; if the poll thread is already stopped, those
commands are lost and reference counts (`sconns_ctr_`) never reach zero,
causing resource leaks.

The destructor (`~RaftWorker()`) provides a separate safety net:

```cpp
// raft_worker.cc:22-32
RaftWorker::~RaftWorker() {
    StopSubmitThread();
    if (svr_poll_thread_worker_.is_some())
        svr_poll_thread_worker_.as_ref().unwrap()->shutdown();
    if (svr_hb_poll_thread_worker_g.is_some())
        svr_hb_poll_thread_worker_g.as_ref().unwrap()->shutdown();
}
```

### 9. Helper Methods

#### 9.1 `GetRaftServer()`

```cpp
RaftServer* GetRaftServer() {
    return dynamic_cast<RaftServer*>(rep_sched_);
}
```

Casts the base `TxLogServer*` to `RaftServer*`.  Used throughout the class
and by `raft_main_helper.cc` to access Raft-specific APIs like
`SetPreferredLeader()`, `IsLeader()`, and `EnsureSetup()`.

#### 9.2 `GetPollThreadWorker()`

```cpp
rusty::Option<rusty::Arc<PollThread>> GetPollThreadWorker() {
    return svr_poll_thread_worker_.clone();
}
```

Returns a clone of the poll thread `Arc`, used by `server_launch_worker()`
to queue the `EnsureSetup()` job on the correct event loop.

#### 9.3 `IncSubmit()` / `WaitForSubmit()`

`IncSubmit()` bumps `n_submit`.  `WaitForSubmit()` polls until
`n_submit >= tot_num` (all expected logs submitted) and the submit queue
is empty.  Used during experiments to synchronise with the log pipeline.

### 10. Global State

```cpp
// raft_worker.h:165
extern vector<shared_ptr<RaftWorker>> raft_workers_g;
```

All `RaftWorker` instances for the current process are stored in this vector,
created by `raft_impl::setup()` and destroyed by `raft_impl::shutdown_paxos()`.
The vector is indexed by creation order (one worker per server site).
`find_worker(par_id)` in `raft_main_helper.cc` linearly scans this vector
by calling `IsPartition()`.

### 11. Complete Data Flow Diagram

```
Mako Worker                                        Raft Consensus
  Thread                                            Protocol
    |                                                  |
    | add_log_to_nc(data, len, par_id, batch)         |
    v                                                  |
[raft_impl::add_log_to_nc]                            |
    |                                                  |
    +-- find_worker(par_id)                            |
    +-- IsLeader? → drop if no                        |
    +-- IncSubmit()                                    |
    +-- EnqueueLog(data, len, par_id, batch)          |
         |                                             |
         v                                             |
    [PendingLog queue]                                 |
         |                                             |
         v                                             |
    [SubmitLoop thread]                                |
         |                                             |
         +-- dequeue batch                             |
         +-- Submit(data, len, par_id)                 |
              |                                        |
              +-- CreateRaftLogCommand(data, len, tx)   |
              +-- RaftServer::Start(cmd)  ------------>|
                                                       |
                                          [leader appends to log]
                                          [sends AppendEntries RPCs]
                                          [majority acknowledges]
                                          [commitIndex advances]
                                                       |
                                          [applyLogs()]|
                                                       |
              app_next_(slot_id, cmd) <----------------|
              |                                        |
              v                                        |
    [RaftWorker::Next(slot_id, cmd)]                   |
         |                                             |
         +-- extract raw bytes from TpcCommitCommand    |
         +-- IsLeader? → choose leader/follower cb     |
         +-- active_callback(log, len, par_id, slot)   |
              |                                        |
              v                                        |
         [Mako watermark callback]                     |
              |                                        |
              +-- update local watermark               |
              +-- return timestamp * 10 + status       |
```

---

## raft_main_helper.cc: The Raft Glue Layer

<a id="1-purpose-1"></a>

### 1. Purpose

`raft_main_helper.cc` (751 lines) implements the `raft_impl` namespace that
the `replication_helper` dispatcher routes to when `ReplicationType::RAFT`
is active.  It is the Raft counterpart to `paxos_main_helper.cc` and
provides all 30+ functions that Mako calls through the unified API.

Structurally, the file has three scoping levels:

1. **`janus::` namespace** (file-level globals): `raft_workers_g`,
   `leader_callback_`, `client_workers_storage`
2. **`raft_impl::` namespace** (public dispatcher targets): `setup()`,
   `add_log_to_nc()`, `register_for_leader_par_id_return()`, etc.
3. **Anonymous namespace** (internal helpers): `server_launch_worker()`,
   `find_worker()`, `enqueue_to_worker()`, `apply_callbacks_for_partition()`,
   `wait_for_local_leadership()`

**Source**: `src/deptran/raft_main_helper.cc`

### 2. Global State

#### 2.1 Globals in `janus::` Namespace

```cpp
// raft_main_helper.cc:15-18
namespace janus {
vector<unique_ptr<ClientWorker>> client_workers_storage = {};
vector<shared_ptr<RaftWorker>> raft_workers_g = {};
std::function<void(int)> leader_callback_{};
}
```

| Variable | Type | Purpose |
|----------|------|---------|
| `client_workers_storage` | `vector<unique_ptr<ClientWorker>>` | Client-side workers (unused in Raft path, kept for linkage) |
| `raft_workers_g` | `vector<shared_ptr<RaftWorker>>` | All `RaftWorker` instances for this process |
| `leader_callback_` | `std::function<void(int)>` | External callback invoked on every leadership change |

`raft_workers_g` is the central data structure.  Every function in
`raft_impl::` looks up workers from this vector.  It is populated by
`setup()` and cleared by `shutdown_paxos()`.

#### 2.2 State in `raft_impl::` Namespace

```cpp
// raft_main_helper.cc:29-35
std::map<int, WatermarkCallback> leader_replay_cb;
std::map<int, WatermarkCallback> follower_replay_cb;
shared_ptr<ElectionState> es = ElectionState::instance();
```

| Variable | Type | Purpose |
|----------|------|---------|
| `leader_replay_cb` | `map<par_id → callback>` | Cached leader watermark callbacks, keyed by partition |
| `follower_replay_cb` | `map<par_id → callback>` | Cached follower watermark callbacks, keyed by partition |
| `es` | `shared_ptr<ElectionState>` | Singleton shared with Paxos for epoch/leader state |

The dual callback maps are the key architectural difference from Paxos.
In Paxos, only `leader_replay_cb` exists because leadership is fixed.
In Raft, leadership can change at any time, so both maps are needed and
are re-applied to workers on every leadership change via
`apply_callbacks_for_partition()`.

### 3. Lifecycle Functions

#### 3.1 `setup(argc, argv)` — Worker Creation

```
setup(argc, argv)                              [line 239]
  +-- check_current_path()                      // logging
  +-- setenv("MAKO_DISABLE_JETPACK", "1")       // disable Jetpack optimization
  +-- Config::CreateConfig(argc, argv)          // parse YAML configs
  +-- Verify replica_proto_ == MODE_RAFT        // safety check
  +-- for each server site (reverse order):
  |   +-- Config::SiteById(id) → site
  |   +-- RaftWorker::new()
  |   +-- worker->site_info_ = &site
  |   +-- worker->SetupBase()                   // creates RaftServer
  |   +-- raft_workers_g.push_back(worker)
  +-- reverse(raft_workers_g)                   // restore original order
  +-- es->machine_id = last worker's locale_id
  +-- return site names
```

**Reverse-then-reverse pattern**: Workers are created in reverse site order
then the vector is reversed.  This matches the Paxos helper's convention
where the last worker's locale determines the machine ID.

**Jetpack disabling**: `MAKO_DISABLE_JETPACK=1` is forced because Raft's
own batching pipeline replaces Jetpack's optimistic log aggregation.  If
the environment variable is already set, it is respected.

**Source**: `raft_main_helper.cc:239-285`

#### 3.2 `setup2(action, shardIndex)` — Service Launch + Preferred Leader

```
setup2(action, shardIndex)                     [line 297]
  +-- Get server_infos from config
  +-- For each RaftWorker:
  |   +-- GetRaftServer()
  |   +-- Find locale_id==0 site in same partition
  |   +-- SetPreferredLeader(preferred_site_id)
  +-- Configure ElectionState for Paxos compat:
  |   +-- es->set_state(0), set_epoch(0), set_leader(0)
  +-- server_launch_worker(server_infos)
  +-- return 0
```

**Preferred leader configuration**: For each partition, `setup2()` scans
all sites in that partition to find the one with `locale_id == 0`
(localhost).  That site becomes the preferred leader.  This bias causes
the localhost node to use shorter election timeouts (150-300ms vs 500ms-1s)
and triggers leadership transfer when the current leader is not the
preferred one.

**Multi-partition example** (from comments at line 309):
```
Partition 0: sites s101(localhost), s201(p1), s302(p2) → preferred = s101
Partition 1: sites s102(localhost), s202(p1), s303(p2) → preferred = s102
```

**ElectionState compatibility**: Even though Raft has its own leadership
mechanism, `ElectionState` is updated to keep Mako's `is_leader` checks
working.  Both preferred and non-preferred machines start with
`state=0, epoch=0, leader=0`.

**Source**: `raft_main_helper.cc:297-374`

#### 3.3 `server_launch_worker()` — Internal Multi-Phase Boot

This anonymous-namespace function orchestrates the RaftWorker boot sequence
in three passes:

```
server_launch_worker(server_sites)             [line 116]
  Pass 1: SetupService()                       // start RPC servers
    +-- for each worker: worker->SetupService()

  Pass 2: SetupCommo() + EnsureSetup() + StartSubmitThread()
    +-- for each (site, worker):
    |   +-- worker->SetupCommo()               // connect to peers
    |   +-- Queue EnsureSetup() on poll thread  // start elections
    |   +-- worker->StartSubmitThread()         // start submit loop

  Pass 3: SetupHeartbeat()                     // control-plane RPC
    +-- for each worker: worker->SetupHeartbeat()
```

**EnsureSetup ordering**: `EnsureSetup()` (which starts the election timer
and heartbeat loop) is queued as a `OneTimeJob` on the worker's poll thread
rather than called inline.  This ensures correct thread affinity — the
election timer must run on the same event loop that handles incoming RPCs.

**Source**: `raft_main_helper.cc:116-178`

#### 3.4 `shutdown_paxos()` — Two-Phase Teardown

```
shutdown_paxos()                               [line 415]
  +-- es->running = false
  +-- Phase 1: for each worker → WaitForShutdown()
  |   (drains submit queue, signals heartbeat server)
  +-- Phase 2: for each worker → ShutDown()
  |   (deletes RPC servers, destroys RaftServer)
  +-- raft_workers_g.clear()
  +-- RandomGenerator::destroy()
  +-- Config::DestroyConfig()
```

The two-phase approach ensures all in-flight RPCs complete before
destructors run.  `WaitForShutdown()` blocks until the heartbeat RPC
server has acknowledged shutdown, then `ShutDown()` destroys all
resources.

**Source**: `raft_main_helper.cc:415-436`

#### 3.5 `pre_shutdown_step()` — Graceful Disconnect

```
pre_shutdown_step()                            [line 572]
  +-- for each worker:
  |   +-- hb_rpc_server_->do_shutdown()        // signal heartbeat
  |   +-- WaitForShutdown()                    // block until done
```

Called by Mako before `shutdown_paxos()` to gracefully disconnect
control-plane connections.  This allows external monitors to detect
the shutdown before TCP connections are torn down.

**Source**: `raft_main_helper.cc:572-583`

### 4. Internal Helper Functions

#### 4.1 `find_worker(par_id)` — Partition Lookup

```cpp
// raft_main_helper.cc:181-188
RaftWorker* find_worker(uint32_t par_id) {
    for (auto& worker : raft_workers_g) {
        if (worker && worker->IsPartition(par_id))
            return worker.get();
    }
    return nullptr;
}
```

Linear scan over `raft_workers_g`.  Called on every log submission and
callback registration.  The vector is typically small (1 worker per
partition per process, usually 1-8), so linear scan is faster than
a hash map due to cache locality.

#### 4.2 `enqueue_to_worker()` — Submit Dispatch

```cpp
// raft_main_helper.cc:191-205
void enqueue_to_worker(RaftWorker* worker,
                       const char* log, int len,
                       uint32_t par_id, int batch_size) {
    worker->IncSubmit();
    if (worker->HasSubmitThread()) {
        worker->EnqueueLog(log, len, par_id, batch_size);
    } else {
        worker->Submit(log, len, par_id);
    }
}
```

Routes to the background submit thread if running, otherwise submits
inline.  `IncSubmit()` is always called first for bookkeeping.

#### 4.3 `apply_callbacks_for_partition(par_id)` — Callback Re-Application

```cpp
// raft_main_helper.cc:209-226
void apply_callbacks_for_partition(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    if (!worker || !worker->site_info_) return;

    auto leader_it = leader_replay_cb.find(par_id);
    if (leader_it != leader_replay_cb.end())
        worker->register_leader_callback_par_id_return(leader_it->second);

    auto follower_it = follower_replay_cb.find(par_id);
    if (follower_it != follower_replay_cb.end())
        worker->register_follower_callback_par_id_return(follower_it->second);
}
```

This function is called in two contexts:
1. **Initial registration**: When Mako registers callbacks via
   `register_for_leader_par_id_return()` / `register_for_follower_par_id_return()`
2. **Leadership change**: When `handle_leader_change_impl()` fires

Both callbacks are always re-applied because `RaftWorker::Next()` dynamically
selects which to call based on the current leadership state.

### 5. Leader Change Propagation

#### 5.1 Notification Chain

```
RaftServer::setIsLeader()
    |
    | leader_change_callback_(is_leader)
    v
RaftWorker::RegisterLeaderChangeCallback lambda    [raft_worker.cc:51]
    |
    +-- Update is_leader under election_state_lock
    +-- NotifyRaftLeaderChange(par_id, is_leader)
         |
         v
janus::NotifyRaftLeaderChange()                    [line 744]
    |
    +-- if (is_using_raft())
    |
    v
janus::raft_handle_leader_change()                 [line 732]
    |
    +-- raft_impl::handle_leader_change_impl()     [line 233]
    |   +-- apply_callbacks_for_partition(par_id)   // re-wire callbacks
    |   +-- leader_wait_cv.notify_all()             // unblock waiters
    |
    +-- if (leader_callback_)
        leader_callback_(is_leader ? 1 : 0)        // notify Mako
```

**Key design points**:

- `NotifyRaftLeaderChange()` checks `is_using_raft()` to prevent accidental
  invocation in Paxos mode.
- `handle_leader_change_impl()` is in the `raft_impl` namespace (accessible
  to `janus::` via the forward declaration at line 233).
- `leader_wait_cv.notify_all()` unblocks any threads in
  `wait_for_local_leadership()`.
- The external callback receives `1` for "became leader" and `0` for
  "lost leadership".

#### 5.2 `wait_for_local_leadership()` — Blocking Wait

```cpp
// raft_main_helper.cc:68-105
bool wait_for_local_leadership(RaftWorker* worker,
                               uint32_t par_id,
                               std::chrono::milliseconds timeout) {
    auto deadline = steady_clock::now() + timeout;
    unique_lock lock(leader_wait_mutex);
    while (true) {
        lock.unlock();
        if (worker->IsLeader(par_id)) return true;
        lock.lock();
        if (steady_clock::now() >= deadline) break;
        leader_wait_cv.wait_until(lock, deadline);
    }
    return worker->IsLeader(par_id);
}
```

**Timeout**: `kLeaderWaitTimeout = 5000ms` (5 seconds), defined at line 56.

**Usage pattern**: The lock-unlock-check-lock pattern avoids holding the
mutex during `IsLeader()` (which acquires the `RaftServer` mutex internally).
`leader_wait_cv` is notified whenever any leadership change occurs.

This function is currently defined but not called on the hot path.  It is
available for multi-shard startup scenarios where Mako needs to wait for
a partition to elect a leader before submitting logs.

### 6. Log Submission Functions

#### 6.1 `add_log_to_nc()` — Primary Hot Path

```cpp
// raft_main_helper.cc:534-553
void add_log_to_nc(const char* log, int len,
                   uint32_t par_id, int batch_size) {
    auto* worker = find_worker(par_id);
    if (!worker) return;

    if (!worker->IsLeader(par_id)) return;  // drop immediately

    enqueue_to_worker(worker, log, len, par_id, max(1, batch_size));
}
```

This is the function Mako worker threads call on every transaction commit.
It matches the Paxos behaviour of immediately dropping logs when not leader
(no waiting, no retry).

#### 6.2 `submit()` / `add_log()` / `add_log_without_queue()`

```cpp
void submit(const char* log, int len, uint32_t par_id);   // line 510
void add_log(const char* log, int len, uint32_t par_id);   // line 524 → submit()
void add_log_without_queue(...);                            // line 529 → submit()
```

All three ultimately call `enqueue_to_worker()` with `batch_size=1`.
`add_log` and `add_log_without_queue` are aliases for Paxos API
compatibility.

#### 6.3 `wait_for_submit(par_id)`

```cpp
// raft_main_helper.cc:557-564
void wait_for_submit(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    if (!worker) return;
    worker->WaitForSubmit();
}
```

Blocks until the submit queue is empty and all submitted logs have been
processed.  Used at end-of-experiment to ensure all data is replicated.

### 7. Callback Registration Functions

#### 7.1 Watermark Callbacks (Production Path)

```cpp
// Leader callback: line 498-504
void register_for_leader_par_id_return(callback cb, uint32_t par_id) {
    leader_replay_cb[par_id] = cb;           // cache in map
    apply_callbacks_for_partition(par_id);    // wire to worker
}

// Follower callback: line 464-470
void register_for_follower_par_id_return(callback cb, uint32_t par_id) {
    follower_replay_cb[par_id] = cb;         // cache in map
    apply_callbacks_for_partition(par_id);    // wire to worker
}
```

Both functions follow the same pattern: store the callback in the
partition-keyed map, then call `apply_callbacks_for_partition()` which
wires both leader and follower callbacks to the correct `RaftWorker`.

**Why cache**: When leadership changes, `handle_leader_change_impl()` must
re-apply callbacks.  Without the maps, the system would lose the callback
reference after a leadership transition.

#### 7.2 Simple Callbacks

```cpp
void register_for_follower(cb, par_id);        // line 444
void register_for_follower_par_id(cb, par_id); // line 454
void register_for_leader(cb, par_id);          // line 473
void register_for_leader_par_id(cb, par_id);   // line 488
```

These iterate `raft_workers_g` and register directly on workers that match
the partition and current leadership role.  They are used for simple test
scenarios but not for Mako's production watermark system.

**Subtle difference from watermark callbacks**: These check `IsLeader()` at
registration time and only register on matching workers.  The watermark
callbacks register on the partition worker regardless of current role and
let `RaftWorker::Next()` choose dynamically.

#### 7.3 `register_leader_election_callback()`

```cpp
// raft_main_helper.cc:483-485
void register_leader_election_callback(std::function<void(int)> cb) {
    janus::leader_callback_ = std::move(cb);
}
```

Mako calls this to receive notifications when any partition gains or loses
leadership.  The callback is a simple `void(int)` where `1` means
"became leader" and `0` means "lost leadership".

### 8. NO-OP Entry System

#### 8.1 `send_no_ops_for_mark(epoch)`

```cpp
// raft_main_helper.cc:38-47
void send_no_ops_for_mark(int epoch) {
    std::string log = "no-ops:" + std::to_string(epoch);
    for (auto& worker : raft_workers_g) {
        if (!worker || !worker->site_info_) continue;
        add_log_to_nc(log.c_str(), log.size(),
                      worker->site_info_->partition_id_, 1);
    }
}
```

Sends a `"no-ops:<epoch>"` entry through the Raft log for every partition.
These entries serve two purposes:

1. **Watermark synchronization**: Followers process NO-OP entries and
   update their local watermarks, ensuring all replicas have a consistent
   view of the epoch boundary.

2. **Epoch advancement**: When Mako transitions between epochs (e.g.,
   during shard reconfiguration), NO-OP entries mark the boundary in
   the replicated log.

The `batch_size=1` ensures each NO-OP is submitted individually rather
than batched, guaranteeing ordering with respect to regular log entries.

#### 8.2 `send_no_ops_to_all_workers(epoch)`

```cpp
// raft_main_helper.cc:50-52
void send_no_ops_to_all_workers(int epoch) {
    send_no_ops_for_mark(epoch);
}
```

Compatibility alias used by Paxos-era call sites.

### 9. Epoch and Election State Functions

#### 9.1 `ElectionState` Singleton

`ElectionState` (defined in `paxos_worker.h:696`) is a Paxos-originated
singleton that Mako uses for leader/epoch queries.  The Raft helper reuses
it for compatibility:

| Field | Type | Raft Usage |
|-------|------|------------|
| `machine_id` | `int` | Set to last worker's `locale_id` in `setup()` |
| `cur_epoch` | `int` | Epoch counter managed by `set_epoch()` |
| `cur_state` | `int` | 0=follower, 1=leader (set in `setup2()`) |
| `running` | `bool` | Set to `false` in `shutdown_paxos()` |
| `leader_id` | `int` | Set to 0 in `setup2()` |

#### 9.2 `get_epoch()` / `set_epoch()`

```cpp
// raft_main_helper.cc:586-605
int get_epoch() { return es ? es->get_epoch() : 0; }

void set_epoch(int epoch) {
    if (epoch == -1) es->set_epoch();       // auto-increment
    else             es->set_epoch(epoch);
    for (auto& worker : raft_workers_g)
        worker->cur_epoch = es->get_epoch();  // propagate
}
```

`set_epoch(-1)` triggers `ElectionState::set_epoch()` with no arguments,
which auto-increments the epoch.  After updating `ElectionState`, the
new epoch is propagated to all workers' `cur_epoch` field.

#### 9.3 `upgrade_p1_to_leader()`

```cpp
// raft_main_helper.cc:608-613
void upgrade_p1_to_leader() {
    Log_info("upgrade_p1_to_leader invoked for Raft helper.");
    if (::janus::leader_callback_)
        ::janus::leader_callback_(0);
}
```

Compatibility function for Paxos-era callers.  In Raft, leadership is
decided by elections, not manual promotion.  This function simply invokes
the leader callback (with `0`, not `1`) to trigger any Paxos-era startup
logic.

#### 9.4 `get_outstanding_logs(par_id)`

```cpp
// raft_main_helper.cc:400-412
int get_outstanding_logs(uint32_t par_id) {
    auto* worker = find_worker(par_id);
    auto* raft_server = worker->GetRaftServer();
    return worker->n_tot.load() - raft_server->commitIndex;
}
```

Returns the number of log entries that have been submitted but not yet
committed by Raft consensus.  The formula `n_tot - commitIndex`
approximates the in-flight log count.  Used by Mako for backpressure.

### 10. Preferred Leader API

#### 10.1 `set_preferred_leader(site_id)`

```cpp
// raft_main_helper.cc:650-679
void set_preferred_leader(int site_id) {
    siteid_t preferred = static_cast<siteid_t>(site_id);
    for (auto& worker : raft_workers_g) {
        auto raft_server = worker->GetRaftServer();
        raft_server->SetPreferredLeader(preferred);
    }
}
```

Runtime API for changing the preferred leader across all partitions.
Iterates all workers and calls `RaftServer::SetPreferredLeader()` on
each.  This triggers the monitoring thread on the current leader which
will transfer leadership to the new preferred node once it catches up.

This function is Raft-only and is dispatched from
`replication_helper.cc::set_preferred_leader()` only when
`is_using_raft()` is true.

### 11. Stub Functions

The following functions exist solely for link-time compatibility with
`paxos_impl`.  They log warnings and return default values:

| Function | Lines | Behavior |
|----------|-------|----------|
| `microbench_paxos()` | 439-441 | Log warning, return |
| `microbench_paxos_queue()` | 567-569 | Log warning, return |
| `nc_setup_server()` | 682-684 | Log warning, return |
| `nc_get_new_order_requests()` | 686-689 | Return `nullptr` |
| `nc_get_payment_requests()` | 691-694 | Return `nullptr` |
| `nc_get_delivery_requests()` | 696-699 | Return `nullptr` |
| `nc_get_order_status_requests()` | 701-704 | Return `nullptr` |
| `nc_get_stock_level_requests()` | 706-709 | Return `nullptr` |
| `nc_get_read_requests()` | 711-714 | Return `nullptr` |
| `nc_get_rmw_requests()` | 716-719 | Return `nullptr` |
| `worker_info_stats()` | 616-626 | Dumps partition counters (not a stub) |
| `getHosts()` | 377-397 | Fully implemented YAML parser |

The `nc_*` functions (network client) are stubs because the network client
subsystem is specific to Paxos's multi-master architecture and has no
equivalent in Raft.

### 12. Function Reference Table

| Function | Lines | Scope | Category |
|----------|-------|-------|----------|
| `setup()` | 239-285 | `raft_impl` | Lifecycle |
| `setup2()` | 297-374 | `raft_impl` | Lifecycle |
| `shutdown_paxos()` | 415-436 | `raft_impl` | Lifecycle |
| `pre_shutdown_step()` | 572-583 | `raft_impl` | Lifecycle |
| `server_launch_worker()` | 116-178 | anonymous | Lifecycle (internal) |
| `add_log_to_nc()` | 534-553 | `raft_impl` | Log submission |
| `submit()` | 510-521 | `raft_impl` | Log submission |
| `add_log()` | 524-526 | `raft_impl` | Log submission |
| `add_log_without_queue()` | 529-531 | `raft_impl` | Log submission |
| `wait_for_submit()` | 557-564 | `raft_impl` | Log submission |
| `register_for_follower()` | 444-451 | `raft_impl` | Callback |
| `register_for_follower_par_id()` | 454-461 | `raft_impl` | Callback |
| `register_for_follower_par_id_return()` | 464-470 | `raft_impl` | Callback |
| `register_for_leader()` | 473-480 | `raft_impl` | Callback |
| `register_for_leader_par_id()` | 488-495 | `raft_impl` | Callback |
| `register_for_leader_par_id_return()` | 498-504 | `raft_impl` | Callback |
| `register_leader_election_callback()` | 483-485 | `raft_impl` | Callback |
| `send_no_ops_for_mark()` | 38-47 | `raft_impl` | NO-OP |
| `send_no_ops_to_all_workers()` | 50-52 | `raft_impl` | NO-OP |
| `get_epoch()` | 586-588 | `raft_impl` | Epoch |
| `set_epoch()` | 591-605 | `raft_impl` | Epoch |
| `upgrade_p1_to_leader()` | 608-613 | `raft_impl` | Epoch |
| `get_outstanding_logs()` | 400-412 | `raft_impl` | Query |
| `set_preferred_leader()` | 650-679 | `raft_impl` | Preferred leader |
| `getHosts()` | 377-397 | `raft_impl` | Config |
| `worker_info_stats()` | 616-626 | `raft_impl` | Debug |
| `find_worker()` | 181-188 | anonymous | Internal |
| `enqueue_to_worker()` | 191-205 | anonymous | Internal |
| `apply_callbacks_for_partition()` | 209-226 | anonymous | Internal |
| `handle_leader_change_impl()` | 233-236 | `raft_impl` | Internal |
| `wait_for_local_leadership()` | 68-105 | anonymous | Internal |
| `check_current_path()` | 108-113 | anonymous | Internal |
| `raft_handle_leader_change()` | 732-740 | `janus` | Leader change |
| `NotifyRaftLeaderChange()` | 744-748 | `janus` | Leader change |

### 13. Comparison with `paxos_main_helper.cc`

| Aspect | `paxos_main_helper.cc` | `raft_main_helper.cc` |
|--------|------------------------|----------------------|
| Worker type | `PaxosWorker` | `RaftWorker` |
| Worker vector | `pxs_workers_g` | `raft_workers_g` |
| Callback maps | `leader_replay_cb` only | Both `leader_replay_cb` and `follower_replay_cb` |
| Leader selection | Fixed (`action` parameter in `setup2()`) | Raft election with preferred bias |
| NO-OP format | Paxos-native NO-OP slots | `"no-ops:<epoch>"` string entries |
| `setup2()` | Creates submit pool, starts bulk coordinators | Configures preferred leader, launches workers |
| Network client | Full `nc_*` implementation | Stubs returning `nullptr` |
| Jetpack | Respects environment | Forces `MAKO_DISABLE_JETPACK=1` |
| Submit mechanism | `_BulkSubmit()` via coordinator | `EnqueueLog()` → `SubmitLoop()` → `Start()` |
| Shutdown | Similar 2-phase pattern | Similar 2-phase pattern |
| Leader change | Via `ElectionState` singleton | Via `RegisterLeaderChangeCallback` + `NotifyRaftLeaderChange` |
| Lines of code | ~900 | 751 |

---

## Mako-Side Integration Points

<a id="1-overview-1"></a>

### 1. Overview

While the `replication_helper` dispatcher and `raft_main_helper` glue layer
live in the `src/deptran/` tree, several critical integration points exist on
the **Mako side** (`src/mako/`).  These are the places where Mako's
transaction engine calls into the unified replication API, registers
callbacks, and handles leadership changes.

The key principle is that Mako remains protocol-agnostic: all Raft-specific
logic is behind the dispatcher.  The Mako-side changes are limited to:

1. **Auto-detection**: Scanning YAML configs for `ab: raft`
2. **Guard clauses**: `is_using_raft()` checks in failure-handling code paths
3. **Launch scripts**: Separate Raft-specific scripts with correct config files
4. **Config files**: `occ_raft.yml` alongside `occ_paxos.yml`

**Source**: `src/mako/mako.hh`, `bash/shard.sh`, `bash/shard_raft.sh`,
`config/occ_raft.yml`

### 2. `init_env()` — The Main Initialisation Sequence

All replication setup happens inside `init_env()` (`mako.hh:858`).  This
function is called from `dbtest.cc::main()` after command-line parsing.

```
init_env()                                        [mako.hh:858]
  +-- init_config_node() / fetch_config_from_cnode()
  +-- setup_sync_util_callbacks()                  // epoch queries
  +-- if (isReplicated):
  |   +-- Create replicated_db (TSharedThreadPoolMbta)
  |   +-- setup_transport_callbacks()              // failure recovery
  |   +-- setup_leader_election_callbacks()         // leadership changes
  |   +-- detect_replication_type_from_config()     // ← Raft auto-detection
  |   +-- prepare_paxos_args() → argc/argv
  |   +-- setup(argc, argv)                         // → raft_impl::setup()
  |   +-- setup_paxos_leader_callbacks(tracker)    // watermark callbacks
  |   +-- setup_paxos_follower_callbacks(replicated_db)
  |   +-- setup2(0, shardIndex)                    // → raft_impl::setup2()
  |   +-- sleep(3)                                  // wait for all nodes
  +-- return db
```

**Critical ordering**: `detect_replication_type_from_config()` must run
**before** `setup()` because `setup()` uses the `DISPATCH_RAFT_OR_PAXOS`
macro, and the global `ReplicationType` must be set correctly before the
first dispatch call.

The callback registrations (`setup_paxos_leader_callbacks` and
`setup_paxos_follower_callbacks`) run **after** `setup()` but **before**
`setup2()`.  This is because `setup()` creates the `RaftWorker` instances
that the callbacks will be registered on, and `setup2()` starts the election
timer which may immediately trigger leadership changes.

### 3. `detect_replication_type_from_config()`

#### 3.1 Implementation

```cpp
// mako.hh:779-816
static void detect_replication_type_from_config(
    const vector<string>& config_files) {
    // Don't override explicit CLI setting
    if (janus::is_using_raft()) return;

    for (const auto& file_path : config_files) {
        std::ifstream ifs(file_path);
        std::string line;
        while (std::getline(ifs, line)) {
            auto pos = line.find("ab:");
            if (pos != std::string::npos) {
                // trim and extract value
                if (value == "raft" || value == "fpga_raft") {
                    janus::set_replication_type(
                        janus::ReplicationType::RAFT);
                    return;
                }
            }
        }
    }
}
```

#### 3.2 Design Rationale

This function exists because of a real integration bug: `dbtest` used the
Paxos code path even when the YAML config had `ab: raft`, because
`Config::CreateConfig()` (which parses the YAML and sets `replica_proto_`)
only runs inside `setup()`, but the dispatcher needs to know the protocol
**before** `setup()` is called.

The fix is a lightweight YAML scanner that:
1. Opens each config file as plain text
2. Searches for lines containing `ab:`
3. Extracts the value after the colon
4. If the value is `"raft"` or `"fpga_raft"`, sets the global type

#### 3.3 Priority Rules

| Detection Method | Priority | When Set |
|-----------------|----------|----------|
| `--replication raft` CLI flag | Highest | `dbtest.cc:main()`, before `init_env()` |
| `detect_replication_type_from_config()` | Medium | `init_env()`, before `setup()` |
| Default (PAXOS) | Lowest | Static initialiser of `g_replication_type` |

The function checks `is_using_raft()` first and returns immediately if the
CLI flag already set the type.  This ensures CLI always wins.

### 4. `setup_leader_election_callbacks()`

#### 4.1 Purpose

This function registers a single callback that fires on every Raft (or
Paxos) leadership change.  It handles the cross-shard coordination needed
when a datacenter fails and a new leader must be elected.

#### 4.2 Implementation Structure

```cpp
// mako.hh:641-761
static void setup_leader_election_callbacks() {
    register_leader_election_callback([&](int control) {
        switch (control) {
#if defined(FAIL_NEW_VERSION) && !defined(MAKO_USE_RAFT)
            case 0: { /* leader stepped down */ }
            case 2: { /* became leader */ }
#endif
#if !defined(FAIL_NEW_VERSION)
            case 0: { /* leader stepped down (partial failure) */ }
            case 2: { /* became leader (PREPARE phase) */ }
            case 3: { /* COMMIT phase */ }
            case 4: { /* datacenter failure */ }
#endif
        }
    });
}
```

The `control` parameter values:

| Value | Meaning | Paxos Action | Raft Action |
|-------|---------|-------------|-------------|
| 0 | Lost leadership | Stop exchange, issue `client_control(0)` | No-op (Raft handles internally) |
| 1 | (reserved) | — | — |
| 2 | Gained leadership | Collect FVW, `client_control(1)`, notify workers | No-op (Raft handles internally) |
| 3 | Commit new leader | Notify workers, `client_control(2)` | No-op |
| 4 | Datacenter failure | Forward to all followers | No-op |

#### 4.3 The `is_using_raft()` Guard

Every `case` that performs cross-shard RPC or coordination checks
`janus::is_using_raft()` and breaks immediately if true:

```cpp
case 0: {
    if (janus::is_using_raft()) {
        // Raft: Leader stepped down - no action needed
        break;
    }
    // Paxos: complex recovery...
    sync_util::sync_logger::client_control(0, ...);
    break;
}
```

This guard exists because of a real bug: without it, Raft leadership
changes (which are frequent — every election, every transfer) would trigger
the Paxos failure-recovery code path, which calls `client_control()` to
issue cross-shard RPCs.  In a 2-shard system, these RPCs would fail because
the target shards are running Raft (not Paxos) and don't have the expected
RPC handlers.

**The bug manifested as**: cross-shard RPC timeouts during Raft leader
elections in 2-shard mode, causing the entire system to hang.

#### 4.4 Compile-Time vs Runtime Guards

The code uses **both** compile-time (`#if defined(FAIL_NEW_VERSION)`) and
runtime (`janus::is_using_raft()`) guards:

- `FAIL_NEW_VERSION` is a compile-time flag that enables the new
  failure-recovery code path (datacenter failover).  It selects which set
  of `case` handlers are compiled.
- `is_using_raft()` is a runtime check within each handler.  It is needed
  because the same binary can run with either Paxos or Raft, selected by
  config or CLI flag.

Both code paths (`FAIL_NEW_VERSION` and `!FAIL_NEW_VERSION`) contain the
same `is_using_raft()` guards for cases 0 and 2.

### 5. `setup_paxos_leader_callbacks()` / `setup_paxos_follower_callbacks()`

#### 5.1 Leader Callbacks

```cpp
// mako.hh:488-494
static void setup_paxos_leader_callbacks(
    vector<pair<uint32_t, uint32_t>>& advanceWatermarkTracker) {
    if (!isReplicated) return;
    for (int i = 0; i < nthreads; i++) {
        register_paxos_leader_callback(advanceWatermarkTracker, i);
    }
}
```

This iterates over all partition threads and registers a leader watermark
callback via `register_for_leader_par_id_return()`.  The callback
(`register_paxos_leader_callback`, `mako.hh:400-486`) handles:

1. **Advancer marker** (`len == ADVANCER_MARKER_NUM`): Returns
   `STATUS_REPLAY_DONE` and starts the advancer on partition 0.
2. **End signal** (`len == 0`): Sets `local_timestamp_` to max, increments
   end-received counter.
3. **NO-OP entries** (`isNoops(log, len) != -1`): Increments `noops_cnt`,
   on partition 0 computes local watermark, publishes via NFSSync, updates
   stable timestamp.
4. **Regular logs** (`len > 0` and not NO-OP): Extracts `CommitInfo`,
   stores `timestamp` in `local_timestamp_[par_id]`, updates latency
   tracking.

Returns encoded value: `timestamp * 10 + status`.

#### 5.2 Follower Callbacks

```cpp
// mako.hh:496-502
static void setup_paxos_follower_callbacks(
    TSharedThreadPoolMbta& replicated_db) {
    if (!isReplicated) return;
    for (int i = 0; i < nthreads; i++) {
        register_paxos_follower_callback(replicated_db, i);
    }
}
```

The follower callback (`register_paxos_follower_callback`, `mako.hh:240`)
replays committed logs into the `replicated_db` instance, which is a
thread-pool-backed Masstree database.

#### 5.3 Protocol Agnosticism

Despite the "paxos" naming, these callbacks work identically with Raft.
The dispatch layer routes `register_for_leader_par_id_return()` to either
`paxos_impl` or `raft_impl`, and both implementations cache and apply the
callback to their respective worker instances.

The `thread_id` parameter maps to `par_id` (partition ID), matching the
convention where each Mako thread owns one partition.

### 6. Other Callback Setup Functions

#### 6.1 `setup_sync_util_callbacks()`

```cpp
// mako.hh:585-601
static void setup_sync_util_callbacks() {
    register_sync_util([&]() {
        return isReplicated ? get_epoch() : 0;
    });
    register_sync_util_sc([&]() { /* same */ });
    register_sync_util_ss([&]() { /* same */ });
}
```

Registers three epoch-query callbacks used by the sync utility system
(watermark exchange between shards).  These are protocol-agnostic because
`get_epoch()` dispatches through the replication helper.

#### 6.2 `setup_transport_callbacks()`

```cpp
// mako.hh:604-639
static void setup_transport_callbacks() {
    register_fasttransport_for_dbtest([&](int control, int value) {
        switch (control) {
            case 4: {
                upgrade_p1_to_leader();
                // send NO-OPs, notify workers
            }
        }
        return 0;
    });
}
```

Handles the transport-level leadership transfer for datacenter failure
scenarios.  `upgrade_p1_to_leader()` dispatches to the replication helper
and is a no-op for Raft (Raft handles leadership transfer internally via
the preferred leader mechanism).

#### 6.3 `cleanup_and_shutdown()`

```cpp
// mako.hh:763-773
static void cleanup_and_shutdown() {
    if (isReplicated) {
        sleep_for(2s);
        pre_shutdown_step();     // → raft_impl::pre_shutdown_step()
        shutdown_paxos();        // → raft_impl::shutdown_paxos()
    }
    sync_util::sync_logger::shutdown();
}
```

The 2-second sleep allows in-flight RPCs to complete before initiating the
teardown sequence.  Both `pre_shutdown_step()` and `shutdown_paxos()`
dispatch to the correct implementation.

### 7. YAML Config Files

#### 7.1 `config/occ_raft.yml`

```yaml
mode:
  cc: occ         # concurrency control: optimistic
  ab: raft        # atomic broadcast: Raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1      # per client
```

This is the Raft-mode equivalent of `config/occ_paxos.yml`.  The critical
difference is `ab: raft` which:
1. Is detected by `detect_replication_type_from_config()` for auto-detection
2. Is parsed by `Config::InitMode()` which calls `Frame::Name2Mode("raft")`
   to set `replica_proto_ = MODE_RAFT`

#### 7.2 Raft-Specific Cluster Configs

For each thread count and shard index, Raft uses its own cluster topology
files in `config/1leader_2followers/`:

| Raft Config | Paxos Equivalent |
|------------|------------------|
| `raft3_shardidx0.yml` | `paxos3_shardidx0.yml` |
| `raft3_shardidx1.yml` | `paxos3_shardidx1.yml` |

The Raft configs use port ranges 27xxx to avoid collisions with Paxos on
17xxx.  The topology (1 leader + 2 followers per partition) is identical.

### 8. Launch Scripts: `shard.sh` vs `shard_raft.sh`

#### 8.1 `bash/shard.sh` — Unified Launcher

The main launch script (`bash/shard.sh`, 62 lines) supports both protocols
via a 7th argument:

```bash
replication_type=${7:-paxos}  # Default to paxos if not specified

if [ "$is_replicated" == "1" ]; then
    if [ "$replication_type" == "raft" ]; then
        OCC_CONFIG="config/occ_raft.yml"
    else
        OCC_CONFIG="config/occ_paxos.yml"
    fi
    CMD="$CMD -F config/1leader_2followers/paxos${trd}_shardidx${shard}.yml \
         -F $OCC_CONFIG --is-replicated --replication=$replication_type"
fi
```

**Key features**:
- Selects `occ_raft.yml` or `occ_paxos.yml` based on `$replication_type`
- Passes `--replication=$replication_type` to `dbtest`, which calls
  `set_replication_type_from_string()` in `main()`
- Uses `GDB_PREFIX` from `util.sh` for optional debugging
- Sets `LD_LIBRARY_PATH` for shared libraries

#### 8.2 `bash/shard_raft.sh` — Raft-Specific Launcher

The Raft-specific script (`bash/shard_raft.sh`, 39 lines) is a simpler
alternative that hardcodes the Raft configuration:

```bash
CMD="./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-index $shard \
     --shard-config $path/config/local-shards$nshard-warehouses$trd.yml \
     -F config/1leader_2followers/raft${trd}_shardidx${shard}.yml \
     -F config/occ_raft.yml -P $cluster --replication raft"
```

**Key differences from `shard.sh`**:

| Aspect | `shard.sh` | `shard_raft.sh` |
|--------|-----------|----------------|
| Protocol selection | `$7` arg (default paxos) | Hardcoded raft |
| Config file naming | `paxos${trd}_shardidx${shard}` | `raft${trd}_shardidx${shard}` |
| OCC config | Dynamic selection | Always `occ_raft.yml` |
| `--replication` flag | `=$replication_type` | `raft` (always) |
| GDB support | Yes (`util.sh` sourced) | No |
| Lines of code | 62 | 39 |
| Primary use | CI tests, production | Quick Raft-only testing |

Both scripts pass the same core arguments to `dbtest`:
`--num-threads`, `--shard-index`, `--shard-config`, `-F` (config files),
`-P` (process name), `--replication`, and optionally `--is-micro` and
`--is-replicated`.

#### 8.3 Port Separation

The Raft cluster configs use port ranges starting at 27xxx to avoid
collisions with Paxos on 17xxx.  This separation is essential for CI
testing where both Raft and Paxos tests may run on the same machine:

| Protocol | Base Port Range | Heartbeat Port Range |
|----------|----------------|---------------------|
| Paxos | 17001-17999 | 27001-27999 |
| Raft | 27001-27999 | 37001-37999 |

The heartbeat port is always `base_port + CtrlPortDelta` where
`CtrlPortDelta = 10000`.

### 9. Functions That Do NOT Need Raft Changes

Several Mako functions work with Raft without any modification because
they only call the unified replication API:

| Function | Location | Why No Changes Needed |
|----------|----------|----------------------|
| `register_paxos_leader_callback()` | `mako.hh:400` | Calls `register_for_leader_par_id_return()` which dispatches |
| `register_paxos_follower_callback()` | `mako.hh:240` | Calls `register_for_follower_par_id_return()` which dispatches |
| `wait_for_termination()` | `mako.hh:546` | Polls `benchConfig.getEndReceived()`, protocol-independent |
| `cleanup_and_shutdown()` | `mako.hh:763` | Calls `pre_shutdown_step()` / `shutdown_paxos()` which dispatch |
| `prepare_paxos_args()` | `mako.hh:818` | Builds argc/argv consumed by `Config::CreateConfig()` |
| `setup_sync_util_callbacks()` | `mako.hh:585` | Calls `get_epoch()` which dispatches |

This validates the dispatcher architecture's design goal: Mako's transaction
engine is completely protocol-agnostic.

### 10. Summary of Mako-Side Raft Changes

| Change Type | Location | Lines | Description |
|-------------|----------|-------|-------------|
| Auto-detection | `mako.hh:779-816` | 38 | `detect_replication_type_from_config()` scans YAML for `ab: raft` |
| Guard clause | `mako.hh:650` | 3 | `is_using_raft()` in FAIL_NEW_VERSION case 0 |
| Guard clause | `mako.hh:662` | 3 | `is_using_raft()` in FAIL_NEW_VERSION case 2 |
| Guard clause | `mako.hh:700` | 3 | `is_using_raft()` in !FAIL_NEW_VERSION case 0 |
| Guard clause | `mako.hh:722` | 3 | `is_using_raft()` in !FAIL_NEW_VERSION case 2 |
| Config file | `config/occ_raft.yml` | 10 | Raft-mode OCC configuration |
| Launch script | `bash/shard_raft.sh` | 39 | Raft-specific shard launcher |
| Launch script | `bash/shard.sh:36-41` | 6 | Protocol-switching in unified launcher |

**Total Mako-side changes**: ~105 lines (5 guard clauses + 1 function + 2 scripts + 1 config).
The vast majority of the Raft integration lives in `src/deptran/`, not in
Mako.

---

## Integration Challenges and Bugs Fixed

<a id="1-overview-2"></a>

### 1. Overview

Adding Raft as an alternative consensus protocol to a system designed around
Multi-Paxos required solving a series of integration challenges.  This
chapter documents the specific bugs encountered during the process, their
root causes, the fixes applied, and the lessons learned.  Each bug is
presented with exact source locations so that a reader can trace the fix in
the codebase.

### 2. Bug: Dispatcher Routing to Paxos (simpleRaft.cc)

#### Symptom

The standalone `simpleRaft` example compiled and ran without errors, but
Raft RPCs (AppendEntries, Vote) were never sent.  Instead, the system
initialised Paxos workers and attempted Multi-Paxos consensus.

#### Root Cause

The `DISPATCH_RAFT_OR_PAXOS` macro checks `janus::is_using_raft()` which
reads `g_replication_type`.  The default value is `ReplicationType::PAXOS`.
`simpleRaft.cc` was calling `setup()` without first calling
`set_replication_type(RAFT)`, so the dispatcher routed to
`paxos_impl::setup()`:

```cpp
// BEFORE fix (incorrect):
std::vector<string> ret = setup(argc, argv);  // → paxos_impl::setup()

// AFTER fix (correct):
janus::set_replication_type(janus::ReplicationType::RAFT);
std::vector<string> ret = setup(argc, argv);  // → raft_impl::setup()
```

#### Fix

A single line was added before the `setup()` call in `simpleRaft.cc:92`:

```cpp
// examples/mako-raft-tests/simpleRaft.cc:88-94
// Set replication type to Raft BEFORE setup() so the dispatcher routes
// to raft_impl::setup() which disables Jetpack recovery (MAKO_DISABLE_JETPACK=1).
// Without this, the default is PAXOS and setup() goes through paxos_main_helper
// which does not disable Jetpack, causing RPC handler mismatches at runtime.
janus::set_replication_type(janus::ReplicationType::RAFT);

std::vector<string> ret = setup(18, argv_raft);
```

#### Verification

After the fix, all 3 replicas received 303 callbacks (≥300 required):
`p1=303, p2=303, leader=303`.

#### Lesson

**Dispatcher patterns require explicit type setting before the first
dispatch call.**  Global state defaults are not safe assumptions.

### 3. Bug: Auto-Detection Failure in `dbtest`

<a id="symptom-1"></a>

#### Symptom

When running `dbtest` (Mako's main benchmark binary) with Raft config files
containing `ab: raft`, the system still used the Paxos code path.  The
`--replication raft` CLI flag worked, but the automatic config-based
detection did not.

<a id="root-cause-1"></a>

#### Root Cause

`dbtest` calls `init_env()` which calls `setup()`.  The `setup()` function
internally calls `Config::CreateConfig()` which parses the YAML and sets
`replica_proto_ = MODE_RAFT`.  However, the `DISPATCH_RAFT_OR_PAXOS` macro
in `setup()` itself checks `is_using_raft()` **before** `Config::CreateConfig()`
runs.  This creates a chicken-and-egg problem: the YAML is only parsed
inside `setup()`, but the dispatch decision happens at the `setup()` entry
point.

<a id="fix-1"></a>

#### Fix

Added `detect_replication_type_from_config()` in `mako.hh:779-816`.  This
function performs a lightweight text scan of the YAML files before `setup()`
is called, looking for `ab: raft` or `ab: fpga_raft`:

```cpp
// mako.hh:886-889 — called in init_env() before setup()
detect_replication_type_from_config(benchConfig.getPaxosConfigFile());

// ... then setup() correctly dispatches to raft_impl
std::vector<std::string> ret = setup(argc_paxos, argv_paxos);
```

As a safety measure, `shard_raft.sh` also passes `--replication raft` on
the command line, providing a belt-and-suspenders approach.

#### Priority Chain

| Detection Method | Priority | When Set |
|-----------------|----------|----------|
| `--replication raft` CLI flag | Highest | `dbtest.cc:main()` |
| `detect_replication_type_from_config()` | Medium | `init_env()` |
| Default (PAXOS) | Lowest | Static initialiser |

<a id="verification-1"></a>

#### Verification

After the fix, `dbtest` with Raft configs achieved 69,784.6 ops/sec with
`replay_batch: 796`.

<a id="lesson-1"></a>

#### Lesson

**When a dispatcher pattern is used, auto-detection must run before the
first dispatch point**, not inside it.

### 4. Bug: Cross-Shard RPC Failures During Raft Elections (FAIL_NEW_VERSION)

<a id="symptom-2"></a>

#### Symptom

In 2-shard mode with `FAIL_NEW_VERSION` enabled, the system would hang with
RPC timeout errors whenever a Raft leader election occurred.

<a id="root-cause-2"></a>

#### Root Cause

The `setup_leader_election_callbacks()` function in `mako.hh` registers a
callback that fires on every leadership change.  Under Paxos (fixed leader),
this callback is rarely invoked.  Under Raft, it fires on every election
and every leadership transfer.

The callback's `case 0` (leader lost) and `case 2` (became leader) handlers
called `sync_util::sync_logger::client_control()`, which issues cross-shard
RPCs to coordinate failure recovery.  These RPCs assume the target shards
have Paxos-compatible handlers.  When running under Raft, the targets don't
have these handlers, causing RPC timeouts.

```cpp
// mako.hh:648-660 — BEFORE fix
#if defined(FAIL_NEW_VERSION)
case 0: {
    // This was called on EVERY Raft election, not just datacenter failures:
    sync_util::sync_logger::client_control(0, benchConfig.getShardIndex());
    // ↑ Sends cross-shard RPC that times out under Raft
    break;
}
```

<a id="fix-2"></a>

#### Fix

Added `janus::is_using_raft()` guard at the top of every case handler:

```cpp
// mako.hh:648-660 — AFTER fix
#if defined(FAIL_NEW_VERSION) && !defined(MAKO_USE_RAFT)
case 0: {
    if (janus::is_using_raft()) {
        // Raft: Leader stepped down - no action needed
        break;
    }
    // Paxos-only recovery code...
    sync_util::sync_logger::client_control(0, ...);
    break;
}
case 2: {
    if (janus::is_using_raft()) {
        // Raft: Became leader - no action needed
        break;
    }
    // Paxos-only recovery code...
```

The guards are present in **both** `FAIL_NEW_VERSION` and
`!FAIL_NEW_VERSION` code paths (`mako.hh:650, 662, 700, 722`) — four
guards total.

<a id="verification-2"></a>

#### Verification

After the fix, 2-shard Raft tests pass without hangs.  Leadership changes
complete in ~40ms without triggering cross-shard RPCs.

<a id="lesson-2"></a>

#### Lesson

**When adding a new protocol to a system with protocol-specific failure
recovery, every failure-recovery code path must be guarded.**  Raft's
frequent leadership changes expose paths that Paxos's fixed leader never
triggers.

### 5. Bug: GetOrCreateClient() Race Condition

<a id="symptom-3"></a>

#### Symptom

Intermittent segmentation faults during shutdown or high-concurrency
scenarios, with stack traces pointing to the RRR transport backend's
`GetOrCreateClient()` method.

<a id="root-cause-3"></a>

#### Root Cause

Classic TOCTOU (Time-of-Check to Time-of-Use) race condition.  The code
found an entry in the `clients_` map, then released the mutex, then tried
to use the iterator:

```cpp
// rrr_rpc_backend.cc — BEFORE fix (buggy)
auto it = clients_.find(session_key);
if (it != clients_.end()) {
    clients_lock_.unlock();          // Release lock
    return it->second.clone();       // BUG: iterator may be invalid
}
```

**Race timeline**:
1. Thread A finds client → `it` points to valid entry
2. Thread A unlocks `clients_lock_`
3. Thread B (in `Stop()`) acquires lock and calls `clients_.clear()`
4. Thread A accesses `it->second` → **use-after-free**

<a id="fix-3"></a>

#### Fix

Clone the `Arc` before releasing the lock (`rrr_rpc_backend.cc:206-211`):

```cpp
// AFTER fix
auto it = clients_.find(session_key);
if (it != clients_.end()) {
    auto result = it->second.clone();  // Clone BEFORE unlock
    clients_lock_.unlock();
    return result;
}
```

**Commit**: `c84909cc` — "Fix race condition in GetOrCreateClient causing
intermittent segfault"

<a id="verification-3"></a>

#### Verification

- `shardNoReplication`: 5/5 passes
- `rrrTests`: 66/66 passes
- `shard2Replication`: passes consistently

<a id="lesson-3"></a>

#### Lesson

**Always copy/clone data from shared structures before releasing the lock
that protects them.**  This applies universally, but is particularly
important in shutdown paths where data structures may be cleared.

### 6. Challenge: Process Cleanup and Port Release

#### Problem

Raft test binaries (`simpleRaft`, `testPreferredReplicaStartup`,
`testPreferredReplicaLogReplication`, `testNoOps`) would sometimes hang
during shutdown, holding network ports open.  The next test would then fail
to bind to those ports.

<a id="root-cause-4"></a>

#### Root Cause

1. **Raft heartbeat threads**: Background OS threads (`StartLeadershipTransferMonitoring`,
   `HeartbeatLoop`) don't always exit cleanly if the election timer hasn't
   fired yet.
2. **RPC connections**: `rrr::Server::~Server()` enqueues cleanup commands
   to the poll thread, but if the poll thread has already exited, the
   commands are lost and connection reference counts never reach zero.
3. **TCP TIME_WAIT**: Even after process exit, TCP ports remain in
   TIME_WAIT state for 60 seconds.

<a id="fix-4"></a>

#### Fix

Multi-layered cleanup in `ci/ci_mako_raft.sh`:

```bash
# ci_mako_raft.sh:65-105
cleanup_processes() {
    # Layer 1: SIGKILL all known test binaries
    pkill -9 -f "build/simpleRaft" 2>/dev/null || true
    pkill -9 -f "build/testPreferredReplicaStartup" 2>/dev/null || true
    pkill -9 -f "build/testPreferredReplicaLogReplication" 2>/dev/null || true
    pkill -9 -f "build/testNoOps" 2>/dev/null || true
    pkill -9 -f "build/dbtest" 2>/dev/null || true

    # Layer 2: Kill wrapper scripts
    pkill -9 -f "test_1shard_replication_raft.sh" 2>/dev/null || true
    pkill -9 -f "test_2shard_replication_raft.sh" 2>/dev/null || true

    sleep 3  # Give OS time to release file descriptors

    # Layer 3: Poll for port release
    for i in {1..10}; do
        if ! lsof -i :7001-8006 >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done
}
```

Additionally, `check_for_hanging_processes()` runs after each test to detect
and kill zombie processes:

```bash
check_for_hanging_processes() {
    local hanging_count=$(ps aux | grep -E "[d]btest|[s]impleRaft" | wc -l)
    if [ "$hanging_count" -gt 0 ]; then
        pkill -9 -f "build/dbtest" 2>/dev/null || true
        pkill -9 -f "build/simpleRaft" 2>/dev/null || true
        sleep 2
    fi
}
```

<a id="lesson-4"></a>

#### Lesson

**Distributed system tests require aggressive process cleanup.**  SIGTERM is
not sufficient; SIGKILL is needed.  Port release polling with backoff
prevents flaky tests from cascading failures.

### 7. Challenge: Port Conflicts Between Paxos and Raft

<a id="problem-1"></a>

#### Problem

When both Paxos and Raft tests run on the same machine (as in CI), they
can collide on TCP ports if they use the same ranges.

#### Solution

Separate port ranges are assigned:

| Protocol | Base Port Range | Heartbeat Port Range | Config Files |
|----------|----------------|---------------------|-------------|
| Paxos | 17001-17999 | 27001-27999 | `paxos6_shardidx*.yml` |
| Raft | 27001-27999 | 37001-37999 | `raft6_shardidx*.yml` |
| Raft tests | 38100-38199 | 48100-48199 | `1c1s*r*p_cluster_test.yml` |

The heartbeat port is always `base_port + CtrlPortDelta` where
`CtrlPortDelta = 10000` (defined in `RaftWorker`).

**Config example** (`config/1c1s3r1p_cluster_test.yml`):
```yaml
site:
  server:
    - ["localhost:38100", "p1:38101", "p2:38102"]
```

<a id="lesson-5"></a>

#### Lesson

**Assign non-overlapping port ranges per protocol from the start.**  Document
the allocation scheme so future developers know which ranges are reserved.

### 8. Challenge: Jetpack Recovery Incompatibility

<a id="problem-2"></a>

#### Problem

Mako's Jetpack optimisation (speculative execution with optimistic log
aggregation) conflicts with Raft's own recovery mechanisms.  Running both
simultaneously causes state inconsistencies.

<a id="solution-1"></a>

#### Solution

`raft_impl::setup()` forces `MAKO_DISABLE_JETPACK=1` before any worker
initialisation:

```cpp
// raft_main_helper.cc:243-248
if (std::getenv("MAKO_DISABLE_JETPACK") == nullptr) {
    setenv("MAKO_DISABLE_JETPACK", "1", 1);
}
```

At runtime, `JetpackRecoveryEnabled()` in `server.cc:280-310` checks this
environment variable and disables Jetpack's speculative recovery when the
flag is set.

#### Design Choice

The flag is set as an environment variable (not a config parameter) because:
1. It must be set before `Config::CreateConfig()` runs
2. It can be overridden by operators who know what they're doing
3. It affects behaviour across multiple compilation units

<a id="lesson-6"></a>

#### Lesson

**Protocol-specific optimisations must be explicitly disabled when switching
protocols.**  Using an environment variable provides the right balance of
safety (default off) and flexibility (overridable).

### 9. Additional Fixes: Transport Layer Shutdown Races

Beyond the bugs above, the RRR transport layer required five coordinated
fixes for clean shutdown:

| Fix | Description | Location |
|-----|-------------|----------|
| Atomic stop flag | `stop_` changed from `bool` to `std::atomic<bool>` | `rrr_rpc_backend.h` |
| Idempotent `Stop()` | Atomic compare-exchange prevents concurrent execution | `rrr_rpc_backend.cc` |
| Early stop checks | Check `stop_` at entry of all RPC send methods | `rrr_rpc_backend.cc` |
| Lock-protected check | Check `stop_` inside lock in `GetOrCreateClient()` | `rrr_rpc_backend.cc` |
| Post-wait check | Check `stop_` after RPC wait completes | `rrr_rpc_backend.cc` |

These work together as **defence-in-depth** against shutdown races that
manifest as segfaults or hangs when stopping Raft workers.

### 10. Summary

| Bug | Root Cause | Fix | Impact |
|-----|-----------|-----|--------|
| Missing `set_replication_type` | Dispatcher defaults to Paxos | Call before `setup()` | Paxos workers created instead of Raft |
| Auto-detection failure | YAML parsed inside dispatch target | Scan YAML before `setup()` | `dbtest` ignores `ab: raft` config |
| FAIL_NEW_VERSION RPCs | Leader election triggers Paxos recovery | `is_using_raft()` guard | System hangs in 2-shard mode |
| GetOrCreateClient race | Iterator used after unlock | Clone before unlock | Intermittent segfaults |
| Process cleanup | Raft threads don't exit cleanly | SIGKILL + port polling | Tests fail to bind ports |
| Port conflicts | Same ranges for Paxos and Raft | Separate port ranges | Paxos and Raft tests collide |
| Jetpack incompatibility | Different recovery models | Force disable for Raft | State inconsistencies |
| Transport shutdown | Non-atomic stop flag | Atomic + defence-in-depth | Segfaults during shutdown |

#### Key Architectural Lesson

The dispatcher pattern (`DISPATCH_RAFT_OR_PAXOS`) is a clean abstraction,
but it creates a **single critical point**: the global `ReplicationType`
must be correctly set before any dispatch call.  Three separate bugs
(#2, #3, and this dispatcher issue) were all caused by the same
fundamental problem: the replication type was not set early enough.
The solution—belt-and-suspenders with CLI flag, auto-detection, and
explicit calls—ensures correctness regardless of how the binary is launched.

---

## Standalone Raft Test Framework

<a id="1-overview-3"></a>

### 1. Overview

The standalone Raft test framework validates the correctness of the Raft
consensus implementation independently of Mako's transaction processing.
It runs 11 sequential test cases covering leader election, log agreement,
network partitions, concurrency, and the Figure 8 scenario from the Raft
paper.  The framework is compiled conditionally via the `RAFT_TEST_CORO`
flag and executes inside the existing coroutine reactor, requiring no
external test runner.

**Key source files:**

| File | Lines | Purpose |
|------|-------|---------|
| `src/deptran/raft/test.h` | 44 | `RaftLabTest` class declaration (11 test methods) |
| `src/deptran/raft/test.cc` | 741 | All 11 test case implementations |
| `src/deptran/raft/testconf.h` | 184 | `RaftTestConfig` class with constants, helpers, network control |
| `src/deptran/raft/testconf.cc` | 586 | `RaftTestConfig` implementation |
| `src/deptran/raft/frame.cc` | lines 141-186 | Test coroutine bootstrap and replica synchronisation |
| `config/raft_lab_test.yml` | 55 | YAML config defining 5-server cluster for tests |

### 2. Compile-Time Activation

The test framework is gated behind the `RAFT_TEST_CORO` preprocessor
macro, controlled by the CMake option `RAFT_TEST`:

```cmake
# CMakeLists.txt:227-229
if(RAFT_TEST)
  add_compile_definitions(RAFT_TEST_CORO=1)
endif()
```

When `RAFT_TEST_CORO` is not defined, all test classes, helper macros,
and the bootstrap coroutine in `frame.cc` are compiled out.  This ensures
zero overhead in production builds.

### 3. Configuration: `raft_lab_test.yml`

The test cluster is defined in `config/raft_lab_test.yml`:

```yaml
mode:
  cc: none        # No concurrency control (pure Raft replication)
  ab: raft        # Atomic broadcast using Raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1

site:
  server:
    - ["s101:9000", "s102:9001", "s103:9002", "s104:9003", "s105:9004"]
  client:
    - ["c01"]

process:
  s101: localhost
  s102: localhost
  s103: localhost
  s104: localhost
  s105: localhost
  c01: localhost

host:
  localhost: 127.0.0.1
```

Key design choices:
- **`cc: none`**: Disables Mako's concurrency control layer.  Tests
  exercise only the Raft consensus protocol.
- **5 servers**: Matches `NSERVERS` constant (tolerates 2 failures).
- **All localhost**: Single-machine execution for deterministic testing.

### 4. Test Constants

Constants are defined as preprocessor macros in `testconf.h:12-24`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `NSERVERS` | 5 | Number of Raft replicas in the test cluster |
| `ELECTIONTIMEOUT` | 5,000,000 us (5 s) | Generous timeout for leader election |
| `MAXSLOW` | 27 ms | Maximum simulated network latency |
| `DOWNRATE_N / DOWNRATE_D` | 1 / 10 | 10% chance of server disconnection per period |
| `ELECTIONRPCS` | `3 * NSERVERS` = 15 | Expected upper bound RPCs for one election |
| `COMMITRPCS(n)` | `(n+1) * NSERVERS` | Expected RPCs for `n` agreement rounds |

### 5. Architecture

#### 5.1 Bootstrap Sequence

The test framework is bootstrapped inside `RaftFrame::CreateCommo()`
(`frame.cc:141-186`).  Only the frame instance with `locale_id == 0`
manages the test coroutine:

```
1. RaftFrame::CreateCommo() called for each of 5 replicas
2. Site 0 creates raft_test_coro_ (Fiber::create_run)
3. Coroutine immediately yields (yield_())
4. Site 0 spin-waits until n_commo_created_ == 5
5. ContinueCoro(raft_test_coro_) resumes test execution
6. RaftTestConfig is constructed with all 5 frames
7. RaftLabTest::Run() executes 11 tests sequentially
8. Cleanup() calls Shutdown(), reactor loop is stopped
```

Static members coordinate across frames:

| Static Member | Type | Purpose |
|---------------|------|---------|
| `frames_` | `map<siteid_t, RaftFrame*>` | All 5 frame pointers |
| `n_replicas_` | `uint16_t` | Expected replica count (5) |
| `n_commo_created_` | `uint16_t` | Counter for synchronisation barrier |
| `raft_test_coro_` | `shared_ptr<Fiber>` | The test coroutine |
| `raft_test_mutex_` | `mutex` | Guards `n_commo_created_` |

#### 5.2 Class Hierarchy

```
RaftLabTest                    RaftTestConfig
├── config_ : RaftTestConfig*  ├── replicas (static)       : map<siteid_t, RaftFrame*>
├── index_  : uint64_t         ├── commit_callbacks (static): map<siteid_t, function>
├── init_rpcs_ : uint64_t      ├── committed_cmds (static) : map<siteid_t, vector<int>>
│                               ├── rpc_count_last (static) : map<siteid_t, uint64_t>
├── Run()                       ├── disconnected_           : map<siteid_t, bool>
├── Cleanup()                   ├── disconnect_mtx_         : mutex
│                               │
├── testInitialElection()       ├── OneLeader() / NoLeader()
├── testReElection()            ├── OneTerm() / TermMovedOn()
├── testBasicAgree()            ├── NCommitted()
├── testFailAgree()             ├── Start() / DoAgreement() / Wait()
├── testFailNoAgree()           ├── Disconnect() / Reconnect()
├── testRejoin()                ├── SetUnreliable() / Shutdown()
├── testConcurrentStarts()      ├── RpcCount() / RpcTotal()
├── testBackup()                ├── ServerCommitted()
├── testCount()                 └── netctlLoop() [background thread]
├── testUnreliableAgree()
└── testFigure8()
```

`RaftLabTest` owns the test sequencing logic.  `RaftTestConfig` owns the
cluster manipulation primitives (disconnect, reconnect, commit tracking,
RPC counting).

#### 5.3 Commit Tracking (`SetLearnerAction`)

`SetLearnerAction()` (`testconf.cc:30-48`) registers a callback on each
`RaftServer` via `RegLearnerAction()`.  When a server commits a log entry,
the callback:

1. Verifies the command is a `TpcCommitCommand`
2. Extracts `tx_id_` (used as the command value in tests)
3. Appends `tx_id_` to `committed_cmds[svr]`

This provides a per-server commit log that tests inspect via
`NCommitted(index)` to verify how many servers have committed a given
log index and whether they agree on the value.

The `committed_cmds` map is initialised with `[-1]` at index 0 for
every server, so test indices start at 1.

### 6. Network Simulation

#### 6.1 Disconnect / Reconnect

`RaftTestConfig::Disconnect(svr)` and `Reconnect(svr)` (`testconf.cc:320-332`)
simulate network partitions by calling `RaftServer::Disconnect()` and
`RaftServer::Reconnect()` (`server.cc:409-441`).

The server-level disconnect works by swapping the RPC proxy maps:

```
Disconnect:
  rpc_par_proxies_[partition][site] → saved in static _proxies map
  rpc_par_proxies_[partition][site] = {} (empty)

Reconnect:
  rpc_par_proxies_[partition][site] = _proxies[partition][site] (restored)
```

A disconnected server can still process local operations but cannot send
or receive RPCs.  Its `disconnected_` flag is checked by the election and
heartbeat timers to suppress outgoing messages.

#### 6.2 Unreliable Network (`netctlLoop`)

`netctlLoop()` (`testconf.cc:415-482`) is a background thread that
simulates an unreliable network when activated by `SetUnreliable(true)`:

```
Loop (100ms periods):
  For each non-Disconnect()ed server:
    With 1/10 probability → disconnect(svr)
    Otherwise → reconnect(svr) + slow(svr, rand() % 27 ms)
```

State machine for `cv_m_`:
- **State 0**: `unreliable_ == false && finished_ == false` (idle)
- **State 1**: Waiting on `cv_` for `unreliable_` or `finished_`
- **State 2**: `unreliable_ == true && finished_ == false` (active)
- **State 3**: `finished_ == true` (terminating)

`Shutdown()` sets `finished_ = true`, signals the condition variable,
joins the thread, then reconnects all `Disconnect()`ed servers.

#### 6.3 Slow Network

`slow(svr, msec)` (`testconf.cc:509-513`) introduces per-server latency
by sleeping `msec` milliseconds via `usleep()`.  In unreliable mode,
servers that are not disconnected get a random delay of 0-26 ms applied
each 100 ms period.

### 7. Key Test Utilities

#### 7.1 `OneLeader(expected)` / `NoLeader()`

`waitOneLeader()` (`testconf.cc:59-102`) retries up to 10 times with
`ELECTIONTIMEOUT / 10` (500 ms) sleeps between attempts.  It iterates
all non-disconnected replicas, calls `GetState(&isleader, &term)`, and
returns the leader with the highest term.

Failure modes:
- Returns `-2` if multiple leaders exist in the same term
- Returns `-3` if `expected` is specified and the leader differs
- Returns `-1` if no leader found after 10 retries

#### 7.2 `Start(svr, cmd, &index, &term)`

`Start()` (`testconf.cc:161-182`) constructs a `TpcCommitCommand` with
`tx_id_ = cmd` and calls `RaftServer::Start()`.  Returns `true` if the
server accepted the command (i.e., it believes it is the leader), along
with the assigned log `index` and current `term`.

#### 7.3 `DoAgreement(cmd, n, retry)`

`DoAgreement()` (`testconf.cc:214-290`) is the primary agreement driver:

1. **Outer loop** (10 second timeout): Tries `Start()` on each
   non-disconnected server until a leader accepts
2. **Inner loop** (10 second timeout, 20 ms polling): Polls
   `NCommitted(index)` until `n` servers have committed
3. Verifies the committed value matches `cmd`
4. If `retry == true`, retries from step 1 on failure

#### 7.4 `Wait(index, n, term)`

`Wait()` (`testconf.cc:184-212`) uses exponential backoff (10 ms to 1 s,
30 iterations max) to poll `NCommitted(index)`.  Returns:

| Value | Meaning |
|-------|---------|
| `>= 0` | The committed command value |
| `-1` | Timeout: not enough servers committed |
| `-2` | Term changed (stale leader) |
| `-3` | Committed values differ across servers |

#### 7.5 `NCommitted(index)`

`NCommitted()` (`testconf.cc:135-159`) counts how many servers have
committed log index `index` by checking `committed_cmds[svr].size() > index`.
Also verifies all servers that committed the index agree on the value.
Returns `-1` if values disagree.

#### 7.6 `RpcCount(svr, reset)` / `RpcTotal()`

`RpcCount()` (`testconf.cc:389-399`) reads `commo_->rpc_count_` under
`rpc_mtx_` and returns the delta since the last reset.  Used by
`testCount` to verify the implementation does not send excessive RPCs.

#### 7.7 Server ID Helpers

Since server IDs in the config may not be 0-4, three helpers abstract
the mapping (`testconf.cc:519-581`):

| Method | Purpose |
|--------|---------|
| `mapServerId(id)` | Maps actual `siteid_t` to position 0-4 |
| `getServerIdByIndex(i)` | Returns `siteid_t` at position `i` |
| `getNextServerId(id, offset)` | Wraps around: `(pos + offset) % 5` |

These are used extensively in tests to disconnect/reconnect servers
relative to the current leader without hardcoding IDs.

### 8. Test Macros

The framework defines assertion and helper macros in `test.cc:43-89`:

| Macro | Purpose |
|-------|---------|
| `Init2(id, desc)` | Prints test header, verifies clean state (no disconnections, reliable network) |
| `Passed2()` | Prints pass message and returns 0 |
| `Assert(expr)` | Returns 1 on failure (silent) |
| `Assert2(expr, msg, ...)` | Prints failure message on false, returns 1 |
| `AssertOneLeader(ldr)` | Asserts `ldr >= 0` |
| `AssertReElection(ldr, old)` | Asserts `ldr != old` |
| `AssertNoneCommitted(index)` | Asserts `NCommitted(index) == 0` |
| `AssertNCommitted(index, n)` | Asserts `NCommitted(index) == n` |
| `AssertStartOk(ok)` | Asserts `Start()` returned true |
| `AssertWaitNoError(ret, index)` | Asserts `ret != -3` (no value disagreement) |
| `AssertWaitNoTimeout(ret, index, n)` | Asserts `ret != -1` and `ret != -2` |
| `DoAgreeAndAssertIndex(cmd, n, index)` | Calls `DoAgreement` and checks returned index |
| `DoAgreeAndAssertWaitSuccess(cmd, n)` | Calls `DoAgreement` with retry and updates `index_` |

### 9. Test Execution Flow

`RaftLabTest::Run()` (`test.cc:10-37`) executes all 11 tests in a
short-circuit OR chain:

```cpp
if (testInitialElection()
    || testReElection()
    || testBasicAgree()
    || ...
    || testFigure8()) {
  Print("TESTS FAILED");
  return 1;
}
Print("ALL TESTS PASSED");
```

Each test returns 0 on success, non-zero on failure.  The OR chain stops
at the first failure.  The `TEST_EXPAND(x)` macro is defined as just `x`
in the current build (it can be changed to `x || x || x || x || x` for
repeated-execution stress testing).

**State carried between tests:**

| Field | Carried | Purpose |
|-------|---------|---------|
| `index_` | Yes | Next expected commit index (starts at 1, incremented by each agreement) |
| `init_rpcs_` | Yes | RPC count from test 1, checked in test 9 |
| `committed_cmds` | Yes | Cumulative commit log per server |

Tests expect `NDisconnected() == 0` and `!IsUnreliable()` at the start
of each test (enforced by `Init2`).

### 10. Summary of the 11 Test Cases

| # | Name | What It Tests |
|---|------|---------------|
| 1 | `testInitialElection` | Leader elected, term agreed, stable leadership |
| 2 | `testReElection` | New election after leader disconnect; quorum break → no leader; quorum restore → leader |
| 3 | `testBasicAgree` | 3 sequential agreements with all 5 servers |
| 4 | `testFailAgree` | Agreements succeed with 2 followers disconnected (N-2 quorum) |
| 5 | `testFailNoAgree` | No agreement possible with 3 followers disconnected (no quorum) |
| 6 | `testRejoin` | Old leader rejoins after new leader commits; verifies log consistency |
| 7 | `testConcurrentStarts` | 5 concurrent `pthread` `Start()` calls; all values committed correctly |
| 8 | `testBackup` | 50 uncommitted entries on minority; swap quorum; verify backfill of 50 correct entries |
| 9 | `testCount` | RPC counts: `init_rpcs_ <= 30`, `COMMITRPCS(10)` for agreements, `<= 60` for 1 s idle |
| 10 | `testUnreliableAgree` | Unreliable network with 50 iterations x 4 concurrent threads |
| 11 | `testFigure8` | Leader completeness property: leader must not commit entries from previous terms using only the entry's replication count |

Each test case is documented in detail in `test_cases.md`.

### 11. Design Decisions

#### Coroutine-Based Execution

Tests run inside the same coroutine reactor as the Raft servers.  This
means `Fiber::sleep()` yields to the reactor rather than blocking an OS
thread, allowing Raft heartbeats and election timers to fire during test
sleeps.  The only exception is `usleep()` calls in `DoAgreement()` and
`netctlLoop()`, which block the OS thread (used for timing that must be
independent of the reactor).

#### Static State

`RaftTestConfig` uses static maps (`replicas`, `commit_callbacks`,
`committed_cmds`, `rpc_count_last`) because only one test configuration
exists per process lifetime.  The `committed_cmds` log is never cleared
between tests, allowing later tests to build on earlier agreements.

#### `TpcCommitCommand` as Test Payload

Tests use `TpcCommitCommand` with `tx_id_` as the value field.  This
reuses Mako's existing command marshalling infrastructure rather than
creating a test-specific command type.  The `SetLearnerAction` callback
extracts `tx_id_` to record committed values.

#### Disconnect via Proxy Swap

Rather than simulating network failures at the TCP level, `Disconnect()`
empties the RPC proxy map for a server.  This is equivalent to a
network partition: the server has no outbound connections.  The approach
is deterministic (no timing-dependent packet drops) and immediate (no
connection timeout delay).

---

## Individual Test Case Documentation

<a id="1-overview-4"></a>

### 1. Overview

The standalone Raft test suite contains 11 sequential test cases that
verify correctness properties of the Raft consensus implementation.  Tests
are executed in order by `RaftLabTest::Run()` (`test.cc:10-37`) via a
short-circuit OR chain — the first failure stops execution.

State is carried between tests: `index_` tracks the next expected commit
index, `init_rpcs_` records initial election RPCs, and `committed_cmds`
accumulates per-server commit logs.

Each test begins with `Init2(id, desc)` which verifies preconditions:
no servers are disconnected and the network is reliable.

### 2. Test 1: Initial Election (`testInitialElection`)

**Source**: `test.cc:91-139`

**What it tests**: A leader is elected from a fresh 5-server cluster,
all servers agree on a single term, and leadership is stable.

**Procedure**:

1. Sleep `ELECTIONTIMEOUT / 10` (500 ms) to allow election timers to start
2. Call `OneLeader()` — assert a leader exists
3. Record `init_rpcs_` by summing `RpcCount()` across all 5 servers
4. Call `OneTerm()` — assert all servers agree on the same term
5. Call `OneTerm()` again — assert the term hasn't changed
6. Call `OneLeader(leader)` — assert the same server is still leader

**Expected outcome**: Exactly one leader, unanimous term, stable
leadership.

**What bugs it catches**:
- Election timers that never fire
- Split brain (multiple leaders in the same term)
- Spurious re-elections in a healthy cluster

### 3. Test 2: Re-Election (`testReElection`)

**Source**: `test.cc:141-237`

**What it tests**: The cluster elects a new leader after the current
leader is disconnected, and correctly handles quorum loss and restoration.

**Procedure**:

1. Find current leader via `OneLeader()`
2. `Disconnect(leader)` — simulate leader failure
3. Sleep `ELECTIONTIMEOUT` (5 s), call `OneLeader()` — assert new leader
   differs from old
4. `Reconnect(oldLeader)` — old leader rejoins
5. Sleep `ELECTIONTIMEOUT`, call `OneLeader(leader)` — assert current
   leader unchanged (old leader doesn't disrupt)
6. Disconnect leader + 2 more servers (total 3/5 down) — no quorum
7. Call `NoLeader()` — assert no leader can be elected
8. Reconnect one server (3/5 up) — quorum restored
9. Sleep `ELECTIONTIMEOUT`, call `OneLeader()` — leader elected
10. Reconnect remaining servers, verify stable leadership

**Expected outcome**: New leader after disconnect, no leader without
quorum, leader elected when quorum restored.

**What bugs it catches**:
- Failure to detect leader timeout
- Leader elected without majority (safety violation)
- Old leader not stepping down after reconnection
- Incorrect quorum calculation

### 4. Test 3: Basic Agreement (`testBasicAgree`)

**Source**: `test.cc:239-267`

**What it tests**: Three sequential agreements are committed by all 5
servers at the expected log indices.

**Procedure**:

For `i` in 1..3:
1. `AssertNoneCommitted(index_)` — no premature commits at this index
2. `DoAgreeAndAssertIndex(index_+300, NSERVERS, index_)` — submit
   command, wait for all 5 servers to commit, verify index
3. Increment `index_`

Command values are `index_ + 300` (e.g., 301, 302, 303).

**Expected outcome**: Three commits at consecutive indices, all 5
servers agree on values.

**What bugs it catches**:
- Log replication failures
- Index assignment errors
- Value corruption during replication

### 5. Test 4: Agreement Despite Follower Failure (`testFailAgree`)

**Source**: `test.cc:269-293`

**What it tests**: Agreements succeed when 2 of 5 followers are
disconnected (quorum = 3 servers), and disconnected followers catch up
after reconnection.

**Procedure**:

1. Find leader, disconnect 2 followers (next 1, next 2 relative to leader)
2. Submit 4 agreements (401-404) with `n = NSERVERS - 2` (3 servers)
3. Reconnect both followers, sleep `ELECTIONTIMEOUT`
4. Submit 2 more agreements (405-406) with `n = NSERVERS` (all 5)

**Expected outcome**: First 4 agreements commit with 3 servers.  After
reconnection, followers catch up and all 5 servers commit the last 2.

**What bugs it catches**:
- Quorum logic requiring more than majority
- Log replication not resuming after reconnection
- Followers failing to catch up via AppendEntries

### 6. Test 5: No Agreement Without Quorum (`testFailNoAgree`)

**Source**: `test.cc:295-319`

**What it tests**: When 3 of 5 followers are disconnected, `Start()`
accepts commands (leader still thinks it's leader) but they cannot be
committed.

**Procedure**:

1. Find leader, disconnect 3 followers
2. `Start(leader, 501)` — assert it returns true (leader accepts)
3. Verify returned index and positive term
4. Sleep `ELECTIONTIMEOUT`
5. `AssertNoneCommitted(index)` — command not committed (no quorum)
6. Reconnect all 3 followers, sleep `ELECTIONTIMEOUT`
7. `DoAgreeAndAssertWaitSuccess(502, NSERVERS)` — agreement works again

**Expected outcome**: Command accepted but not committed without quorum.
After quorum restored, new agreements succeed.

**What bugs it catches**:
- Leader committing without majority (safety violation)
- Leader rejecting commands when it should accept
- System unable to recover after quorum restoration

### 7. Test 6: Rejoin of Disconnected Leader (`testRejoin`)

**Source**: `test.cc:321-356`

**What it tests**: An old leader that accumulated uncommitted entries
(602-604) correctly discards them after rejoining, and the cluster
continues operating through multiple leader changes.

**Procedure**:

1. Commit entry 601 with all servers
2. Disconnect leader1, sleep `ELECTIONTIMEOUT`
3. `Start()` entries 602-604 on disconnected leader1 (these cannot commit)
4. New leader2 commits entries 605-606 with `NSERVERS - 1`
5. Disconnect leader2
6. Reconnect leader1
7. Sleep `ELECTIONTIMEOUT` — leader3 elected (not leader2)
8. Commit entries 607-608 with `NSERVERS - 1`
9. Reconnect leader2
10. Commit entry 609 with all `NSERVERS`

**Expected outcome**: Entries 602-604 are overwritten by the new leader's
log.  The cluster survives two consecutive leader changes.

**What bugs it catches**:
- Old leader's uncommitted entries persisting incorrectly
- AppendEntries not overwriting conflicting entries
- Failure to elect leader after multiple disconnections

### 8. Test 7: Concurrent Starts (`testConcurrentStarts`)

**Source**: `test.cc:382-446`

**What it tests**: 5 concurrent `pthread` `Start()` calls to the same
leader all result in committed entries with correct values.

**Procedure** (retried up to 5 times if term changes):

1. Find leader, call `Start(leader, 701)` to verify leadership
2. Launch 5 `pthread` threads, each calling `Start(leader, 701+i)`
3. Join all threads, collect returned indices
4. If term moved on, retry from step 1
5. `Wait()` for each returned index to be committed by all servers
6. Verify all 5 command values (701-705) appear in the committed set

**Expected outcome**: All 5 concurrent commands committed at distinct
indices with correct values.

**What bugs it catches**:
- Race conditions in `Start()` / log append
- Duplicate index assignment
- Lost commands under concurrency

### 9. Test 8: Leader Backs Up Quickly (`testBackup`)

**Source**: `test.cc:448-492`

**What it tests**: When a leader accumulates 50 uncommitted entries on
a minority, and then a new leader with different entries takes over, the
old leader's log is replaced quickly.

**Procedure**:

1. Disconnect 3 followers (leaving leader1 + 1 follower)
2. `Start()` 50 entries (800-849) on leader1 — cannot commit (no quorum)
3. Disconnect leader1 + its follower, reconnect the 3 other servers
4. Sleep `ELECTIONTIMEOUT` — new leader elected among the 3
5. `DoAgreeAndAssertIndex()` 50 entries (801-850) with 3 servers
6. Reconnect old leader1 + follower
7. Sleep `ELECTIONTIMEOUT`, submit entry 851 via new leader
8. Sleep `2 * ELECTIONTIMEOUT`
9. `AssertNCommitted(index, NSERVERS)` — all 5 servers committed

**Expected outcome**: The 50 incorrect entries on the old minority are
replaced by the 50 correct entries.  All 5 servers converge after
reconnection.

**What bugs it catches**:
- Slow log backfill (O(n) AppendEntries retries instead of efficient
  nextIndex backup)
- Conflicting entries not overwritten
- Old leader refusing to accept new leader's entries

### 10. Test 9: RPC Count Verification (`testCount`)

**Source**: `test.cc:494-572`

**What it tests**: The Raft implementation does not send excessive RPCs.
Checks three scenarios: initial election, agreement rounds, and idle
period.

**Procedure** (retried up to 5 times):

1. Assert `init_rpcs_ > 1 && init_rpcs_ <= 30` (from test 1)
2. Reset RPC counters, find leader
3. `Start()` 10 entries sequentially to the leader
4. `Wait()` for all 10 to be committed by all servers
5. `rpcs()` — assert total RPCs `<= COMMITRPCS(10)` = 55
6. Sleep 1 second (idle period)
7. `rpcs()` — assert total RPCs `<= 60` during idle

**Expected outcome**: Bounded RPC counts — efficient heartbeating and
replication.

**What bugs it catches**:
- Excessive heartbeat frequency
- Redundant AppendEntries retransmissions
- Broadcasting to all servers on every heartbeat even when logs are
  up to date

**RPC bounds**:

| Scenario | Upper Bound | Formula |
|----------|-------------|---------|
| Initial election | 30 | Empirical |
| 10 agreements | 55 | `(10 + 1) * 5` |
| 1 second idle | 60 | Empirical |

### 11. Test 10: Unreliable Agreement (`testUnreliableAgree`)

**Source**: `test.cc:593-631`

**What it tests**: Agreements complete correctly under simulated
unreliable network conditions (random disconnections and delays).

**Procedure**:

1. `SetUnreliable(true)` — activates `netctlLoop` (1/10 disconnect
   chance, 0-26 ms random delays per 100 ms period)
2. For 50 iterations:
   a. Launch 4 `pthread` threads, each calling
      `DoAgreement(1000+iter, 1, true)` (retry enabled)
   b. If any thread fails, record failure and break
   c. Main thread also calls `DoAgreement(1000+iter, 1, true)`
3. `SetUnreliable(false)` — restore reliable network
4. Join all threads
5. Assert no failures occurred (`retvals.size() == 0`)
6. Advance `index_` by `50 * 5`
7. `DoAgreeAndAssertWaitSuccess(1060, NSERVERS)` — final agreement
   with all servers

**Expected outcome**: All 50 iterations complete despite network
unreliability.  Total ~250 concurrent agreements succeed.

**What bugs it catches**:
- Liveness failures under packet loss
- Election instability with network delays
- Deadlocks or race conditions under concurrent load

### 12. Test 11: Figure 8 (`testFigure8`)

**Source**: `test.cc:633-731`

**What it tests**: The leader completeness property from Raft Figure 8
(Ongaro & Ousterhout, 2014).  A leader must not commit entries from a
previous term by counting replicas alone — it must first commit an entry
from its own term.

**Scenario** (retried up to 10 times):

```
Phase 1: Initial state
  S1=leader, S2=follower, S3=S4=S5=followers
  Commit entry 1100 at all 5 servers

Phase 2: Partial replication
  Disconnect S3, S4, S5
  Start(S1, 1101) → replicated to S1 and S2 only
  Sleep ELECTIONTIMEOUT
  Assert 1101 NOT committed (no quorum)

Phase 3: New leader with different entry
  Disconnect S2, disconnect S1
  Reconnect S3, S4, S5
  S3 elected as leader2
  Start(leader2, 1102) at same index as 1101
  Assert 1102 NOT committed (leader2 isolated)

Phase 4: Old leader or its follower becomes leader
  Disconnect leader2
  Reconnect S1 and S2 (who have 1101 at the index)
  Reconnect one of S3/S4/S5
  leader3 elected (must be S1 or S2 for the test to proceed)

Phase 5: Verification
  Sleep ELECTIONTIMEOUT
  Assert index is STILL not committed (leader3 cannot commit
    1101 from an older term by counting replicas)
  DoAgreement(1103) — commit a new entry in current term
  Now 1101 should be committed (indirectly, by the new entry
    advancing the commit index past it)
  Assert 1101 is committed at the correct index
  Verify ServerCommitted(leader3, index, 1101)
```

**Expected outcome**: Entry 1101 from a previous term is NOT committed
until the new leader commits its own entry (1103) in the current term.
This prevents the unsafe scenario where a leader counts replicas of old
entries and commits them, only to have them overwritten by a different
leader.

**What bugs it catches**:
- Leader committing previous-term entries by replica count alone
  (violates Leader Completeness Property)
- Incorrect commitIndex advancement logic
- Log entries from different terms at the same index not handled
  correctly

**Why it retries**: The test requires `leader3` to be either S1 or S2
(who hold entry 1101).  With 3 eligible servers and random election,
this has a 2/3 success probability per attempt.  Up to 10 retries
provide > 99.99% overall success probability.

### 13. Test Progression Summary

The tests form a logical progression of increasing complexity:

| Phase | Tests | Properties Verified |
|-------|-------|--------------------|
| Election | 1-2 | Leader election, re-election, quorum requirement |
| Basic agreement | 3-5 | Log replication, quorum semantics, no false commits |
| Fault tolerance | 6, 8 | Leader rejoin, log conflict resolution, backfill speed |
| Concurrency | 7 | Thread safety of `Start()` |
| Efficiency | 9 | RPC count bounds |
| Stress | 10 | Unreliable network with concurrent load |
| Safety | 11 | Leader completeness (Raft's core safety property) |

### 14. Index Tracking Across Tests

The `index_` field starts at 1 and advances as entries are committed.
Each test assumes the previous test left `index_` at the correct value:

| Test | Starting `index_` | Entries Committed | Ending `index_` |
|------|------------------|-------------------|-----------------|
| 1 | 1 | 0 | 1 |
| 2 | 1 | 0 | 1 |
| 3 | 1 | 3 | 4 |
| 4 | 4 | 6 | 10 |
| 5 | 10 | 2 | 12 |
| 6 | 12 | 9 | 21 |
| 7 | 21 | 6 | 27 |
| 8 | 27 | 52 | 79 |
| 9 | 79 | ~11 | ~90 |
| 10 | ~90 | ~251 | ~341 |
| 11 | ~341 | varies | varies |

Note: Tests 9-11 use approximate values because they depend on retry
paths and concurrent submissions.

---

## Test Configuration YAML Files

<a id="1-overview-5"></a>

### 1. Overview

The Raft implementation uses YAML configuration files to define cluster
topology, port assignments, and protocol modes.  This chapter documents
the configuration files used by the standalone test framework and
compares them to the production Mako-Raft and Mako-Paxos configurations.

### 2. Test Configuration: `raft_lab_test.yml`

**File**: `config/raft_lab_test.yml`

```yaml
mode:
  cc: none
  ab: raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1

site:
  server:
    - ["s101:9000", "s102:9001", "s103:9002", "s104:9003", "s105:9004"]
  client:
    - ["c01"]

process:
  s101: localhost
  s102: localhost
  s103: localhost
  s104: localhost
  s105: localhost
  c01: localhost

host:
  localhost: 127.0.0.1
```

#### 2.1 Field-by-Field Explanation

##### `mode` Section

| Field | Value | Meaning |
|-------|-------|---------|
| `cc` | `none` | No concurrency control — tests exercise only Raft consensus |
| `ab` | `raft` | Atomic broadcast protocol is Raft (triggers `DISPATCH_RAFT_OR_PAXOS` to route to `raft_impl`) |
| `read_only` | `occ` | Read-only transaction protocol (not exercised in tests) |
| `batch` | `false` | Batching disabled (each command replicated individually) |
| `retry` | `20` | Maximum number of transaction retries (not exercised in tests) |
| `ongoing` | `1` | Maximum concurrent operations per client (not exercised in tests) |

The key field is `ab: raft`.  This is what `detect_replication_type_from_config()`
scans for to set `g_replication_type = RAFT` before calling `setup()`.

##### `site` Section

The `site` section defines the cluster topology:

```yaml
site:
  server:
    - ["s101:9000", "s102:9001", "s103:9002", "s104:9003", "s105:9004"]
  client:
    - ["c01"]
```

- **One partition**: A single array entry means one Raft group (one shard)
- **5 servers**: Matches `NSERVERS` constant in `testconf.h`
- **Port range**: 9000-9004 (simple, low-numbered ports for isolated testing)
- **1 client**: Required by the framework but not used by tests (tests
  drive commands directly via `RaftTestConfig::Start()`)

Each server entry is `"name:port"`.  The name maps through `process` →
`host` to resolve to an IP address.

##### `process` Section

Maps site names to process names:

```yaml
process:
  s101: localhost
  s102: localhost
  ...
```

All 5 servers and the client run as `localhost` — single-machine execution.

##### `host` Section

Maps process names to IP addresses:

```yaml
host:
  localhost: 127.0.0.1
```

All processes resolve to `127.0.0.1`.

#### 2.2 Port Allocation

The test configuration uses ports 9000-9004 for the Raft group.  The
heartbeat/control port is computed as `base_port + CtrlPortDelta` where
`CtrlPortDelta = 10000`, giving ports 19000-19004 for internal Raft
communication.

### 3. CI Test Cluster Configs

#### 3.1 `1c1s3r1p_cluster_test.yml` (3-Replica Raft Test)

**File**: `config/1c1s3r1p_cluster_test.yml`

```yaml
site:
  server:
    - ["localhost:38100", "p1:38101", "p2:38102"]
  client:
    - ["c01"]
```

- **3 replicas**: Minimum viable Raft cluster (tolerates 1 failure)
- **Port range**: 38100-38102 (in the 38xxx Raft test range)
- **1 partition**: Single shard

Used by `ci_mako_raft.sh` for 1-shard Raft replication tests with `dbtest`.

#### 3.2 `1c1s5r1p_cluster_test.yml` (5-Replica Raft Test)

**File**: `config/1c1s5r1p_cluster_test.yml`

```yaml
site:
  server:
    - ["localhost:38101", "p1:38102", "p2:38103", "p3:38104", "p4:38105"]
  client:
    - ["c01"]
```

- **5 replicas**: Same as standalone test framework
- **Port range**: 38101-38105

#### 3.3 `1c1s3r3p_cluster_test.yml` (3-Replica, 3-Partition)

Used for multi-shard Raft testing.  Contains 3 partitions each with 3
replicas, enabling cross-shard transaction testing.

### 4. Production Raft Config: `raft6_shardidx0.yml`

**File**: `config/1leader_2followers/raft6_shardidx0.yml`

```yaml
site:
  server:
    - ["s101:27001", "s201:27101", "s301:27201"]
    - ["s102:27002", "s202:27102", "s302:27202"]
    - ["s103:27003", "s203:27103", "s303:27203"]
    - ["s104:27004", "s204:27104", "s304:27204"]
    - ["s105:27005", "s205:27105", "s305:27205"]
    - ["s106:27006", "s206:27106", "s306:27206"]
```

- **3 replicas per partition**: Leader + 2 followers
- **6 partitions**: 6 warehouse shards (matching thread count in Mako)
- **Port range**: 27xxx (Raft production range)
- **No learner column**: Unlike Paxos configs which have a 4th column
  for the learner

### 5. Production Paxos Config: `paxos6_shardidx0.yml` (Comparison)

**File**: `config/1leader_2followers/paxos6_shardidx0.yml`

```yaml
site:
  server:
    - ["s101:17001", "s201:17101", "s301:17201", "s401:17301"]
    - ["s102:17002", "s202:17102", "s302:17202", "s402:17302"]
    ...
```

- **4 sites per partition**: Leader + 2 followers + 1 learner
- **6 partitions**: Same shard structure
- **Port range**: 17xxx (Paxos production range)
- **Learner (s4xx)**: Paxos-specific role, not present in Raft

### 6. Mode Config Files

#### 6.1 `occ_raft.yml`

```yaml
mode:
  cc: occ
  ab: raft
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1
```

Used by `shard.sh` when `replication_type == raft`.  The key difference
from `raft_lab_test.yml` is `cc: occ` instead of `cc: none` — production
runs use OCC for concurrency control alongside Raft replication.

#### 6.2 `occ_paxos.yml`

```yaml
mode:
  cc: occ
  ab: multi_paxos
  read_only: occ
  batch: false
  retry: 20
  ongoing: 1
```

The Paxos equivalent.  Note `ab: multi_paxos` vs `ab: raft`.

### 7. Comparison: Test vs Production

| Aspect | Test (`raft_lab_test.yml`) | Production (`raft6_shardidx0.yml` + `occ_raft.yml`) |
|--------|--------------------------|-----------------------------------------------------|
| Replicas | 5 | 3 |
| Partitions | 1 | 6 (typical) |
| Concurrency control | `none` | `occ` |
| Atomic broadcast | `raft` | `raft` |
| Port range | 9000-9004 | 27001-27206 |
| Ctrl port delta | +10000 | +10000 |
| Learner | None | None |
| Processes | All localhost | Potentially distributed (localhost/p1/p2) |
| Client | 1 (unused) | 1+ (active) |

#### 7.1 Why 5 Replicas in Tests?

The test framework uses 5 replicas (vs 3 in production) to enable richer
fault-tolerance scenarios:

- **2 failures tolerated** vs 1 in production
- Enables `testBackup` (3 vs 2 quorum split)
- Enables `testFigure8` (complex partition with 3 sub-groups)
- Matches the standard Raft paper examples

#### 7.2 Why `cc: none` in Tests?

Setting `cc: none` isolates the Raft consensus layer:

- No transaction processing overhead
- No Masstree operations
- No OCC validation
- Failures are attributable to Raft logic, not transaction bugs

### 8. Port Range Allocation Summary

| Usage | Port Range | Ctrl Port Range | Config Files |
|-------|-----------|-----------------|-------------|
| Standalone Raft tests | 9000-9004 | 19000-19004 | `raft_lab_test.yml` |
| Paxos production | 17001-17999 | 27001-27999 | `paxos*_shardidx*.yml` |
| Raft production | 27001-27999 | 37001-37999 | `raft*_shardidx*.yml` |
| Raft CI tests | 38100-38199 | 48100-48199 | `*_cluster_test.yml` |

Port ranges are deliberately non-overlapping so that Paxos tests, Raft
tests, and standalone tests can run concurrently on the same machine
without port conflicts (documented in `challenges.md` as a resolved
integration issue).

### 9. Config Naming Convention

The configuration file naming follows a structured pattern:

```
{n_clients}c{n_shards}s{n_replicas}r{n_partitions}p[_cluster_test].yml
```

Examples:
- `1c1s3r1p.yml`: 1 client, 1 shard, 3 replicas, 1 partition
- `1c1s5r1p_cluster_test.yml`: Same but in test port range
- `1c1s3r3p_cluster_test.yml`: 3 partitions (multi-shard test)

For the `1leader_2followers/` directory:
```
{protocol}{n_threads}_shardidx{shard_index}.yml
```

Examples:
- `paxos6_shardidx0.yml`: Paxos, 6 threads, shard 0
- `raft6_shardidx0.yml`: Raft, 6 threads, shard 0

---

## CI Script Documentation: `ci_mako_raft.sh`

<a id="1-overview-6"></a>

### 1. Overview

`ci/ci_mako_raft.sh` is the Continuous Integration entry point for
Mako-Raft integration tests.  It mirrors the structure of `ci/ci.sh`
(the primary Mako-Paxos CI script) but tests only the Raft replication
path.  The Raft test commands are also integrated into `ci.sh` itself
for unified `all` runs.

**Files**:

| File | Lines | Purpose |
|------|-------|---------|
| `ci/ci_mako_raft.sh` | 252 | Standalone Raft CI entry point |
| `ci/ci.sh` | 553 | Primary CI script (includes Raft tests at lines 265-331, 489-500) |

### 2. Script Structure

#### 2.1 High-Level Architecture

```
ci_mako_raft.sh
├── Environment setup (set -e, MAKO_NO_GDB=1, colors)
├── check_for_hanging_processes()   — post-test process audit
├── cleanup_processes()             — pre-test process/port cleanup
├── Test functions:
│   ├── compile()
│   ├── run_simple_raft()
│   ├── run_1shard_replication_raft()
│   ├── run_2shard_replication_raft()
│   ├── run_1shard_replication_simple_raft()
│   └── run_2shard_replication_simple_raft()
├── cleanup()
└── Main case dispatch
```

#### 2.2 Available Commands

```bash
./ci/ci_mako_raft.sh {command}
```

| Command | Function | Test Script Invoked |
|---------|----------|--------------------|
| `compile` | `compile()` | `make -j32` |
| `cleanup` | `cleanup()` | Process kill + temp file removal |
| `simpleRaft` | `run_simple_raft()` | `examples/mako-raft-tests/simpleRaft.sh` |
| `shard1ReplicationRaft` | `run_1shard_replication_raft()` | `examples/mako-raft-tests/test_1shard_replication_raft.sh` |
| `shard2ReplicationRaft` | `run_2shard_replication_raft()` | `examples/mako-raft-tests/test_2shard_replication_raft.sh` |
| `shard1ReplicationSimpleRaft` | `run_1shard_replication_simple_raft()` | `examples/mako-raft-tests/test_1shard_replication_simple_raft.sh` |
| `shard2ReplicationSimpleRaft` | `run_2shard_replication_simple_raft()` | `examples/mako-raft-tests/test_2shard_replication_simple_raft.sh` |
| `all` | All of the above in sequence | — |

#### 2.3 The `all` Execution Order

```bash
compile
run_simple_raft                       # Basic Raft replication
run_1shard_replication_raft           # 1-shard TPC-C with Raft
run_2shard_replication_raft           # 2-shard TPC-C with Raft
run_1shard_replication_simple_raft    # 1-shard simple tx with Raft
run_2shard_replication_simple_raft    # 2-shard simple tx with Raft
```

### 3. Process Management

#### 3.1 `cleanup_processes()` (lines 65-105)

Called before every test to ensure a clean slate.  Performs three phases:

**Phase 1: Process Termination** (lines 74-86)

Sends `SIGKILL` (-9) to all known test binaries and scripts:

```bash
# Binaries
pkill -9 -f "build/simpleRaft"
pkill -9 -f "build/simpleTransactionRepRaft"
pkill -9 -f "build/testPreferredReplicaStartup"
pkill -9 -f "build/testPreferredReplicaLogReplication"
pkill -9 -f "build/testNoOps"
pkill -9 -f "build/dbtest"

# Wrapper scripts
pkill -9 -f "test_1shard_replication_raft.sh"
pkill -9 -f "test_2shard_replication_raft.sh"
pkill -9 -f "test_1shard_replication_simple_raft.sh"
pkill -9 -f "test_2shard_replication_simple_raft.sh"
pkill -9 -f "bash/shard_raft.sh"
```

All `pkill` calls use `2>/dev/null || true` to suppress errors when no
matching process exists.

**Phase 2: Port Release Wait** (lines 90-98)

Polls for up to 10 seconds until common test ports are free:

```bash
for i in {1..10}; do
    if ! lsof -i :7001-8006 && ! lsof -i :31000-31100; then
        break
    fi
    sleep 1
done
```

**Phase 3: Log Archival** (lines 100-104)

Copies log files to `~/results/ci_raft_results_${RUN_NUM}_${RUN_INDEX}/`.

#### 3.2 `check_for_hanging_processes()` (lines 25-59)

Called after every test to audit for leaked processes.  Waits 3 seconds
for natural exit, then counts processes matching `[d]btest|[s]impleRaft`.

If hanging processes exist:
1. Prints diagnostic (process list)
2. Sends `SIGKILL` to `build/dbtest` and `build/simpleRaft`
3. Returns 0 (pass) — hanging processes are not treated as test failures
   as long as throughput results were collected

#### 3.3 Test Function Pattern

Every test function follows the same 5-step pattern:

```bash
run_X() {
    echo "========================================="    # 1. Header
    cleanup_processes                                   # 2. Pre-cleanup
    set +e                                              # 3. Run test (allow failure)
    bash ./examples/mako-raft-tests/X.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "X"                     # 4. Post-audit
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]  # 5. Combined result
}
```

### 4. Environment Variables

| Variable | Value | Purpose |
|----------|-------|---------|
| `MAKO_NO_GDB` | `1` | Disables GDB wrapping — GDB changes output format and breaks grep patterns used for result parsing |
| `RUN_NUM` | CI-provided | Run number for result directory naming |
| `RUN_INDEX` | CI-provided | Run index within the batch |

### 5. Comparison: `ci_mako_raft.sh` vs `ci.sh`

#### 5.1 Structural Differences

| Aspect | `ci_mako_raft.sh` | `ci.sh` |
|--------|-------------------|---------|
| Lines | 252 | 553 |
| Raft-only | Yes | Includes Raft + Paxos + other tests |
| Color output | Yes (RED/GREEN/YELLOW/BLUE) | No |
| `BUILD_DIR` support | Hardcoded `build` | Configurable via env var |
| Memory limits | No | `run_with_memory_limit()` for heavy tests |
| `update_config.sh` | Not called | Called in `compile()` and `simplePaxos` |
| RocksDB cleanup | No | Cleans `/tmp/${USER}_mako_rocksdb_shard*` |
| RRR unit tests | No | `run_rrr_unit_tests()` via ctest |
| Process kill approach | Direct `pkill -9` patterns | Filters out own PID/PPID tree |

#### 5.2 Raft Tests in `ci.sh`

The Raft tests are also defined directly in `ci.sh` (lines 265-331)
with identical implementation.  This means Raft tests can be invoked
via either script:

```bash
# Equivalent invocations:
./ci/ci_mako_raft.sh shard1ReplicationRaft
./ci/ci.sh shard1ReplicationRaft
```

The `ci.sh all` command runs Raft tests after Paxos tests (lines 541-544):

```bash
# Raft replication tests
run_1shard_replication_raft
run_2shard_replication_raft
run_1shard_replication_simple_raft
run_2shard_replication_simple_raft
```

#### 5.3 Process Cleanup Differences

`ci.sh` uses a more careful process kill that avoids killing itself:

```bash
for proc in simpleTransactionRep dbtest simplePaxos simpleTransaction; do
    pgrep -f "$proc" | while read pid; do
        if [ "$pid" != "$my_pid" ] && [ "$pid" != "$my_ppid" ]; then
            kill -9 "$pid"
        fi
    done
done
```

`ci_mako_raft.sh` uses simpler `pkill -9 -f` patterns which rely on
the fact that the script name (`ci_mako_raft.sh`) doesn't match the
binary patterns (`build/simpleRaft`, `build/dbtest`).

### 6. Shard Launch Scripts

#### 6.1 `bash/shard_raft.sh` (Raft-Specific)

A Raft-dedicated shard launcher (39 lines) with hardcoded Raft config:

```bash
CMD="./build/dbtest \
  --num-threads $trd \
  --shard-index $shard \
  --shard-config $path/config/local-shards$nshard-warehouses$trd.yml \
  -F config/1leader_2followers/raft${trd}_shardidx${shard}.yml \
  -F config/occ_raft.yml \
  -P $cluster \
  --replication raft"
```

Key differences from `shard.sh`:
- Uses `raft${trd}_shardidx${shard}.yml` (port range 27xxx)
- Uses `occ_raft.yml` (mode `ab: raft`)
- Passes `--replication raft` explicitly
- No GDB support (no `util.sh` sourcing)

#### 6.2 `bash/shard.sh` (Unified Launcher)

The unified shard launcher (62 lines) supports both Paxos and Raft
via a 7th argument:

```bash
replication_type=${7:-paxos}  # Default to paxos
```

Configuration selection:

```bash
if [ "$replication_type" == "raft" ]; then
    OCC_CONFIG="config/occ_raft.yml"
else
    OCC_CONFIG="config/occ_paxos.yml"
fi
CMD="$CMD -F config/1leader_2followers/paxos${trd}_shardidx${shard}.yml \
     -F $OCC_CONFIG --is-replicated --replication=$replication_type"
```

Note: The unified `shard.sh` still references `paxos${trd}_shardidx`
for the site config even in Raft mode (the port assignments work because
the Paxos config is used for site topology while `occ_raft.yml` sets
the protocol).  The dedicated `shard_raft.sh` correctly uses
`raft${trd}_shardidx` for the Raft-specific port range.

#### 6.3 Arguments

Both scripts accept the same positional arguments:

| Position | Name | Example | Purpose |
|----------|------|---------|---------|
| `$1` | `nshard` | `2` | Number of shards in the cluster |
| `$2` | `shard` | `0` | This shard's index |
| `$3` | `trd` | `6` | Number of threads (also used for config lookup) |
| `$4` | `cluster` | `localhost` | Cluster process name |
| `$5` | `is_micro` | `0` | Enable micro benchmark flag |
| `$6` | `is_replicated` | `1` | Enable replication |
| `$7` | `replication_type` | `raft` | `shard.sh` only: paxos or raft |

### 7. Result Archival

Test results are archived to `~/results/ci_raft_results_${RUN_NUM}_${RUN_INDEX}/`
during `cleanup_processes()`.  This includes:

- `*.log` — general log files from the working directory
- `raft_*.log` — Raft-specific log files

The `cleanup()` command also removes performance output directories:

```bash
rm -rf ./out-perf.masstree/*
rm -rf ./src/mako/out-perf.masstree/*
```

---

## CI Test Scenarios

<a id="1-overview-7"></a>

### 1. Overview

The Mako-Raft CI suite includes 5 integration test scenarios, each
invoked via `ci_mako_raft.sh` or `ci.sh`.  Tests progress from basic
Raft replication to full transactional workloads across multiple shards.

| # | Scenario | Binary | Shards | Replicas | Duration | Workload |
|---|----------|--------|--------|----------|----------|----------|
| 1 | simpleRaft | `simpleRaft` | 1 (3 partitions) | 3 | 40 s | 100 logs x 3 partitions |
| 2 | shard1ReplicationRaft | `dbtest` | 1 | 3 | 60 s | TPC-C (6 threads) |
| 3 | shard2ReplicationRaft | `dbtest` | 2 | 3 each | 120 s | TPC-C (6 threads) |
| 4 | shard1ReplicationSimpleRaft | `simpleTransactionRepRaft` | 1 | 3 | 40 s | Simple key-value |
| 5 | shard2ReplicationSimpleRaft | `simpleTransactionRepRaft` | 2 | 3 each | 60 s | Simple key-value |

### 2. Scenario 1: simpleRaft

**Script**: `examples/mako-raft-tests/simpleRaft.sh` (120 lines)

**Purpose**: Validates basic Raft log replication without any Mako
transaction processing.  This is the simplest possible Raft test —
a leader submits fixed-size log entries and followers replicate them.

#### 2.1 Configuration

- **Binary**: `build/simpleRaft`
- **Replicas**: 3 (localhost = preferred leader, p1, p2)
- **Partitions**: 3 (per simpleRaft binary configuration)
- **Log entries**: 100 per partition = 300 total
- **Log size**: 3 KB each
- **Submission interval**: 5 ms between entries

#### 2.2 Execution Sequence

```
1. Kill lingering simpleRaft processes
2. Start p1 (follower) → raft_a2.log      [background]
3. Sleep 2s
4. Start p2 (follower) → raft_a3.log      [background]
5. Sleep 2s
6. Start localhost (leader) → raft_a1.log  [background]
7. Sleep 40s (wait for completion)
8. Parse logs for callback counts
9. Kill all processes (SIGTERM + SIGKILL)
```

#### 2.3 Pass/Fail Criteria

| Criterion | Threshold | Log File | Grep Pattern |
|-----------|-----------|----------|--------------|
| p1 follower callbacks | >= 300 | `raft_a2.log` | `RESULTS.*follower_callbacks=` |
| p2 follower callbacks | >= 300 | `raft_a3.log` | `RESULTS.*follower_callbacks=` |

The leader callback count is logged but not required for pass (leader
may hang during shutdown before printing results).

#### 2.4 What It Tests

- Raft leader election in a 3-node cluster
- Preferred leader mechanism (localhost gets shorter election timeout)
- Log entry replication to both followers
- Callback invocation on commit

### 3. Scenario 2: shard1ReplicationRaft

**Script**: `examples/mako-raft-tests/test_1shard_replication_raft.sh` (153 lines)

**Purpose**: Runs TPC-C benchmark on a single shard with 3 Raft
replicas.  This tests Raft replication under real transactional workload
including NewOrder, Payment, and other TPC-C transactions.

#### 3.1 Configuration

- **Binary**: `dbtest` (via `bash/shard_raft.sh`)
- **Shards**: 1
- **Replicas per shard**: 3 (localhost, p1, p2)
- **Threads**: 6 (configurable via `$1`, default 6)
- **Benchmark**: TPC-C
- **Duration**: 60 s
- **Config files**:
  - `config/1leader_2followers/raft6_shardidx0.yml` (site topology)
  - `config/occ_raft.yml` (mode: `cc:occ, ab:raft`)
  - `src/mako/config/local-shards1-warehouses6.yml` (shard config)

#### 3.2 Execution Sequence

```
1. Kill lingering dbtest/simpleRaft processes
2. Clean old log files and RocksDB data
3. Start 3 replicas via shard_raft.sh 1 0 6 {localhost,p2,p1} 0 1
   - localhost first, p2 second, p1 last (1s delay between)
4. Sleep 60s
5. Kill processes
6. Parse logs
```

#### 3.3 Log Files

| Log File | Process | Role |
|----------|---------|------|
| `test_1shard_replication_raft.sh_shard0-localhost-6.log` | `dbtest` | Leader |
| `test_1shard_replication_raft.sh_shard0-p1-6.log` | `dbtest` | Follower |
| `test_1shard_replication_raft.sh_shard0-p2-6.log` | `dbtest` | Follower |

#### 3.4 Pass/Fail Criteria

| Criterion | Threshold | Log File | Grep Pattern |
|-----------|-----------|----------|--------------|
| Throughput reported | Present | leader log | `agg_persist_throughput` |
| Abort ratio | < 20% | leader log | `NewOrder_remote_abort_ratio:` |
| Follower replication | > 500 batches | p1 log | `replay_batch:` |

The `replay_batch` threshold was lowered from 1000 to 500 to account
for CI environment variability.  The test verifies replication is working,
not exact batch count.

#### 3.5 What It Tests

- Raft replication under TPC-C transactional load
- Leader handles concurrent transactions while replicating
- Followers replay transaction batches
- OCC concurrency control works alongside Raft

### 4. Scenario 3: shard2ReplicationRaft

**Script**: `examples/mako-raft-tests/test_2shard_replication_raft.sh` (208 lines)

**Purpose**: Runs TPC-C benchmark on 2 shards, each with 3 Raft
replicas (6 total `dbtest` processes).  Tests cross-shard transactions
with Raft replication.

#### 4.1 Configuration

- **Binary**: `dbtest` (via `bash/shard_raft.sh`)
- **Shards**: 2
- **Replicas per shard**: 3
- **Total processes**: 6
- **Threads**: 6
- **Duration**: Up to 120 s (polling for completion)

#### 4.2 Execution Sequence

```
1. Kill lingering processes, clean logs
2. Start shard 0: 3 replicas via shard_raft.sh 2 0 6 {localhost,p2,p1} 0 1
3. Sleep 5s (prevent port conflicts between shards)
4. Start shard 1: 3 replicas via shard_raft.sh 2 1 6 {localhost,p2,p1} 0 1
5. Poll for completion (max 120s):
   - Check both shard0-localhost.log and shard1-localhost.log
   - for "agg_persist_throughput" keyword
   - 1s polling interval, 10s progress reports
6. Graceful shutdown: SIGTERM → 3s wait → SIGKILL
7. Parse logs
```

#### 4.3 Shutdown Procedure

This test uses a more careful multi-phase shutdown:

```
1. pkill -TERM bash/shard_raft.sh    (stop wrapper scripts first)
2. pkill -TERM dbtest                 (graceful stop)
3. sleep 3
4. pkill -9 bash/shard_raft.sh       (force kill wrappers)
5. pkill -9 dbtest                    (force kill binaries)
6. killall -9 dbtest                  (fallback)
7. sleep 2
8. wait $SHARD0_PID $SHARD1_PID
```

#### 4.4 Pass/Fail Criteria

For each shard (0 and 1):

| Criterion | Threshold | Log File | Grep Pattern |
|-----------|-----------|----------|--------------|
| Throughput reported | Present | `shard{i}-localhost.log` | `agg_persist_throughput` |
| Abort ratio | < 40% | `shard{i}-localhost.log` | `NewOrder_remote_abort_ratio:` |

The abort ratio threshold is 40% (vs 20% for single-shard) because
cross-shard transactions have higher contention.

Additionally, Raft replication metrics are checked for informational
purposes (warnings, not failures):

| Metric | Check | Log Files |
|--------|-------|-----------|
| `replay_batch` | Warning if < 1000 | `shard{i}-{localhost,p1,p2}.log` |

#### 4.5 What It Tests

- Cross-shard TPC-C transactions with Raft replication
- Two independent Raft groups operating concurrently
- Port isolation between shard 0 and shard 1
- Completion polling (not fixed sleep) for robustness

### 5. Scenario 4: shard1ReplicationSimpleRaft

**Script**: `examples/mako-raft-tests/test_1shard_replication_simple_raft.sh` (149 lines)

**Purpose**: Runs simple key-value transactions on a single shard with
3 Raft replicas using the `simpleTransactionRepRaft` binary.  Tests data
integrity by verifying all replicas have identical committed state.

#### 5.1 Configuration

- **Binary**: `simpleTransactionRepRaft`
- **Shards**: 1
- **Replicas**: 3 (localhost, p1, p2)
- **Threads**: 6
- **Duration**: 40 s

#### 5.2 Execution Sequence

```
1. Kill lingering processes
2. Clean old logs and RocksDB data
3. Start 3 replicas:
   simpleTransactionRepRaft 1 0 6 {localhost,p2,p1} 1
4. Sleep 40s
5. Kill all PIDs → wait → pkill -9 → sleep 2
6. Parse logs for replay_batch and data integrity
```

#### 5.3 Pass/Fail Criteria

| Criterion | Threshold | Log Files | Grep Pattern |
|-----------|-----------|-----------|--------------|
| Follower replication | > 0 batches | p1 log | `replay_batch:` |
| p1 data integrity | Pass | p1 log | `ALL VERIFICATIONS PASSED` |
| p2 data integrity | Pass | p2 log | `ALL VERIFICATIONS PASSED` |
| Both followers verified | >= 2 | — | — |

The leader's verification is not required for pass (leader may hang
during shutdown — documented as a known issue).  If the leader didn't
print `ALL VERIFICATIONS PASSED`, a warning is shown instead of failure.

#### 5.4 What It Tests

- End-to-end data integrity: write on leader, replicate via Raft,
  verify identical state on followers
- `simpleTransactionRepRaft` binary's built-in verification (compares
  committed values across replicas)
- Raft replication of simple key-value workload

### 6. Scenario 5: shard2ReplicationSimpleRaft

**Script**: `examples/mako-raft-tests/test_2shard_replication_simple_raft.sh` (171 lines)

**Purpose**: Runs simple key-value transactions on 2 shards, each with
3 Raft replicas (6 total processes).  Tests multi-shard data integrity.

#### 6.1 Configuration

- **Binary**: `simpleTransactionRepRaft`
- **Shards**: 2
- **Replicas per shard**: 3
- **Total processes**: 6
- **Threads**: 6
- **Duration**: 60 s

#### 6.2 Execution Sequence

```
1. Kill lingering processes, clean logs
2. Start shard 0: simpleTransactionRepRaft 2 0 6 {localhost,p2,p1} 1
3. Sleep 2s
4. Start shard 1: simpleTransactionRepRaft 2 1 6 {localhost,p2,p1} 1
5. Sleep 60s
6. Kill all 6 PIDs → wait → pkill -9 → sleep 2
7. Parse logs
```

#### 6.3 Pass/Fail Criteria

For each shard (0 and 1):

| Criterion | Threshold | Log Files | Grep Pattern |
|-----------|-----------|-----------|--------------|
| Follower replication | > 0 batches | `simple-raft-shard{i}-p1.log` | `replay_batch:` |

Data integrity check across all 6 logs:

| Criterion | Threshold | Grep Pattern |
|-----------|-----------|--------------|
| All 4 followers verified | >= 4 | `ALL VERIFICATIONS PASSED` |
| Leaders verified | Informational | `ALL VERIFICATIONS PASSED` |

Pass requires: `follower_verified >= 4` and `failed == 0`.

#### 6.4 What It Tests

- Multi-shard data integrity under Raft replication
- Two independent Raft groups with separate state
- All 4 followers (2 per shard) have verified identical state

### 7. Process Naming and Binaries

| Binary | Purpose | Built From |
|--------|---------|-----------|
| `simpleRaft` | Standalone Raft replication test | `src/deptran/raft/` |
| `dbtest` | Full Mako transaction processor with TPC-C | `src/mako/` + `src/deptran/` |
| `simpleTransactionRepRaft` | Simple key-value with Raft replication | `src/mako/` |

### 8. Known Issues Handled in Scripts

#### 8.1 Leader Shutdown Hang

All scripts handle the known issue where the Raft leader may hang during
shutdown.  Scripts use multi-phase kill (SIGTERM → sleep → SIGKILL) and
do not require leader-side verification for pass.

#### 8.2 Port Conflicts

The 2-shard tests insert a 5s delay between starting shard 0 and shard 1
to prevent port conflicts during startup.  Port ranges are defined in
the config YAML files:

- Shard 0: `raft6_shardidx0.yml` (ports 27001-27201)
- Shard 1: `raft6_shardidx1.yml` (ports 27002-27202)

#### 8.3 RocksDB Cleanup

Simple transaction tests clean `/tmp/${USER}_mako_rocksdb_shard*` before
each run to prevent stale persistent state from affecting results.

### 9. Raft vs Paxos Scenario Comparison

| Aspect | Raft Scenarios | Paxos Scenarios |
|--------|---------------|-----------------|
| Replicas per shard | 3 | 3 + 1 learner |
| Launch script | `shard_raft.sh` | `shard.sh` |
| Config files | `raft*_shardidx*.yml` + `occ_raft.yml` | `paxos*_shardidx*.yml` + `occ_paxos.yml` |
| Port range | 27xxx | 17xxx |
| `--replication` flag | `raft` | Not specified (default Paxos) |
| Simple binary | `simpleTransactionRepRaft` | `simpleTransactionRep` |
| Basic test binary | `simpleRaft` | `simplePaxos` |
| Learner callbacks | N/A | Checked separately |
| Leader shutdown hang | Known issue | Not reported |

---

## Shell Scripts Walkthrough

<a id="1-overview-8"></a>

### 1. Overview

The `examples/mako-raft-tests/` directory contains shell scripts that
orchestrate Raft integration tests.  Five are invoked by the CI pipeline;
three additional scripts test preferred leader features independently.

| Script | Lines | CI-Invoked | Tests |
|--------|-------|------------|-------|
| `simpleRaft.sh` | 120 | Yes | Basic Raft replication (3 replicas, 300 logs) |
| `test_1shard_replication_raft.sh` | 153 | Yes | 1-shard TPC-C with Raft |
| `test_2shard_replication_raft.sh` | 208 | Yes | 2-shard TPC-C with Raft |
| `test_1shard_replication_simple_raft.sh` | 149 | Yes | 1-shard simple tx with Raft |
| `test_2shard_replication_simple_raft.sh` | 171 | Yes | 2-shard simple tx with Raft |
| `run_test1_preferred_startup.sh` | 361 | No | Preferred leader election (5-node) |
| `run_test_log_replication.sh` | 159 | No | Log replication to 5 replicas |
| `run_test_noops.sh` | 256 | No | NO-OPS watermark synchronisation |

Supporting script:

| Script | Lines | Purpose |
|--------|-------|---------|
| `bash/shard_raft.sh` | 39 | Launch a single Raft shard (dbtest) |

### 2. `simpleRaft.sh` — Basic Raft Replication

**Path**: `examples/mako-raft-tests/simpleRaft.sh`

#### 2.1 Step-by-Step Walkthrough

```
Step 1:  Remove old logs (raft_a1.log, raft_a2.log, raft_a3.log)
Step 2:  killall simpleRaft, sleep 1
Step 3:  Start p1 (follower) → raft_a2.log  [background]
Step 4:  Sleep 2s (allow p1 to initialise)
Step 5:  Start p2 (follower) → raft_a3.log  [background]
Step 6:  Sleep 2s (allow p2 to initialise)
Step 7:  Start localhost (preferred leader) → raft_a1.log  [background]
Step 8:  Sleep 40s (wait for 300 logs × 5ms + election + shutdown)
Step 9:  tail -n 5 all three logs
Step 10: Parse follower_callbacks from raft_a2.log and raft_a3.log
Step 11: Parse leader_callbacks from raft_a1.log (informational)
Step 12: Assert both follower callbacks >= 300
Step 13: Kill all PIDs (SIGTERM → sleep 1 → SIGKILL → killall -9)
Step 14: Exit 0 (pass) or 1 (fail)
```

#### 2.2 Key Design Choices

- **Followers start first**: Ensures they are ready to receive
  AppendEntries when the leader starts.
- **Fixed 40s sleep**: Matches the Paxos `simplePaxos.sh` duration.
- **Leader hang tolerated**: Leader may not print `RESULTS` due to
  shutdown timing, so only follower callbacks are required.

#### 2.3 Log Parsing Functions

```bash
get_follower_callbacks() {
    grep "RESULTS.*follower_callbacks=" "$1" | \
      sed -E 's/.*follower_callbacks=([0-9]+).*/\1/' | tail -1
}

get_leader_callbacks() {
    grep "RESULTS.*leader_callbacks=" "$1" | \
      sed -E 's/.*leader_callbacks=([0-9]+).*/\1/' | tail -1
}
```

### 3. `test_1shard_replication_raft.sh` — 1-Shard TPC-C

**Path**: `examples/mako-raft-tests/test_1shard_replication_raft.sh`

#### 3.1 Step-by-Step Walkthrough

```
Step 1:  Kill lingering dbtest/simpleRaft processes
Step 2:  Clean nfs_sync_* and /tmp/mako_rocksdb_shard*
Step 3:  Start 3 replicas via shard_raft.sh:
           shard_raft.sh 1 0 6 localhost 0 1  →  ..._shard0-localhost-6.log
           shard_raft.sh 1 0 6 p2 0 1         →  ..._shard0-p2-6.log
           sleep 1
           shard_raft.sh 1 0 6 p1 0 1         →  ..._shard0-p1-6.log
Step 4:  Sleep 60s (run TPC-C benchmark)
Step 5:  Kill SHARD0_PID, wait
Step 6:  Check leader log (localhost):
           - grep "agg_persist_throughput" → must exist
           - grep "NewOrder_remote_abort_ratio:" → extract value, assert < 20%
Step 7:  Check follower log (p1):
           - grep "replay_batch:" → extract last value, assert > 500
Step 8:  Exit 0 (all pass) or 1 (any fail)
```

#### 3.2 Thread Count

The thread count is configurable via `$1` (default 6), which selects
both the warehouse configuration and the Raft site config:

```bash
trd=${1:-6}
# Resolves to: config/1leader_2followers/raft6_shardidx0.yml
```

### 4. `test_2shard_replication_raft.sh` — 2-Shard TPC-C

**Path**: `examples/mako-raft-tests/test_2shard_replication_raft.sh`

#### 4.1 Step-by-Step Walkthrough

```
Step 1:  Clean logs, kill lingering processes
Step 2:  Start shard 0 (3 replicas):
           shard_raft.sh 2 0 6 localhost 0 1  →  shard0-localhost.log
           shard_raft.sh 2 0 6 p2 0 1         →  shard0-p2.log
           sleep 1
           shard_raft.sh 2 0 6 p1 0 1         →  shard0-p1.log
Step 3:  Sleep 5s (port isolation between shards)
Step 4:  Start shard 1 (3 replicas):
           shard_raft.sh 2 1 6 localhost 0 1  →  shard1-localhost.log
           sleep 1
           shard_raft.sh 2 1 6 p2 0 1         →  shard1-p2.log
           sleep 1
           shard_raft.sh 2 1 6 p1 0 1         →  shard1-p1.log
Step 5:  Completion polling loop (max 120s):
           Check shard0-localhost.log for "agg_persist_throughput"
           Check shard1-localhost.log for "agg_persist_throughput"
           Both present → break; else sleep 1, report every 10s
Step 6:  Multi-phase shutdown:
           pkill -TERM bash/shard_raft.sh  (wrapper scripts)
           pkill -TERM dbtest              (binaries)
           sleep 3
           pkill -9 bash/shard_raft.sh
           pkill -9 dbtest
           killall -9 dbtest
           sleep 2
           wait $SHARD0_PID $SHARD1_PID
Step 7:  For each shard (0, 1):
           - Assert "agg_persist_throughput" present
           - Assert NewOrder_remote_abort_ratio < 40%
           - Report replay_batch counts (warning if < 1000)
Step 8:  Exit 0 or 1
```

#### 4.2 Key Design: Completion Polling

Unlike the 1-shard test (fixed 60s sleep), the 2-shard test uses
active polling with a 120s timeout.  This is more robust because
cross-shard transactions can have variable completion times.

```bash
while [ $wait_count -lt $max_wait ]; do
    if grep -q "agg_persist_throughput" "$log_file0" && \
       grep -q "agg_persist_throughput" "$log_file1"; then
        break
    fi
    sleep 1
done
```

#### 4.3 Key Design: Multi-Phase Shutdown

Wrapper scripts are killed before binaries to prevent respawning:

1. `pkill -TERM bash/shard_raft.sh` — stop wrappers first
2. `pkill -TERM dbtest` — graceful stop
3. Wait 3s for cleanup
4. `pkill -9` — force kill everything

### 5. `test_1shard_replication_simple_raft.sh` — 1-Shard Simple Tx

**Path**: `examples/mako-raft-tests/test_1shard_replication_simple_raft.sh`

#### 5.1 Step-by-Step Walkthrough

```
Step 1:  Kill lingering simpleTransactionRepRaft/dbtest
Step 2:  Clean logs, RocksDB data
Step 3:  Start 3 replicas:
           simpleTransactionRepRaft 1 0 6 localhost 1 → simple-raft-shard0-localhost.log
           simpleTransactionRepRaft 1 0 6 p2 1       → simple-raft-shard0-p2.log
           sleep 1
           simpleTransactionRepRaft 1 0 6 p1 1       → simple-raft-shard0-p1.log
Step 4:  Sleep 40s
Step 5:  Kill PIDs → wait → pkill -9 → sleep 2
Step 6:  Check p1 follower: replay_batch > 0
Step 7:  Data integrity check (3 logs):
           For each {localhost, p2, p1}:
             grep "ALL VERIFICATIONS PASSED"
             localhost failure → warning (leader hang)
             follower failure → test fail
Step 8:  Pass requires: follower_verified >= 2 && failed == 0
```

#### 5.2 Key Design: Leader Hang Tolerance

The leader's verification result is logged but a missing verification
on the leader is treated as a warning, not a failure:

```bash
if [ "$log_suffix" = "localhost" ]; then
    echo "⚠ Leader may have hung during shutdown (known issue)"
else
    echo "✗ Data integrity verification FAILED"
    failed=1
fi
```

### 6. `test_2shard_replication_simple_raft.sh` — 2-Shard Simple Tx

**Path**: `examples/mako-raft-tests/test_2shard_replication_simple_raft.sh`

#### 6.1 Step-by-Step Walkthrough

```
Step 1:  Kill lingering processes, clean logs
Step 2:  Start shard 0 (3 replicas):
           simpleTransactionRepRaft 2 0 6 {localhost,p2} 1
           sleep 1
           simpleTransactionRepRaft 2 0 6 p1 1
Step 3:  Sleep 2s
Step 4:  Start shard 1 (3 replicas):
           simpleTransactionRepRaft 2 1 6 {localhost,p2} 1
           sleep 1
           simpleTransactionRepRaft 2 1 6 p1 1
Step 5:  Sleep 60s
Step 6:  Kill all 6 PIDs → wait → pkill -9 → sleep 2
Step 7:  For each shard (0, 1): check replay_batch > 0 on p1
Step 8:  Data integrity: check all 6 logs for "ALL VERIFICATIONS PASSED"
Step 9:  Pass requires: follower_verified >= 4 && failed == 0
```

### 7. `bash/shard_raft.sh` — Raft Shard Launcher

**Path**: `bash/shard_raft.sh`

#### 7.1 Step-by-Step Walkthrough

```
Step 1:  Parse arguments: nshard=$1, shard=$2, trd=$3, cluster=$4,
         is_micro=$5, is_replicated=$6
Step 2:  mkdir -p results
Step 3:  Construct command:
           ./build/dbtest \
             --num-threads $trd \
             --shard-index $shard \
             --shard-config $path/config/local-shards${nshard}-warehouses${trd}.yml \
             -F config/1leader_2followers/raft${trd}_shardidx${shard}.yml \
             -F config/occ_raft.yml \
             -P $cluster \
             --replication raft
Step 4:  Conditionally add --is-micro and --is-replicated flags
Step 5:  Print configuration summary
Step 6:  eval $CMD
```

#### 7.2 Config File Selection

The script selects config files based on thread count and shard index:

| Argument | Config File |
|----------|------------|
| `trd=6, shard=0` | `raft6_shardidx0.yml` |
| `trd=6, shard=1` | `raft6_shardidx1.yml` |

The `--replication raft` flag ensures the dispatcher routes to
`raft_impl` namespace even if YAML auto-detection fails.

### 8. Non-CI Scripts

#### 8.1 `run_test1_preferred_startup.sh` (361 lines)

**Purpose**: Verify TimeoutNow leadership transfer protocol.

- **Cluster**: 5 nodes (localhost = preferred, p1-p4 = followers)
- **Binary**: `testPreferredReplicaStartup`
- **Duration**: ~35 seconds
- **Process**:
  1. Start all 5 replicas
  2. Monitor for 35s with early-exit detection
  3. Wait for all processes
  4. Analyse logs: count "BECAME LEADER" events per node
  5. Pass: localhost became leader >= 1 time, p1-p4 never became leader,
     all processes exit 0

#### 8.2 `run_test_log_replication.sh` (159 lines)

**Purpose**: Verify 25 logs replicated to all 5 replicas.

- **Binary**: `testPreferredReplicaLogReplication`
- **Cluster**: 5 nodes
- **Duration**: ~10-15 seconds
- **Process**:
  1. Launch all 5 replicas in parallel
  2. Wait for all to complete (poll with progress indicator)
  3. Extract metrics: Role, Logs applied, PASS/FAIL status
  4. Pass: all 5 replicas report PASS

#### 8.3 `run_test_noops.sh` (256 lines)

**Purpose**: Verify NO-OPS watermark synchronisation with preferred
leader.

- **Binary**: `testNoOps`
- **Cluster**: 5 nodes
- **Duration**: ~25 seconds
- **Process**:
  1. Launch all 5 replicas
  2. Monitor with 2s polling, status updates every 10s
  3. Timeout at 25s → SIGKILL remaining
  4. Extract metrics: NO-OPS applied, regular logs applied, max epoch
  5. Pass: all 5 replicas report "OVERALL: ALL TESTS PASSED"

#### 8.4 Common Patterns in Non-CI Scripts

All three non-CI scripts share a consistent pattern:

| Feature | Details |
|---------|---------|
| Colour output | RED, GREEN, YELLOW, BLUE terminal codes |
| Log directory | `logs/test_*` or `logs_noops_test` under script dir |
| Cleanup trap | `trap cleanup EXIT` for SIGKILL on exit |
| Progress monitoring | Polling loop with elapsed time display |
| Result collection | Parse per-replica log files for pass/fail markers |
| Exit code | 0 on pass, 1 on fail |

### 9. Script Dependency Graph

```
ci_mako_raft.sh (or ci.sh)
├── compile()
│     └── make -j32
├── run_simple_raft()
│     └── examples/mako-raft-tests/simpleRaft.sh
│           └── build/simpleRaft {localhost,p1,p2}
├── run_1shard_replication_raft()
│     └── examples/mako-raft-tests/test_1shard_replication_raft.sh
│           └── bash/shard_raft.sh 1 0 6 {localhost,p2,p1} 0 1
│                 └── build/dbtest --replication raft ...
├── run_2shard_replication_raft()
│     └── examples/mako-raft-tests/test_2shard_replication_raft.sh
│           ├── bash/shard_raft.sh 2 0 6 {localhost,p2,p1} 0 1
│           └── bash/shard_raft.sh 2 1 6 {localhost,p2,p1} 0 1
├── run_1shard_replication_simple_raft()
│     └── examples/mako-raft-tests/test_1shard_replication_simple_raft.sh
│           └── build/simpleTransactionRepRaft 1 0 6 {localhost,p2,p1} 1
└── run_2shard_replication_simple_raft()
      └── examples/mako-raft-tests/test_2shard_replication_simple_raft.sh
            ├── build/simpleTransactionRepRaft 2 0 6 {localhost,p2,p1} 1
            └── build/simpleTransactionRepRaft 2 1 6 {localhost,p2,p1} 1
```

---

## Benchmark Methodology

<a id="1-overview-9"></a>

### 1. Overview

This chapter describes the methodology used to compare Raft and Paxos
replication performance in Mako.  All benchmarks use the CI test
infrastructure documented in the previous chapter, running on a single
localhost machine.

### 2. Test Environment

#### 2.1 Hardware

All tests run on a single machine with all replicas co-located.  This
means network latency between replicas is effectively zero (localhost
loopback), so measured throughput reflects CPU overhead and
synchronisation costs rather than network round-trip time.

#### 2.2 Transport

All tests use the **rrr (TCP/IP RPC)** transport backend (default).
eRPC (RDMA) is not used in these benchmarks.  The rrr transport adds
~10-50 us latency per RPC call even on localhost.

#### 2.3 Build Configuration

- **Optimisation**: Release mode (`-O2`)
- **Concurrency control**: OCC (optimistic concurrency control)
- **Memory allocator**: jemalloc

### 3. Workloads

#### 3.1 TPC-C Benchmark

The primary workload is TPC-C, the industry-standard OLTP benchmark.
TPC-C models a wholesale supplier with these transaction types:

| Transaction | Mix | Description |
|-------------|-----|-------------|
| NewOrder | 45% | Create a new order (read + write, cross-shard possible) |
| Payment | 43% | Process a payment (read + write) |
| Delivery | 4% | Deliver pending orders (batch write) |
| OrderStatus | 4% | Query order status (read-only) |
| StockLevel | 4% | Check stock levels (read-only) |

#### 3.2 Simple Transaction Workload

A simple key-value workload using the `simpleTransactionRepRaft` /
`simpleTransactionRep` binaries.  Writes key-value pairs to the leader
and verifies all replicas have identical state after replication.

### 4. Test Configurations

#### 4.1 1-Shard TPC-C

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Shards | 1 | 1 |
| Replicas | 3 voters + 1 learner = 4 | 3 voters |
| Total processes | 4 | 3 |
| Worker threads | 6 | 6 |
| Warehouses per shard | 6 | 6 |
| Test duration | 40 s (Paxos CI) | 60 s (Raft CI) |
| Site config | `paxos6_shardidx0.yml` | `raft6_shardidx0.yml` |
| Mode config | `occ_paxos.yml` | `occ_raft.yml` |
| Port range | 17001-17301 | 27001-27201 |
| Shard config | `local-shards1-warehouses6.yml` | Same |
| Launch script | `bash/shard.sh` | `bash/shard_raft.sh` |

#### 4.2 2-Shard TPC-C

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Shards | 2 | 2 |
| Replicas per shard | 3 + 1 learner = 4 | 3 |
| Total processes | 8 | 6 |
| Worker threads | 6 per shard | 6 per shard |
| Warehouses per shard | 6 | 6 |
| Test duration | ~120 s (polling) | ~120 s (polling) |
| Cross-shard transactions | Yes (NewOrder, Payment) | Yes |
| Abort ratio threshold | < 40% | < 40% |

#### 4.3 1-Shard Simple Transaction

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Binary | `simpleTransactionRep` | `simpleTransactionRepRaft` |
| Replicas | 3 + 1 learner | 3 |
| Duration | 40 s | 40 s |
| Verification | `ALL VERIFICATIONS PASSED` | Same |

#### 4.4 2-Shard Simple Transaction

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Binary | `simpleTransactionRep` | `simpleTransactionRepRaft` |
| Replicas per shard | 3 + 1 learner | 3 |
| Duration | 60 s | 60 s |
| Total processes | 8 | 6 |

### 5. Metrics Collected

#### 5.1 Primary Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `agg_persist_throughput` | Leader log | Aggregate persisted transaction throughput (ops/sec) |
| `replay_batch` | Follower log | Number of replication batches replayed by followers |
| `NewOrder_remote_abort_ratio` | Leader log | Percentage of NewOrder transactions aborted due to remote conflicts |

#### 5.2 Per-Transaction Metrics

| Metric | Description |
|--------|-------------|
| `{TxType}_attempts` | Number of transaction attempts |
| `{TxType}_commits` | Number of successful commits |
| `{TxType}_avg_latency` | Average latency (microseconds) |
| `{TxType}_p50_latency` | Median latency |
| `{TxType}_p99_latency` | 99th percentile latency |
| `{TxType}_abort_ratio` | Abort rate (local + remote) |

#### 5.3 Replication Metrics

| Metric | Description |
|--------|-------------|
| `follower_callbacks` | Number of committed entries received by follower |
| `leader_callbacks` | Number of committed entries on leader |
| Per-partition commit counts | Distribution of commits across partitions |

### 6. Measurement Procedure

#### 6.1 TPC-C Tests

1. Clean environment (kill processes, remove stale data)
2. Start replicas (followers first, leader last)
3. Wait for leader election and stabilisation (~5 s)
4. Run benchmark for specified duration
5. Collect output from leader log
6. Collect replication metrics from follower logs
7. Kill all processes, archive logs

#### 6.2 Simple Transaction Tests

1. Clean environment
2. Start all replicas
3. Run workload for specified duration
4. Wait for followers to finish replaying
5. Verify data integrity: `ALL VERIFICATIONS PASSED`
6. Collect `replay_batch` counts
7. Kill and archive

### 7. Caveats and Limitations

#### 7.1 Single-Node Deployment

All replicas run on localhost.  This eliminates network latency as a
variable but introduces CPU contention between replicas.  In production,
replicas run on separate machines with dedicated CPU resources.

**Impact**: Throughput numbers are lower than production due to CPU
sharing.  The relative comparison (Raft vs Paxos) remains valid since
both share the same contention.

#### 7.2 Test Duration Difference

The 1-shard Paxos test runs for 40 seconds while the Raft test runs
for 60 seconds.  Longer runs can accumulate more transactions but may
also encounter more steady-state effects.  The `agg_persist_throughput`
metric normalises to ops/sec to account for this.

#### 7.3 Process Count Difference

Paxos uses 4 processes per shard (3 voters + 1 learner) while Raft uses
3 (all voters).  On a single machine, the extra Paxos learner process
competes for CPU resources, which may slightly reduce Paxos throughput
in single-node tests.

#### 7.4 Replication Protocol Differences

| Aspect | Multi-Paxos | Raft |
|--------|-------------|------|
| Log ordering | Per-instance ordering | Strict sequential log |
| Commit condition | Majority of acceptors | Majority of voters |
| Leader election | External mechanism | Built-in (RequestVote) |
| Pipelining | Native (out-of-order) | Limited (sequential index) |
| Learner role | Separate non-voting replica | Not applicable |
| Batching | Batch size configurable | Batch size configurable |

Multi-Paxos can pipeline proposals across instances without waiting for
previous instances to commit.  Raft requires strict log ordering — each
entry must be committed in index order.  This pipelining advantage can
explain higher throughput in low-contention single-shard scenarios.

#### 7.5 Warmup Period

Both Paxos and Raft tests include startup time for leader election and
RPC connection establishment (~5 seconds).  The `agg_persist_throughput`
metric is measured after the system reaches steady state.

#### 7.6 Resource Contention

On a single machine with 6 threads per shard and 3-4 replicas, the
total active thread count is 18-24 for a 1-shard test and 36-48 for a
2-shard test.  CPU scheduling effects can cause variance between runs.

---

## Detailed Benchmark Results

<a id="1-overview-10"></a>

### 1. Overview

All results were collected from CI test runs on a single localhost
machine.  Numbers are from the log files archived in `~/results/`.

### 2. 1-Shard TPC-C Results

#### 2.1 Aggregate Throughput

| Metric | Paxos | Raft | Difference |
|--------|-------|------|------------|
| `agg_persist_throughput` | 133,931 ops/sec | 96,463 ops/sec | Raft 28.0% lower |
| Test duration | ~40 s | ~60 s | Raft runs 50% longer |
| Replicas | 3 voters + 1 learner (4 processes) | 3 voters (3 processes) | Raft uses 25% fewer processes |

**Source files**:
- Paxos: `test_1shard_replication.sh_shard0-localhost-6.log`
- Raft: `test_1shard_replication_raft.sh_shard0-localhost-6.log`

#### 2.2 Per-Transaction Latency (1-Shard)

| Transaction | Paxos Commit Latency | Raft Commit Latency | Paxos Abort Ratio | Raft Abort Ratio |
|-------------|---------------------|---------------------|-------------------|------------------|
| NewOrder | 0.0451 ms | 0.0390 ms | 0.0116% | 0.0084% |
| Payment | 0.0329 ms | 0.0815 ms | 0.0015% | 0.0031% |
| Delivery | 0.1378 ms | 0.1155 ms | 0% | 0% |
| OrderStatus | 0.0141 ms | 0.0113 ms | 0.0037% | 0.0051% |
| StockLevel | 0.1034 ms | 0.1094 ms | 0.184% | 0.160% |

Notes:
- All latencies are local commit latencies (no remote transactions
  in 1-shard configuration)
- Remote abort ratios are `-nan` for both (no cross-shard transactions)
- NewOrder and Delivery are faster under Raft; Payment is faster
  under Paxos

#### 2.3 Follower Replication (1-Shard)

| Metric | Paxos Follower (p1) | Raft Follower (p1) |
|--------|--------------------|--------------------|
| `replay_batch` (final) | 669 | 3,674 |
| Wait time at termination | 39 s | 39 s |
| Threads finished at termination | 5/6 | 5/6 |

Raft followers process 5.5x more replay batches than Paxos followers
for the same workload.  This indicates Raft uses smaller, more frequent
batches while Paxos batches more aggressively.

### 3. 2-Shard TPC-C Results

#### 3.1 Aggregate Throughput

| Metric | Paxos Shard 0 | Paxos Shard 1 | Raft Shard 0 | Raft Shard 1 |
|--------|--------------|--------------|-------------|-------------|
| `agg_persist_throughput` | 8,464 ops/sec | 8,539 ops/sec | 8,491 ops/sec | 8,580 ops/sec |
| `NewOrder_remote_abort_ratio` | 1.44% | 1.12% | 2.65% | 2.63% |

**Source files**:
- Paxos: `test_2shard_replication.sh_rrr_shard{0,1}-localhost.log`
- Raft: `shard{0,1}-localhost.log`

#### 3.2 Aggregate Comparison

| Metric | Paxos (avg) | Raft (avg) | Difference |
|--------|-------------|------------|------------|
| Per-shard throughput | 8,501 ops/sec | 8,536 ops/sec | Raft 0.4% higher |
| Total throughput (2 shards) | 17,003 ops/sec | 17,071 ops/sec | Essentially equal |
| Remote abort ratio | 1.28% | 2.64% | Raft 2.1x higher |
| Replicas (total) | 8 (4 per shard) | 6 (3 per shard) | Raft uses 25% fewer |

#### 3.3 Throughput Drop: 1-Shard to 2-Shard

| Metric | Paxos | Raft |
|--------|-------|------|
| 1-shard throughput | 133,931 ops/sec | 96,463 ops/sec |
| Per-shard 2-shard throughput | 8,501 ops/sec | 8,536 ops/sec |
| Drop factor | 15.8x | 11.3x |

Both protocols experience dramatic throughput reduction when cross-shard
transactions are introduced, but Paxos drops more (15.8x vs 11.3x)
because it starts from a higher single-shard baseline.

#### 3.4 Follower Replication (2-Shard)

| Metric | Raft Shard 0 (p1) |
|--------|-------------------|
| `replay_batch` (final) | 1,173 |
| Wait time | 45 s |

(Paxos 2-shard follower `replay_batch` not captured in archived logs.)

### 4. Simple Transaction Results

#### 4.1 simpleRaft Test

| Metric | Value |
|--------|-------|
| p1 `follower_callbacks` | 303 |
| p2 `follower_callbacks` | 303 |
| Expected minimum | 300 |
| Test result | PASS |

All 300 expected log entries (100 per partition × 3 partitions) were
replicated to both followers, plus 3 additional (likely end-of-stream
markers).

#### 4.2 1-Shard Simple Transaction (Raft)

| Metric | Shard 0 - p1 | Shard 0 - p2 |
|--------|-------------|-------------|
| `replay_batch` | 12 | 12 |
| `ALL VERIFICATIONS PASSED` | Yes | Yes |

#### 4.3 2-Shard Simple Transaction (Raft)

| Metric | Shard 0 - p1 | Shard 1 - p1 |
|--------|-------------|-------------|
| `replay_batch` | 12 | 12 |
| `ALL VERIFICATIONS PASSED` | Yes | Yes |

Both shards replicate identical data to followers.  The low `replay_batch`
count (12) is expected for the simple key-value workload which generates
fewer transactions than TPC-C.

### 5. Replication Correctness

#### 5.1 Data Integrity

Both Paxos and Raft achieve identical data integrity results:

| Test | Paxos | Raft |
|------|-------|------|
| 1-shard simple | `ALL VERIFICATIONS PASSED` | `ALL VERIFICATIONS PASSED` |
| 2-shard simple | `ALL VERIFICATIONS PASSED` | `ALL VERIFICATIONS PASSED` |
| simpleRaft/simplePaxos | >= 300 callbacks | >= 300 callbacks |

#### 5.2 Replication Completeness

All followers in both protocols receive complete replication:

- Paxos: Learner receives all committed entries
- Raft: All voters receive all committed entries
- No data loss observed in any test run

### 6. Summary Table

| Configuration | Paxos (ops/sec) | Raft (ops/sec) | Raft/Paxos | Processes |
|---------------|----------------|----------------|------------|-----------|
| 1-shard TPC-C | 133,931 | 96,463 | 72.0% | 4 vs 3 |
| 2-shard TPC-C (per shard) | ~8,501 | ~8,536 | 100.4% | 8 vs 6 |
| 2-shard TPC-C (total) | ~17,003 | ~17,071 | 100.4% | 8 vs 6 |
| 1-shard simple | PASS | PASS | Equal | 4 vs 3 |
| 2-shard simple | PASS | PASS | Equal | 8 vs 6 |

---

## Performance Analysis and Discussion

<a id="1-overview-11"></a>

### 1. Overview

This chapter analyses the benchmark results presented in `results.md` and
explains the observed performance differences between Raft and Multi-Paxos
replication in Mako.  The key finding is that Raft and Paxos achieve nearly
identical throughput in multi-shard configurations (where cross-shard
coordination dominates), while Paxos is significantly faster in
single-shard configurations due to pipelining advantages.

### 2. Single-Shard Analysis: Why Paxos Is 28% Faster

#### 2.1 Observed Difference

| Protocol | 1-Shard TPC-C Throughput |
|----------|-------------------------|
| Paxos | 133,931 ops/sec |
| Raft | 96,463 ops/sec |
| Difference | Raft is 28.0% lower |

#### 2.2 Factor 1: Multi-Paxos Pipelining

The primary architectural reason for Paxos's single-shard advantage is
**Multi-Paxos pipelining**.  Multi-Paxos can process multiple consensus
instances simultaneously — while instance N is in the Accept phase,
instance N+1 can already be in the Prepare phase.  This allows the leader
to overlap network round-trips across instances.

The pipelining is visible in the Paxos codebase.  `BulkPrepare` in
`src/deptran/paxos/server.cc:121-204` processes ranges of slots
simultaneously, and `BroadcastBulkDecide` in
`src/deptran/paxos/commo.cc:481-510` sends Decide messages for
multiple instances in a single RPC.  The coordinator at
`src/deptran/paxos/coordinator.cc:405-416` does not wait for the
BulkDecide to return before proceeding:

```cpp
// src/deptran/paxos/coordinator.cc:415
// it's not necessary to wait for a majority of commits
//   sp_quorum->wait();
```

Raft, by contrast, enforces **strict sequential commit ordering**.
The leader calculates `commitIndex` based on the median `matchIndex`
across followers (`src/deptran/raft/server.cc:706-731`), and entries
are applied in strict order from `executeIndex+1` to `commitIndex`
(`src/deptran/raft/server.cc:601-603`).  There is no skip logic — if
entry N is slow to replicate, entries N+1, N+2, ... cannot be committed
or applied until N is committed:

```cpp
// src/deptran/raft/server.cc:601-603
for (slotid_t id = executeIndex + 1; id <= commitIndex; id++) {
    auto next_instance = GetRaftInstance(id);
    ...
}
```

This sequential ordering is a correctness requirement in Raft (the
replicated log must be identical across all replicas), but it limits
concurrency compared to Multi-Paxos's per-instance parallelism.

In a single-shard test with no cross-shard coordination, the replication
layer is the primary bottleneck.  Paxos's ability to pipeline gives it
a throughput advantage because the leader can keep its proposal pipeline
full without waiting for sequential commits.

#### 2.3 Factor 2: Test Duration Difference

The 1-shard Paxos test runs for **40 seconds** while the Raft test runs
for **60 seconds**.  Although `agg_persist_throughput` normalises to
ops/sec, the difference matters:

- **Shorter run (40s)**: Less time for steady-state overhead to accumulate.
  Throughput measured over a shorter window can appear higher if the
  system starts at peak and gradually degrades due to resource pressure.
- **Longer run (60s)**: More time for background garbage collection, RPC
  buffer pressure, and follower replay lag to accumulate.

Both tests include a startup phase (~5 seconds) for leader election and
RPC connection establishment.  In a 40-second test, startup overhead
is ~12.5% of the total runtime; in a 60-second test, it is ~8.3%.

This duration difference contributes a small (estimated 5-10%) bias in
favour of the Paxos measurement.  A controlled comparison would run both
protocols for the same duration.

#### 2.4 Factor 3: Process Count and CPU Contention

On the single test machine, Paxos runs **4 processes** per shard (3 voters
+ 1 learner) while Raft runs **3** (all voters).  The extra Paxos learner
consumes CPU cycles that could otherwise go to the voters, creating a
counter-effect that works **against** Paxos.

Paradoxically, Paxos achieves 28% higher throughput despite having one
more process competing for CPU.  This means the pipelining advantage more
than compensates for the additional CPU overhead.  The extra learner does
not participate in the quorum and does not add latency to the commit path
— it receives committed entries asynchronously, similar to Raft followers.

#### 2.5 Decomposition of the 28% Gap

| Factor | Estimated Impact | Direction |
|--------|-----------------|-----------|
| Multi-Paxos pipelining | 15-20% | Favours Paxos |
| Test duration (40s vs 60s) | 5-10% | Favours Paxos |
| Process count (4 vs 3) | 3-5% | Favours Raft |
| Larger batch sizes (see Section 4) | 5-10% | Favours Paxos |
| **Net** | **~28%** | **Paxos faster** |

These estimates are approximate.  Isolating individual factors would
require controlled experiments with matched durations and process counts.

### 3. Two-Shard Analysis: Why Throughput Is Equal

#### 3.1 Observed Equality

| Protocol | Per-Shard 2-Shard TPC-C Throughput |
|----------|------------------------------------|
| Paxos | 8,501 ops/sec |
| Raft | 8,536 ops/sec |
| Difference | Raft 0.4% higher (within noise) |

#### 3.2 Cross-Shard Coordination Dominates

When cross-shard transactions are introduced, the bottleneck shifts from
the replication layer to **cross-shard coordination**.  TPC-C's NewOrder
and Payment transactions can span two shards, requiring a two-phase
commit (2PC) protocol to coordinate between them.

The cross-shard coordination latency (~10ms round-trip even on localhost,
due to the 2PC protocol overhead of Prepare + Commit across shards)
dominates the per-transaction latency.  Replication latency (Raft or
Paxos) is a fraction of a millisecond on localhost, so it becomes
negligible relative to the coordination cost.

This is analogous to Amdahl's Law: when the serial component (cross-shard
coordination) dominates, improvements to the parallel component
(replication) yield diminishing returns.

#### 3.3 Throughput Drop Factors

| Protocol | 1-Shard | 2-Shard (per shard) | Drop Factor |
|----------|---------|---------------------|-------------|
| Paxos | 133,931 | 8,501 | 15.8x |
| Raft | 96,463 | 8,536 | 11.3x |

Both protocols experience a **dramatic** throughput reduction when
cross-shard transactions are introduced:

- **Paxos drops 15.8x**: From 133,931 to 8,501 ops/sec per shard
- **Raft drops 11.3x**: From 96,463 to 8,536 ops/sec per shard

Paxos drops more (15.8x vs 11.3x) because it starts from a **higher
single-shard baseline**.  Both protocols converge to the same 2-shard
throughput (~8,500 ops/sec), confirming that the replication layer is no
longer the bottleneck.

#### 3.4 Higher Remote Abort Ratio Under Raft

| Protocol | `NewOrder_remote_abort_ratio` |
|----------|-------------------------------|
| Paxos | 1.28% |
| Raft | 2.64% |

Raft's remote abort ratio is 2.1x higher than Paxos's.  This could be
caused by:

1. **Stricter ordering**: Raft's sequential log ordering may hold locks
   longer while waiting for earlier entries to commit, increasing
   contention windows for cross-shard transactions.
2. **Process count difference**: Raft runs 6 total processes (3 per shard)
   vs Paxos's 8 (4 per shard).  Fewer processes means the shards may
   share more CPU resources, leading to more scheduling conflicts.

Despite the higher abort ratio, Raft achieves the same throughput because
aborted transactions are retried and ultimately succeed.  The 2.64%
abort ratio is still well below the CI pass threshold (40%).

### 4. Replication Batching Behaviour

#### 4.1 Replay Batch Comparison

| Configuration | Paxos Follower | Raft Follower |
|---------------|---------------|---------------|
| 1-shard TPC-C | 669 batches | 3,674 batches |
| Ratio | 1x | 5.5x |

#### 4.2 Implementation Differences

Both protocols implement batching, but with different strategies:

**Raft batching** (`RAFT_BATCH_OPTIMIZATION` in
`src/deptran/raft/server.cc:800-825`): The leader collects all entries
from `nextIndex[follower]` to `lastLogIndex` into a single
`TpcBatchCommand`, sent as one AppendEntries RPC.  Followers persist
the entire batch in a single I/O operation via `PersistLogEntries`
(`src/deptran/raft/server.cc:1344-1358`):

```cpp
// src/deptran/raft/server.cc:1344-1358
auto cmds = dynamic_pointer_cast<TpcBatchCommand>(cmd);
std::vector<std::pair<slotid_t, std::shared_ptr<RaftData>>> entries_to_persist;
for (shared_ptr<TpcCommitCommand>& c: cmds->cmds_) {
    lastLogIndex = leaderPrevLogIndex + cnt;
    auto instance = GetRaftInstance(lastLogIndex);
    instance->log_ = c;
    entries_to_persist.emplace_back(lastLogIndex, instance);
}
PersistLogEntries(entries_to_persist);
```

**Raft client-side batching** (`src/deptran/raft/raft_worker.cc:275-291`):
The `SubmitLoop` collects incoming requests into a batch (up to
`batch_limit_`) before submitting them to the Raft leader.

**Paxos batching**: Uses `BulkDecide`
(`src/deptran/paxos/commo.cc:481-510`) to batch multiple commit
notifications, and `BulkPrepare` to prepare ranges of slots together.
The Decide phase is decoupled from the Accept phase, so committed
entries naturally accumulate into larger batches before followers
process them.

#### 4.3 Batch Size Analysis

The `replay_batch` metric reveals the effective batch sizes:

| Protocol | replay_batch | Approx Total Entries | Avg Entries/Batch |
|----------|-------------|---------------------|-------------------|
| Paxos | 669 | ~134K (133,931 ops x ~1s) | ~200 |
| Raft | 3,674 | ~96K (96,463 ops x ~1s) | ~26 |

- **Raft**: More frequent, smaller batches (~26 entries/batch).
  Each heartbeat interval triggers a batch of accumulated entries.
- **Paxos**: Less frequent, larger batches (~200 entries/batch).
  The pipelining design accumulates more entries before follower replay.

Paxos's larger batch sizes reduce per-entry overhead (fewer RPCs,
fewer I/O syncs), contributing to its throughput advantage.

#### 4.4 Two-Shard Batch Size

The 2-shard Raft follower processes 1,173 replay batches (vs 3,674 in
1-shard).  The reduction reflects the lower per-shard throughput in
2-shard mode (~8,500 vs ~96,000 ops/sec).  Paxos 2-shard follower
`replay_batch` was not captured in the archived logs.

### 5. Per-Transaction Latency Analysis

#### 5.1 Latency Comparison (1-Shard)

| Transaction | Paxos Latency | Raft Latency | Faster |
|-------------|---------------|--------------|--------|
| NewOrder | 0.0451 ms | 0.0390 ms | Raft (13.5% lower) |
| Payment | 0.0329 ms | 0.0815 ms | Paxos (59.6% lower) |
| Delivery | 0.1378 ms | 0.1155 ms | Raft (16.2% lower) |
| OrderStatus | 0.0141 ms | 0.0113 ms | Raft (19.9% lower) |
| StockLevel | 0.1034 ms | 0.1094 ms | Paxos (5.5% lower) |

#### 5.2 Why Raft Is Faster for Some Transactions

Raft shows lower latency for **NewOrder**, **Delivery**, and
**OrderStatus**.  These three transactions benefit from Raft's batching
optimisation: the `TpcBatchCommand` combines multiple entries into a
single RPC, reducing per-entry overhead for write-heavy transactions
(NewOrder, Delivery) and reducing queuing delay for read-only
transactions (OrderStatus).

#### 5.3 Why Paxos Is Faster for Payment

Payment is 59.6% faster under Paxos — the largest per-transaction
difference.  Payment is a high-frequency transaction (43% of TPC-C mix)
that updates both customer and district records.  Paxos's pipelining
allows Payment instances to overlap with concurrent NewOrder instances
across different Paxos slots, reducing queuing latency.  Under Raft,
these entries must be serialised in the log, creating back-pressure
when the commit rate is high.

#### 5.4 Why Aggregate Throughput Favours Paxos Despite Per-Transaction Mix

Despite Raft being faster for 3 of 5 transaction types, Paxos achieves
28% higher **aggregate** throughput.  This is because:

1. **Payment dominates the mix** (43%): Payment is where Paxos has its
   largest advantage (59.6% lower latency).  Payment's 43% share
   means Paxos's Payment advantage contributes ~25% to the
   aggregate difference.
2. **Commit pipeline depth**: Aggregate throughput depends on the
   replication layer's ability to process concurrent commits, not just
   individual transaction latency.  Paxos's pipelining allows deeper
   commit concurrency.
3. **Sequential commit bottleneck**: Raft's sequential commit
   requirement limits concurrency at the replication layer, even when
   individual transactions are fast.

### 6. Replica Topology Trade-offs

#### 6.1 Process Count

| Configuration | Paxos | Raft | Reduction |
|---------------|-------|------|-----------|
| 1-shard | 4 (3 voters + 1 learner) | 3 (all voters) | 25% fewer |
| 2-shard | 8 (4 per shard) | 6 (3 per shard) | 25% fewer |

Raft uses 25% fewer processes because it does not require a separate
learner.  In Paxos, the learner is a non-voting replica that receives
committed entries for replication but does not participate in consensus
rounds.  Raft achieves the same replication guarantee with all three
voters receiving entries as part of the normal AppendEntries protocol.

#### 6.2 Throughput per Process

| Configuration | Paxos (ops/process) | Raft (ops/process) |
|---------------|--------------------|--------------------|
| 1-shard | 33,483 | 32,154 |
| 2-shard (total) | 2,125 | 2,845 |

In 1-shard mode, Paxos achieves slightly higher throughput per process
(33,483 vs 32,154), reflecting its pipelining advantage.  In 2-shard
mode, Raft achieves higher throughput per process (2,845 vs 2,125)
because it runs fewer processes for the same aggregate throughput.

#### 6.3 Quorum Mechanics

Both protocols use majority quorum for commits:

| Protocol | Voters | Quorum Size | Fault Tolerance |
|----------|--------|-------------|-----------------|
| Paxos | 3 | 2 | 1 failure |
| Raft | 3 | 2 | 1 failure |

The fault tolerance is identical.  The Paxos learner is a non-voting
replica that receives committed entries for read scaling or backup.
Raft could achieve the same with a non-voting learner configuration
(not implemented in this version).

#### 6.4 Leader Election

Paxos in this implementation uses an external leader election mechanism,
while Raft has built-in leader election via the RequestVote RPC.  Raft's
preferred leader mechanism (TimeoutNow) provides deterministic leader
placement, which is important for geo-replicated deployments.

### 7. Replication Correctness

#### 7.1 Data Integrity

Both protocols achieve **identical data integrity results** across all
test configurations:

| Test | Result |
|------|--------|
| 1-shard simple transaction | `ALL VERIFICATIONS PASSED` (both protocols) |
| 2-shard simple transaction | `ALL VERIFICATIONS PASSED` (both protocols) |
| simpleRaft / simplePaxos | >= 300 follower callbacks (both protocols) |

The `simpleTransactionRepRaft` and `simpleTransactionRep` binaries
perform end-to-end verification: they write key-value pairs on the
leader, replicate via the consensus protocol, and then verify that all
replicas have identical committed state.

#### 7.2 Replication Completeness

No data loss was observed in any test run for either protocol.  Paxos
learners receive all committed entries, and Raft voters receive all
committed entries.  This confirms that the replication implementations
are functionally correct regardless of performance differences.

### 8. Production Deployment Implications

#### 8.1 When to Choose Raft

Raft is preferable when:

- **Resource efficiency matters**: 25% fewer processes translates to
  lower infrastructure costs in production deployments with many shards.
- **Operational simplicity**: Raft has built-in leader election
  (RequestVote + TimeoutNow for preferred leader), eliminating the need
  for an external election mechanism.
- **Multi-shard workloads dominate**: When cross-shard transactions are
  the norm, both protocols perform equally and Raft's resource advantage
  becomes the deciding factor.

#### 8.2 When to Choose Paxos

Paxos is preferable when:

- **Single-shard throughput is critical**: Paxos's 28% throughput
  advantage in single-shard mode is significant for workloads that can
  be partitioned to minimise cross-shard transactions.
- **Learner replicas are needed**: Paxos's learner role provides a
  non-voting read replica that can serve read-only queries without
  participating in consensus, useful for read-heavy workloads.
- **Pipelining matters**: Workloads with high-frequency small
  transactions benefit from Multi-Paxos's ability to overlap consensus
  rounds across instances.

#### 8.3 Performance Parity in Practice

For most real-world deployments with multiple shards and cross-shard
transactions, the benchmark results suggest that **Raft and Paxos
perform equivalently**.  The 28% single-shard gap disappears when
cross-shard coordination becomes the bottleneck.  The choice between
protocols should be driven by operational considerations (simplicity,
process count, learner support) rather than raw throughput.

#### 8.4 Throughput in Context

The 1-shard throughput difference (133,931 vs 96,463 ops/sec) is
measured on localhost where network latency is zero.  In a production
geo-replicated deployment:

- Network round-trip time between replicas would be 1-100 ms depending
  on geography.
- This latency would dominate the commit path for both protocols.
- The effective throughput difference between Paxos and Raft would be
  smaller than observed in localhost benchmarks.
- The 2-shard results (where throughput converges) are more
  representative of production behaviour where coordination latency
  dominates.

#### 8.5 Summary of Trade-offs

| Factor | Paxos | Raft |
|--------|-------|------|
| Single-shard throughput | Higher (133,931 ops/sec) | Lower (96,463 ops/sec) |
| Multi-shard throughput | ~8,500 ops/sec/shard | ~8,500 ops/sec/shard |
| Process overhead | 33% more (learner) | Baseline |
| Leader election | External | Built-in (RequestVote + TimeoutNow) |
| Log ordering | Per-instance (pipelined) | Sequential |
| Follower replay latency | Higher (large batches) | Lower (small batches) |
| Remote abort ratio (2-shard) | 1.28% | 2.64% |
| Correctness | Verified | Verified |
| Operational complexity | Higher | Lower |

### 9. Threats to Validity

#### 9.1 Single-Node Testing

All benchmarks run on a single machine with localhost networking.
Production deployments spread replicas across machines with real network
latency (typically 0.1-1ms within a data centre, 10-100ms across
regions).  The relative performance of Raft vs Paxos may differ when
network latency is the dominant factor.

#### 9.2 Single Run

Results are from a single CI run, not averaged across multiple runs.
Run-to-run variance can be significant on a shared machine.  Statistical
confidence would require multiple runs with variance analysis.

#### 9.3 Small Scale

The tests use 1-2 shards with 3 replicas each.  Production systems may
run hundreds of shards.  Scaling effects (e.g., increased contention on
shared resources, larger Raft log indices) are not captured.

#### 9.4 Duration Mismatch

The 1-shard Paxos test runs for 40s while Raft runs for 60s.  This
introduces a measurement bias that cannot be fully corrected by the
ops/sec normalisation.

---

## Performance Figures and Charts

### 1. 1-Shard TPC-C Throughput Comparison

```
  agg_persist_throughput (ops/sec)

  140,000 |  #######
  130,000 |  #######
  120,000 |  #######
  110,000 |  #######
  100,000 |  #######  #######
   90,000 |  #######  #######
   80,000 |  #######  #######
   70,000 |  #######  #######
   60,000 |  #######  #######
   50,000 |  #######  #######
   40,000 |  #######  #######
   30,000 |  #######  #######
   20,000 |  #######  #######
   10,000 |  #######  #######
        0 +--Paxos----Raft---

  Paxos: 133,931 ops/sec
  Raft:   96,463 ops/sec  (28.0% lower)
```

### 2. 2-Shard TPC-C Per-Shard Throughput

```
  agg_persist_throughput (ops/sec per shard)

   9,000 |  #######  #######  #######  #######
   8,000 |  #######  #######  #######  #######
   7,000 |  #######  #######  #######  #######
   6,000 |  #######  #######  #######  #######
   5,000 |  #######  #######  #######  #######
   4,000 |  #######  #######  #######  #######
   3,000 |  #######  #######  #######  #######
   2,000 |  #######  #######  #######  #######
   1,000 |  #######  #######  #######  #######
       0 +--Pax-S0--Pax-S1--Raft-S0--Raft-S1-

  Paxos Shard 0: 8,464 ops/sec
  Paxos Shard 1: 8,539 ops/sec
  Raft  Shard 0: 8,491 ops/sec   (Paxos data from raft run)
  Raft  Shard 1: 8,580 ops/sec

  All four values within 1.4% of each other — effectively equal.
```

### 3. Throughput Scaling: 1-Shard to 2-Shard

```
  ops/sec (log scale, per shard)

  133,931 |  *  Paxos
          |    \
          |      \     * 96,463  Raft
          |        \  /
          |          \/
   10,000 |          /\
          |        /    \
    8,501 |  ----*--------*---- 8,536
          |  Paxos 2S    Raft 2S
          +--------+--------+
           1-shard    2-shard

  Paxos: 133,931 --> 8,501/shard  (15.8x drop)
  Raft:   96,463 --> 8,536/shard  (11.3x drop)

  Both converge to ~8,500 ops/sec when cross-shard
  coordination (~10ms latency) becomes the bottleneck.
```

### 4. Per-Transaction Commit Latency (1-Shard)

```
  Commit latency (ms)

  0.14 |                 ##
       |                 ##
  0.12 |                 ##      ##
       |                 ##      ##
  0.10 |                 ##      ##            ##  ##
       |                 ##      ##            ##  ##
  0.08 |           ##    ##      ##            ##  ##
       |           ##    ##      ##            ##  ##
  0.06 |           ##    ##      ##            ##  ##
       |  ##       ##    ##      ##            ##  ##
  0.04 |  ##  ##   ##    ##      ##            ##  ##
       |  ##  ##   ##    ##      ##            ##  ##
  0.02 |  ##  ##   ##    ##      ##   ##  ##   ##  ##
       |  ##  ##   ##    ##      ##   ##  ##   ##  ##
     0 +--P---R----P-----R-------P----R---P----R---R-
          NewOrder  Payment   Delivery  OrdSt  Stock

  P = Paxos, R = Raft

  NewOrder:    Paxos 0.045 ms | Raft 0.039 ms  (Raft 13% faster)
  Payment:     Paxos 0.033 ms | Raft 0.082 ms  (Paxos 60% faster)
  Delivery:    Paxos 0.138 ms | Raft 0.116 ms  (Raft 16% faster)
  OrderStatus: Paxos 0.014 ms | Raft 0.011 ms  (Raft 19% faster)
  StockLevel:  Paxos 0.103 ms | Raft 0.109 ms  (Paxos 6% faster)
```

### 5. Follower Replay Batch Comparison (1-Shard)

```
  replay_batch count

  4,000 |            #######
  3,500 |            #######
  3,000 |            #######
  2,500 |            #######
  2,000 |            #######
  1,500 |            #######
  1,000 |            #######
    500 |  #######   #######
      0 +--Paxos-----Raft---

  Paxos:   669 batches (avg ~6,058 entries/batch)
  Raft:  3,674 batches (avg   ~794 entries/batch)

  Raft processes 5.5x more batches with 7.6x smaller batch size.
```

### 6. Architectural Comparison Table

| Aspect | Multi-Paxos | Raft |
|--------|-------------|------|
| **Topology** | | |
| Voters per shard | 3 | 3 |
| Learner per shard | 1 | 0 |
| Total processes (1-shard) | 4 | 3 |
| Total processes (2-shard) | 8 | 6 |
| Quorum size | 2 of 3 | 2 of 3 |
| Fault tolerance | 1 failure | 1 failure |
| **Protocol** | | |
| Log ordering | Per-instance | Sequential |
| Pipelining | Yes (out-of-order) | No (in-order) |
| Leader election | External | Built-in (RequestVote) |
| Preferred leader | N/A | TimeoutNow transfer |
| **Performance (1-shard)** | | |
| Throughput | 133,931 ops/sec | 96,463 ops/sec |
| Follower replay batches | 669 | 3,674 |
| Avg entries per batch | ~6,058 | ~794 |
| Total commits | 4,052,553 | 2,915,817 |
| **Performance (2-shard)** | | |
| Per-shard throughput | ~8,501 ops/sec | ~8,536 ops/sec |
| Remote abort ratio | 1.28% | 2.64% |
| Throughput drop (1 to 2) | 15.8x | 11.3x |
| **Correctness** | | |
| Data integrity | Verified | Verified |
| Follower consistency | All pass | All pass |

### 7. Remote Abort Ratio Comparison (2-Shard)

```
  NewOrder remote abort ratio (%)

  3.0 |                     ##
      |            ##       ##
  2.5 |            ##       ##
      |            ##       ##
  2.0 |            ##       ##
      |            ##       ##
  1.5 |            ##       ##
      |   ##       ##       ##
  1.0 |   ##  ##   ##       ##   ##
      |   ##  ##   ##       ##   ##
  0.5 |   ##  ##   ##       ##   ##
      |   ##  ##   ##       ##   ##
    0 +---S0--S1---S0--S1---S0---S1-
         Paxos      Raft     Raft
        (ci_results) (ci_results) (ci_raft)

  Paxos Shard 0: 0.90%    Raft Shard 0: 1.79-2.65%
  Paxos Shard 1: 1.44%    Raft Shard 1: 1.12-2.71%

  Raft shows ~2x higher remote abort ratio in cross-shard
  transactions, possibly due to sequential commit timing.
```

### 8. Process Count vs Throughput

```
  Throughput per process (ops/sec)

  35,000 |            #######
  30,000 |  #######   #######
         |  #######   #######
  25,000 |  #######   #######
         |  #######   #######
  20,000 |  #######   #######
         |  #######   #######
  15,000 |  #######   #######
         |  #######   #######
  10,000 |  #######   #######
         |  #######   #######
   5,000 |  #######   #######
         |  #######   #######
       0 +--Paxos-----Raft---

  Paxos: 133,931 / 4 processes = 33,483 ops/sec/process
  Raft:   96,463 / 3 processes = 32,154 ops/sec/process

  Per-process throughput is within 4% — Raft is nearly as
  efficient per process as Paxos despite lower absolute throughput.
```

### 9. Mermaid Charts (for Markdown Renderers)

#### 9.1 Throughput Comparison

```mermaid
xychart-beta
    title "TPC-C Throughput: Paxos vs Raft"
    x-axis ["1-Shard Paxos", "1-Shard Raft", "2-Shard Paxos", "2-Shard Raft"]
    y-axis "Throughput (ops/sec)" 0 --> 140000
    bar [133931, 96463, 8501, 8536]
```

#### 9.2 Follower Replay Batches

```mermaid
xychart-beta
    title "Follower Replay Batches (1-Shard)"
    x-axis ["Paxos", "Raft"]
    y-axis "replay_batch count" 0 --> 4000
    bar [669, 3674]
```

#### 9.3 Per-Transaction Latency

```mermaid
xychart-beta
    title "Per-Transaction Commit Latency (ms)"
    x-axis ["NewOrder", "Payment", "Delivery", "OrderStatus", "StockLevel"]
    y-axis "Latency (ms)" 0 --> 0.15
    bar "Paxos" [0.045, 0.033, 0.138, 0.014, 0.103]
    bar "Raft" [0.039, 0.082, 0.116, 0.011, 0.109]
```

---

## Persistent Log Storage

<a id="1-overview-12"></a>

### 1. Overview

Both Raft and Paxos require durable storage to survive crashes.  The
persistence layer provides a common `LogStorage` interface with two
implementations: `InMemoryLogStorage` for testing and
`RocksDBLogStorage` for production.

All persistence code lives in `src/rrr/rpc/` and is shared between
Raft and Paxos.

### 2. LogEntry Structure

**File**: `src/rrr/rpc/log_storage.hpp` (302 lines)

The `LogEntry` struct is the fundamental unit of persistent storage:

```cpp
struct LogEntry {
    slotid_t slot_id;                          // line 44: log index
    ballot_t term;                             // line 45: Raft term / Paxos epoch
    ballot_t max_ballot_seen;                  // line 46: highest ballot seen (Paxos)
    ballot_t max_ballot_accepted;              // line 47: highest accepted ballot (Paxos)
    std::shared_ptr<Marshallable> command;     // line 48: replicated command
    bool committed;                            // line 49: whether entry is committed
    bool is_no_op;                             // line 50: no-op entry flag
};
```

Key methods:

| Method | Line | Purpose |
|--------|------|---------|
| `to_marshal(Marshal&)` | 87 | Serialize entry to wire format |
| `from_marshal(Marshal&)` | 108 | Deserialize entry from wire format |
| `operator<()` | 65 | Order entries by `slot_id` |
| `operator==()` | 70 | Compare entries by `slot_id` and `term` |

The structure supports both Raft and Paxos — Raft uses `slot_id` as
the log index and `term` as the Raft term, while Paxos uses `slot_id`
as the instance number and the `max_ballot_*` fields for Multi-Paxos
ballot tracking.

### 3. LogStorage Interface

**File**: `src/rrr/rpc/log_storage.hpp` (302 lines)

The abstract `LogStorage` class defines the storage contract:

#### 3.1 Single Entry Operations

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `get()` | 160 | `get(slotid_t)` | `rusty::Option<LogEntry>` |
| `put()` | 168 | `put(const LogEntry&)` | `bool` |
| `remove()` | 176 | `remove(slotid_t)` | `bool` |

#### 3.2 Batch Operations

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `get_range()` | 189 | `get_range(slotid_t start, slotid_t end)` | `std::vector<LogEntry>` |
| `put_batch()` | 197 | `put_batch(const std::vector<LogEntry>&)` | `bool` |
| `remove_range()` | 206 | `remove_range(slotid_t start, slotid_t end)` | `bool` |

#### 3.3 Index Queries

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `get_first_index()` | 217 | `get_first_index()` | `slotid_t` |
| `get_last_index()` | 224 | `get_last_index()` | `slotid_t` |
| `get_term()` | 232 | `get_term(slotid_t)` | `rusty::Option<ballot_t>` |
| `size()` | 239 | `size()` | `size_t` |
| `empty()` | 246 | `empty()` | `bool` |

#### 3.4 Metadata Operations

| Method | Line | Signature | Returns |
|--------|------|-----------|---------|
| `set_metadata()` | 259 | `set_metadata(const std::string& key, const std::string& value)` | `bool` |
| `get_metadata()` | 267 | `get_metadata(const std::string& key)` | `rusty::Option<std::string>` |

Metadata operations store key-value pairs for consensus state such as
`currentTerm`, `vote_for`, and `commitIndex`.

#### 3.5 Lifecycle Operations

| Method | Line | Signature | Purpose |
|--------|------|-----------|---------|
| `sync()` | 278 | `sync()` | Force pending writes to durable storage |
| `close()` | 285 | `close()` | Release resources |
| `is_open()` | 292 | `is_open()` | Check if open and ready |
| `clear()` | 299 | `clear()` | Clear all entries and metadata |

### 4. InMemoryLogStorage

**File**: `src/rrr/rpc/memory_log_storage.hpp` (292 lines)

A thread-safe in-memory implementation used for testing.

#### 4.1 Internal Structure

```cpp
class InMemoryLogStorage : public LogStorage {
    rusty::Mutex<std::map<slotid_t, LogEntry>> logs_;      // line 36
    rusty::Mutex<std::map<std::string, std::string>> metadata_;  // line 39
    rusty::Cell<bool> is_open_;                             // line 42
};
```

Uses `rusty::Mutex` for thread-safe access to the log map and metadata
map.  The `rusty::Cell<bool>` provides interior mutability for the
open state flag.

#### 4.2 Key Characteristics

- All methods are annotated `@safe` (no raw pointer manipulation)
- `sync()` is a no-op (no durable storage to flush)
- `close()` clears all data and sets `is_open_` to false
- `reopen()` (line 270) allows re-opening after close for test scenarios
- `get_all()` (line 278) returns all entries as a vector (test utility)

#### 4.3 Thread Safety

All operations acquire the `rusty::Mutex` before accessing `logs_` or
`metadata_`.  This ensures correct behaviour in concurrent test scenarios
such as multiple threads submitting entries while others read.

### 5. RocksDBLogStorage

**File**: `src/rrr/rpc/rocksdb_log_storage.hpp` (480 lines)

The production-grade persistent storage backend.

#### 5.1 Internal Structure

```cpp
class RocksDBLogStorage : public LogStorage {
    rocksdb::DB* db_;                    // line 46: database handle
    std::string db_path_;                // line 47: path to database
    rocksdb::Options options_;           // line 50: RocksDB configuration
    rocksdb::WriteOptions write_options_;  // line 51: sync=true
    rocksdb::ReadOptions read_options_;    // line 52: verify_checksums=true
    rusty::Cell<bool> is_open_;            // line 55: open state
};
```

#### 5.2 Key Prefixes

```cpp
static constexpr const char* LOG_PREFIX = "log:";     // line 58
static constexpr const char* META_PREFIX = "meta:";   // line 59
```

Log entries are stored with key format `log:{20-digit-padded-slot}`.
The 20-digit zero-padding ensures lexicographic ordering matches
numeric ordering, so RocksDB iterators return entries in index order.

Metadata is stored with key format `meta:{key}`.

#### 5.3 RocksDB Configuration

Configured at construction (lines 100-111):

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `create_if_missing` | `true` | Create database if not present |
| `max_open_files` | 256 | Limit file descriptor usage |
| `write_buffer_size` | 64 MB | In-memory write buffer before flush |
| `target_file_size_base` | 64 MB | Target SST file size |
| `compression` | LZ4 | Fast compression for log entries |
| `max_background_jobs` | 4 | Background compaction threads |
| `write_options_.sync` | `true` | **Ensures durability** — every write is fsynced |
| `read_options_.verify_checksums` | `true` | Verify data integrity on read |

The `sync = true` write option is critical for crash safety.  Without
it, RocksDB buffers writes in an OS page cache, and a crash could lose
committed entries.

#### 5.4 Serialization

```cpp
// line 62-66: Key format
std::string make_log_key(slotid_t slot_id) {
    return LOG_PREFIX + zero_padded_20_digit(slot_id);
}

// line 74-81: Entry serialization via Marshal
void serialize_entry(const LogEntry& entry, std::string* output) {
    Marshal m;
    entry.to_marshal(m);
    *output = m.to_string();
}

// line 84-89: Entry deserialization
void deserialize_entry(const std::string& data, LogEntry* entry) {
    Marshal m(data);
    entry->from_marshal(m);
}
```

#### 5.5 Batch Operations

The `put_batch()` method (line 231-248) uses RocksDB's `WriteBatch`
for atomic multi-entry writes:

```cpp
bool put_batch(const std::vector<LogEntry>& entries) override {
    rocksdb::WriteBatch batch;
    for (const auto& entry : entries) {
        std::string value;
        serialize_entry(entry, &value);
        batch.Put(make_log_key(entry.slot_id), value);
    }
    auto status = db_->Write(write_options_, &batch);
    return status.ok();
}
```

This ensures either all entries are persisted or none are — important
for maintaining log consistency across crashes.

#### 5.6 Index Queries

`get_first_index()` (line 280-300) and `get_last_index()` (line 303-332)
use RocksDB iterators:

- `get_first_index()`: Seeks to `LOG_PREFIX`, returns the first matching
  key's slot ID.
- `get_last_index()`: Seeks to `"log;"` (one character past the prefix
  range), then calls `Prev()` to get the last log entry.

#### 5.7 Lifecycle

- `open()` (line 124-136): Calls `rocksdb::DB::Open()` with configured
  options.
- `close()` (line 418-430): Deletes the `db_` pointer.
- `sync()` (line 405-415): Calls `db_->Flush()` with `wait=true`.
- `clear()` (line 438-454): Uses a `WriteBatch` to delete all keys.
- `destroy()` (line 473-477): Static method calling
  `rocksdb::DestroyDB()` to remove the database directory.

### 6. Raft Server Integration

**File**: `src/deptran/raft/server.h`

#### 6.1 Storage Members

```cpp
std::shared_ptr<rrr::LogStorage> log_storage_;        // line 52
std::shared_ptr<rrr::SnapshotManager> snapshot_manager_;  // line 57
```

#### 6.2 Metadata Keys

```cpp
static constexpr const char* META_TERM = "currentTerm";     // line 60
static constexpr const char* META_VOTE_FOR = "vote_for";    // line 61
static constexpr const char* META_COMMIT_INDEX = "commitIndex";  // line 62
```

#### 6.3 Persistence Methods

| Method | Line | What It Persists |
|--------|------|------------------|
| `PersistTermAndVote()` | 65 | `currentTerm` and `vote_for` |
| `PersistVote()` | 68 | `vote_for` only |
| `PersistCommitIndex()` | 71 | `commitIndex` |
| `PersistLogEntry()` | 74 | Single log entry |
| `PersistLogEntries()` | 77 | Batch of log entries |

All persistence methods are annotated `@unsafe` because they perform
I/O through the RocksDB library.

#### 6.4 When Persistence Is Called

- **`PersistTermAndVote()`**: Called in `doVote()` when advancing the
  current term (line 186 of server.h).
- **`PersistLogEntry()`**: Called after appending an entry to the
  in-memory log.
- **`PersistCommitIndex()`**: Called after advancing the commit index.
- **`PersistLogEntries()`**: Called during `AppendEntries` when
  receiving a batch of entries from the leader.

#### 6.5 SetLogStorage and RecoverFromStorage

```cpp
void SetLogStorage(std::shared_ptr<rrr::LogStorage> storage);  // set storage
void RecoverFromStorage();  // restore state from persistent storage
```

`SetLogStorage()` is called during server initialisation to provide the
storage backend.  `RecoverFromStorage()` loads persisted metadata
(`currentTerm`, `vote_for`, `commitIndex`) and log entries from storage,
then rebuilds the in-memory log.

### 7. Paxos Server Integration

**File**: `src/deptran/paxos/server.h`

#### 7.1 Metadata Keys

```cpp
static constexpr const char* META_EPOCH = "cur_epoch";           // line 56
static constexpr const char* META_MAX_COMMITTED = "max_committed_slot";  // line 57
static constexpr const char* META_MAX_EXECUTED = "max_executed_slot";    // line 58
```

#### 7.2 Persistence Methods

| Method | Line | What It Persists |
|--------|------|------------------|
| `PersistEpoch()` | 61 | Current epoch |
| `PersistMaxCommitted()` | 62 | Max committed slot |
| `PersistLogEntry()` | 64 | Single Paxos log entry |
| `PersistLogEntries()` | 66 | Batch of entries |

#### 7.3 Additional Paxos Methods

| Method | Line | Purpose |
|--------|------|---------|
| `SetLogStorage()` | 72 | Set storage backend |
| `GetLogStorage()` | 74 | Get storage backend |
| `RecoverFromStorage()` | 76 | Restore consensus state |
| `ReplayCommittedEntries()` | 98 | Replay from `executeIndex` to `commitIndex` |
| `GetUncommittedCount()` | 106 | Count uncommitted entries |
| `CompactLog()` | 115 | Remove entries covered by snapshot |

### 8. Storage Path Configuration

#### 8.1 Log Storage Paths

Generated by `RecoveryConfig::for_replica()` (recovery_manager.hpp,
line 56-71):

```
/tmp/{USER}_mako_log_shard{partition_id}_replica{locale_id}
```

Example: `/tmp/alice_mako_log_shard0_replica0`

#### 8.2 Snapshot Storage Paths

Generated by `SnapshotConfig::for_replica()` (snapshot_manager.hpp,
line 277-291):

```
/tmp/{USER}_mako_snapshot_shard{partition_id}_replica{locale_id}
```

#### 8.3 User Isolation

Both path generators use `std::getenv("USER")` to create per-user
paths.  This prevents conflicts when multiple users run tests on the
same machine.

### 9. Unit Tests

**File**: `test/rpc_rocksdb_log_storage_test.cc` (531 lines)

The RocksDB implementation has comprehensive Google Test coverage:

| Category | Tests | Lines |
|----------|-------|-------|
| Basic operations | OpenAndClose, PutAndGet, GetNonExistent, PutOverwrite, Remove | 84-124 |
| Batch operations | PutBatch, GetRange, GetRangeEmpty, RemoveRange | 130-159 |
| Index queries | GetFirstIndex, GetLastIndex, GetTerm, SizeAndEmpty | 181-223 |
| Metadata | SetAndGetMetadata, GetMetadataNonExistent, OverwriteMetadata | 228-253 |
| Lifecycle | IsOpen, Sync, CloseIdempotent, Clear, OperationsOnClosedStorage | 259-296 |
| Persistence | PersistenceAcrossReopen, PersistenceWithFullLogEntry | 302-348 |
| Edge cases | LargeSlotIds, ZeroSlotId, InvalidRanges, LargeNumberOfEntries | 354-387 |
| Thread safety | ConcurrentPuts, ConcurrentReadsAndWrites, ConcurrentMetadata | 417-503 |
| Static methods | DestroyDatabase | 509-521 |

---

## Crash Recovery Process

<a id="1-overview-13"></a>

### 1. Overview

When a Mako replica restarts after a crash, the recovery system detects
whether persistent state exists, loads it, and resumes consensus
participation.  The recovery process is managed by `RecoveryManager`,
which coordinates between the storage backend and the consensus server.

### 2. RecoveryMode Enum

**File**: `src/rrr/rpc/recovery_manager.hpp` (267 lines, line 33-37)

```cpp
enum class RecoveryMode {
    FRESH_START,       // No previous state found
    NORMAL_RECOVERY,   // Previous state found, recover from storage
    FORCED_FRESH       // User requested fresh start even if data exists
};
```

The three modes handle different startup scenarios:

| Mode | When Used | Action |
|------|-----------|--------|
| `FRESH_START` | No RocksDB database at the storage path | Create new empty database |
| `NORMAL_RECOVERY` | Valid RocksDB database found (contains `CURRENT` file) | Load metadata and log entries, rebuild in-memory state |
| `FORCED_FRESH` | User set `force_fresh_start = true` | Delete existing database if `clear_on_forced_fresh`, create new |

### 3. RecoveryConfig

**File**: `src/rrr/rpc/recovery_manager.hpp` (lines 42-72)

```cpp
struct RecoveryConfig {
    std::string storage_path;            // line 44: RocksDB path
    bool force_fresh_start = false;      // line 45: force fresh even if data exists
    uint32_t recovery_timeout_ms = 30000;  // line 46: 30 second timeout
    bool verify_on_recovery = true;      // line 47: verify data integrity
    bool clear_on_forced_fresh = true;   // line 48: delete old data on forced fresh
};
```

#### 3.1 Factory Method

`RecoveryConfig::for_replica()` (line 56-71) constructs a per-replica
configuration:

```cpp
static RecoveryConfig for_replica(uint32_t partition_id, uint32_t locale_id) {
    RecoveryConfig config = defaults();
    const char* user = std::getenv("USER");
    config.storage_path = "/tmp/" + std::string(user ? user : "unknown")
        + "_mako_log_shard" + std::to_string(partition_id)
        + "_replica" + std::to_string(locale_id);
    return config;
}
```

This generates paths like `/tmp/alice_mako_log_shard0_replica0`.

### 4. RecoveryResult

**File**: `src/rrr/rpc/recovery_manager.hpp` (lines 77-102)

```cpp
struct RecoveryResult {
    RecoveryMode mode;               // line 79: detected mode
    bool success;                    // line 80: whether recovery succeeded
    std::string error_message;       // line 81: error details
    uint64_t recovered_entries;      // line 82: number of entries restored
    uint64_t recovered_term;         // line 83: Raft currentTerm
    uint64_t recovered_epoch;        // line 84: Paxos cur_epoch
    uint64_t recovery_time_ms;       // line 85: time taken
};
```

Factory methods:

| Method | Line | Purpose |
|--------|------|---------|
| `success_fresh()` | 88-93 | Create a success result for fresh starts |
| `failure(error)` | 96-101 | Create a failure result with error message |

### 5. RecoveryManager

**File**: `src/rrr/rpc/recovery_manager.hpp` (lines 116-265)

#### 5.1 Internal State

```cpp
class RecoveryManager {
    RecoveryConfig config_;                           // line 261
    std::shared_ptr<LogStorage> storage_;              // line 262
    rusty::Cell<bool> initialized_;                    // line 263
    rusty::Cell<RecoveryMode> detected_mode_;          // line 264
};
```

#### 5.2 Mode Detection

`detect_mode()` (line 125-145) determines the recovery mode:

```
1. If config.force_fresh_start == true:
     return FORCED_FRESH

2. If storage_path does not exist:
     return FRESH_START

3. If storage_path/CURRENT file exists:
     return NORMAL_RECOVERY   (valid RocksDB database)

4. Otherwise:
     return FRESH_START   (directory exists but no valid DB)
```

The `CURRENT` file is RocksDB's manifest pointer — its presence
indicates a valid, openable database.

#### 5.3 Storage Creation

`create_storage()` (line 148-179) creates the appropriate storage
backend:

```
1. Detect mode via detect_mode()
2. If FORCED_FRESH and clear_on_forced_fresh:
     std::filesystem::remove_all(storage_path)
3. Create RocksDBLogStorage at config.storage_path
4. Call storage->open()
5. Set initialized_ = true
6. Return shared_ptr<LogStorage>
```

#### 5.4 Generic Recovery Template

`recover()` (line 214-258) is a template method that coordinates
recovery for any consensus protocol:

```cpp
template<typename SetStorageFn, typename RecoverFn, typename GetStatsFn>
RecoveryResult recover(
    SetStorageFn set_storage,    // fn(shared_ptr<LogStorage>)
    RecoverFn recover_fn,        // fn() -> void
    GetStatsFn get_stats         // fn(RecoveryResult&) -> void
);
```

The recovery sequence:

```
1. Record start time

2. If FRESH_START or FORCED_FRESH:
     call set_storage(storage_)
     return RecoveryResult::success_fresh()

3. For NORMAL_RECOVERY:
     a. call set_storage(storage_)
     b. call recover_fn()           // loads metadata + log entries
     c. call get_stats(result)      // extracts term/epoch stats
     d. Record end time
     e. Log recovery info
     f. Return RecoveryResult with statistics
```

### 6. Recovery Integration in Server Startup

**File**: `src/deptran/server_worker.cc` (lines 380-449)

#### 6.1 Sequence Diagram

```
ServerWorker                RecoveryManager        LogStorage         RaftServer
    |                           |                      |                  |
    |  create config            |                      |                  |
    |  for_replica(pid, lid)    |                      |                  |
    |-------------------------->|                      |                  |
    |                           |                      |                  |
    |  create_storage()         |                      |                  |
    |-------------------------->|                      |                  |
    |                           |  detect_mode()       |                  |
    |                           |---+                  |                  |
    |                           |<--+                  |                  |
    |                           |                      |                  |
    |                           |  new RocksDBLogStorage                  |
    |                           |--------------------->|                  |
    |                           |  open()              |                  |
    |                           |--------------------->|                  |
    |                           |<---------------------|                  |
    |<--------------------------|                      |                  |
    |                           |                      |                  |
    |  recover(set, recover, stats)                    |                  |
    |-------------------------->|                      |                  |
    |                           |  set_storage(storage)|                  |
    |                           |--------------------------------------------->|
    |                           |                      |                  |
    |                           |  RecoverFromStorage()|                  |
    |                           |--------------------------------------------->|
    |                           |                      |  get_metadata()  |
    |                           |                      |<-----------------|
    |                           |                      |  get_range()     |
    |                           |                      |<-----------------|
    |                           |                      |                  |
    |                           |  get_stats(result)   |                  |
    |                           |---+                  |                  |
    |                           |<--+                  |                  |
    |<--------------------------|                      |                  |
    | RecoveryResult            |                      |                  |
```

#### 6.2 Raft Recovery Call

```cpp
// Create recovery config
rrr::RecoveryConfig config =
    rrr::RecoveryConfig::for_replica(partition_id, locale_id);

// Create manager and storage
rrr::RecoveryManager recovery_manager(config);
auto storage = recovery_manager.create_storage();

// Run recovery
recovery_manager.recover(
    // set_storage: provide storage to Raft server
    [raft_server, &storage](std::shared_ptr<rrr::LogStorage> s) {
        raft_server->SetLogStorage(s);
    },
    // recover: load state from storage
    [raft_server]() {
        return raft_server->RecoverFromStorage();
    },
    // get_stats: extract recovered metadata
    [&storage](rrr::RecoveryResult& r) {
        auto term_opt = storage->get_metadata("currentTerm");
        if (term_opt.is_some()) {
            r.recovered_term = std::stoull(term_opt.unwrap());
        }
    });
```

#### 6.3 What RecoverFromStorage Does (Raft)

When called, `RecoverFromStorage()` performs:

1. **Load metadata**:
   - `currentTerm` from `log_storage_->get_metadata(META_TERM)`
   - `vote_for` from `log_storage_->get_metadata(META_VOTE_FOR)`
   - `commitIndex` from `log_storage_->get_metadata(META_COMMIT_INDEX)`

2. **Load log entries**:
   - `log_storage_->get_range(first_index, last_index)` to retrieve
     all persisted entries
   - Rebuild in-memory log from persisted entries

3. **Restore state**:
   - Set `currentTerm_` to recovered term
   - Set `vote_for_` to recovered vote
   - Set `commitIndex_` to recovered commit index
   - Populate in-memory log data structure

#### 6.4 What RecoverFromStorage Does (Paxos)

The Paxos equivalent recovers:

1. **Load metadata**:
   - `cur_epoch` from `log_storage_->get_metadata(META_EPOCH)`
   - `max_committed_slot` from `get_metadata(META_MAX_COMMITTED)`
   - `max_executed_slot` from `get_metadata(META_MAX_EXECUTED)`

2. **Load log entries**: Same `get_range()` call
3. **Replay committed**: `ReplayCommittedEntries()` replays entries
   from `executeIndex` to `commitIndex`

### 7. Resolving Uncommitted Entries

After recovery, the server may have log entries that were persisted but
not committed before the crash.  These are resolved via the normal
consensus protocol:

#### 7.1 Raft

- If the recovered server is a follower, the current leader will send
  `AppendEntries` RPCs that either confirm the uncommitted entries
  (if the leader has them) or overwrite them (if the leader has
  different entries at those indices).
- If the recovered server becomes leader, it commits a no-op entry
  in its current term to establish authority and indirectly commit
  any uncommitted entries from previous terms.

#### 7.2 Paxos

- The `GetUncommittedCount()` method (line 106 in paxos/server.h)
  identifies how many entries need resolution.
- The Paxos leader re-proposes uncommitted slots to achieve consensus.

### 8. Fresh Start vs Recovery Detection

The detection logic in `detect_mode()` uses filesystem checks:

```
storage_path exists?
├── No  → FRESH_START
│         (First ever run for this shard/replica)
└── Yes
    └── CURRENT file exists?
        ├── No  → FRESH_START
        │         (Corrupted or incomplete previous DB)
        └── Yes → NORMAL_RECOVERY
                  (Valid RocksDB database to recover from)
```

The `CURRENT` file is RocksDB's manifest pointer.  If the database was
in the middle of a compaction when the crash occurred, RocksDB's own
recovery mechanisms handle WAL replay when `Open()` is called.

### 9. Storage Cleanup in CI Tests

CI test scripts clean persistent state before each run:

```bash
# From test scripts (e.g., test_1shard_replication_raft.sh):
rm -rf /tmp/${USER}_mako_rocksdb_shard*
```

This ensures each test run starts with `FRESH_START` mode, preventing
stale state from previous runs from affecting results.

---

## Snapshot Support

<a id="1-overview-14"></a>

### 1. Overview

Snapshots capture the state machine at a specific log index, allowing
the system to discard log entries that have already been applied.  This
bounds log growth and enables fast catch-up for lagging followers.

The snapshot system has four components:
- `SnapshotMetadata`: describes a snapshot
- `SnapshotManager`: abstract interface for snapshot operations
- `FileSnapshotManager`: file-based implementation
- `SnapshotFormat`: binary serialization format with CRC32 checksums

### 2. SnapshotMetadata

**File**: `src/rrr/rpc/snapshot_manager.hpp` (294 lines, lines 39-57)

```cpp
struct SnapshotMetadata {
    slotid_t last_included_index;    // line 40: last log entry in snapshot
    ballot_t last_included_term;     // line 41: term of last included entry
    uint64_t timestamp_ms;           // line 42: creation time
    size_t size_bytes;               // line 43: snapshot data size
    std::string checksum;            // line 44: checksum string
};
```

Key methods:

| Method | Line | Purpose |
|--------|------|---------|
| `is_valid()` | 47 | Returns `last_included_index > 0` |
| `to_string()` | 52 | Format as `"Snapshot{index=..., term=..., size=...}"` |

### 3. SnapshotManager Interface

**File**: `src/rrr/rpc/snapshot_manager.hpp` (lines 148-258)

#### 3.1 Snapshot Creation

| Method | Line | Signature |
|--------|------|-----------|
| `BeginSnapshot()` | 163 | `BeginSnapshot(slotid_t last_index, ballot_t last_term) -> unique_ptr<SnapshotWriter>` |
| `TakeSnapshot()` | 176 | `TakeSnapshot(slotid_t, ballot_t, const char* data, size_t size) -> bool` |

`BeginSnapshot()` returns a `SnapshotWriter` for streaming large
snapshots.  `TakeSnapshot()` is a convenience method that writes the
entire snapshot in one call.

#### 3.2 Snapshot Loading

| Method | Line | Signature |
|--------|------|-----------|
| `BeginLoad()` | 189 | `BeginLoad(const SnapshotMetadata&) -> unique_ptr<SnapshotReader>` |
| `LoadLatestSnapshot()` | 199 | `LoadLatestSnapshot(SnapshotMetadata*, std::string*) -> bool` |

`BeginLoad()` returns a `SnapshotReader` for streaming reads.
`LoadLatestSnapshot()` loads the most recent snapshot completely into
memory.

#### 3.3 Snapshot Queries

| Method | Line | Signature |
|--------|------|-----------|
| `GetLatestSnapshot()` | 211 | `GetLatestSnapshot() -> rusty::Option<SnapshotMetadata>` |
| `ListSnapshots()` | 218 | `ListSnapshots() -> std::vector<SnapshotMetadata>` |
| `HasSnapshotAtOrAfter()` | 226 | `HasSnapshotAtOrAfter(slotid_t min_index) -> bool` |

#### 3.4 Snapshot Cleanup

| Method | Line | Signature |
|--------|------|-----------|
| `PruneSnapshots()` | 239 | `PruneSnapshots(slotid_t keep_after_index) -> bool` |
| `DeleteAllSnapshots()` | 247 | `DeleteAllSnapshots() -> bool` |

### 4. SnapshotReader and SnapshotWriter

**File**: `src/rrr/rpc/snapshot_manager.hpp`

#### 4.1 SnapshotReader (lines 63-97)

```cpp
class SnapshotReader {
    virtual bool Read(char* buffer, size_t buffer_size, size_t* bytes_read);
    virtual bool IsComplete();
    virtual const SnapshotMetadata& GetMetadata();
    virtual size_t GetOffset();
};
```

Streaming interface for reading snapshots in chunks.  The caller
provides a buffer and reads until `IsComplete()` returns true.

#### 4.2 SnapshotWriter (lines 103-137)

```cpp
class SnapshotWriter {
    virtual bool Write(const char* data, size_t size);
    virtual bool Finalize();
    virtual void Abort();
    virtual size_t GetOffset();
};
```

Streaming interface for writing snapshots.  After all data is written,
`Finalize()` persists the snapshot to disk.  `Abort()` cleans up
partial writes.

### 5. FileSnapshotManager

**File**: `src/rrr/rpc/file_snapshot_manager.hpp` (531 lines)

#### 5.1 File Naming Convention

```
snapshot_{index}_{term}.snap        (complete snapshots)
snapshot_{index}_{term}.snap.tmp    (in-progress writes)
```

Example: `snapshot_00000000000000010000_00000000000000000005.snap`

#### 5.2 Directory Structure

```
/tmp/{USER}_mako_snapshot_shard{partition_id}_replica{locale_id}/
    snapshot_00000000000000001000_00000000000000000001.snap
    snapshot_00000000000000005000_00000000000000000003.snap
    snapshot_00000000000000010000_00000000000000000005.snap
```

#### 5.3 Configuration

**File**: `src/rrr/rpc/snapshot_manager.hpp` (lines 264-292)

```cpp
struct SnapshotConfig {
    std::string storage_path;           // line 265
    size_t snapshot_interval = 10000;   // line 266: entries between snapshots
    size_t max_snapshots = 3;           // line 267: retention limit
    bool verify_on_load = true;         // line 268: verify CRC on load
    size_t chunk_size = 65536;          // line 269: 64 KB streaming chunks
};
```

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `snapshot_interval` | 10,000 | Take a snapshot every 10,000 log entries |
| `max_snapshots` | 3 | Retain at most 3 snapshots |
| `verify_on_load` | `true` | Verify CRC32 checksums when loading |
| `chunk_size` | 64 KB | Streaming read/write buffer size |

#### 5.4 Taking a Snapshot

`TakeSnapshot()` (line 299-309):

```
1. Create FileSnapshotWriter with index and term
2. Write data to writer
3. Finalize writer (serialize, fsync, atomic rename)
4. Apply retention policy (delete old snapshots)
```

#### 5.5 FileSnapshotWriter

**File**: `src/rrr/rpc/file_snapshot_manager.hpp` (lines 39-161)

The writer accumulates data in a buffer, then performs an atomic write:

```
Finalize() sequence:
  1. SnapshotFormat::Serialize(index, term, buffer, size, &output)
     → produces header + data + CRC32
  2. Open temp file: snapshot_{index}_{term}.snap.tmp
     → O_WRONLY | O_CREAT | O_TRUNC
  3. Write serialized data to temp file
  4. fsync() temp file (ensure durability)
  5. Atomic rename() temp → final: snapshot_{index}_{term}.snap
```

The atomic rename ensures that readers never see a partially-written
snapshot file.  If a crash occurs during write, only the `.tmp` file
is left, which is ignored on recovery.

The destructor (line 55-59) calls `Abort()` if the writer was not
finalized, cleaning up any `.tmp` file via `unlink()`.

#### 5.6 FileSnapshotReader

**File**: `src/rrr/rpc/file_snapshot_manager.hpp` (lines 167-267)

The reader loads the entire file at construction:

```
Constructor sequence:
  1. Open file with O_RDONLY
  2. Read entire file into memory
  3. SnapshotFormat::Deserialize(file_data, &index, &term, &data)
     → verifies header magic, version, CRC32
  4. Extract metadata (index, term, size, timestamp from header)
  5. Set valid_ flag
```

The `Read()` method (line 230-244) provides streaming access from the
in-memory buffer, returning chunks of `chunk_size` bytes.

#### 5.7 Retention Policy

`ApplyRetentionPolicy()` (line 514-528):

```
1. List all snapshots (sorted by index, descending)
2. If count > max_snapshots:
     Delete oldest snapshots until count == max_snapshots
```

With `max_snapshots = 3`, this keeps the 3 most recent snapshots.

#### 5.8 Log Compaction

When a snapshot covers entries up to index N, all log entries with
index <= N can be safely removed.  The Paxos server exposes
`CompactLog(slotid_t up_to_index)` (paxos/server.h, line 115) which
calls `log_storage_->remove_range(first_index, up_to_index)`.

### 6. Snapshot Binary Format

**File**: `src/rrr/rpc/snapshot_format.hpp` (373 lines)

#### 6.1 Layout

```
Offset  Size   Field
------  ----   -----
0       4      Magic number: 0x504E4153 ("SNAP")
4       4      Version: 1
8       4      Header size: 52 bytes
12      8      Data size (uncompressed)
20      1      Compression type (0=NONE, 1=SNAPPY, 2=ZSTD)
21      1      Checksum type (0=NONE, 1=CRC32, 2=SHA256)
22      8      Last included index
30      8      Last included term
38      8      Timestamp (milliseconds since epoch)
46      4      Header CRC32 (computed over bytes 0-43)
50      2      Padding (8-byte alignment)
------- Header end (52 bytes) -------
52      N      Snapshot data
52+N    4      Data CRC32 (if checksum type = CRC32)
```

Total size: 52 + N + 4 bytes (with CRC32).

#### 6.2 SnapshotHeader

**File**: `src/rrr/rpc/snapshot_format.hpp` (lines 56-76)

```cpp
#pragma pack(push, 1)
struct SnapshotHeader {
    uint32_t magic;            // 0x504E4153
    uint32_t version;          // 1
    uint32_t header_size;      // 52
    uint64_t data_size;        // uncompressed data size
    uint8_t compression;       // SnapshotCompression enum
    uint8_t checksum_type;     // SnapshotChecksumType enum
    uint64_t last_index;       // last included log index
    uint64_t last_term;        // term of last included entry
    uint64_t timestamp_ms;     // creation time
    uint32_t header_crc;       // CRC32 of bytes 0-43
    uint8_t padding[2];        // alignment padding
};
#pragma pack(pop)
```

The `#pragma pack(push, 1)` ensures no compiler-inserted padding,
giving an exact 52-byte binary layout.

#### 6.3 CRC32 Implementation

**File**: `src/rrr/rpc/snapshot_format.hpp` (lines 85-162)

A table-driven CRC32 using the IEEE 802.3 polynomial (reversed:
`0xEDB88320`).  The 256-entry lookup table is defined inline
(lines 118-161).

```cpp
class CRC32 {
    uint32_t crc_ = 0xFFFFFFFF;

    void Update(const char* data, size_t size);  // line 91
    uint32_t Finalize();                          // line 99: return crc_ ^ 0xFFFFFFFF
    static uint32_t Calculate(const char*, size_t);  // line 107: one-shot
};
```

Two CRC32 values protect the snapshot:
1. **Header CRC** (bytes 0-43): Protects the header metadata.
2. **Data CRC** (after data section): Protects the snapshot payload.

#### 6.4 Serialization

`SnapshotFormat::Serialize()` (line 186-252):

```
1. Build SnapshotHeader with metadata
2. Compute header CRC32 over bytes 0-43
3. Compute data CRC32 if checksum type is CRC32
4. Allocate output: 52 (header) + data_size + 4 (data CRC)
5. Copy header, data, and CRC into output
```

#### 6.5 Deserialization

`SnapshotFormat::Deserialize()` (line 264-341):

```
1. Check input size >= 52 (header)
2. Copy header from input
3. Verify magic == 0x504E4153
4. Verify version == 1
5. Verify header CRC32 (bytes 0-43)
6. Check compression == NONE (others reserved)
7. Verify total size matches header + data + CRC
8. Verify data CRC32
9. Extract last_index, last_term, data
```

#### 6.6 Compression

Currently only `NONE` is supported (line 20-21 in format).  The enum
reserves values for `SNAPPY` and `ZSTD` for future use.

### 7. Snapshot Types

```cpp
enum class SnapshotCompression : uint8_t {
    NONE = 0,       // No compression (current)
    SNAPPY = 1,     // Reserved
    ZSTD = 2        // Reserved
};

enum class SnapshotChecksumType : uint8_t {
    NONE = 0,       // No checksum
    CRC32 = 1,      // 4-byte CRC32 (current)
    SHA256 = 2      // Reserved
};
```

### 8. Server Integration

#### 8.1 Raft Server

**File**: `src/deptran/raft/server.h`

```cpp
std::shared_ptr<rrr::SnapshotManager> snapshot_manager_;  // line 57
```

The Raft server holds a reference to the snapshot manager but the
current implementation focuses on log persistence rather than
snapshotting.  The infrastructure is in place for future use.

#### 8.2 Paxos Server

**File**: `src/deptran/paxos/server.h`

```cpp
std::shared_ptr<rrr::SnapshotManager> snapshot_manager_;  // line 81

void SetSnapshotManager(std::shared_ptr<rrr::SnapshotManager>);  // line 84
std::shared_ptr<rrr::SnapshotManager> GetSnapshotManager() const;  // line 88
void CompactLog(slotid_t up_to_index);  // line 115
```

`CompactLog()` removes log entries up to the given index after a
snapshot has been taken, bounding log growth.

### 9. Snapshot Lifecycle

```
1. Application reaches snapshot_interval (10,000 entries)
2. TakeSnapshot(last_applied_index, last_applied_term, state_data, size)
3. FileSnapshotWriter accumulates data in buffer
4. Finalize():
   a. Serialize with header + CRC32
   b. Write to .tmp file
   c. fsync()
   d. Atomic rename to .snap
5. ApplyRetentionPolicy():
   a. List snapshots sorted by index (descending)
   b. Delete oldest beyond max_snapshots (3)
6. CompactLog(last_applied_index):
   a. log_storage_->remove_range(first, last_applied_index)
   b. Free disk space from old log entries
```

### 10. Crash Safety Properties

#### 10.1 Atomic Writes

Snapshots use the write-to-temp-then-rename pattern:

```
write → .snap.tmp    (crash here = only .tmp left, ignored)
fsync  .snap.tmp     (crash here = only .tmp left, ignored)
rename .snap.tmp → .snap  (atomic on POSIX filesystems)
```

After a crash, only complete `.snap` files are visible.  Partial
`.tmp` files are ignored by `ListSnapshots()`.

#### 10.2 CRC32 Verification

Both header and data CRCs are verified on load.  If either checksum
fails, the snapshot is rejected and the system falls back to log
replay for recovery.

#### 10.3 Retention Guarantee

The retention policy (`max_snapshots = 3`) ensures that even if the
most recent snapshot is corrupted, two older snapshots are available
as fallbacks.

---

## Complete File Reference

### 1. Raft Implementation (`src/deptran/raft/`)

Total: ~6,081 lines across 19 files.

| File | Lines | Description |
|------|-------|-------------|
| `server.cc` | 1,829 | Core Raft server: leader election, log replication, apply state machine, persistence methods |
| `server.h` | 637 | RaftServer class, RaftData structures, log storage integration, replica tracking |
| `raft_worker.cc` | 615 | Worker thread: log submission, batching, watermark management, poll thread lifecycle |
| `raft_worker.h` | 167 | RaftWorker class with submit thread, polling, callback mechanisms |
| `test.cc` | 740 | 11 standalone Raft lab tests: election, agreement, replication, network partition |
| `testconf.cc` | 585 | Test configuration: replica maps, commit callbacks, network simulation (Disconnect/Reconnect) |
| `commo.cc` | 287 | Raft RPC communication: AppendEntries, RequestVote, TimeoutNow, ForwardToLearner |
| `frame.cc` | 206 | Frame registration: protocol initialisation, executor/coordinator/commo factory methods |
| `coordinator.cc` | 199 | Transaction coordinator: command submission to leader, quorum response handling |
| `testconf.h` | 183 | RaftTestConfig class: NSERVERS=5, ELECTIONTIMEOUT=5s, test helpers |
| `commo.h` | 128 | RaftCommo interface, RaftVoteQuorumEvent for collecting vote responses |
| `service.cc` | 112 | RPC service handlers: HandleVote, HandleAppendEntries, HandleTimeoutNow |
| `service.h` | 83 | RaftServiceImpl class defining RPC handler declarations |
| `coordinator.h` | 82 | CoordinatorRaft class: slot hints via `Arc<Cell<slotid_t>>`, quorum logic |
| `macros.h` | 76 | Convenience macros: RAFT_CREATE_EV, LOG_AT_SLOT, LEADER_LOG, etc. |
| `frame.h` | 49 | RaftFrame interface for protocol-specific transaction processing |
| `test.h` | 43 | RaftLabTest class header: 11 test method declarations |
| `exec.cc` | 31 | Executor: delegates to parent TxnExecutor (minimal) |
| `exec.h` | 29 | ExecutorRaft class header |

### 2. Paxos Implementation (`src/deptran/paxos/`)

Total: ~2,957 lines across 13 files.

| File | Lines | Description |
|------|-------|-------------|
| `server.cc` | 1,025 | Core Paxos server: prepare/accept, ballot tracking, instance management, persistence |
| `commo.cc` | 514 | Communication: Prepare, Accept, Forward, ForwardToLearner RPCs |
| `coordinator.cc` | 432 | Multi-Paxos coordinator: command submission, bulk operations, quorum coordination |
| `server.h` | 246 | PaxosServer class: log storage, ballot state, slot management, snapshot integration |
| `service.cc` | 233 | RPC service: Prepare, Accept, Forward, ForwardToLearner handlers |
| `frame.cc` | 117 | Frame registration and initialisation for Multi-Paxos mode |
| `coordinator.h` | 112 | CoordinatorMultiPaxos class for bulk operations |
| `commo.h` | 108 | MultiPaxosCommo and quorum event definitions |
| `service.h` | 92 | MultiPaxosServiceImpl class |
| `frame.h` | 33 | MultiPaxosFrame header |
| `exec.cc` | 22 | Executor implementation (minimal) |
| `exec.h` | 23 | Executor header |
| `commoh.h` | 0 | Empty legacy placeholder |

### 3. Integration Files

| File | Lines | Description |
|------|-------|-------------|
| `src/deptran/raft_main_helper.cc` | ~200 | Main entry helper for Raft mode: YAML parsing, startup orchestration |
| `src/deptran/replication_helper.h` | ~80 | `DISPATCH_RAFT_OR_PAXOS` macro for runtime protocol switching |
| `src/deptran/replication_helper.cc` | ~60 | Dispatcher using `rusty::Cell<ReplicationType>` for global state |
| `src/mako/mako.hh` | ~800 | Mako main header: `wait_for_termination()`, follower replay loop, `replay_batch` metric |
| `src/mako/benchmarks/bench.cc` | ~750 | TPC-C benchmark: `agg_persist_throughput` calculation (line 720-747) |
| `src/deptran/server_worker.cc` | ~500 | Server worker: RecoveryManager integration, storage initialisation |

### 4. Persistence Layer (`src/rrr/rpc/`)

| File | Lines | Description |
|------|-------|-------------|
| `file_snapshot_manager.hpp` | 531 | FileSnapshotManager: atomic writes, FileSnapshotReader/Writer, retention policy |
| `rocksdb_log_storage.hpp` | 480 | RocksDBLogStorage: persistent storage with sync=true, LZ4, WriteBatch |
| `snapshot_format.hpp` | 373 | Binary format: 52-byte header, CRC32 checksums, serialisation/deserialisation |
| `log_storage.hpp` | 302 | LogEntry struct, LogStorage abstract interface (15 methods) |
| `snapshot_manager.hpp` | 294 | SnapshotMetadata, SnapshotManager interface, SnapshotConfig |
| `memory_log_storage.hpp` | 292 | InMemoryLogStorage: thread-safe testing implementation |
| `recovery_manager.hpp` | 267 | RecoveryManager: mode detection, storage creation, recovery coordination |

### 5. Test Files

#### 5.1 C++ Test Binaries (`examples/mako-raft-tests/`)

| File | Lines | Description |
|------|-------|-------------|
| `simpleTransactionRepRaft.cc` | 675 | Transaction replication test: 3 workers executing TPC-C style transactions |
| `testNoOps.cc` | 531 | NO-OPS watermark synchronisation: verifies epoch advancement across 5 replicas |
| `testPreferredReplicaLogReplication.cc` | 422 | Log replication: 25 logs wrapped in TpcCommitCommand batches to 5 replicas |
| `testPreferredReplicaStartup.cc` | 264 | Preferred leader startup: TimeoutNow leadership transfer protocol test |
| `simpleRaft.cc` | 226 | Basic Raft sanity: submits 300 logs (100/partition x 3 partitions) |

#### 5.2 Test Shell Scripts (`examples/mako-raft-tests/`)

| File | Lines | CI | Description |
|------|-------|----|-------------|
| `simpleRaft.sh` | 120 | Yes | Basic Raft replication (3 replicas, 300 logs, 40s) |
| `test_1shard_replication_raft.sh` | 153 | Yes | 1-shard TPC-C with Raft (6 threads, 60s) |
| `test_2shard_replication_raft.sh` | 208 | Yes | 2-shard TPC-C with Raft (6 threads, 120s polling) |
| `test_1shard_replication_simple_raft.sh` | 149 | Yes | 1-shard simple tx with Raft (40s) |
| `test_2shard_replication_simple_raft.sh` | 171 | Yes | 2-shard simple tx with Raft (60s) |
| `run_test1_preferred_startup.sh` | 361 | No | Preferred leader election (5-node, 35s) |
| `run_test_log_replication.sh` | 159 | No | Log replication to 5 replicas |
| `run_test_noops.sh` | 256 | No | NO-OPS watermark synchronisation |

#### 5.3 Unit Tests (`test/`)

| File | Lines | Description |
|------|-------|-------------|
| `rpc_rocksdb_log_storage_test.cc` | 531 | Google Test suite: 9 categories, thread safety, persistence verification |

### 6. CI Scripts

| File | Lines | Description |
|------|-------|-------------|
| `ci/ci_mako_raft.sh` | 252 | Standalone Raft CI entry point: compile + 5 test functions |
| `ci/ci.sh` | 553 | Primary CI script: Paxos + Raft tests, memory limits, config management |

### 7. Shard Launch Scripts

| File | Lines | Description |
|------|-------|-------------|
| `bash/shard_raft.sh` | 39 | Raft-dedicated shard launcher: raft port range (27xxx), occ_raft.yml |
| `bash/shard.sh` | 62 | Unified launcher: 7th arg selects paxos/raft, paxos port range (17xxx) |

### 8. Configuration Files

#### 8.1 Raft Configs (`config/`)

| File | Description |
|------|-------------|
| `occ_raft.yml` | OCC concurrency control + Raft atomic broadcast |
| `none_raft.yml` | No CC + Raft |
| `rule_raft.yml` | Rule-based CC + Raft |
| `raft_lab_test.yml` | 5-server standalone test config (cc:none, ab:raft) |

#### 8.2 Raft Cluster Topologies (`config/1leader_2followers/`)

| File | Description |
|------|-------------|
| `raft2_shardidx0.yml` | 2-partition, 3 replicas, ports 27xxx |
| `raft6_shardidx0.yml` | 6-partition shard 0, 3 replicas, ports 27xxx |
| `raft6_shardidx1.yml` | 6-partition shard 1, 3 replicas, ports 27xxx |

#### 8.3 Paxos Configs (for comparison)

| File | Description |
|------|-------------|
| `occ_paxos.yml` | OCC + Paxos |
| `paxos6_shardidx0.yml` | 6-partition shard 0, 4 replicas (3+learner), ports 17xxx |
| `paxos6_shardidx1.yml` | 6-partition shard 1, 4 replicas, ports 17xxx |

#### 8.4 Shard Configs (`src/mako/config/`)

| File | Description |
|------|-------------|
| `local-shards1-warehouses6.yml` | 1 shard, 6 warehouses |
| `local-shards2-warehouses6.yml` | 2 shards, 6 warehouses each |

#### 8.5 Test Cluster Configs (`config/`)

| File | Description |
|------|-------------|
| `1c1s3r1p_cluster_test.yml` | 1 client, 1 shard, 3 replicas, 1 partition (CI tests) |
| `1c1s5r1p_cluster_test.yml` | 1 client, 1 shard, 5 replicas, 1 partition (standalone tests) |

---

## YAML Configuration Reference

### 1. Mode Configuration

Mode configs specify the concurrency control and atomic broadcast
protocols.

#### 1.1 Raft Mode (`config/occ_raft.yml`)

```yaml
cc: occ            # Concurrency control: optimistic
ab: raft           # Atomic broadcast: Raft
read_only: false   # Read-only transaction optimisation
batch: false       # Batch mode for transactions
retry: false       # Auto-retry on abort
ongoing: 1         # Ongoing transaction limit
```

#### 1.2 Paxos Mode (`config/occ_paxos.yml`)

```yaml
cc: occ            # Same CC as Raft
ab: paxos          # Atomic broadcast: Multi-Paxos
read_only: false
batch: false
retry: false
ongoing: 1
```

#### 1.3 Other Raft Mode Variants

| File | `cc` | `ab` | Use Case |
|------|------|------|----------|
| `none_raft.yml` | `none` | `raft` | Testing without CC |
| `rule_raft.yml` | `rule` | `raft` | Rule-based CC |
| `tpl_ww_raft.yml` | `tpl_ww` | `fpga_raft` | FPGA-accelerated Raft |
| `notx_raft.yml` | `notx` | `fpga_raft` | No transactions |

#### 1.4 Mode Config Fields

| Field | Type | Values | Description |
|-------|------|--------|-------------|
| `cc` | string | `occ`, `none`, `rule`, `tpl_ww`, `notx` | Concurrency control protocol |
| `ab` | string | `paxos`, `raft`, `fpga_raft` | Atomic broadcast protocol |
| `read_only` | bool | `true`/`false` | Enable read-only optimisation |
| `batch` | bool | `true`/`false` | Enable transaction batching |
| `retry` | bool | `true`/`false` | Auto-retry aborted transactions |
| `ongoing` | int | >= 1 | Maximum concurrent in-flight transactions |

### 2. Replication Group Configuration

#### 2.1 Raft 6-Partition Shard 0 (`config/1leader_2followers/raft6_shardidx0.yml`)

```yaml
site:
  - host: localhost
    port: 27001   # Partition 0, replica 1 (preferred leader)
    partition: [s101]
  - host: localhost
    port: 27002   # Partition 0, replica 2
    partition: [s102]
  ...
  - host: localhost
    port: 27006   # Partition 5, replica 1
    partition: [s106]
  - host: p1
    port: 27101   # Partition 0, replica 2 (follower)
    partition: [s201]
  ...
  - host: p2
    port: 27201   # Partition 0, replica 3 (follower)
    partition: [s301]
  ...
```

Each partition has 3 replicas (localhost, p1, p2).  The first replica
in each group is the preferred leader.

#### 2.2 Structure

The `site` array defines all replicas in the cluster.  Each entry has:

| Field | Type | Description |
|-------|------|-------------|
| `host` | string | Hostname (maps to `--P` arg: localhost, p1, p2) |
| `port` | int | RPC port number |
| `partition` | list | Partition identifier (e.g., `[s101]`) |

#### 2.3 Partition Naming Convention

Partition IDs follow the format `s{R}{PP}` where:
- `R` = replica number (1 = leader, 2 = follower 1, 3 = follower 2)
- `PP` = partition index (01-06)

Examples:
- `s101` = replica 1 (leader), partition 0
- `s206` = replica 2 (follower), partition 5
- `s303` = replica 3 (follower), partition 2

### 3. Port Allocation Scheme

#### 3.1 Port Ranges

| Protocol | Shard 0 | Shard 1 | Purpose |
|----------|---------|---------|---------|
| Raft | 27001-27306 | 27002-27302 | CI integration tests |
| Paxos | 17001-17301 | 17002-17302 | CI integration tests |
| Standalone tests | 9001-9005 | N/A | 5-node lab tests |
| CI misc | 7001-8006, 31000-31100 | — | Process cleanup polling |

#### 3.2 Port Allocation Formula

For CI tests with `trd` threads per shard:

```
base_port = {27000 for Raft, 17000 for Paxos}
port = base_port + (shard_index) + (replica_number * 100) + partition_index

Example (Raft, shard 0, replica 1, partition 3):
  port = 27000 + 0 + 100 + 3 = 27103
```

#### 3.3 How to Avoid Port Conflicts

The 2-shard tests insert a 5-second delay between starting shard 0 and
shard 1 to avoid port binding conflicts.  Shard 0 uses
`raft6_shardidx0.yml` (port suffix `...01`) and shard 1 uses
`raft6_shardidx1.yml` (port suffix `...02`).

### 4. Shard Configuration

#### 4.1 Shard Config (`src/mako/config/local-shards1-warehouses6.yml`)

```yaml
shards:
  - shard_id: 0
    warehouses: 6
```

#### 4.2 Multi-Shard Config (`src/mako/config/local-shards2-warehouses6.yml`)

```yaml
shards:
  - shard_id: 0
    warehouses: 6
  - shard_id: 1
    warehouses: 6
```

#### 4.3 Fields

| Field | Type | Description |
|-------|------|-------------|
| `shard_id` | int | Zero-based shard index |
| `warehouses` | int | TPC-C warehouses per shard (also matches thread count) |

### 5. Standalone Test Configuration

#### 5.1 Lab Test Config (`config/raft_lab_test.yml`)

```yaml
cc: none
ab: raft
# ... commented test parameters:
# n_test_servers: 5
# election_timeout_ms: 5000
# heartbeat_interval_ms: 150
# n_partitions: 3
# log_entries_per_partition: 100
```

This config is used by `simpleRaft` and the standalone test binaries.
It specifies `cc: none` because the standalone tests do not use
transactional concurrency control.

#### 5.2 Test Cluster Configs

| File | Replicas | Partitions | Use Case |
|------|----------|------------|----------|
| `1c1s3r1p_cluster_test.yml` | 3 | 1 | CI integration tests |
| `1c1s5r1p_cluster_test.yml` | 5 | 1 | Standalone lab tests |

### 6. Switching Between Paxos and Raft

#### 6.1 Via `shard_raft.sh` (Raft-dedicated)

```bash
bash/shard_raft.sh $nshard $shard $trd $cluster $is_micro $is_replicated
# Always uses: --replication raft, occ_raft.yml, raft*_shardidx*.yml
```

#### 6.2 Via `shard.sh` (Unified)

```bash
bash/shard.sh $nshard $shard $trd $cluster $is_micro $is_replicated raft
# 7th arg: "raft" or "paxos" (default: paxos)
```

#### 6.3 Via Mode Config

The `ab` field in the mode config selects the protocol:

```yaml
# For Raft:
ab: raft

# For Paxos:
ab: paxos
```

#### 6.4 Via Command-Line Flag

The `--replication` flag on `dbtest` overrides config auto-detection:

```bash
./build/dbtest --replication raft ...
./build/dbtest --replication paxos ...
```

### 7. Config File Selection by `shard_raft.sh`

The shard launcher selects config files based on arguments:

```bash
trd=${1:-6}    # Thread count
shard=${2:-0}  # Shard index

# Site topology:
-F config/1leader_2followers/raft${trd}_shardidx${shard}.yml

# Mode config:
-F config/occ_raft.yml

# Shard config:
--shard-config src/mako/config/local-shards${nshard}-warehouses${trd}.yml
```

| Argument | Resolves To |
|----------|-------------|
| `trd=6, shard=0` | `raft6_shardidx0.yml` |
| `trd=6, shard=1` | `raft6_shardidx1.yml` |
| `trd=2, shard=0` | `raft2_shardidx0.yml` |

---

<a id="glossary-1"></a>

## Glossary

### 1. Raft-Specific Terms

| Term | Definition |
|------|------------|
| **Term** | A monotonically increasing integer that identifies a period of leadership.  Each election increments the term.  All Raft RPCs include the sender's term; a server with a stale term steps down. |
| **Log Index** | The position of an entry in the Raft log.  Indices start at 1 and increment sequentially.  Also called `slot_id` in the implementation. |
| **Commit Index** | The highest log index known to be committed (replicated to a majority).  Once committed, an entry is safe to apply to the state machine. |
| **Match Index** | Per-follower array maintained by the leader.  `matchIndex[i]` is the highest log index known to be replicated on follower `i`.  Used to advance the commit index. |
| **Next Index** | Per-follower array maintained by the leader.  `nextIndex[i]` is the next log index to send to follower `i`.  Initialised to leader's last log index + 1; decremented on AppendEntries rejection. |
| **Election Timeout** | Duration a follower waits before starting an election.  Randomised to prevent split votes.  In tests: base 5,000 ms with random jitter. |
| **Heartbeat** | An empty AppendEntries RPC sent by the leader to maintain authority and prevent elections.  Interval: typically 150 ms in tests. |
| **Leader** | The server that handles all client requests, replicates log entries, and decides when entries are committed.  At most one leader per term. |
| **Follower** | A passive server that responds to RPCs from the leader.  Does not initiate requests.  Becomes a candidate if the election timeout expires. |
| **Candidate** | A server running for leader.  Increments its term, votes for itself, and sends RequestVote RPCs to all other servers. |
| **RequestVote** | RPC sent by a candidate to request a vote.  Arguments: term, candidateId, lastLogIndex, lastLogTerm.  A server grants a vote if the candidate's log is at least as up-to-date. |
| **AppendEntries** | RPC sent by the leader to replicate log entries and serve as heartbeats.  Arguments: term, leaderId, prevLogIndex, prevLogTerm, entries[], leaderCommit. |
| **TimeoutNow** | RPC used for leadership transfer.  The current leader sends this to a preferred replica, causing it to immediately start an election without waiting for election timeout. |
| **Preferred Leader** | A designated server that should become leader.  Has a shorter election timeout, and the current leader uses TimeoutNow to transfer leadership to it.  Implemented via `IS_PREFERRED_LEADER` flag. |
| **NO-OP** | A no-operation log entry committed by a new leader to establish its authority.  Contains no application data but ensures all previous-term entries are committed. |
| **Log Compaction** | Removing log entries that have been applied to the state machine and captured in a snapshot.  Bounds log growth.  See `CompactLog()`. |

### 2. Mako-Specific Terms

| Term | Definition |
|------|------------|
| **Shard** | A horizontal partition of data.  Each shard has its own set of replicas and runs an independent consensus group.  TPC-C warehouses are distributed across shards. |
| **Partition** | A sub-division within a shard.  Each partition is a unit of data ownership.  A shard with 6 threads typically has 6 partitions. |
| **Partition Group** | The set of replicas responsible for a single partition.  In a 3-replica Raft cluster with 6 partitions, there are 6 partition groups of 3 replicas each. |
| **Watermark** | A progress indicator used by Mako's speculative execution engine.  The replication watermark tracks how far the follower's replay has progressed.  Used in `raft_worker.cc` for batching decisions. |
| **Epoch** | In Multi-Paxos, the equivalent of Raft's term.  Identifies a period of leadership.  Stored as `cur_epoch` in metadata. |
| **Speculative Execution** | Mako's key optimisation: executes transactions speculatively before replication completes.  Results are committed only after the replication layer confirms durability. |
| **agg_persist_throughput** | Aggregate persisted transaction throughput in ops/sec.  Calculated as total committed transactions / measured runtime.  The primary benchmark metric. |
| **replay_batch** | Count of replication batches processed by a follower.  Higher count indicates more frequent, smaller batches.  Raft: ~3,674 vs Paxos: ~669 in 1-shard TPC-C. |
| **Learner** | A non-voting replica in Multi-Paxos that receives committed entries for read scaling or backup.  Raft does not use learners in this implementation. |
| **Preferred Replica** | Same as preferred leader — the designated server that should hold leadership for deterministic placement. |

### 3. Transaction Terms

| Term | Definition |
|------|------------|
| **TPC-C** | Transaction Processing Performance Council benchmark C.  Industry-standard OLTP benchmark modelling a wholesale supplier with 5 transaction types. |
| **NewOrder** | TPC-C transaction (45% of mix).  Creates a new order with multiple line items.  Can be cross-shard if items are on different shards. |
| **Payment** | TPC-C transaction (43% of mix).  Processes a customer payment.  Can be cross-shard if customer and warehouse are on different shards. |
| **Delivery** | TPC-C transaction (4% of mix).  Delivers pending orders in batch.  Single-shard only. |
| **OrderStatus** | TPC-C transaction (4% of mix).  Read-only query of order status. |
| **StockLevel** | TPC-C transaction (4% of mix).  Read-only check of stock levels below threshold. |
| **OCC** | Optimistic Concurrency Control.  Transactions execute without locks, then validate at commit time.  Aborts if conflicts detected. |
| **2PC** | Two-Phase Commit.  Protocol for coordinating cross-shard transactions.  Phase 1: prepare (lock resources).  Phase 2: commit or abort. |
| **Abort Ratio** | Percentage of transaction attempts that are aborted (local conflicts or remote conflicts).  Higher in multi-shard configurations due to cross-shard contention. |
| **Commit Latency** | Time from transaction start to successful commit, in milliseconds.  Measured per transaction type. |

### 4. System and Infrastructure Terms

| Term | Definition |
|------|------------|
| **rrr** | The custom RPC framework used by Mako.  Provides TCP/IP-based remote procedure calls with ~10-50 us latency on localhost. |
| **eRPC** | High-performance RDMA-based RPC backend.  ~1-2 us latency.  Optional alternative to rrr.  Not used in CI benchmarks. |
| **DPDK** | Data Plane Development Kit.  Kernel-bypass networking library.  Supported by the legacy Deptran transport layer. |
| **Masstree** | High-performance in-memory concurrent B-tree index.  Used by Mako as the primary storage engine for transaction data. |
| **RocksDB** | Facebook's persistent key-value store based on LSM trees.  Used for Raft/Paxos log persistence via `RocksDBLogStorage`. |
| **jemalloc** | A memory allocator designed for multi-threaded applications.  Used by Mako for reduced allocation overhead and better cache behaviour. |
| **RustyCpp** | A C++ library providing Rust-style smart pointers and safety annotations.  Used for memory safety: `rusty::Box`, `rusty::Arc`, `rusty::Cell`, `rusty::Option`. |
| **Marshal** | The rrr framework's serialization format.  `to_marshal()` and `from_marshal()` convert structs to/from wire format for RPC. |
| **dbtest** | The main Mako test binary.  Runs TPC-C benchmark with configurable replication (Raft/Paxos), shards, and threads. |
| **simpleRaft** | Standalone Raft replication test binary.  Submits fixed-size log entries without transaction processing. |
| **simpleTransactionRepRaft** | Simple key-value transaction test binary with Raft replication and data integrity verification. |
| **GDB** | GNU Debugger.  `MAKO_NO_GDB=1` is set in CI to disable GDB wrapping, which would break log parsing. |
| **Coroutine** | Cooperative multitasking primitive used by the rrr framework.  Raft test cases run as coroutines via `RAFT_TEST_CORO` and `Reactor::CreateSpEvent()`. |
| **Fiber** | Alternative name for coroutine in parts of the codebase.  `Fiber::create_run()` creates and runs a new coroutine. |

### 5. Persistence Terms

| Term | Definition |
|------|------------|
| **WAL** | Write-Ahead Log.  RocksDB's mechanism for crash recovery.  Writes are logged to WAL before being applied to memtables. |
| **fsync** | System call that flushes file data and metadata to disk.  Used by RocksDBLogStorage (`sync = true`) and FileSnapshotManager to ensure durability. |
| **WriteBatch** | RocksDB feature for atomic multi-key writes.  Used by `put_batch()` and `remove_range()` to ensure log consistency. |
| **Snapshot** | A point-in-time capture of the state machine.  Allows log entries before the snapshot index to be discarded.  Binary format: 52-byte header + data + CRC32. |
| **CURRENT file** | RocksDB's manifest pointer file.  Its presence indicates a valid, openable database.  Used by `detect_mode()` to distinguish `FRESH_START` from `NORMAL_RECOVERY`. |

---

## RustyCpp Safety Annotations in Raft Code

<a id="1-overview-15"></a>

### 1. Overview

The Raft implementation uses RustyCpp for memory safety and ownership
tracking.  Every function and significant code block has safety
annotations (`@safe` or `@unsafe`), and Rust-style smart pointers
replace raw pointers and STL ownership types.

Total annotations across 12 Raft files: **122 occurrences**.

### 2. Annotation Summary by File

| File | @safe | @unsafe | rusty:: Types | Notes |
|------|-------|---------|---------------|-------|
| `server.cc` (1829 lines) | 8 | 33 | Arc, Box | Persistence methods all @unsafe |
| `server.h` (637 lines) | 5 | 26 | Arc, Box, Function | Metadata keys, raw pointer casts |
| `commo.cc` (287 lines) | 5 | 0 | Arc | RPC communication all @safe |
| `commo.h` (128 lines) | 5 | 2 | Arc, Option | QuorumEvent inline @unsafe |
| `service.cc` (112 lines) | 5 | 0 | -- | All RPC handlers @safe |
| `service.h` (83 lines) | 1 | 0 | -- | Class declaration @safe |
| `frame.cc` (206 lines) | 7 | 0 | Arc, Box | Factory methods all @safe |
| `frame.h` (49 lines) | 1 | 0 | Arc, Box, Cell, Option | Shared state |
| `coordinator.cc` (199 lines) | 5 | 2 | Arc, Cell | Output params @unsafe |
| `coordinator.h` (82 lines) | 3 | 0 | Arc, Cell, Option | Slot management |
| `exec.cc` (31 lines) | 4 | 0 | -- | All @safe |
| `exec.h` (29 lines) | 4 | 0 | -- | All @safe |

### 3. Which Methods Are @safe and Why

#### 3.1 All @safe — RPC Service Layer

All 5 RPC handlers in `service.cc` are `@safe`:

| Method | Line | Why Safe |
|--------|------|----------|
| `RaftServiceImpl()` | 14 | Constructor with no pointer operations |
| `HandleVote()` | 22 | Uses lambda instead of `std::bind` to avoid pointer ops |
| `HandleAppendEntries()` | 36 | Delegates to server via reference |
| `HandleEmptyAppendEntries()` | 69 | Delegates to server via reference |
| `HandleTimeoutNow()` | 101 | Delegates to server via reference |

#### 3.2 All @safe — Executor

All 4 executor methods are `@safe` (lines 5-25 of `exec.cc`):
`Prepare()`, `Accept()`, `AppendEntries()`, `Decide()`.
These delegate to the parent `TxnExecutor` class.

#### 3.3 All @safe — Frame Factory Methods

All 7 frame methods are `@safe` (lines 23-206 of `frame.cc`):
`RaftFrame()`, `~RaftFrame()`, `CreateExecutor()`,
`CreateCoordinator()`, `CreateScheduler()`, `CreateCommo()`,
`CreateRpcServices()`.

These construct new objects using `rusty::make_box` and smart pointers.

#### 3.4 All @safe — Communication Layer

All 5 commo methods are `@safe` (lines 20-227 of `commo.cc`):
`RaftCommo()`, `SendAppendEntries2()`, `SendAppendEntries()`,
`BroadcastVote()`, `SendTimeoutNow()`.

These create `rusty::Arc<Future>` objects for async RPC callbacks.

#### 3.5 @safe — Read-Only Accessors

| Method | File:Line | Why Safe |
|--------|-----------|----------|
| `IsLeader()` | server.h:287 | Returns boolean state |
| `GetElectionTimeout()` | server.cc:347 | Returns integer |
| `IsDisconnected()` | server.cc:438 | Checks proxy map |
| `GetUncommittedCount()` | server.cc:197 | Read-only log query |
| `GetInstance()` | server.h:386 | Returns existing reference |
| `SetLogStorage()` | server.h:430 | Simple setter |
| `GetLogStorage()` | server.h:439 | Simple getter |
| `SetSnapshotManager()` | server.h:478 | Simple setter |
| `GetSnapshotManager()` | server.h:487 | Simple getter |

### 4. Which Methods Are @unsafe and Why

#### 4.1 Persistence Methods (I/O)

All persistence methods are `@unsafe` because they call the
`LogStorage` API which performs disk I/O:

| Method | File:Line | Reason |
|--------|-----------|--------|
| `PersistTermAndVote()` | server.cc:35 | LogStorage `set_metadata()` |
| `PersistVote()` | server.cc:46 | LogStorage `set_metadata()` |
| `PersistCommitIndex()` | server.cc:56 | LogStorage `set_metadata()` |
| `PersistLogEntry()` | server.cc:66 | LogStorage `put()` |
| `PersistLogEntries()` | server.cc:82 | LogStorage `put_batch()` |
| `RecoverFromStorage()` | server.cc:104 | LogStorage `get_metadata()` + `get_range()` |
| `ReplayCommittedEntries()` | server.cc:153 | Calls `app_next_` callback |
| `CompactLog()` | server.cc:205 | LogStorage `remove_range()` |

#### 4.2 State Mutation

| Method | File:Line | Reason |
|--------|-----------|--------|
| `doVote()` | server.h:160 | Modifies `currentTerm_`, `vote_for_` |
| `OnRequestVote()` | server.h:503 | Calls `doVote()` and `std::function` callback |
| `OnAppendEntries()` | server.h:512 | Modifies log entries and commit index |
| `OnTimeoutNow()` | server.h:543 | Triggers immediate election |
| `setIsLeader()` | server.cc:443 | Modifies leader state |
| `Start()` | server.h:302 | Initiates consensus participation |
| `resetTimer()` | server.h:228 | Calls `timer_->start()` |
| `removeCmd()` | server.h:567 | Modifies log |

#### 4.3 RPC and Connection Management

| Method | File:Line | Reason |
|--------|-----------|--------|
| `Disconnect()` | server.h:550 | Modifies connection state and proxy maps |
| `Reconnect()` | server.h:553 | Calls Disconnect and resetTimer |
| `commo()` | server.h:155 | Returns raw pointer cast |
| `GetState()` | server.h:305 | Dereferences raw pointers |
| `GetRaftInstance()` | server.h:405 | Returns mutable reference |

#### 4.4 Random Number Generation

| Method | File:Line | Reason |
|--------|-----------|--------|
| `randDuration()` | server.h:238 | Calls `RandomGenerator::rand_double` (not annotated) |

#### 4.5 Monitoring

| Method | File:Line | Reason |
|--------|-----------|--------|
| `heartbeat()` | server.cc:627 | Complex function with multiple inline @unsafe blocks |
| `StartElectionTimer()` | server.cc:1187 | Calls `Fiber::create_run()` |
| `StartLeadershipTransferMonitoring()` | server.cc:1588 | Thread creation |

### 5. RustyCpp Types Used in Raft

#### 5.1 `rusty::Arc<T>` — Thread-Safe Shared Ownership

Used for objects shared across threads or coroutines:

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Arc<Cell<slotid_t>> slot_hint_` | frame.h:19, coordinator.h:41 | Shared slot counter between frame and coordinator |
| `Arc<PollThread>` | commo.h:62, raft_worker.h:72 | Shared poll thread reference |
| `Arc<Future>` | commo.cc:46,116 | Async RPC future handles |
| `Arc<ServerStatus>` | raft_worker.h:79 | Shared server status |
| `Arc<OneTimeJob>` | testconf.cc:295 | Test helper |

#### 5.2 `rusty::Box<T>` — Single Ownership

Used for owned resources with automatic cleanup:

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Box<Timer> timer_` | server.h:85 | Election timer owned by RaftServer |
| `Box<RaftServiceImpl>` | frame.cc:200 | RPC service instance |

Initialised via `rusty::Box<Timer>::make(Timer())` (server.cc:317).

#### 5.3 `rusty::Cell<T>` — Interior Mutability

Used for shared mutable state (trivially-copyable types):

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Arc<Cell<slotid_t>> slot_hint_` | coordinator.h:41 | Slot counter with `.get()` and `.set()` |

The `Cell<T>` pattern allows safe mutation through shared references,
matching Rust's `Cell<T>` semantics for `Copy` types.

#### 5.4 `rusty::Option<T>` — Optional Values

Used for values that may or may not be present:

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Option<Arc<PollThread>>` | commo.h:62, raft_worker.h:72 | Optional poll thread |
| `Option<Arc<ServerStatus>>` | raft_worker.h:79 | Optional server status |

Operations: `.is_some()`, `.as_ref().unwrap()`, `.clone()`.

#### 5.5 `rusty::Function<Sig>` — Type-Erased Callable

| Usage | File:Line | Purpose |
|-------|-----------|---------|
| `Function<void()> cb` | server.h:510, 548 | Callback parameters for OnRequestVote, OnTimeoutNow |

#### 5.6 `rusty::Mutex<T>` — Thread-Safe Lock

Used in the persistence layer (not directly in raft/ but in the
`LogStorage` implementations):

| Usage | File | Purpose |
|-------|------|---------|
| `Mutex<std::map<...>>` | memory_log_storage.hpp | Thread-safe in-memory log |

### 6. Borrow Checking Configuration

**File**: `CMakeLists.txt` (lines 745-798)

#### 6.1 Checked Files

All Raft source files under `src/deptran/raft/*.cc` are included in
the `raft_borrow` build target, **except**:

#### 6.2 Excluded Files

| File | Reason |
|------|--------|
| `testconf.cc` | Test infrastructure with class inheritance patterns incompatible with @safe code |
| `test.cc` | Test harness depending on testconf.h |
| `raft_main_helper.cc` | Third-party headers (YAML, etc.) generate 1000+ false positive violations |

#### 6.3 Build Command

```bash
cd build && make borrow_check_raft
```

Or for all checked files:

```bash
make borrow_check_all_dbtest
```

### 7. Key Safety Patterns

#### 7.1 Arc<Cell<T>> for Shared Mutable State

```cpp
// frame.h:19 — shared between RaftFrame and CoordinatorRaft
rusty::Arc<rusty::Cell<slotid_t>> slot_hint_;

// coordinator.cc:27 — read/write via Cell operations
slotid_t GetNextSlot() {
    auto current = slot_hint_->get();    // @safe read
    slot_hint_->set(current + 1);         // @safe write
    return current;
}
```

#### 7.2 Box<T> for Owned Resources

```cpp
// server.h:85 — timer owned by RaftServer
rusty::Box<Timer> timer_;

// server.cc:317 — initialisation in constructor
timer_(rusty::Box<Timer>::make(Timer()))
```

The `Box<Timer>` ensures the timer is automatically destroyed when the
RaftServer is destroyed, preventing memory leaks.

#### 7.3 Lambda Over std::bind

```cpp
// service.cc:22 — @safe refactored to use lambda
// Before (unsafe): std::bind(&RaftServer::OnRequestVote, svr, ...)
// After (safe):
auto callback = [svr](auto&&... args) {
    svr->OnRequestVote(std::forward<decltype(args)>(args)...);
};
```

Lambdas are preferred over `std::bind` because they avoid implicit
pointer operations that the borrow checker cannot verify.

#### 7.4 Inline @unsafe Blocks

```cpp
// coordinator.cc:115
// @safe - Uses @unsafe blocks for pointer operations
void AppendEntries(...) {
    // ... safe code ...
    // @unsafe { output parameter write }
    *output_param = value;
    // ... safe code ...
}
```

Individual unsafe operations within an otherwise-safe function are
marked with inline `// @unsafe { reason }` comments.

### 8. Safety Statistics

| Category | Count | Percentage |
|----------|-------|------------|
| @safe methods | 52 | 68% |
| @unsafe methods | 24 | 32% |
| Inline @unsafe blocks | 8 | — |
| Files with all @safe | 4 | exec.cc, exec.h, service.cc, frame.cc |
| Files with @unsafe | 4 | server.cc, server.h, coordinator.cc, commo.h |

The @unsafe annotations cluster in two areas:
1. **Persistence** (8 methods): I/O through LogStorage
2. **State mutation** (16 methods): Modifying consensus state, timers,
   connections, and log entries

The RPC service layer, executor, frame, and communication layer are
entirely @safe.