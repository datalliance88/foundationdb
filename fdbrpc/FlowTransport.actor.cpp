/*
 * FlowTransport.actor.cpp
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

#include "fdbrpc/FlowTransport.h"

#include <unordered_map>
#if VALGRIND
#include <memcheck.h>
#endif

#include "fdbrpc/crc32c.h"
#include "fdbrpc/fdbrpc.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/genericactors.actor.h"
#include "fdbrpc/simulator.h"
#include "flow/ActorCollection.h"
#include "flow/Error.h"
#include "flow/flow.h"
#include "flow/Net2Packet.h"
#include "flow/TDMetric.actor.h"
#include "flow/ObjectSerializer.h"
#include "flow/ProtocolVersion.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

static NetworkAddressList g_currentDeliveryPeerAddress = NetworkAddressList();

const UID WLTOKEN_ENDPOINT_NOT_FOUND(-1, 0);
const UID WLTOKEN_PING_PACKET(-1, 1);
const UID TOKEN_IGNORE_PACKET(0, 2);
const uint64_t TOKEN_STREAM_FLAG = 1;


class EndpointMap : NonCopyable {
public:
	EndpointMap();
	void insert( NetworkMessageReceiver* r, Endpoint::Token& token, uint32_t priority );
	NetworkMessageReceiver* get( Endpoint::Token const& token );
	uint32_t getPriority( Endpoint::Token const& token );
	void remove( Endpoint::Token const& token, NetworkMessageReceiver* r );

private:
	void realloc();

	struct Entry {
		union {
			uint64_t uid[2];  // priority packed into lower 32 bits; actual lower 32 bits of token are the index in data[]
			uint32_t nextFree;
		};
		NetworkMessageReceiver* receiver;
		Endpoint::Token& token() { return *(Endpoint::Token*)uid; }
	};
	std::vector<Entry> data;
	uint32_t firstFree;
};

EndpointMap::EndpointMap()
 : firstFree(-1)
{
}

void EndpointMap::realloc() {
	int oldSize = data.size();
	data.resize( std::max(128, oldSize*2) );
	for(int i=oldSize; i<data.size(); i++) {
		data[i].receiver = 0;
		data[i].nextFree = i+1;
	}
	data[data.size()-1].nextFree = firstFree;
	firstFree = oldSize;
}

void EndpointMap::insert( NetworkMessageReceiver* r, Endpoint::Token& token, uint32_t priority ) {
	if (firstFree == uint32_t(-1)) realloc();
	int index = firstFree;
	firstFree = data[index].nextFree;
	token = Endpoint::Token( token.first(), (token.second()&0xffffffff00000000LL) | index );
	data[index].token() = Endpoint::Token( token.first(), (token.second()&0xffffffff00000000LL) | priority );
	data[index].receiver = r;
}

NetworkMessageReceiver* EndpointMap::get( Endpoint::Token const& token ) {
	uint32_t index = token.second();
	if ( index < data.size() && data[index].token().first() == token.first() && ((data[index].token().second()&0xffffffff00000000LL)|index)==token.second() )
		return data[index].receiver;
	return 0;
}

uint32_t EndpointMap::getPriority( Endpoint::Token const& token ) {
	uint32_t index = token.second();
	if ( index < data.size() && data[index].token().first() == token.first() && ((data[index].token().second()&0xffffffff00000000LL)|index)==token.second() )
		return data[index].token().second();
	return TaskUnknownEndpoint;
}

void EndpointMap::remove( Endpoint::Token const& token, NetworkMessageReceiver* r ) {
	uint32_t index = token.second();
	if ( index < data.size() && data[index].token().first() == token.first() && ((data[index].token().second()&0xffffffff00000000LL)|index)==token.second() && data[index].receiver == r ) {
		data[index].receiver = 0;
		data[index].nextFree = firstFree;
		firstFree = index;
	}
}

struct EndpointNotFoundReceiver : NetworkMessageReceiver {
	EndpointNotFoundReceiver(EndpointMap& endpoints) {
		//endpoints[WLTOKEN_ENDPOINT_NOT_FOUND] = this;
		Endpoint::Token e = WLTOKEN_ENDPOINT_NOT_FOUND;
		endpoints.insert(this, e, TaskDefaultEndpoint);
		ASSERT( e == WLTOKEN_ENDPOINT_NOT_FOUND );
	}
	virtual void receive( ArenaReader& reader ) {
		// Remote machine tells us it doesn't have endpoint e
		Endpoint e; reader >> e;
		IFailureMonitor::failureMonitor().endpointNotFound(e);
	}

	virtual void receive(ArenaObjectReader& reader) {
		Endpoint e;
		reader.deserialize(e);
		IFailureMonitor::failureMonitor().endpointNotFound(e);
	}
};

struct PingReceiver : NetworkMessageReceiver {
	PingReceiver(EndpointMap& endpoints) {
		Endpoint::Token e = WLTOKEN_PING_PACKET;
		endpoints.insert(this, e, TaskReadSocket);
		ASSERT( e == WLTOKEN_PING_PACKET );
	}
	virtual void receive( ArenaReader& reader ) {
		ReplyPromise<Void> reply; reader >> reply;
		reply.send(Void());
	}
	virtual void receive(ArenaObjectReader& reader) {
		ReplyPromise<Void> reply;
		reader.deserialize(reply);
		reply.send(Void());
	}
};

class TransportData {
public:
	TransportData(uint64_t transportId)
	  : endpointNotFoundReceiver(endpoints),
		pingReceiver(endpoints),
		warnAlwaysForLargePacket(true),
		lastIncompatibleMessage(0),
		transportId(transportId),
		numIncompatibleConnections(0)
	{
		degraded = Reference<AsyncVar<bool>>( new AsyncVar<bool>(false) );
	}

	~TransportData();

	void initMetrics() {
		bytesSent.init(LiteralStringRef("Net2.BytesSent"));
		countPacketsReceived.init(LiteralStringRef("Net2.CountPacketsReceived"));
		countPacketsGenerated.init(LiteralStringRef("Net2.CountPacketsGenerated"));
		countConnEstablished.init(LiteralStringRef("Net2.CountConnEstablished"));
		countConnClosedWithError.init(LiteralStringRef("Net2.CountConnClosedWithError"));
		countConnClosedWithoutError.init(LiteralStringRef("Net2.CountConnClosedWithoutError"));
	}

	struct Peer* getPeer( NetworkAddress const& address, bool openConnection = true );

	// Returns true if given network address 'address' is one of the address we are listening on.
	bool isLocalAddress(const NetworkAddress& address) const;

	NetworkAddressList localAddresses;
	std::vector<Future<Void>> listeners;
	std::unordered_map<NetworkAddress, struct Peer*> peers;
	std::unordered_map<NetworkAddress, std::pair<double, double>> closedPeers;
	Reference<AsyncVar<bool>> degraded;
	bool warnAlwaysForLargePacket;

	// These declarations must be in exactly this order
	EndpointMap endpoints;
	EndpointNotFoundReceiver endpointNotFoundReceiver;
	PingReceiver pingReceiver;
	// End ordered declarations

	Int64MetricHandle bytesSent;
	Int64MetricHandle countPacketsReceived;
	Int64MetricHandle countPacketsGenerated;
	Int64MetricHandle countConnEstablished;
	Int64MetricHandle countConnClosedWithError;
	Int64MetricHandle countConnClosedWithoutError;

	std::map<NetworkAddress, std::pair<uint64_t, double>> incompatiblePeers;
	uint32_t numIncompatibleConnections;
	std::map<uint64_t, double> multiVersionConnections;
	double lastIncompatibleMessage;
	uint64_t transportId;

	Future<Void> multiVersionCleanup;
};

#define CONNECT_PACKET_V0 0x0FDB00A444020001LL
#define CONNECT_PACKET_V0_SIZE 14

#pragma pack( push, 1 )
struct ConnectPacket {
	// The value does not inclueds the size of `connectPacketLength` itself,
	// but only the other fields of this structure.
	uint32_t connectPacketLength;
	ProtocolVersion protocolVersion;      // Expect currentProtocolVersion

	uint16_t canonicalRemotePort;  // Port number to reconnect to the originating process
	uint64_t connectionId;         // Multi-version clients will use the same Id for both connections, other connections will set this to zero. Added at protocol Version 0x0FDB00A444020001.

	 // IP Address to reconnect to the originating process. Only one of these must be populated.
	uint32_t canonicalRemoteIp4;

	enum ConnectPacketFlags {
		  FLAG_IPV6 = 1
	};
	uint16_t flags;
	uint8_t canonicalRemoteIp6[16];

	ConnectPacket() {
		memset(this, 0, sizeof(*this));
	}

	IPAddress canonicalRemoteIp() const {
		if (isIPv6()) {
			IPAddress::IPAddressStore store;
			memcpy(store.data(), canonicalRemoteIp6, sizeof(canonicalRemoteIp6));
			return IPAddress(store);
		} else {
			return IPAddress(canonicalRemoteIp4);
		}
	}

	void setCanonicalRemoteIp(const IPAddress& ip) {
		if (ip.isV6()) {
			flags = flags | FLAG_IPV6;
			memcpy(&canonicalRemoteIp6, ip.toV6().data(), 16);
		} else {
			flags = flags & ~FLAG_IPV6;
			canonicalRemoteIp4 = ip.toV4();
		}
	}

	bool isIPv6() const { return flags & FLAG_IPV6; }

	uint32_t totalPacketSize() const { return connectPacketLength + sizeof(connectPacketLength); }

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, connectPacketLength);
		if(connectPacketLength > sizeof(ConnectPacket) - sizeof(connectPacketLength)) {
			ASSERT(!g_network->isSimulated());
			throw serialization_failed();
		}

		serializer(ar, protocolVersion, canonicalRemotePort, connectionId, canonicalRemoteIp4);
		if (ar.isDeserializing && !ar.protocolVersion().hasIPv6()) {
			flags = 0;
		} else {
			// We can send everything in serialized packet, since the current version of ConnectPacket
			// is backward compatible with CONNECT_PACKET_V0.
			serializer(ar, flags);
			ar.serializeBytes(&canonicalRemoteIp6, sizeof(canonicalRemoteIp6));
		}
	}
};

#pragma pack( pop )

ACTOR static Future<Void> connectionReader(TransportData* transport, Reference<IConnection> conn, Peer* peer,
                                           Promise<Peer*> onConnected);

static PacketID sendPacket( TransportData* self, ISerializeSource const& what, const Endpoint& destination, bool reliable, bool openConnection );

struct Peer : NonCopyable {
	TransportData* transport;
	NetworkAddress destination;
	UnsentPacketQueue unsent;
	ReliablePacketList reliable;
	AsyncTrigger dataToSend;  // Triggered when unsent.empty() becomes false
	Future<Void> connect;
	AsyncTrigger resetPing;
	bool compatible;
	bool outgoingConnectionIdle;  // We don't actually have a connection open and aren't trying to open one because we don't have anything to send
	double lastConnectTime;
	double reconnectionDelay;
	int peerReferences;
	bool incompatibleProtocolVersionNewer;
	int64_t bytesReceived;

	explicit Peer( TransportData* transport, NetworkAddress const& destination )
		: transport(transport), destination(destination), outgoingConnectionIdle(false), lastConnectTime(0.0), reconnectionDelay(FLOW_KNOBS->INITIAL_RECONNECTION_TIME), 
		  compatible(true), incompatibleProtocolVersionNewer(false), peerReferences(-1), bytesReceived(0)
	{
		connect = connectionKeeper(this);
	}

	void send(PacketBuffer* pb, ReliablePacket* rp, bool firstUnsent) {
		unsent.setWriteBuffer(pb);
		if (rp) reliable.insert(rp);
		if (firstUnsent) dataToSend.trigger();
	}

	void prependConnectPacket() {
		// Send the ConnectPacket expected at the beginning of a new connection
		ConnectPacket pkt;
		if(transport->localAddresses.address.isTLS() == destination.isTLS()) {
			pkt.canonicalRemotePort = transport->localAddresses.address.port;
			pkt.setCanonicalRemoteIp(transport->localAddresses.address.ip);
		} else if(transport->localAddresses.secondaryAddress.present()) {
			pkt.canonicalRemotePort = transport->localAddresses.secondaryAddress.get().port;
			pkt.setCanonicalRemoteIp(transport->localAddresses.secondaryAddress.get().ip);
		} else {
			// a "mixed" TLS/non-TLS connection is like a client/server connection - there's no way to reverse it
			pkt.canonicalRemotePort = 0;
			pkt.setCanonicalRemoteIp(IPAddress(0));
		}

		pkt.connectPacketLength = sizeof(pkt) - sizeof(pkt.connectPacketLength);
		pkt.protocolVersion = currentProtocolVersion;
		if (g_network->useObjectSerializer()) {
			pkt.protocolVersion.addObjectSerializerFlag();
		}
		pkt.connectionId = transport->transportId;

		PacketBuffer* pb_first = new PacketBuffer;
		PacketWriter wr( pb_first, nullptr, Unversioned() );
		pkt.serialize(wr);
		unsent.prependWriteBuffer(pb_first, wr.finish());
	}

	void discardUnreliablePackets() {
		// Throw away the current unsent list, dropping the reference count on each PacketBuffer that accounts for presence in the unsent list
		unsent.discardAll();

		// If there are reliable packets, compact reliable packets into a new unsent range
		if(!reliable.empty()) {
			PacketBuffer* pb = unsent.getWriteBuffer();
			pb = reliable.compact(pb, nullptr);
			unsent.setWriteBuffer(pb);
		}
	}

	void onIncomingConnection( Reference<IConnection> conn, Future<Void> reader ) {
		// In case two processes are trying to connect to each other simultaneously, the process with the larger canonical NetworkAddress
		// gets to keep its outgoing connection.
		if ( !destination.isPublic() && !outgoingConnectionIdle ) throw address_in_use();
		NetworkAddress compatibleAddr = transport->localAddresses.address;
		if(transport->localAddresses.secondaryAddress.present() && transport->localAddresses.secondaryAddress.get().isTLS() == destination.isTLS()) {
			compatibleAddr = transport->localAddresses.secondaryAddress.get();
		}

		if ( !destination.isPublic() || outgoingConnectionIdle || destination > compatibleAddr ) {
			// Keep the new connection
			TraceEvent("IncomingConnection", conn->getDebugID())
				.suppressFor(1.0)
				.detail("FromAddr", conn->getPeerAddress())
				.detail("CanonicalAddr", destination)
				.detail("IsPublic", destination.isPublic());

			connect.cancel();
			prependConnectPacket();
			connect = connectionKeeper( this, conn, reader );
		} else {
			TraceEvent("RedundantConnection", conn->getDebugID())
				.suppressFor(1.0)
				.detail("FromAddr", conn->getPeerAddress().toString())
				.detail("CanonicalAddr", destination)
				.detail("LocalAddr", compatibleAddr);

			// Keep our prior connection
			reader.cancel();
			conn->close();

			// Send an (ignored) packet to make sure that, if our outgoing connection died before the peer made this connection attempt,
			// we eventually find out that our connection is dead, close it, and then respond to the next connection reattempt from peer.
		}
	}

	ACTOR static Future<Void> connectionMonitor( Peer *peer ) {
		state RequestStream< ReplyPromise<Void> > remotePing( Endpoint( {peer->destination}, WLTOKEN_PING_PACKET ) );

		loop {
			if(peer->peerReferences == 0 && peer->reliable.empty() && peer->unsent.empty()) {
				throw connection_unreferenced();
			}

			wait( delayJittered( FLOW_KNOBS->CONNECTION_MONITOR_LOOP_TIME ) );

			// SOMEDAY: Stop monitoring and close the connection after a long period of inactivity with no reliable or onDisconnect requests outstanding

			state ReplyPromise<Void> reply;
			FlowTransport::transport().sendUnreliable( SerializeSource<ReplyPromise<Void>>(reply), remotePing.getEndpoint() );
			state int64_t startingBytes = peer->bytesReceived;
			state int timeouts = 0;
			loop {
				choose {
					when (wait( delay( FLOW_KNOBS->CONNECTION_MONITOR_TIMEOUT ) )) {
						if(startingBytes == peer->bytesReceived) {
							TraceEvent("ConnectionTimeout").suppressFor(1.0).detail("WithAddr", peer->destination);
							throw connection_failed();
						}
						if(timeouts > 1) {
							TraceEvent(SevWarnAlways, "ConnectionSlowPing").suppressFor(1.0).detail("WithAddr", peer->destination).detail("Timeouts", timeouts);
						}
						startingBytes = peer->bytesReceived;
						timeouts++;
					}
					when (wait( reply.getFuture() )) {
						break;
					}
					when (wait( peer->resetPing.onTrigger())) {
						break;
					}
				}
			}
		}
	}

	ACTOR static Future<Void> connectionWriter( Peer* self, Reference<IConnection> conn ) {
		state double lastWriteTime = now();
		loop {
			//wait( delay(0, TaskWriteSocket) );
			wait( delayJittered(std::max<double>(FLOW_KNOBS->MIN_COALESCE_DELAY, FLOW_KNOBS->MAX_COALESCE_DELAY - (now() - lastWriteTime)), TaskWriteSocket) );
			//wait( delay(500e-6, TaskWriteSocket) );
			//wait( yield(TaskWriteSocket) );

			// Send until there is nothing left to send
			loop {
				lastWriteTime = now();

				int sent = conn->write(self->unsent.getUnsent(), /* limit= */ FLOW_KNOBS->MAX_PACKET_SEND_BYTES);
				if (sent) {
					self->transport->bytesSent += sent;
					self->unsent.sent(sent);
				}
				if (self->unsent.empty()) break;

				TEST(true); // We didn't write everything, so apparently the write buffer is full.  Wait for it to be nonfull.
				wait( conn->onWritable() );
				wait( yield(TaskWriteSocket) );
			}

			// Wait until there is something to send
			while ( self->unsent.empty() )
				wait( self->dataToSend.onTrigger() );
		}
	}

	ACTOR static Future<Void> connectionKeeper( Peer* self,
			Reference<IConnection> conn = Reference<IConnection>(),
			Future<Void> reader = Void()) {
		TraceEvent(SevDebug, "ConnectionKeeper", conn ? conn->getDebugID() : UID())
			.detail("PeerAddr", self->destination)
			.detail("ConnSet", (bool)conn);

		// This is used only at client side and is used to override waiting for unsent data to update failure monitoring
		// status. At client, if an existing connection fails, we retry making a connection and if that fails, then only
		// we report that address as failed.
		state bool clientReconnectDelay = false;
		loop {
			try {
				if (!conn) {  // Always, except for the first loop with an incoming connection
					self->outgoingConnectionIdle = true;

					// Wait until there is something to send.
					while (self->unsent.empty()) {
						if (FlowTransport::transport().isClient() && self->destination.isPublic() &&
						    clientReconnectDelay) {
							break;
						}
						wait(self->dataToSend.onTrigger());
					}

					ASSERT( self->destination.isPublic() );
					self->outgoingConnectionIdle = false;
					wait(delayJittered(
					    std::max(0.0, self->lastConnectTime + self->reconnectionDelay -
					                      now()))); // Don't connect() to the same peer more than once per 2 sec
					self->lastConnectTime = now();

					TraceEvent("ConnectingTo", conn ? conn->getDebugID() : UID()).suppressFor(1.0).detail("PeerAddr", self->destination);
					Reference<IConnection> _conn = wait( timeout( INetworkConnections::net()->connect(self->destination), FLOW_KNOBS->CONNECTION_MONITOR_TIMEOUT, Reference<IConnection>() ) );
					if (_conn) {
						if (FlowTransport::transport().isClient()) {
							IFailureMonitor::failureMonitor().setStatus(self->destination, FailureStatus(false));
						}
						if (self->unsent.empty()) {
							_conn->close();
							clientReconnectDelay = false;
							continue;
						} else {
							conn = _conn;
							TraceEvent("ConnectionExchangingConnectPacket", conn->getDebugID())
							    .suppressFor(1.0)
							    .detail("PeerAddr", self->destination);
							self->prependConnectPacket();
						}
					} else {
						TraceEvent("ConnectionTimedOut", conn ? conn->getDebugID() : UID()).suppressFor(1.0).detail("PeerAddr", self->destination);
						if (FlowTransport::transport().isClient()) {
							IFailureMonitor::failureMonitor().setStatus(self->destination, FailureStatus(true));
						}
						throw connection_failed();
					}

					reader = connectionReader( self->transport, conn, self, Promise<Peer*>());
				} else {
					self->outgoingConnectionIdle = false;
				}

				try {
					self->transport->countConnEstablished++;
					wait( connectionWriter( self, conn ) || reader || connectionMonitor(self) );
				} catch (Error& e) {
					if (e.code() == error_code_connection_failed || e.code() == error_code_actor_cancelled ||
					    e.code() == error_code_connection_unreferenced ||
					    (g_network->isSimulated() && e.code() == error_code_checksum_failed))
						self->transport->countConnClosedWithoutError++;
					else
						self->transport->countConnClosedWithError++;
					throw e;
				}

				ASSERT( false );
			} catch (Error& e) {
				if(now() - self->lastConnectTime > FLOW_KNOBS->RECONNECTION_RESET_TIME) {
					self->reconnectionDelay = FLOW_KNOBS->INITIAL_RECONNECTION_TIME;
				} else {
					self->reconnectionDelay = std::min(FLOW_KNOBS->MAX_RECONNECTION_TIME, self->reconnectionDelay * FLOW_KNOBS->RECONNECTION_TIME_GROWTH_RATE);
				}
				self->discardUnreliablePackets();
				reader = Future<Void>();
				bool ok = e.code() == error_code_connection_failed || e.code() == error_code_actor_cancelled ||
				          e.code() == error_code_connection_unreferenced ||
				          (g_network->isSimulated() && e.code() == error_code_checksum_failed);

				if(self->compatible) {
					TraceEvent(ok ? SevInfo : SevWarnAlways, "ConnectionClosed", conn ? conn->getDebugID() : UID()).error(e, true).suppressFor(1.0).detail("PeerAddr", self->destination);
				}
				else {
					TraceEvent(ok ? SevInfo : SevWarnAlways, "IncompatibleConnectionClosed", conn ? conn->getDebugID() : UID()).error(e, true).suppressFor(1.0).detail("PeerAddr", self->destination);
				}

				if(self->destination.isPublic() && IFailureMonitor::failureMonitor().getState(self->destination).isAvailable()) {
					auto& it = self->transport->closedPeers[self->destination];
					if(now() - it.second > FLOW_KNOBS->TOO_MANY_CONNECTIONS_CLOSED_RESET_DELAY) {
						it.first = now();
					} else if(now() - it.first > FLOW_KNOBS->TOO_MANY_CONNECTIONS_CLOSED_TIMEOUT) {
						TraceEvent(SevWarnAlways, "TooManyConnectionsClosed", conn ? conn->getDebugID() : UID()).suppressFor(5.0).detail("PeerAddr", self->destination);
						self->transport->degraded->set(true);
					}
					it.second = now();
				}

				if (conn) {
					if (FlowTransport::transport().isClient()) {
						clientReconnectDelay = true;
					}
					conn->close();
					conn = Reference<IConnection>();
				}
				IFailureMonitor::failureMonitor().notifyDisconnect( self->destination );  //< Clients might send more packets in response, which needs to go out on the next connection
				if (e.code() == error_code_actor_cancelled) throw;
				// Try to recover, even from serious errors, by retrying

				if(self->peerReferences <= 0 && self->reliable.empty() && self->unsent.empty()) {
					TraceEvent("PeerDestroy").error(e).suppressFor(1.0).detail("PeerAddr", self->destination);
					self->connect.cancel();
					self->transport->peers.erase(self->destination);
					delete self;
					return Void();
				}
			}
		}
	}
};

TransportData::~TransportData() {
	for(auto &p : peers) {
		p.second->connect.cancel();
		delete p.second;
	}
}

ACTOR static void deliver(TransportData* self, Endpoint destination, ArenaReader reader, bool inReadSocket) {
	int priority = self->endpoints.getPriority(destination.token);
	if (priority < TaskReadSocket || !inReadSocket) {
		wait( delay(0, priority) );
	} else {
		g_network->setCurrentTask( priority );
	}

	auto receiver = self->endpoints.get(destination.token);
	if (receiver) {
		try {
			g_currentDeliveryPeerAddress = destination.addresses;
			if (g_network->useObjectSerializer()) {
				StringRef data = reader.arenaReadAll();
				ASSERT(data.size() > 8);
				ArenaObjectReader objReader(reader.arena(), reader.arenaReadAll());
				receiver->receive(objReader);
			} else {
				receiver->receive(reader);
			}
			g_currentDeliveryPeerAddress = { NetworkAddress() };
		} catch (Error& e) {
			g_currentDeliveryPeerAddress = {NetworkAddress()};
			TraceEvent(SevError, "ReceiverError").error(e).detail("Token", destination.token.toString()).detail("Peer", destination.getPrimaryAddress());
			throw;
		}
	} else if (destination.token.first() & TOKEN_STREAM_FLAG) {
		// We don't have the (stream) endpoint 'token', notify the remote machine
		if (destination.token.first() != -1) {
			sendPacket(self,
			           SerializeSource<Endpoint>(Endpoint(self->localAddresses, destination.token)),
			           Endpoint(destination.addresses, WLTOKEN_ENDPOINT_NOT_FOUND), false, true);
		}
	}

	if( inReadSocket )
		g_network->setCurrentTask( TaskReadSocket );
}

static void scanPackets(TransportData* transport, uint8_t*& unprocessed_begin, const uint8_t* e, Arena& arena,
                        NetworkAddress const& peerAddress, ProtocolVersion peerProtocolVersion) {
	// Find each complete packet in the given byte range and queue a ready task to deliver it.
	// Remove the complete packets from the range by increasing unprocessed_begin.
	// There won't be more than 64K of data plus one packet, so this shouldn't take a long time.
	uint8_t* p = unprocessed_begin;

	const bool checksumEnabled = !peerAddress.isTLS();
	loop {
		uint32_t packetLen, packetChecksum;

		//Retrieve packet length and checksum
		if (checksumEnabled) {
			if (e-p < sizeof(uint32_t) * 2) break;
			packetLen = *(uint32_t*)p; p += sizeof(uint32_t);
			packetChecksum = *(uint32_t*)p; p += sizeof(uint32_t);
		} else {
			if (e-p < sizeof(uint32_t)) break;
			packetLen = *(uint32_t*)p; p += sizeof(uint32_t);
		}

		if (packetLen > FLOW_KNOBS->PACKET_LIMIT) {
			TraceEvent(SevError, "Net2_PacketLimitExceeded").detail("FromPeer", peerAddress.toString()).detail("Length", (int)packetLen);
			throw platform_error();
		}

		if (e-p<packetLen) break;
		ASSERT( packetLen >= sizeof(UID) );

		if (checksumEnabled) {
			bool isBuggifyEnabled = false;
			if(g_network->isSimulated() && g_network->now() - g_simulator.lastConnectionFailure > g_simulator.connectionFailuresDisableDuration && BUGGIFY_WITH_PROB(0.0001)) {
				g_simulator.lastConnectionFailure = g_network->now();
				isBuggifyEnabled = true;
				TraceEvent(SevInfo, "BitsFlip");
				int flipBits = 32 - (int) floor(log2(deterministicRandom()->randomUInt32()));

				uint32_t firstFlipByteLocation = deterministicRandom()->randomUInt32() % packetLen;
				int firstFlipBitLocation = deterministicRandom()->randomInt(0, 8);
				*(p + firstFlipByteLocation) ^= 1 << firstFlipBitLocation;
				flipBits--;

				for (int i = 0; i < flipBits; i++) {
					uint32_t byteLocation = deterministicRandom()->randomUInt32() % packetLen;
					int bitLocation = deterministicRandom()->randomInt(0, 8);
					if (byteLocation != firstFlipByteLocation || bitLocation != firstFlipBitLocation) {
						*(p + byteLocation) ^= 1 << bitLocation;
					}
				}
			}

			uint32_t calculatedChecksum = crc32c_append(0, p, packetLen);
			if (calculatedChecksum != packetChecksum) {
				if (isBuggifyEnabled) {
					TraceEvent(SevInfo, "ChecksumMismatchExp").detail("PacketChecksum", (int)packetChecksum).detail("CalculatedChecksum", (int)calculatedChecksum);
				} else {
					TraceEvent(SevWarnAlways, "ChecksumMismatchUnexp").detail("PacketChecksum", (int)packetChecksum).detail("CalculatedChecksum", (int)calculatedChecksum);
				}
				throw checksum_failed();
			} else {
				if (isBuggifyEnabled) {
					TraceEvent(SevError, "ChecksumMatchUnexp").detail("PacketChecksum", (int)packetChecksum).detail("CalculatedChecksum", (int)calculatedChecksum);
				}
			}
		}

#if VALGRIND
		VALGRIND_CHECK_MEM_IS_DEFINED(p, packetLen);
#endif
		ArenaReader reader(arena, StringRef(p, packetLen), AssumeVersion(currentProtocolVersion));
		UID token;
		reader >> token;

		++transport->countPacketsReceived;

		if (packetLen > FLOW_KNOBS->PACKET_WARNING) {
			TraceEvent(transport->warnAlwaysForLargePacket ? SevWarnAlways : SevWarn, "Net2_LargePacket")
				.suppressFor(1.0)
				.detail("FromPeer", peerAddress.toString())
				.detail("Length", (int)packetLen)
				.detail("Token", token);

			if(g_network->isSimulated())
				transport->warnAlwaysForLargePacket = false;
		}

		ASSERT(!reader.empty());
		deliver(transport, Endpoint({ peerAddress }, token), std::move(reader), true);

		unprocessed_begin = p = p + packetLen;
	}
}

// Given unprocessed buffer [begin, end), check if next packet size is known and return
// enough size for the next packet, whose format is: {size, optional_checksum, data} +
// next_packet_size.
static int getNewBufferSize(const uint8_t* begin, const uint8_t* end, const NetworkAddress& peerAddress) {
	const int len = end - begin;
	if (len < sizeof(uint32_t)) {
		return FLOW_KNOBS->MIN_PACKET_BUFFER_BYTES;
	}
	const uint32_t packetLen = *(uint32_t*)begin;
	if (packetLen > FLOW_KNOBS->PACKET_LIMIT) {
		TraceEvent(SevError, "Net2_PacketLimitExceeded").detail("FromPeer", peerAddress.toString()).detail("Length", (int)packetLen);
		throw platform_error();
	}
	return std::max<uint32_t>(FLOW_KNOBS->MIN_PACKET_BUFFER_BYTES,
	                          packetLen + sizeof(uint32_t) * (peerAddress.isTLS() ? 2 : 3));
}

ACTOR static Future<Void> connectionReader(
		TransportData* transport,
		Reference<IConnection> conn,
		Peer *peer,
		Promise<Peer*> onConnected)
{
	// This actor exists whenever there is an open or opening connection, whether incoming or outgoing
	// For incoming connections conn is set and peer is initially nullptr; for outgoing connections it is the reverse

	state Arena arena;
	state uint8_t* unprocessed_begin = nullptr;
	state uint8_t* unprocessed_end = nullptr;
	state uint8_t* buffer_end = nullptr;
	state bool expectConnectPacket = true;
	state bool compatible = false;
	state bool incompatiblePeerCounted = false;
	state bool incompatibleProtocolVersionNewer = false;
	state NetworkAddress peerAddress;
	state ProtocolVersion peerProtocolVersion;

	peerAddress = conn->getPeerAddress();
	if (peer == nullptr) {
		ASSERT( !peerAddress.isPublic() );
	}
	try {
		loop {
			loop {
				state int readAllBytes = buffer_end - unprocessed_end;
				if (readAllBytes < FLOW_KNOBS->MIN_PACKET_BUFFER_FREE_BYTES) {
					Arena newArena;
					const int unproc_len = unprocessed_end - unprocessed_begin;
					const int len = getNewBufferSize(unprocessed_begin, unprocessed_end, peerAddress);
					uint8_t* const newBuffer = new (newArena) uint8_t[ len ];
					memcpy( newBuffer, unprocessed_begin, unproc_len );
					arena = newArena;
					unprocessed_begin = newBuffer;
					unprocessed_end = newBuffer + unproc_len;
					buffer_end = newBuffer + len;
					readAllBytes = buffer_end - unprocessed_end;
				}

				state int totalReadBytes = 0;
				while (true) {
					const int len = std::min<int>(buffer_end - unprocessed_end, FLOW_KNOBS->MAX_PACKET_SEND_BYTES);
					if (len == 0) break;
					state int readBytes = conn->read(unprocessed_end, unprocessed_end + len);
					if (readBytes == 0) break;
					wait(yield(TaskReadSocket));
					totalReadBytes += readBytes;
					unprocessed_end += readBytes;
				}
				if (peer) {
					peer->bytesReceived += totalReadBytes;
				}
				if (totalReadBytes == 0) break;
				state bool readWillBlock = totalReadBytes != readAllBytes;

				if (expectConnectPacket && unprocessed_end-unprocessed_begin>=CONNECT_PACKET_V0_SIZE) {
					// At the beginning of a connection, we expect to receive a packet containing the protocol version and the listening port of the remote process
					int32_t connectPacketSize = ((ConnectPacket*)unprocessed_begin)->totalPacketSize();
					if ( unprocessed_end-unprocessed_begin >= connectPacketSize ) {
						auto protocolVersion = ((ConnectPacket*)unprocessed_begin)->protocolVersion;
						BinaryReader pktReader(unprocessed_begin, connectPacketSize, AssumeVersion(protocolVersion));
						ConnectPacket pkt;
						serializer(pktReader, pkt);

						uint64_t connectionId = pkt.connectionId;
						if(g_network->useObjectSerializer() != pkt.protocolVersion.hasObjectSerializerFlag() ||
						   !pkt.protocolVersion.isCompatible(currentProtocolVersion)) {
							incompatibleProtocolVersionNewer = pkt.protocolVersion > currentProtocolVersion;
							NetworkAddress addr = pkt.canonicalRemotePort
							                          ? NetworkAddress(pkt.canonicalRemoteIp(), pkt.canonicalRemotePort)
							                          : conn->getPeerAddress();
							if(connectionId != 1) addr.port = 0;

							if(!transport->multiVersionConnections.count(connectionId)) {
								if(now() - transport->lastIncompatibleMessage > FLOW_KNOBS->CONNECTION_REJECTED_MESSAGE_DELAY) {
									TraceEvent(SevWarn, "ConnectionRejected", conn->getDebugID())
									    .detail("Reason", "IncompatibleProtocolVersion")
									    .detail("LocalVersion", currentProtocolVersion.version())
									    .detail("RejectedVersion", pkt.protocolVersion.version())
									    .detail("VersionMask", ProtocolVersion::compatibleProtocolVersionMask)
									    .detail("Peer", pkt.canonicalRemotePort ? NetworkAddress(pkt.canonicalRemoteIp(), pkt.canonicalRemotePort)
									                                            : conn->getPeerAddress())
									    .detail("ConnectionId", connectionId);
									transport->lastIncompatibleMessage = now();
								}
								if(!transport->incompatiblePeers.count(addr)) {
									transport->incompatiblePeers[ addr ] = std::make_pair(connectionId, now());
								}
							} else if(connectionId > 1) {
								transport->multiVersionConnections[connectionId] = now() + FLOW_KNOBS->CONNECTION_ID_TIMEOUT;
							}

							compatible = false;
							if(!protocolVersion.hasMultiVersionClient()) {
								// Older versions expected us to hang up. It may work even if we don't hang up here, but it's safer to keep the old behavior.
								throw incompatible_protocol_version();
							}
						}
						else {
							compatible = true;
							TraceEvent("ConnectionEstablished", conn->getDebugID())
								.suppressFor(1.0)
								.detail("Peer", conn->getPeerAddress())
								.detail("ConnectionId", connectionId)
								.detail("UseObjectSerializer", false);
						}

						if(connectionId > 1) {
							transport->multiVersionConnections[connectionId] = now() + FLOW_KNOBS->CONNECTION_ID_TIMEOUT;
						}
						unprocessed_begin += connectPacketSize;
						expectConnectPacket = false;

						if (peer != nullptr) {
							peerProtocolVersion = protocolVersion;
							// Outgoing connection; port information should be what we expect
							TraceEvent("ConnectedOutgoing")
							    .suppressFor(1.0)
							    .detail("PeerAddr", NetworkAddress(pkt.canonicalRemoteIp(), pkt.canonicalRemotePort));
							peer->compatible = compatible;
							peer->incompatibleProtocolVersionNewer = incompatibleProtocolVersionNewer;
							if (!compatible) {
								peer->transport->numIncompatibleConnections++;
								incompatiblePeerCounted = true;
							}
							ASSERT( pkt.canonicalRemotePort == peerAddress.port );
							onConnected.send(peer);
						} else {
							peerProtocolVersion = protocolVersion;
							if (pkt.canonicalRemotePort) {
								peerAddress = NetworkAddress(pkt.canonicalRemoteIp(), pkt.canonicalRemotePort, true,
								                             peerAddress.isTLS());
							}
							peer = transport->getPeer(peerAddress);
							peer->compatible = compatible;
							peer->incompatibleProtocolVersionNewer = incompatibleProtocolVersionNewer;
							if (!compatible) {
								peer->transport->numIncompatibleConnections++;
								incompatiblePeerCounted = true;
							}
							onConnected.send( peer );
							wait( delay(0) );  // Check for cancellation
						}
					}
				}
				if (compatible) {
					scanPackets( transport, unprocessed_begin, unprocessed_end, arena, peerAddress, peerProtocolVersion );
				}
				else if(!expectConnectPacket) {
					unprocessed_begin = unprocessed_end;
					peer->resetPing.trigger();
				}

				if (readWillBlock)
					break;

				wait(yield(TaskReadSocket));
			}

			wait( conn->onReadable() );
			wait(delay(0, TaskReadSocket));  // We don't want to call conn->read directly from the reactor - we could get stuck in the reactor reading 1 packet at a time
		}
	}
	catch (Error& e) {
		if (incompatiblePeerCounted) {
			ASSERT(peer && peer->transport->numIncompatibleConnections > 0);
			peer->transport->numIncompatibleConnections--;
		}
		throw;
	}
}

ACTOR static Future<Void> connectionIncoming( TransportData* self, Reference<IConnection> conn ) {
	try {
		state Promise<Peer*> onConnected;
		state Future<Void> reader = connectionReader( self, conn, nullptr, onConnected );
		choose {
			when( wait( reader ) ) { ASSERT(false); return Void(); }
			when( Peer *p = wait( onConnected.getFuture() ) ) {
				p->onIncomingConnection( conn, reader );
			}
			when( wait( delayJittered(FLOW_KNOBS->CONNECTION_MONITOR_TIMEOUT) ) ) {
				TEST(true);  // Incoming connection timed out
				throw timed_out();
			}
		}
		return Void();
	} catch (Error& e) {
		TraceEvent("IncomingConnectionError", conn->getDebugID()).error(e).suppressFor(1.0).detail("FromAddress", conn->getPeerAddress());
		conn->close();
		return Void();
	}
}

ACTOR static Future<Void> listen( TransportData* self, NetworkAddress listenAddr ) {
	state ActorCollectionNoErrors incoming;  // Actors monitoring incoming connections that haven't yet been associated with a peer
	state Reference<IListener> listener = INetworkConnections::net()->listen( listenAddr );
	try {
		loop {
			Reference<IConnection> conn = wait( listener->accept() );
			TraceEvent("ConnectionFrom", conn->getDebugID()).suppressFor(1.0)
				.detail("FromAddress", conn->getPeerAddress())
				.detail("ListenAddress", listenAddr.toString());
			incoming.add( connectionIncoming(self, conn) );
			wait(delay(0) || delay(FLOW_KNOBS->CONNECTION_ACCEPT_DELAY, TaskWriteSocket));
		}
	} catch (Error& e) {
		TraceEvent(SevError, "ListenError").error(e);
		throw;
	}
}

Peer* TransportData::getPeer( NetworkAddress const& address, bool openConnection ) {
	auto peer = peers.find(address);
	if (peer != peers.end()) {
		return peer->second;
	}
	if(!openConnection) {
		return nullptr;
	}
	Peer* newPeer = new Peer(this, address);
	peers[address] = newPeer;
	return newPeer;
}

bool TransportData::isLocalAddress(const NetworkAddress& address) const {
	return address == localAddresses.address || (localAddresses.secondaryAddress.present() && address == localAddresses.secondaryAddress.get());
}

ACTOR static Future<Void> multiVersionCleanupWorker( TransportData* self ) {
	loop {
		wait(delay(FLOW_KNOBS->CONNECTION_CLEANUP_DELAY));
		for(auto it = self->incompatiblePeers.begin(); it != self->incompatiblePeers.end();) {
			if( self->multiVersionConnections.count(it->second.first) ) {
				it = self->incompatiblePeers.erase(it);
			} else {
				it++;
			}
		}

		for(auto it = self->multiVersionConnections.begin(); it != self->multiVersionConnections.end();) {
			if( it->second < now() ) {
				it = self->multiVersionConnections.erase(it);
			} else {
				it++;
			}
		}
	}
}

FlowTransport::FlowTransport( uint64_t transportId ) : self(new TransportData(transportId)) {
	self->multiVersionCleanup = multiVersionCleanupWorker(self);
}

FlowTransport::~FlowTransport() { delete self; }

void FlowTransport::initMetrics() { self->initMetrics(); }

NetworkAddressList FlowTransport::getLocalAddresses() const {
	return self->localAddresses;
}

NetworkAddress FlowTransport::getLocalAddress() const {
	return self->localAddresses.address;
}

std::map<NetworkAddress, std::pair<uint64_t, double>>* FlowTransport::getIncompatiblePeers() {
	for(auto it = self->incompatiblePeers.begin(); it != self->incompatiblePeers.end();) {
		if( self->multiVersionConnections.count(it->second.first) ) {
			it = self->incompatiblePeers.erase(it);
		} else {
			it++;
		}
	}
	return &self->incompatiblePeers;
}

Future<Void> FlowTransport::bind( NetworkAddress publicAddress, NetworkAddress listenAddress ) {
	ASSERT( publicAddress.isPublic() );
	if(self->localAddresses.address == NetworkAddress()) {
		self->localAddresses.address = publicAddress;
	} else {
		self->localAddresses.secondaryAddress = publicAddress;
	}
	TraceEvent("Binding").detail("PublicAddress", publicAddress).detail("ListenAddress", listenAddress);

	Future<Void> listenF = listen( self, listenAddress );
	self->listeners.push_back(listenF);
	return listenF;
}

Endpoint FlowTransport::loadedEndpoint( const UID& token ) {
	return Endpoint(g_currentDeliveryPeerAddress, token);
}

void FlowTransport::addPeerReference( const Endpoint& endpoint, NetworkMessageReceiver* receiver ) {
	if (FlowTransport::transport().isClient()) {
		IFailureMonitor::failureMonitor().setStatus(endpoint.getPrimaryAddress(), FailureStatus(false));
	}

	if (!receiver->isStream() || !endpoint.getPrimaryAddress().isValid()) return;
	Peer* peer = self->getPeer(endpoint.getPrimaryAddress());
	if(peer->peerReferences == -1) {
		peer->peerReferences = 1;
	} else {
		peer->peerReferences++;
	}
}

void FlowTransport::removePeerReference( const Endpoint& endpoint, NetworkMessageReceiver* receiver ) {
	if (!receiver->isStream() || !endpoint.getPrimaryAddress().isValid()) return;
	Peer* peer = self->getPeer(endpoint.getPrimaryAddress(), false);
	if(peer) {
		peer->peerReferences--;
		if(peer->peerReferences < 0) {
			TraceEvent(SevError, "InvalidPeerReferences")
				.detail("References", peer->peerReferences)
				.detail("Address", endpoint.getPrimaryAddress())
				.detail("Token", endpoint.token);
		}
		if(peer->peerReferences == 0 && peer->reliable.empty() && peer->unsent.empty()) {
			peer->resetPing.trigger();
		}
	}
}

void FlowTransport::addEndpoint( Endpoint& endpoint, NetworkMessageReceiver* receiver, uint32_t taskID ) {
	endpoint.token = deterministicRandom()->randomUniqueID();
	if (receiver->isStream()) {
		endpoint.addresses = self->localAddresses;
		endpoint.token = UID( endpoint.token.first() | TOKEN_STREAM_FLAG, endpoint.token.second() );
	} else {
		endpoint.addresses = NetworkAddressList();
		endpoint.token = UID( endpoint.token.first() & ~TOKEN_STREAM_FLAG, endpoint.token.second() );
	}
	self->endpoints.insert( receiver, endpoint.token, taskID );
}

void FlowTransport::removeEndpoint( const Endpoint& endpoint, NetworkMessageReceiver* receiver ) {
	self->endpoints.remove(endpoint.token, receiver);
}

void FlowTransport::addWellKnownEndpoint( Endpoint& endpoint, NetworkMessageReceiver* receiver, uint32_t taskID ) {
	endpoint.addresses = self->localAddresses;
	ASSERT( ((endpoint.token.first() & TOKEN_STREAM_FLAG)!=0) == receiver->isStream() );
	Endpoint::Token otoken = endpoint.token;
	self->endpoints.insert( receiver, endpoint.token, taskID );
	ASSERT( endpoint.token == otoken );
}

static PacketID sendPacket( TransportData* self, ISerializeSource const& what, const Endpoint& destination, bool reliable, bool openConnection ) {
	if (self->isLocalAddress(destination.getPrimaryAddress())) {
		TEST(true); // "Loopback" delivery
		// SOMEDAY: Would it be better to avoid (de)serialization by doing this check in flow?

		Standalone<StringRef> copy;
		if (g_network->useObjectSerializer()) {
			ObjectWriter wr;
			what.serializeObjectWriter(wr);
			copy = wr.toStringRef();
		} else {
			BinaryWriter wr( AssumeVersion(currentProtocolVersion) );
			what.serializeBinaryWriter(wr);
			copy = wr.toValue();
		}
#if VALGRIND
		VALGRIND_CHECK_MEM_IS_DEFINED(copy.begin(), copy.size());
#endif

		ASSERT(copy.size() > 0);
		deliver(self, destination, ArenaReader(copy.arena(), copy, AssumeVersion(currentProtocolVersion)), false);

		return (PacketID)nullptr;
	} else {
		const bool checksumEnabled = !destination.getPrimaryAddress().isTLS();
		++self->countPacketsGenerated;

		Peer* peer = self->getPeer(destination.getPrimaryAddress(), openConnection);

		// If there isn't an open connection, a public address, or the peer isn't compatible, we can't send
		if (!peer || (peer->outgoingConnectionIdle && !destination.getPrimaryAddress().isPublic()) || (peer->incompatibleProtocolVersionNewer && destination.token != WLTOKEN_PING_PACKET)) {
			TEST(true);  // Can't send to private address without a compatible open connection
			return (PacketID)nullptr;
		}

		bool firstUnsent = peer->unsent.empty();

		PacketBuffer* pb = peer->unsent.getWriteBuffer();
		ReliablePacket* rp = reliable ? new ReliablePacket : 0;

		int prevBytesWritten = pb->bytes_written;
		PacketBuffer* checksumPb = pb;

		PacketWriter wr(pb,rp,AssumeVersion(currentProtocolVersion));  // SOMEDAY: Can we downgrade to talk to older peers?

		// Reserve some space for packet length and checksum, write them after serializing data
		SplitBuffer packetInfoBuffer;
		uint32_t len, checksum = 0;
		int packetInfoSize = sizeof(len);
		if (checksumEnabled) {
			packetInfoSize += sizeof(checksum);
		}

		wr.writeAhead(packetInfoSize , &packetInfoBuffer);
		wr << destination.token;
		what.serializePacketWriter(wr, g_network->useObjectSerializer());
		pb = wr.finish();
		len = wr.size() - packetInfoSize;

		if (checksumEnabled) {
			// Find the correct place to start calculating checksum
			uint32_t checksumUnprocessedLength = len;
			prevBytesWritten += packetInfoSize;
			if (prevBytesWritten >= PacketBuffer::DATA_SIZE) {
				prevBytesWritten -= PacketBuffer::DATA_SIZE;
				checksumPb = checksumPb->nextPacketBuffer();
			}

			// Checksum calculation
			while (checksumUnprocessedLength > 0) {
				uint32_t processLength = std::min(checksumUnprocessedLength, (uint32_t)(PacketBuffer::DATA_SIZE - prevBytesWritten));
				checksum = crc32c_append(checksum, checksumPb->data + prevBytesWritten, processLength);
				checksumUnprocessedLength -= processLength;
				checksumPb = checksumPb->nextPacketBuffer();
				prevBytesWritten = 0;
			}
		}

		// Write packet length and checksum into packet buffer
		packetInfoBuffer.write(&len, sizeof(len));
		if (checksumEnabled) {
			packetInfoBuffer.write(&checksum, sizeof(checksum), sizeof(len));
		}

		if (len > FLOW_KNOBS->PACKET_LIMIT) {
			TraceEvent(SevError, "Net2_PacketLimitExceeded").detail("ToPeer", destination.getPrimaryAddress()).detail("Length", (int)len);
			// throw platform_error();  // FIXME: How to recover from this situation?
		}
		else if (len > FLOW_KNOBS->PACKET_WARNING) {
			TraceEvent(self->warnAlwaysForLargePacket ? SevWarnAlways : SevWarn, "Net2_LargePacket")
				.suppressFor(1.0)
				.detail("ToPeer", destination.getPrimaryAddress())
				.detail("Length", (int)len)
				.detail("Token", destination.token)
				.backtrace();

			if(g_network->isSimulated())
				self->warnAlwaysForLargePacket = false;
		}

#if VALGRIND
		SendBuffer *checkbuf = pb;
		while (checkbuf) {
			int size = checkbuf->bytes_written;
			const uint8_t* data = checkbuf->data;
			VALGRIND_CHECK_MEM_IS_DEFINED(data, size);
			checkbuf = checkbuf -> next;
		}
#endif

		peer->send(pb, rp, firstUnsent);

		return (PacketID)rp;
	}
}

PacketID FlowTransport::sendReliable( ISerializeSource const& what, const Endpoint& destination ) {
	return sendPacket( self, what, destination, true, true );
}

void FlowTransport::cancelReliable( PacketID pid ) {
	ReliablePacket* p = (ReliablePacket*)pid;
	if (p) p->remove();
	// SOMEDAY: Call reliable.compact() if a lot of memory is wasted in PacketBuffers by formerly reliable packets mixed with a few reliable ones.  Don't forget to delref the new PacketBuffers since they are unsent.
}

void FlowTransport::sendUnreliable( ISerializeSource const& what, const Endpoint& destination, bool openConnection ) {
	sendPacket( self, what, destination, false, openConnection );
}

int FlowTransport::getEndpointCount() {
	return -1;
}

Reference<AsyncVar<bool>> FlowTransport::getDegraded() {
	return self->degraded;
}

bool FlowTransport::incompatibleOutgoingConnectionsPresent() {
	return self->numIncompatibleConnections > 0;
}

void FlowTransport::createInstance(bool isClient, uint64_t transportId) {
	g_network->setGlobal(INetwork::enFailureMonitor, (flowGlobalType) new SimpleFailureMonitor());
	g_network->setGlobal(INetwork::enClientFailureMonitor, isClient ? (flowGlobalType)1 : nullptr);
	g_network->setGlobal(INetwork::enFlowTransport, (flowGlobalType) new FlowTransport(transportId));
	g_network->setGlobal(INetwork::enNetworkAddressFunc, (flowGlobalType) &FlowTransport::getGlobalLocalAddress);
	g_network->setGlobal(INetwork::enNetworkAddressesFunc, (flowGlobalType) &FlowTransport::getGlobalLocalAddresses);
}
