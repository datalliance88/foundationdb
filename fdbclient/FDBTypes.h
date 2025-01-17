/*
 * FDBTypes.h
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

#ifndef FDBCLIENT_FDBTYPES_H
#define FDBCLIENT_FDBTYPES_H

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "flow/flow.h"
#include "fdbclient/Knobs.h"

typedef int64_t Version;
typedef uint64_t LogEpoch;
typedef uint64_t Sequence;
typedef StringRef KeyRef;
typedef StringRef ValueRef;
typedef int64_t Generation;

enum {
	tagLocalitySpecial = -1,
	tagLocalityLogRouter = -2,
	tagLocalityRemoteLog = -3,
	tagLocalityUpgraded = -4,
	tagLocalitySatellite = -5,
	tagLocalityLogRouterMapped = -6,
	tagLocalityInvalid = -99
}; //The TLog and LogRouter require these number to be as compact as possible

#pragma pack(push, 1)
struct Tag {
	int8_t locality;
	uint16_t id;

	Tag() : locality(tagLocalitySpecial), id(0) {}
	Tag(int8_t locality, uint16_t id) : locality(locality), id(id) {}

	bool operator == ( const Tag& r ) const { return locality==r.locality && id==r.id; }
	bool operator != ( const Tag& r ) const { return locality!=r.locality || id!=r.id; }
	bool operator < ( const Tag& r ) const { return locality < r.locality || (locality == r.locality && id < r.id); }

	int toTagDataIndex() {
		return locality >= 0 ? 2 * locality : 1 - (2 * locality);
	}

	std::string toString() const {
		return format("%d:%d", locality, id);
	}

	template <class Ar>
	force_inline void serialize_unversioned(Ar& ar) { 
		serializer(ar, locality, id);
	}
};
#pragma pack(pop)

template <class Ar> void load( Ar& ar, Tag& tag ) { tag.serialize_unversioned(ar); }
template <class Ar> void save( Ar& ar, Tag const& tag ) { const_cast<Tag&>(tag).serialize_unversioned(ar); }

template <>
struct struct_like_traits<Tag> : std::true_type {
	using Member = Tag;
	using types = pack<uint16_t, int8_t>;

	template <int i>
	static const index_t<i, types>& get(const Member& m) {
		if constexpr (i == 0) {
			return m.id;
		} else {
			static_assert(i == 1);
			return m.locality;
		}
	}

	template <int i, class Type>
	static const void assign(Member& m, const Type& t) {
		if constexpr (i == 0) {
			m.id = t;
		} else {
			static_assert(i == 1);
			m.locality = t;
		}
	}
};

static const Tag invalidTag {tagLocalitySpecial, 0};
static const Tag txsTag {tagLocalitySpecial, 1};

enum { txsTagOld = -1, invalidTagOld = -100 };

struct TagsAndMessage {
	StringRef message;
	std::vector<Tag> tags;

	TagsAndMessage() {}
	TagsAndMessage(StringRef message, const std::vector<Tag>& tags) : message(message), tags(tags) {}
};

struct KeyRangeRef;
struct KeyValueRef;

template <class Collection>
void uniquify( Collection& c ) {
	std::sort(c.begin(), c.end());
	c.resize( std::unique(c.begin(), c.end()) - c.begin() );
}

static std::string describe( const Tag item ) {
	return format("%d:%d", item.locality, item.id);
}

static std::string describe( const int item ) {
	return format("%d", item);
}

template <class T>
static std::string describe( Reference<T> const& item ) {
	return item->toString();
}

template <class T>
static std::string describe( T const& item ) {
	return item.toString();
}

template <class K, class V>
static std::string describe( std::map<K, V> const& items, int max_items = -1 ) {
	if(!items.size())
		return "[no items]";

	std::string s;
	int count = 0;
	for(auto it = items.begin(); it != items.end(); it++) {
		if( ++count > max_items && max_items >= 0)
			break;
		if (count > 1) s += ",";
		s += describe(it->first) + "=>" + describe(it->second);
	}
	return s;
}

template <class T>
static std::string describeList( T const& items, int max_items ) {
	if(!items.size())
		return "[no items]";

	std::string s;
	int count = 0;
	for(auto const& item : items) {
		if( ++count > max_items && max_items >= 0)
			break;
		if (count > 1) s += ",";
		s += describe(item);
	}
	return s;
}

template <class T>
static std::string describe( std::vector<T> const& items, int max_items = -1 ) {
	return describeList(items, max_items);
}

template <class T>
static std::string describe( std::set<T> const& items, int max_items = -1 ) {
	return describeList(items, max_items);
}

std::string printable( const StringRef& val );
std::string printable( const std::string& val );
std::string printable( const KeyRangeRef& range );
std::string printable( const VectorRef<StringRef>& val );
std::string printable( const VectorRef<KeyValueRef>& val );
std::string printable( const KeyValueRef& val );

template <class T>
std::string printable( const Optional<T>& val ) {
	if( val.present() )
		return printable( val.get() );
	return "[not set]";
}

inline bool equalsKeyAfter( const KeyRef& key, const KeyRef& compareKey ) {
	if( key.size()+1 != compareKey.size() || compareKey[compareKey.size()-1] != 0 )
		return false;
	return compareKey.startsWith( key );
}

struct KeyRangeRef {
	const KeyRef begin, end;
	KeyRangeRef() {}
	KeyRangeRef( const KeyRef& begin, const KeyRef& end ) : begin(begin), end(end) {
		if( begin > end ) {
			throw inverted_range();
		}
	}
	KeyRangeRef( Arena& a, const KeyRangeRef& copyFrom ) : begin(a, copyFrom.begin), end(a, copyFrom.end) {}
	bool operator == ( const KeyRangeRef& r ) const { return begin == r.begin && end == r.end; }
	bool operator != ( const KeyRangeRef& r ) const { return begin != r.begin || end != r.end; }
	bool contains( const KeyRef& key ) const { return begin <= key && key < end; }
	bool contains( const KeyRangeRef& keys ) const { return begin <= keys.begin && keys.end <= end; }
	bool intersects( const KeyRangeRef& keys ) const { return begin < keys.end && keys.begin < end; }
	bool empty() const { return begin == end; }
	bool singleKeyRange() const { return equalsKeyAfter(begin, end); }

	Standalone<KeyRangeRef> withPrefix( const StringRef& prefix ) const {
		return KeyRangeRef( begin.withPrefix(prefix), end.withPrefix(prefix) );
	}

	KeyRangeRef removePrefix( const StringRef& prefix ) const {
		return KeyRangeRef( begin.removePrefix(prefix), end.removePrefix(prefix) );
	}

	const KeyRangeRef& operator = (const KeyRangeRef& rhs) {
		const_cast<KeyRef&>(begin) = rhs.begin;
		const_cast<KeyRef&>(end) = rhs.end;
		return *this;
	}

	int expectedSize() const { return begin.expectedSize() + end.expectedSize(); }

	template <class Ar>
	force_inline void serialize(Ar& ar) {
		serializer(ar, const_cast<KeyRef&>(begin), const_cast<KeyRef&>(end));
		if( begin > end ) {
			throw inverted_range();
		};
	}

	struct ArbitraryOrder {
		bool operator()(KeyRangeRef const& a, KeyRangeRef const& b) const {
			if (a.begin < b.begin) return true;
			if (a.begin > b.begin) return false;
			return a.end < b.end;
		}
	};
};

template<>
struct Traceable<KeyRangeRef> : std::true_type {
	static std::string toString(const KeyRangeRef& value) {
		auto begin = Traceable<StringRef>::toString(value.begin);
		auto end = Traceable<StringRef>::toString(value.end);
		std::string result;
		result.reserve(begin.size() + end.size() + 3);
		std::copy(begin.begin(), begin.end(), std::back_inserter(result));
		result.push_back(' ');
		result.push_back('-');
		result.push_back(' ');
		std::copy(end.begin(), end.end(), std::back_inserter(result));
		return result;
	}
};


inline KeyRangeRef operator & (const KeyRangeRef& lhs, const KeyRangeRef& rhs) {
	KeyRef b = std::max(lhs.begin, rhs.begin), e = std::min(lhs.end, rhs.end);
	if (e < b)
		return KeyRangeRef();
	return KeyRangeRef(b,e);
}

struct KeyValueRef {
	KeyRef key;
	ValueRef value;
	KeyValueRef() {}
	KeyValueRef( const KeyRef& key, const ValueRef& value ) : key(key), value(value) {}
	KeyValueRef( Arena& a, const KeyValueRef& copyFrom ) : key(a, copyFrom.key), value(a, copyFrom.value) {}
	bool operator == ( const KeyValueRef& r ) const { return key == r.key && value == r.value; }
	bool operator != ( const KeyValueRef& r ) const { return key != r.key || value != r.value; }

	int expectedSize() const { return key.expectedSize() + value.expectedSize(); }

	template <class Ar>
	force_inline void serialize(Ar& ar) { serializer(ar, key, value); }

	struct OrderByKey {
		bool operator()(KeyValueRef const& a, KeyValueRef const& b) const {
			return a.key < b.key;
		}
		template <class T>
		bool operator()(T const& a, KeyValueRef const& b) const {
			return a < b.key;
		}
		template <class T>
		bool operator()(KeyValueRef const& a, T const& b) const {
			return a.key < b;
		}
	};

	struct OrderByKeyBack {
		bool operator()(KeyValueRef const& a, KeyValueRef const& b) const {
			return a.key > b.key;
		}
		template <class T>
		bool operator()(T const& a, KeyValueRef const& b) const {
			return a > b.key;
		}
		template <class T>
		bool operator()(KeyValueRef const& a, T const& b) const {
			return a.key > b;
		}
	};
};

template<>
struct Traceable<KeyValueRef> : std::true_type {
	static std::string toString(const KeyValueRef& value) {
		return Traceable<KeyRef>::toString(value.key) + format(":%d", value.value.size());
	}
};

typedef Standalone<KeyRef> Key;
typedef Standalone<ValueRef> Value;
typedef Standalone<KeyRangeRef> KeyRange;
typedef Standalone<KeyValueRef> KeyValue;
typedef Standalone<struct KeySelectorRef> KeySelector; 

enum { invalidVersion = -1, latestVersion = -2 };

inline Key keyAfter( const KeyRef& key ) {
	if(key == LiteralStringRef("\xff\xff"))
		return key;

	Standalone<StringRef> r;
	uint8_t* s = new (r.arena()) uint8_t[ key.size() + 1 ];
	memcpy(s, key.begin(), key.size() );
	s[key.size()] = 0;
	((StringRef&) r) = StringRef( s, key.size() + 1 );
	return r;
}
inline KeyRef keyAfter( const KeyRef& key, Arena& arena ) {
	if(key == LiteralStringRef("\xff\xff"))
		return key;
	uint8_t* t = new ( arena ) uint8_t[ key.size()+1 ];
	memcpy(t, key.begin(), key.size() );
	t[key.size()] = 0;
	return KeyRef(t,key.size()+1);
}
inline KeyRange singleKeyRange( const KeyRef& a ) {
	return KeyRangeRef(a, keyAfter(a));
}
inline KeyRangeRef singleKeyRange( KeyRef const& key, Arena& arena ) {
	uint8_t* t = new ( arena ) uint8_t[ key.size()+1 ];
	memcpy(t, key.begin(), key.size() );
	t[key.size()] = 0;
	return KeyRangeRef( KeyRef(t,key.size()), KeyRef(t, key.size()+1) );
}
inline KeyRange prefixRange( KeyRef prefix ) {
	Standalone<KeyRangeRef> range;
	KeyRef start = KeyRef(range.arena(), prefix);
	KeyRef end = strinc(prefix, range.arena());
	range.contents() = KeyRangeRef(start, end);
	return range;
}
inline KeyRef keyBetween( const KeyRangeRef& keys ) {
	// Returns (one of) the shortest key(s) either contained in keys or equal to keys.end,
	// assuming its length is no more than CLIENT_KNOBS->SPLIT_KEY_SIZE_LIMIT. If the length of
	// the shortest key exceeds that limit, then the end key is returned.
	// The returned reference is valid as long as keys is valid.

	int pos = 0;  // will be the position of the first difference between keys.begin and keys.end
	int minSize = std::min( keys.begin.size(), keys.end.size() );
	for(; pos < minSize && pos < CLIENT_KNOBS->SPLIT_KEY_SIZE_LIMIT; pos++ ) {
		if( keys.begin[pos] != keys.end[pos] ) {
			return keys.end.substr(0,pos+1);
		}
	}

	// If one more character keeps us in the limit, and the latter key is simply
	// longer, then we only need one more byte of the end string.
	if (pos < CLIENT_KNOBS->SPLIT_KEY_SIZE_LIMIT && keys.begin.size() < keys.end.size()) {
		return keys.end.substr(0,pos+1);
	}

	return keys.end;
}

struct KeySelectorRef {
private:
	KeyRef key;		// Find the last item less than key

public:
	bool orEqual;	// (or equal to key, if this is true)
	int offset;		// and then move forward this many items (or backward if negative)
	KeySelectorRef() : orEqual(false), offset(0) {}
	KeySelectorRef( const KeyRef& key, bool orEqual, int offset ) : orEqual(orEqual), offset(offset) {
		setKey(key);
	}

	KeySelectorRef( Arena& arena, const KeySelectorRef& copyFrom ) : key(arena, copyFrom.key), orEqual(copyFrom.orEqual), offset(copyFrom.offset) {}
	int expectedSize() const { return key.expectedSize(); }

	void removeOrEqual(Arena &arena) {
		if(orEqual) {
			setKey(keyAfter(key, arena));
			orEqual = false;
		}
	}

	KeyRef getKey() const {
		return key;
	}

	void setKey(KeyRef const& key) {
		//There are no keys in the database with size greater than KEY_SIZE_LIMIT, so if this key selector has a key which is large,
		//then we can translate it to an equivalent key selector with a smaller key
		if(key.size() > (key.startsWith(LiteralStringRef("\xff")) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT))
			this->key = key.substr(0, (key.startsWith(LiteralStringRef("\xff")) ? CLIENT_KNOBS->SYSTEM_KEY_SIZE_LIMIT : CLIENT_KNOBS->KEY_SIZE_LIMIT)+1);
		else
			this->key = key;
	}

	std::string toString() const {
		if (offset > 0) {
			if (orEqual) return format("%d+firstGreaterThan(%s)", offset-1, printable(key).c_str());
			else return format("%d+firstGreaterOrEqual(%s)", offset-1, printable(key).c_str());
		} else {
			if (orEqual) return format("%d+lastLessOrEqual(%s)", offset, printable(key).c_str());
			else return format("%d+lastLessThan(%s)", offset, printable(key).c_str());
		}
	}

	bool isBackward() const { return !orEqual && offset<=0; } // True if the resolution of the KeySelector depends only on keys less than key
	bool isFirstGreaterOrEqual() const { return !orEqual && offset==1; }
	bool isFirstGreaterThan() const { return orEqual && offset==1; }
	bool isLastLessOrEqual() const { return orEqual && offset==0; }

	// True iff, regardless of the contents of the database, lhs must resolve to a key > rhs
	bool isDefinitelyGreater( KeyRef const& k ) {
		return offset >= 1 && ( isFirstGreaterOrEqual() ? key > k : key >= k );
	}
	// True iff, regardless of the contents of the database, lhs must resolve to a key < rhs
	bool isDefinitelyLess( KeyRef const& k ) {
		return offset <= 0 && ( isLastLessOrEqual() ? key < k : key <= k );
	}

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, key, orEqual, offset);
	}
};

inline bool operator == (const KeySelectorRef& lhs, const KeySelectorRef& rhs) { return lhs.getKey() == rhs.getKey() && lhs.orEqual==rhs.orEqual && lhs.offset==rhs.offset; }
inline KeySelectorRef lastLessThan( const KeyRef& k ) {
	return KeySelectorRef( k, false, 0 );
}
inline KeySelectorRef lastLessOrEqual( const KeyRef& k ) {
	return KeySelectorRef( k, true, 0 );
}
inline KeySelectorRef firstGreaterThan( const KeyRef& k ) {
	return KeySelectorRef( k, true, +1 );
}
inline KeySelectorRef firstGreaterOrEqual( const KeyRef& k ) {
	return KeySelectorRef( k, false, +1 );
}
inline KeySelectorRef operator + (const KeySelectorRef& s, int off) {
	return KeySelectorRef(s.getKey(), s.orEqual, s.offset+off);
}
inline KeySelectorRef operator - (const KeySelectorRef& s, int off) {
	return KeySelectorRef(s.getKey(), s.orEqual, s.offset-off);
}

template <class Val>
struct KeyRangeWith : KeyRange {
	Val value;
	KeyRangeWith() {}
	KeyRangeWith( const KeyRangeRef& range, const Val& value ) : KeyRange(range), value(value) {}
	bool operator == ( const KeyRangeWith& r ) const { return KeyRangeRef::operator==(r) && value == r.value; }

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, ((KeyRange&)*this), value);
	}
};
template <class Val>
static inline KeyRangeWith<Val> keyRangeWith( const KeyRangeRef& range, const Val& value ) {
	return KeyRangeWith<Val>(range, value);
}

struct GetRangeLimits {
	enum { ROW_LIMIT_UNLIMITED = -1, BYTE_LIMIT_UNLIMITED = -1 };

	int rows;
	int minRows;
	int bytes;

	GetRangeLimits() : rows( ROW_LIMIT_UNLIMITED ), minRows(1), bytes( BYTE_LIMIT_UNLIMITED ) {}
	explicit GetRangeLimits( int rowLimit ) : rows( rowLimit ), minRows(1), bytes( BYTE_LIMIT_UNLIMITED ) {}
	GetRangeLimits( int rowLimit, int byteLimit ) : rows( rowLimit ), minRows(1), bytes( byteLimit ) {}

	void decrement( VectorRef<KeyValueRef> const& data );
	void decrement( KeyValueRef const& data );

	// True if either the row or byte limit has been reached
	bool isReached();

	// True if data would cause the row or byte limit to be reached
	bool reachedBy( VectorRef<KeyValueRef> const& data );

	bool hasByteLimit();
	bool hasRowLimit();

	bool hasSatisfiedMinRows();
	bool isValid() { return (rows >= 0 || rows == ROW_LIMIT_UNLIMITED)
							&& (bytes >= 0 || bytes == BYTE_LIMIT_UNLIMITED)
							&& minRows >= 0 && (minRows <= rows || rows == ROW_LIMIT_UNLIMITED); }
};

struct RangeResultRef : VectorRef<KeyValueRef> {
	bool more;  // True if (but not necessarily only if) values remain in the *key* range requested (possibly beyond the limits requested)
	            // False implies that no such values remain
	Optional<KeyRef> readThrough;  // Only present when 'more' is true. When present, this value represent the end (or beginning if reverse) of the range
								   // which was read to produce these results. This is guarenteed to be less than the requested range.
	bool readToBegin;
	bool readThroughEnd;

	RangeResultRef() : more(false), readToBegin(false), readThroughEnd(false) {}
	RangeResultRef( Arena& p, const RangeResultRef& toCopy ) : more( toCopy.more ), readToBegin( toCopy.readToBegin ), readThroughEnd( toCopy.readThroughEnd ), readThrough( toCopy.readThrough.present() ? KeyRef( p, toCopy.readThrough.get() ) : Optional<KeyRef>() ), VectorRef<KeyValueRef>( p, toCopy ) {}
	RangeResultRef( const VectorRef<KeyValueRef>& value, bool more, Optional<KeyRef> readThrough = Optional<KeyRef>() ) : VectorRef<KeyValueRef>( value ), more( more ), readThrough( readThrough ), readToBegin( false ), readThroughEnd( false ) {}
	RangeResultRef( bool readToBegin, bool readThroughEnd ) : more(false), readToBegin(readToBegin), readThroughEnd(readThroughEnd) { }

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, ((VectorRef<KeyValueRef>&)*this), more, readThrough, readToBegin, readThroughEnd);
	}
};

template<>
struct Traceable<RangeResultRef> : std::true_type {
	static std::string toString(const RangeResultRef& value) {
		return Traceable<VectorRef<KeyValueRef>>::toString(value);
	}
};

struct KeyValueStoreType {
	constexpr static FileIdentifier file_identifier = 6560359;
	// These enumerated values are stored in the database configuration, so can NEVER be changed.  Only add new ones just before END.
	enum StoreType {
		SSD_BTREE_V1,
		MEMORY,
		SSD_BTREE_V2,
		SSD_REDWOOD_V1,
		END
	};

	KeyValueStoreType() : type(END) {}
	KeyValueStoreType( StoreType type ) : type(type) {
		if ((uint32_t)type > END)
			this->type = END;
	}
	operator StoreType() const { return StoreType(type); }

	template <class Ar>
	void serialize(Ar& ar) { serializer(ar, type); }

	std::string toString() const {
		switch( type ) {
			case SSD_BTREE_V1: return "ssd-1";
			case SSD_BTREE_V2: return "ssd-2";
			case SSD_REDWOOD_V1: return "ssd-redwood-experimental";
			case MEMORY: return "memory";
			default: return "unknown";
		}
	}

private:
	uint32_t type;
};

template<>
struct Traceable<KeyValueStoreType> : std::true_type {
	static std::string toString(KeyValueStoreType const& value) {
		return value.toString();
	}
};

struct TLogVersion {
	enum Version {
		UNSET = 0,
		// Everything between BEGIN and END should be densely packed, so that we
		// can iterate over them easily.
		// V1 = 1,  // 4.6 is dispatched to via 6.0
		V2 = 2, // 6.0
		V3 = 3, // 6.1
		MIN_SUPPORTED = V2,
		MAX_SUPPORTED = V3,
		MIN_RECRUITABLE = V2,
		DEFAULT = V3,
	} version;

	TLogVersion() : version(UNSET) {}
	TLogVersion( Version v ) : version(v) {}

	operator Version() const {
		return version;
	}

	template <class Ar>
	void serialize(Ar& ar) {
		uint32_t v = (uint32_t)version;
		serializer(ar, v);
		version = (Version)v;
	}

	static ErrorOr<TLogVersion> FromStringRef( StringRef s ) {
		if (s == LiteralStringRef("2")) return V2;
		if (s == LiteralStringRef("3")) return V3;
		return default_error_or();
	}
};

template<>
struct Traceable<TLogVersion> : std::true_type {
	static std::string toString(TLogVersion const& value) {
		return Traceable<Version>::toString(value.version);
	}
};

struct TLogSpillType {
	// These enumerated values are stored in the database configuration, so can NEVER be changed.  Only add new ones just before END.
	enum SpillType {
		UNSET = 0,
		DEFAULT = 2,
		VALUE = 1,
		REFERENCE = 2,
		END = 3,
	};

	TLogSpillType() : type(DEFAULT) {}
	TLogSpillType( SpillType type ) : type(type) {
		if ((uint32_t)type >= END) {
			this->type = UNSET;
		}
	}
	operator SpillType() const { return SpillType(type); }

	template <class Ar>
	void serialize(Ar& ar) { serializer(ar, type); }

	std::string toString() const {
		switch( type ) {
			case VALUE: return "value";
			case REFERENCE: return "reference";
			case UNSET: return "unset";
			default: ASSERT(false);
		}
		return "";
	}

	static ErrorOr<TLogSpillType> FromStringRef( StringRef s ) {
		if ( s == LiteralStringRef("1") ) return VALUE;
		if ( s == LiteralStringRef("2") ) return REFERENCE;
		return default_error_or();
	}

private:
	uint32_t type;
};

//Contains the amount of free and total space for a storage server, in bytes
struct StorageBytes {
	int64_t free;
	int64_t total;
	int64_t used;         // Used by *this* store, not total-free
	int64_t available;    // Amount of disk space that can be used by data structure, including free disk space and internally reusable space

	StorageBytes() { }
	StorageBytes(int64_t free, int64_t total, int64_t used, int64_t available) : free(free), total(total), used(used), available(available) { }

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, free, total, used, available);
	}
};

struct LogMessageVersion {
	// Each message pushed into the log system has a unique, totally ordered LogMessageVersion
	// See ILogSystem::push() for how these are assigned
	Version version;
	uint32_t sub;

	void reset(Version v) {
		version = v;
		sub = 0;
	}

	bool operator<(LogMessageVersion const& r) const {
		if (version<r.version) return true;
		if (r.version<version) return false;
		return sub < r.sub;
	}

	bool operator==(LogMessageVersion const& r) const { return version == r.version && sub == r.sub; }

	std::string toString() const { return format("%lld.%d", version, sub); }

	LogMessageVersion(Version version, uint32_t sub) : version(version), sub(sub) {}
	explicit LogMessageVersion(Version version) : version(version), sub(0) {}
	LogMessageVersion() : version(0), sub(0) {}
	bool empty() const { return (version == 0) && (sub == 0); }
};

struct AddressExclusion {
	IPAddress ip;
	int port;

	AddressExclusion() : ip(0), port(0) {}
	explicit AddressExclusion(const IPAddress& ip) : ip(ip), port(0) {}
	explicit AddressExclusion(const IPAddress& ip, int port) : ip(ip), port(port) {}

	bool operator<(AddressExclusion const& r) const {
		if (ip != r.ip) return ip < r.ip;
		return port < r.port;
	}
	bool operator==(AddressExclusion const& r) const { return ip == r.ip && port == r.port; }

	bool isWholeMachine() const { return port == 0; }
	bool isValid() const { return ip.isValid() || port != 0; }

	bool excludes( NetworkAddress const& addr ) const {
		if(isWholeMachine())
			return ip == addr.ip;
		return ip == addr.ip && port == addr.port;
	}

	// This is for debugging and IS NOT to be used for serialization to persistant state
	std::string toString() const {
		if (!isWholeMachine())
			return formatIpPort(ip, port);
		return ip.toString();
	}

	static AddressExclusion parse( StringRef const& );

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, ip, port);
	}
};

static bool addressExcluded( std::set<AddressExclusion> const& exclusions, NetworkAddress const& addr ) {
	return exclusions.count( AddressExclusion(addr.ip, addr.port) ) || exclusions.count( AddressExclusion(addr.ip) );
}

struct ClusterControllerPriorityInfo {
	enum DCFitness { FitnessPrimary, FitnessRemote, FitnessPreferred, FitnessUnknown, FitnessBad }; //cannot be larger than 7 because of leader election mask

	static DCFitness calculateDCFitness(Optional<Key> const& dcId, std::vector<Optional<Key>> const& dcPriority) {
		if(!dcPriority.size()) {
			return FitnessUnknown;
		} else if(dcPriority.size() == 1) {
			if(dcId == dcPriority[0]) {
				return FitnessPreferred;
			} else {
				return FitnessUnknown;
			}
		} else {
			if(dcId == dcPriority[0]) {
				return FitnessPrimary;
			} else if(dcId == dcPriority[1]) {
				return FitnessRemote;
			} else {
				return FitnessBad;
			}
		}
	}

	uint8_t processClassFitness;
	bool isExcluded;
	uint8_t dcFitness;

	bool operator== (ClusterControllerPriorityInfo const& r) const { return processClassFitness == r.processClassFitness && isExcluded == r.isExcluded && dcFitness == r.dcFitness; }

	ClusterControllerPriorityInfo(uint8_t processClassFitness, bool isExcluded, uint8_t dcFitness) : processClassFitness(processClassFitness), isExcluded(isExcluded), dcFitness(dcFitness) {}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, processClassFitness, isExcluded, dcFitness);
	}
};

struct HealthMetrics {
	struct StorageStats {
		int64_t storageQueue;
		int64_t storageDurabilityLag;
		double diskUsage;
		double cpuUsage;

		bool operator==(StorageStats const &r) const {
			return (
				(storageQueue == r.storageQueue) &&
				(storageDurabilityLag == r.storageDurabilityLag) &&
				(diskUsage == r.diskUsage) &&
				(cpuUsage == r.cpuUsage)
			);
		}

		template <class Ar>
		void serialize(Ar& ar) {
			serializer(ar, storageQueue, storageDurabilityLag, diskUsage, cpuUsage);
		}
	};

	int64_t worstStorageQueue;
	int64_t worstStorageDurabilityLag;
	int64_t worstTLogQueue;
	double tpsLimit;
	bool batchLimited;
	std::map<UID, StorageStats> storageStats;
	std::map<UID, int64_t> tLogQueue;

	HealthMetrics()
		: worstStorageQueue(0)
		, worstStorageDurabilityLag(0)
		, worstTLogQueue(0)
		, tpsLimit(0.0)
		, batchLimited(false)
	{}

	void update(const HealthMetrics& hm, bool detailedInput, bool detailedOutput)
	{
		worstStorageQueue = hm.worstStorageQueue;
		worstStorageDurabilityLag = hm.worstStorageDurabilityLag;
		worstTLogQueue = hm.worstTLogQueue;
		tpsLimit = hm.tpsLimit;
		batchLimited = hm.batchLimited;

		if (!detailedOutput) {
			storageStats.clear();
			tLogQueue.clear();
		} else if (detailedInput) {
			storageStats = hm.storageStats;
			tLogQueue = hm.tLogQueue;
		}
	}

	bool operator==(HealthMetrics const& r) const {
		return (
			worstStorageQueue == r.worstStorageQueue &&
			worstStorageDurabilityLag == r.worstStorageDurabilityLag &&
			worstTLogQueue == r.worstTLogQueue &&
			storageStats == r.storageStats &&
			tLogQueue == r.tLogQueue &&
			batchLimited == r.batchLimited
		);
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, worstStorageQueue, worstStorageDurabilityLag, worstTLogQueue, tpsLimit, batchLimited, storageStats, tLogQueue);
	}
};

#endif
