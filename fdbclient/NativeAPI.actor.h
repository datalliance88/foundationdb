/*
 * NativeAPI.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#if defined(NO_INTELLISENSE) && !defined(FDBCLIENT_NATIVEAPI_ACTOR_G_H)
	#define FDBCLIENT_NATIVEAPI_ACTOR_G_H
	#include "fdbclient/NativeAPI.actor.g.h"
#elif !defined(FDBCLIENT_NATIVEAPI_ACTOR_H)
	#define FDBCLIENT_NATIVEAPI_ACTOR_H


#include "flow/flow.h"
#include "flow/TDMetric.actor.h"
#include "fdbclient/FDBTypes.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/CoordinationInterface.h"
#include "fdbclient/ClusterInterface.h"
#include "fdbclient/ClientLogEvents.h"
#include "flow/actorcompiler.h" // has to be last include

// CLIENT_BUGGIFY should be used to randomly introduce failures at run time (like BUGGIFY but for client side testing)
// Unlike BUGGIFY, CLIENT_BUGGIFY can be enabled and disabled at runtime.
#define CLIENT_BUGGIFY_WITH_PROB(x) (getSBVar(__FILE__, __LINE__, BuggifyType::Client) && deterministicRandom()->random01() < (x))
#define CLIENT_BUGGIFY CLIENT_BUGGIFY_WITH_PROB(P_BUGGIFIED_SECTION_FIRES[int(BuggifyType::Client)])

// Incomplete types that are reference counted
class DatabaseContext;
template <> void addref( DatabaseContext* ptr );
template <> void delref( DatabaseContext* ptr );

void validateOptionValue(Optional<StringRef> value, bool shouldBePresent);

void enableClientInfoLogging();

struct NetworkOptions {
	std::string localAddress;
	std::string clusterFile;
	Optional<std::string> traceDirectory;
	uint64_t traceRollSize;
	uint64_t traceMaxLogsSize;	
	std::string traceLogGroup;
	std::string traceFormat;
	Optional<bool> logClientInfo;
	Standalone<VectorRef<ClientVersionRef>> supportedVersions;
	bool slowTaskProfilingEnabled;
	bool useObjectSerializer;

	// The default values, TRACE_DEFAULT_ROLL_SIZE and TRACE_DEFAULT_MAX_LOGS_SIZE are located in Trace.h.
	NetworkOptions()
	  : localAddress(""), clusterFile(""), traceDirectory(Optional<std::string>()),
	    traceRollSize(TRACE_DEFAULT_ROLL_SIZE), traceMaxLogsSize(TRACE_DEFAULT_MAX_LOGS_SIZE), traceLogGroup("default"),
	    traceFormat("xml"), slowTaskProfilingEnabled(false), useObjectSerializer(false) {}
};

class Database {
public:
	enum { API_VERSION_LATEST = -1 };

	static Database createDatabase( Reference<ClusterConnectionFile> connFile, int apiVersion, LocalityData const& clientLocality=LocalityData(), DatabaseContext *preallocatedDb=nullptr );
	static Database createDatabase( std::string connFileName, int apiVersion, LocalityData const& clientLocality=LocalityData() ); 

	Database() {}  // an uninitialized database can be destructed or reassigned safely; that's it
	void operator= ( Database const& rhs ) { db = rhs.db; }
	Database( Database const& rhs ) : db(rhs.db) {}
	Database(Database&& r) BOOST_NOEXCEPT : db(std::move(r.db)) {}
	void operator= (Database&& r) BOOST_NOEXCEPT { db = std::move(r.db); }

	// For internal use by the native client:
	explicit Database(Reference<DatabaseContext> cx) : db(cx) {}
	explicit Database( DatabaseContext* cx ) : db(cx) {}
	inline DatabaseContext* getPtr() const { return db.getPtr(); }
	inline DatabaseContext* extractPtr() { return db.extractPtr(); }
	DatabaseContext* operator->() const { return db.getPtr(); }

private:
	Reference<DatabaseContext> db;
};

void setNetworkOption(FDBNetworkOptions::Option option, Optional<StringRef> value = Optional<StringRef>() );

// Configures the global networking machinery
void setupNetwork(uint64_t transportId = 0, bool useMetrics = false);

// This call blocks while the network is running.  To use the API in a single-threaded
//  environment, the calling program must have ACTORs already launched that are waiting
//  to use the network.  In this case, the program can terminate by calling stopNetwork()
//  from a callback, thereby releasing this call to return.  In a multithreaded setup
//  this call can be called from a dedicated "networking" thread.  All the network-based
//  callbacks will happen on this second thread.  When a program is finished, the
//  call stopNetwork (from a non-networking thread) can cause the runNetwork() call to
//  return.
//
// Throws network_already_setup if g_network has already been initalized
void runNetwork();

// See above.  Can be called from a thread that is not the "networking thread"
//
// Throws network_not_setup if g_network has not been initalized
void stopNetwork();

/*
 * Starts and holds the monitorLeader and failureMonitorClient actors
 */
class Cluster : public ReferenceCounted<Cluster>, NonCopyable {
public:
	Cluster(Reference<ClusterConnectionFile> connFile,  Reference<AsyncVar<int>> connectedCoordinatorsNum, int apiVersion=Database::API_VERSION_LATEST);
	Cluster(Reference<ClusterConnectionFile> connFile, Reference<AsyncVar<Optional<struct ClusterInterface>>> clusterInterface, Reference<AsyncVar<int>> connectedCoordinatorsNum);

	~Cluster();

	Reference<AsyncVar<Optional<struct ClusterInterface>>> getClusterInterface();
	Reference<ClusterConnectionFile> getConnectionFile() { return connectionFile; }

	Future<Void> onConnected();

private: 
	void init(Reference<ClusterConnectionFile> connFile, bool startClientInfoMonitor, Reference<AsyncVar<int>> connectedCoordinatorsNum, int apiVersion=Database::API_VERSION_LATEST);

	Reference<AsyncVar<Optional<struct ClusterInterface>>> clusterInterface;
	Reference<ClusterConnectionFile> connectionFile;

	Future<Void> failMon;
	Future<Void> connected;
};

struct StorageMetrics;

struct TransactionOptions {
	double maxBackoff;
	uint32_t getReadVersionFlags;
	uint32_t sizeLimit;
	bool checkWritesEnabled : 1;
	bool causalWriteRisky : 1;
	bool commitOnFirstProxy : 1;
	bool debugDump : 1;
	bool lockAware : 1;
	bool readOnly : 1;
	bool firstInBatch : 1;

	TransactionOptions(Database const& cx);
	TransactionOptions();

	void reset(Database const& cx);
};

struct TransactionInfo {
	Optional<UID> debugID;
	int taskID;
	bool useProvisionalProxies;

	explicit TransactionInfo( int taskID ) : taskID(taskID), useProvisionalProxies(false) {}
};

struct TransactionLogInfo : public ReferenceCounted<TransactionLogInfo>, NonCopyable {
	enum LoggingLocation { DONT_LOG = 0, TRACE_LOG = 1, DATABASE = 2 };

	TransactionLogInfo() : logLocation(DONT_LOG) {}
	TransactionLogInfo(LoggingLocation location) : logLocation(location) {}
	TransactionLogInfo(std::string id, LoggingLocation location) : logLocation(location), identifier(id) {}

	void setIdentifier(std::string id) { identifier = id; }
	void logTo(LoggingLocation loc) { logLocation = logLocation | loc; }
	template <typename T>
	void addLog(const T& event) {
		if(logLocation & TRACE_LOG) {
			ASSERT(!identifier.empty())
			event.logEvent(identifier);
		}

		if (flushed) {
			return;
		}

		if(logLocation & DATABASE) {
			logsAdded = true;
			static_assert(std::is_base_of<FdbClientLogEvents::Event, T>::value, "Event should be derived class of FdbClientLogEvents::Event");
			trLogWriter << event;
		}
	}

	BinaryWriter trLogWriter{ IncludeVersion() };
	bool logsAdded{ false };
	bool flushed{ false };
	int logLocation;
	std::string identifier;
};

struct Watch : public ReferenceCounted<Watch>, NonCopyable {
	Key key;
	Optional<Value> value;
	bool valuePresent;
	Optional<Value> setValue;
	bool setPresent;
	Promise<Void> onChangeTrigger;
	Promise<Void> onSetWatchTrigger;
	Future<Void> watchFuture;

	Watch() : watchFuture(Never()), valuePresent(false), setPresent(false) { }
	Watch(Key key) : key(key), watchFuture(Never()), valuePresent(false), setPresent(false) { }
	Watch(Key key, Optional<Value> val) : key(key), value(val), watchFuture(Never()), valuePresent(true), setPresent(false) { }

	void setWatch(Future<Void> watchFuture);
};

class Transaction : NonCopyable {
public:
	explicit Transaction( Database const& cx );
	~Transaction();

	void preinitializeOnForeignThread() {
		committedVersion = invalidVersion;
	}

	void setVersion( Version v );
	Future<Version> getReadVersion() { return getReadVersion(0); }

	Future< Optional<Value> > get( const Key& key, bool snapshot = false );
	Future< Void > watch( Reference<Watch> watch );
	Future< Key > getKey( const KeySelector& key, bool snapshot = false );
	//Future< Optional<KeyValue> > get( const KeySelectorRef& key );
	Future< Standalone<RangeResultRef> > getRange( const KeySelector& begin, const KeySelector& end, int limit, bool snapshot = false, bool reverse = false );
	Future< Standalone<RangeResultRef> > getRange( const KeySelector& begin, const KeySelector& end, GetRangeLimits limits, bool snapshot = false, bool reverse = false );
	Future< Standalone<RangeResultRef> > getRange( const KeyRange& keys, int limit, bool snapshot = false, bool reverse = false ) { 
		return getRange( KeySelector( firstGreaterOrEqual(keys.begin), keys.arena() ), 
			KeySelector( firstGreaterOrEqual(keys.end), keys.arena() ), limit, snapshot, reverse ); 
	}
	Future< Standalone<RangeResultRef> > getRange( const KeyRange& keys, GetRangeLimits limits, bool snapshot = false, bool reverse = false ) { 
		return getRange( KeySelector( firstGreaterOrEqual(keys.begin), keys.arena() ),
			KeySelector( firstGreaterOrEqual(keys.end), keys.arena() ), limits, snapshot, reverse ); 
	}

	Future< Standalone<VectorRef< const char*>>> getAddressesForKey (const Key& key );

	void enableCheckWrites();
	void addReadConflictRange( KeyRangeRef const& keys );
	void addWriteConflictRange( KeyRangeRef const& keys );
	void makeSelfConflicting();

	Future< Void > warmRange( Database cx, KeyRange keys );

	Future< StorageMetrics > waitStorageMetrics( KeyRange const& keys, StorageMetrics const& min, StorageMetrics const& max, StorageMetrics const& permittedError, int shardLimit );
	Future< StorageMetrics > getStorageMetrics( KeyRange const& keys, int shardLimit );
	Future< Standalone<VectorRef<KeyRef>> > splitStorageMetrics( KeyRange const& keys, StorageMetrics const& limit, StorageMetrics const& estimated );

	// If checkWriteConflictRanges is true, existing write conflict ranges will be searched for this key
	void set( const KeyRef& key, const ValueRef& value, bool addConflictRange = true );
	void atomicOp( const KeyRef& key, const ValueRef& value, MutationRef::Type operationType, bool addConflictRange = true );
	// execute operation is similar to set, but the command will reach
	// one of the proxies, all the TLogs and all the storage nodes.
	// instead of setting a key and value on the DB, it executes the command
	// that is passed in the value field.
	// - cmdType can be used for logging purposes
	// - cmdPayload contains the details of the command to be executed:
	// format of the cmdPayload : <binary-path>:<arg1=val1>,<arg2=val2>...
	void execute(const KeyRef& cmdType, const ValueRef& cmdPayload);
	void clear( const KeyRangeRef& range, bool addConflictRange = true );
	void clear( const KeyRef& key, bool addConflictRange = true );
	Future<Void> commit(); // Throws not_committed or commit_unknown_result errors in normal operation

	void setOption( FDBTransactionOptions::Option option, Optional<StringRef> value = Optional<StringRef>() );

	Version getCommittedVersion() { return committedVersion; }   // May be called only after commit() returns success
	Future<Standalone<StringRef>> getVersionstamp(); // Will be fulfilled only after commit() returns success

	Promise<Standalone<StringRef>> versionstampPromise;

	Future<Void> onError( Error const& e );
	void flushTrLogsIfEnabled();

	// These are to permit use as state variables in actors:
	Transaction() : info( TaskDefaultEndpoint ) {}
	void operator=(Transaction&& r) BOOST_NOEXCEPT;

	void reset();
	void fullReset();
	double getBackoff(int errCode);
	void debugTransaction(UID dID) { info.debugID = dID; }

	Future<Void> commitMutations();
	void setupWatches();
	void cancelWatches(Error const& e = transaction_cancelled());

	TransactionInfo info;
	int numErrors;

	std::vector<Reference<Watch>> watches;

	int apiVersionAtLeast(int minVersion) const;

	void checkDeferredError();

	Database getDatabase() const {
		return cx;
	}
	static Reference<TransactionLogInfo> createTrLogInfoProbabilistically(const Database& cx);
	TransactionOptions options;
	double startTime;
	Reference<TransactionLogInfo> trLogInfo;
private:
	Future<Version> getReadVersion(uint32_t flags);
	void setPriority(uint32_t priorityFlag);

	Database cx;

	double backoff;
	Version committedVersion;
	CommitTransactionRequest tr;
	Future<Version> readVersion;
	Promise<Optional<Value>> metadataVersion;
	vector<Future<std::pair<Key, Key>>> extraConflictRanges;
	Promise<Void> commitResult;
	Future<Void> committing;
};

ACTOR Future<Version> waitForCommittedVersion(Database cx, Version version);

std::string unprintable( const std::string& );

int64_t extractIntOption( Optional<StringRef> value, int64_t minValue = std::numeric_limits<int64_t>::min(), int64_t maxValue = std::numeric_limits<int64_t>::max() );

// Takes a snapshot of the cluster, specifically the following persistent
// states: coordinator, TLog and storage state
ACTOR Future<Void> snapCreate(Database cx, StringRef snapCmd, UID snapUID);

#include "flow/unactorcompiler.h"
#endif
