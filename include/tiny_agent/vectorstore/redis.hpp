#pragma once
// ═══════════════════════════════════════════════════════════════════════════════
//  vectorstore/redis.hpp  —  Redis (RediSearch) over a minimal RESP2 client
//
//  Needs a Redis with the search module loaded — redis/redis-stack-server, or
//  Redis 8, or Redis Cloud with search enabled — and nothing else. Redis speaks
//  RESP over a raw TCP socket rather than HTTP, so unlike the Qdrant and Chroma
//  adapters this one cannot ride on httplib's client. The protocol it does need
//  is small enough to carry in this header: encode a command as an array of
//  length-prefixed bulk strings, parse the five RESP2 reply types back. That is
//  the whole client, about 280 lines of code below with the socket handling in
//  it, and it keeps the dependency count where the rest of the library does.
//
//    auto store = RedisVectorStore{"redis://localhost:6379", "my_docs"};
//    store.add("doc_1", "content", embedding, metadata);
//    auto hits = store.search(query_vec, 5);
//
//  How the pieces map onto Redis:
//
//    FT.CREATE     one index per store, created on the first write and sized
//                  from the first embedding, over hashes under "<name>:"
//    HSET          one hash per document; the embedding is a raw little-endian
//                  float32 blob, which is what the vector field expects
//    FT.SEARCH     KNN over that field, with the query vector passed as a
//                  PARAMS blob so it never has to be escaped into query syntax
//    SCAN + DEL,   clear() drops the documents and then the index; the next
//    FT.DROPINDEX  write recreates both
//
//  RediSearch reports a *distance*, lower is better. search() converts to a
//  similarity, so a Retriever reads this store exactly like every other one.
// ═══════════════════════════════════════════════════════════════════════════════

#include "base.hpp"
#include "../core/log.hpp"

// httplib is already a hard dependency of this target, and it is where this
// codebase's socket portability already lives: on Windows it includes
// <winsock2.h> and <ws2tcpip.h> and runs WSAStartup from a static initialiser,
// on POSIX it pulls in <netdb.h>, <sys/socket.h> and friends. Reusing it means
// the Redis client does not open a second, competing copy of that problem.
#include <httplib.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tiny_agent {

namespace detail::redis {

// ── Sockets ──────────────────────────────────────────────────────────────────

#ifdef _WIN32
using socket_handle = SOCKET;
inline const socket_handle kInvalidSocket = INVALID_SOCKET;
inline int close_handle(socket_handle s)   { return ::closesocket(s); }
inline int last_socket_error()             { return ::WSAGetLastError(); }
#else
using socket_handle = int;
inline constexpr socket_handle kInvalidSocket = -1;
inline int close_handle(socket_handle s)   { return ::close(s); }
inline int last_socket_error()             { return errno; }
#endif

// ── RESP2 ────────────────────────────────────────────────────────────────────

struct Reply {
    enum class Type { simple, error, integer, bulk, array, null };

    Type               type    = Type::null;
    std::string        str;        // simple | error | bulk
    long long          integer   = 0;
    std::vector<Reply> elements;   // array

    [[nodiscard]] bool is_error() const { return type == Type::error; }
    [[nodiscard]] bool is_null()   const { return type == Type::null; }
};

// A command is an array of bulk strings. Every argument is length-prefixed, so
// a raw float32 blob travels as an argument with no escaping at all.
inline std::string encode_command(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& a : args)
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    return out;
}

// Pipelining is the same thing back to back: write every command, then read one
// reply per command in order.
inline std::string encode_pipeline(const std::vector<std::vector<std::string>>& cmds) {
    std::string out;
    for (const auto& c : cmds) out += encode_command(c);
    return out;
}

inline long long parse_integer_token(std::string_view s) {
    if (s.empty()) throw Error("redis: empty RESP length token");
    long long        value = 0;
    bool             negative = (s.front() == '-');
    std::string_view digits  = negative || s.front() == '+' ? s.substr(1) : s;
    if (digits.empty()) throw Error("redis: malformed RESP length token");
    for (char c : digits) {
        if (c < '0' || c > '9')
            throw Error("redis: malformed RESP length token '" + std::string(s) + "'");
        value = value * 10 + (c - '0');
    }
    return negative ? -value : value;
}

// Parse one reply off the front of `in`.
//
// Returns false when `in` holds only part of a reply, which is the normal state
// of a socket buffer mid-read: the caller reads more bytes and asks again. A
// byte that cannot start a reply throws instead, because that is a desynced
// stream rather than a slow one.
inline bool parse_reply(std::string_view in, Reply& out, std::size_t& consumed) {
    consumed = 0;
    if (in.empty()) return false;

    const auto eol = in.find("\r\n");
    if (eol == std::string_view::npos) return false;

    const char             tag        = in[0];
    const std::string_view head       = in.substr(1, eol - 1);
    const std::size_t      after_head = eol + 2;

    Reply reply;
    switch (tag) {
    case '+':
        reply.type = Reply::Type::simple;
        reply.str.assign(head);
        consumed = after_head;
        break;
    case '-':
        reply.type = Reply::Type::error;
        reply.str.assign(head);
        consumed = after_head;
        break;
    case ':':
        reply.type    = Reply::Type::integer;
        reply.integer = parse_integer_token(head);
        consumed      = after_head;
        break;
    case '$': {
        const long long n = parse_integer_token(head);
        if (n < 0) {                      // $-1\r\n, the RESP2 null bulk string
            reply.type = Reply::Type::null;
            consumed   = after_head;
            break;
        }
        const std::size_t need = after_head + static_cast<std::size_t>(n) + 2;
        if (in.size() < need) return false;
        reply.type = Reply::Type::bulk;
        reply.str.assign(in.data() + after_head, static_cast<std::size_t>(n));
        consumed = need;
        break;
    }
    case '*': {
        const long long n = parse_integer_token(head);
        if (n < 0) {                      // *-1\r\n, the null array
            reply.type = Reply::Type::null;
            consumed   = after_head;
            break;
        }
        reply.type = Reply::Type::array;
        reply.elements.reserve(static_cast<std::size_t>(n));
        std::size_t offset = after_head;
        for (long long i = 0; i < n; ++i) {
            Reply       child;
            std::size_t used = 0;
            if (!parse_reply(in.substr(offset), child, used)) return false;
            offset += used;
            reply.elements.push_back(std::move(child));
        }
        consumed = offset;
        break;
    }
    default:
        throw Error(std::string("redis: unexpected RESP type byte '") + tag + "'");
    }

    out = std::move(reply);
    return true;
}

// ── Connection string ────────────────────────────────────────────────────────

struct Endpoint {
    std::string host = "127.0.0.1";
    int         port = 6379;
    std::string username;
    std::string password;
    int         db = 0;
};

// redis://[user[:password]@]host[:port][/db], plus the shorthands people
// actually type: "localhost", "localhost:6379", "127.0.0.1:6379".
inline Endpoint parse_url(std::string_view url) {
    Endpoint ep;
    if (url.empty()) throw Error("RedisVectorStore: url must not be empty");

    if (url.starts_with("rediss://"))
        throw Error("RedisVectorStore: rediss:// needs TLS, which this client does not "
                    "speak; terminate TLS in front of it (stunnel, a sidecar, a proxy)");
    if (url.starts_with("redis://")) url.remove_prefix(8);
    else if (url.starts_with("tcp://")) url.remove_prefix(6);

    if (const auto slash = url.find('/'); slash != std::string_view::npos) {
        const auto tail = url.substr(slash + 1);
        if (!tail.empty()) ep.db = static_cast<int>(parse_integer_token(tail));
        url = url.substr(0, slash);
    }

    if (const auto at = url.rfind('@'); at != std::string_view::npos) {
        auto creds = url.substr(0, at);
        url        = url.substr(at + 1);
        if (const auto colon = creds.find(':'); colon != std::string_view::npos) {
            ep.username.assign(creds.substr(0, colon));
            ep.password.assign(creds.substr(colon + 1));
        } else {
            ep.password.assign(creds);   // "redis://secret@host" is the common form
        }
    }

    if (url.starts_with("[")) {                       // [::1]:6379
        const auto close = url.find(']');
        if (close == std::string_view::npos)
            throw Error("RedisVectorStore: unterminated IPv6 host in url");
        ep.host.assign(url.substr(1, close - 1));
        url = url.substr(close + 1);
        if (url.starts_with(":")) ep.port = static_cast<int>(parse_integer_token(url.substr(1)));
    } else if (const auto colon = url.rfind(':'); colon != std::string_view::npos) {
        ep.host.assign(url.substr(0, colon));
        ep.port = static_cast<int>(parse_integer_token(url.substr(colon + 1)));
    } else {
        ep.host.assign(url);
    }

    if (ep.host.empty()) throw Error("RedisVectorStore: url carries no host");
    if (ep.port <= 0 || ep.port > 65535)
        throw Error("RedisVectorStore: url port out of range: " + std::to_string(ep.port));
    return ep;
}

// ── Connection ───────────────────────────────────────────────────────────────
//
// One blocking socket, one read buffer, request/response and pipelined. No
// pooling, no reconnect-on-idle: a store holds one of these and a Redis
// connection is cheap to rebuild if it ever dies.

class Connection {
    socket_handle fd_ = kInvalidSocket;
    std::string   buffer_;

public:
    Connection() = default;
    ~Connection() { disconnect(); }

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : fd_(other.fd_), buffer_(std::move(other.buffer_)) {
        other.fd_ = kInvalidSocket;
    }
    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            disconnect();
            fd_       = other.fd_;
            buffer_   = std::move(other.buffer_);
            other.fd_ = kInvalidSocket;
        }
        return *this;
    }

    [[nodiscard]] bool connected() const { return fd_ != kInvalidSocket; }

    void connect(const std::string& host, int port, int timeout_seconds) {
        disconnect();

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo*  result = nullptr;
        const auto port_s = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &result) != 0 || !result)
            throw Error("RedisVectorStore: cannot resolve " + host + ":" + port_s);

        for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
            socket_handle fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd == kInvalidSocket) continue;
            if (::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0) {
                fd_ = fd;
                break;
            }
            close_handle(fd);
        }
        ::freeaddrinfo(result);

        if (fd_ == kInvalidSocket)
            throw Error("RedisVectorStore: cannot connect to " + host + ":" + port_s
                        + " (errno " + std::to_string(last_socket_error()) + ")");

        set_timeouts(timeout_seconds);
        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&one), sizeof(one));
        buffer_.clear();
    }

    void disconnect() {
        if (fd_ != kInvalidSocket) {
            close_handle(fd_);
            fd_ = kInvalidSocket;
        }
        buffer_.clear();
    }

    // Send one command, read one reply.
    Reply command(const std::vector<std::string>& args) {
        send_all(encode_command(args));
        return read_reply();
    }

    // Send N commands in one write, then read N replies. This is what makes
    // add_batch() a single round trip instead of N.
    std::vector<Reply> pipeline(const std::vector<std::vector<std::string>>& cmds) {
        if (cmds.empty()) return {};
        send_all(encode_pipeline(cmds));
        std::vector<Reply> replies;
        replies.reserve(cmds.size());
        for (std::size_t i = 0; i < cmds.size(); ++i) replies.push_back(read_reply());
        return replies;
    }

private:
    void set_timeouts(int seconds) {
        if (seconds <= 0) return;
#ifdef _WIN32
        DWORD ms = static_cast<DWORD>(seconds) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&ms), sizeof(ms));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = seconds;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    void send_all(const std::string& bytes) {
        if (fd_ == kInvalidSocket) throw Error("RedisVectorStore: write on a closed connection");
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const auto n = ::send(fd_, bytes.data() + sent,
#ifdef _WIN32
                                  static_cast<int>(bytes.size() - sent),
#else
                                  bytes.size() - sent,
#endif
                                  0);
            if (n <= 0)
                throw Error("RedisVectorStore: write failed (errno "
                            + std::to_string(last_socket_error()) + ")");
            sent += static_cast<std::size_t>(n);
        }
    }

    Reply read_reply() {
        if (fd_ == kInvalidSocket) throw Error("RedisVectorStore: read on a closed connection");
        for (;;) {
            Reply       reply;
            std::size_t consumed = 0;
            if (parse_reply(buffer_, reply, consumed)) {
                buffer_.erase(0, consumed);
                return reply;
            }
            char       chunk[8192];
            const auto n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n == 0)
                throw Error("RedisVectorStore: server closed the connection mid-reply");
            if (n < 0)
                throw Error("RedisVectorStore: read failed (errno "
                            + std::to_string(last_socket_error())
                            + "); a timeout looks like this too");
            buffer_.append(chunk, static_cast<std::size_t>(n));
        }
    }
};

} // namespace detail::redis

// ── Store ────────────────────────────────────────────────────────────────────

struct RedisConfig {
    std::string username;                  // ACL user; leave empty for "default"
    std::string password;                  // overrides anything in the url
    std::string algorithm       = "HNSW";  // HNSW | FLAT
    std::string distance_metric = "COSINE";// COSINE | L2 | IP
    int         timeout_seconds = 30;
    Log         log;
};

class RedisVectorStore {
    using Reply      = detail::redis::Reply;
    using Connection = detail::redis::Connection;

    std::string  url_;
    std::string  index_;
    RedisConfig  config_;
    detail::redis::Endpoint endpoint_;
    Connection   conn_;
    bool         index_ensured_ = false;
    int          dimensions_    = 0;

    [[nodiscard]] std::string key_prefix() const { return index_ + ":"; }

    void ensure_connected() {
        if (conn_.connected()) return;
        conn_.connect(endpoint_.host, endpoint_.port, config_.timeout_seconds);

        const std::string& password = config_.password.empty() ? endpoint_.password
                                                               : config_.password;
        const std::string& username = config_.username.empty() ? endpoint_.username
                                                               : config_.username;
        if (!password.empty()) {
            // Two-argument AUTH is the ACL form; one-argument authenticates as
            // "default", which is what a plain requirepass server wants.
            auto reply = username.empty()
                ? conn_.command({"AUTH", password})
                : conn_.command({"AUTH", username, password});
            require_ok(reply, "AUTH");
        }
        if (endpoint_.db != 0)
            require_ok(conn_.command({"SELECT", std::to_string(endpoint_.db)}), "SELECT");
    }

    static void require_ok(const Reply& reply, const char* what) {
        if (reply.is_error())
            throw Error(std::string("RedisVectorStore::") + what + " rejected: "
                        + reply.str.substr(0, 512));
    }

    void ensure_index(int dims) {
        if (index_ensured_) return;
        ensure_connected();
        dimensions_ = dims;

        if (!conn_.command({"FT.INFO", index_}).is_error()) {
            index_ensured_ = true;
            return;
        }

        config_.log.info("redis", "creating index '" + index_ + "' (dims="
            + std::to_string(dims) + " algorithm=" + config_.algorithm
            + " metric=" + config_.distance_metric + ")");

        auto reply = conn_.command(build_create_index_command(
            index_, key_prefix(), dims, config_.algorithm, config_.distance_metric));
        // Another writer may have created it between the FT.INFO and the
        // FT.CREATE; that is the state we wanted, not a failure.
        if (reply.is_error() && reply.str.find("already exists") == std::string::npos)
            throw Error("RedisVectorStore: failed to create index '" + index_ + "': "
                        + reply.str.substr(0, 512));
        index_ensured_ = true;
    }

public:
    RedisVectorStore(std::string url, std::string index, RedisConfig cfg = {})
        : url_(std::move(url))
        , index_(std::move(index))
        , config_(std::move(cfg))
    {
        if (index_.empty())
            throw Error("RedisVectorStore: index name must not be empty");
        endpoint_ = detail::redis::parse_url(url_);
    }

    // ── Wire format, as pure functions ───────────────────────────────────────
    //
    // Everything the server sees is built here, so a test can assert the exact
    // command a live Redis would receive without a Redis being present.

    // RediSearch wants a raw float32 blob, little-endian. Emitting the bytes
    // least-significant first makes that true on a big-endian host too.
    static std::string encode_vector(const std::vector<float>& v) {
        std::string out;
        out.reserve(v.size() * 4);
        for (float f : v) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
        }
        return out;
    }

    // Only the vector field goes in the schema. content and metadata ride along
    // as plain hash fields: FT.SEARCH RETURN reads them straight off the hash,
    // and not indexing text nobody queries by keeps the index small.
    static std::vector<std::string>
    build_create_index_command(const std::string& index, const std::string& prefix,
                               int dims, const std::string& algorithm,
                               const std::string& metric) {
        return {"FT.CREATE", index, "ON", "HASH", "PREFIX", "1", prefix, "SCHEMA",
                "embedding", "VECTOR", algorithm, "6",
                "TYPE", "FLOAT32",
                "DIM", std::to_string(dims),
                "DISTANCE_METRIC", metric};
    }

    static std::vector<std::string>
    build_hset_command(const std::string& prefix, const Document& doc) {
        return {"HSET", prefix + doc.id,
                "content",   doc.content,
                "metadata",  (doc.metadata.is_null() ? json::object() : doc.metadata).dump(),
                "embedding", encode_vector(doc.embedding)};
    }

    // "*=>[KNN k @embedding $BLOB AS vector_score]" is the KNN form. The query
    // vector travels as a PARAMS blob rather than inside the query string, so
    // binary bytes never meet the query parser. DIALECT 2 is what makes the
    // vector syntax available at all.
    static std::vector<std::string>
    build_search_command(const std::string& index, const std::string& blob, int top_k) {
        const auto k = std::to_string(top_k < 1 ? 1 : top_k);
        return {"FT.SEARCH", index,
                "*=>[KNN " + k + " @embedding $BLOB AS vector_score]",
                "PARAMS", "2", "BLOB", blob,
                "RETURN", "3", "content", "metadata", "vector_score",
                "SORTBY", "vector_score",
                "LIMIT", "0", k,
                "DIALECT", "2"};
    }

    static std::vector<std::string> build_count_command(const std::string& index) {
        return {"FT.SEARCH", index, "*", "LIMIT", "0", "0", "DIALECT", "2"};
    }

    // RediSearch reports a distance. Every store in tiny_agent returns a
    // similarity, higher is closer, so convert on the way out.
    //
    //   COSINE  distance is 1 - cosine similarity, so invert it and the number
    //           matches FlatVectorStore's score exactly
    //   IP      distance is 1 - inner product, same inversion
    //   L2      squared euclidean, unbounded above; 1/(1+d) maps it into (0,1]
    //           monotonically, which preserves the ranking
    static float distance_to_similarity(const std::string& metric, float distance) {
        if (metric == "L2") return 1.0f / (1.0f + distance);
        return 1.0f - distance;
    }

    // FT.SEARCH answers with [total, key, [field, value, …], key, [field, …], …].
    static std::vector<SearchResult>
    parse_search_reply(const Reply& reply, const std::string& prefix,
                       const std::string& metric = "COSINE") {
        if (reply.is_error())
            throw Error("RedisVectorStore::search rejected: " + reply.str.substr(0, 512));
        if (reply.type != Reply::Type::array)
            throw Error("RedisVectorStore::search: unexpected reply shape");

        std::vector<SearchResult> out;
        out.reserve(reply.elements.size() / 2);

        for (std::size_t i = 1; i + 1 < reply.elements.size(); i += 2) {
            const auto& key    = reply.elements[i];
            const auto& fields = reply.elements[i + 1];
            if (fields.type != Reply::Type::array) continue;

            SearchResult hit;
            hit.id = key.str.starts_with(prefix) ? key.str.substr(prefix.size()) : key.str;
            hit.metadata = json::object();

            for (std::size_t f = 0; f + 1 < fields.elements.size(); f += 2) {
                const std::string& name  = fields.elements[f].str;
                const std::string& value = fields.elements[f + 1].str;
                if (name == "content") {
                    hit.content = value;
                } else if (name == "metadata") {
                    // A document written by something other than this adapter
                    // may hold anything here; a bad parse loses the metadata,
                    // not the hit.
                    if (json parsed = json::parse(value, nullptr, false); parsed.is_object())
                        hit.metadata = std::move(parsed);
                } else if (name == "vector_score") {
                    hit.score = distance_to_similarity(
                        metric, std::strtof(value.c_str(), nullptr));
                }
            }
            out.push_back(std::move(hit));
        }
        return out;
    }

    // SCAN's MATCH takes a glob, so a prefix holding glob metacharacters has to
    // be escaped or it would match the wrong keys — or, worse, delete them.
    static std::string glob_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s) {
            if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

    // ── The four methods ─────────────────────────────────────────────────────

    void add(const std::string& id, const std::string& content,
             const std::vector<float>& embedding, const json& metadata) {
        add_batch({{id, content, embedding, metadata}});
    }

    void add_batch(const std::vector<Document>& docs) {
        if (docs.empty()) return;
        const auto dims = static_cast<int>(docs.front().embedding.size());
        ensure_index(dims);

        // A hash whose vector is the wrong length is accepted by HSET and then
        // silently dropped from the index, which looks exactly like a write
        // that worked. Catch it here instead.
        for (const auto& d : docs)
            if (static_cast<int>(d.embedding.size()) != dimensions_)
                throw Error("RedisVectorStore::add: document '" + d.id + "' has "
                            + std::to_string(d.embedding.size()) + " dimensions, index '"
                            + index_ + "' expects " + std::to_string(dimensions_));

        std::vector<std::vector<std::string>> cmds;
        cmds.reserve(docs.size());
        const auto prefix = key_prefix();
        for (const auto& d : docs) cmds.push_back(build_hset_command(prefix, d));

        for (const auto& reply : conn_.pipeline(cmds)) require_ok(reply, "add");
        config_.log.debug("redis", "wrote " + std::to_string(docs.size()) + " hash(es)");
    }

    [[nodiscard]] std::vector<SearchResult>
    search(const std::vector<float>& query, int top_k = 4) {
        ensure_connected();
        auto reply = conn_.command(
            build_search_command(index_, encode_vector(query), top_k));
        return parse_search_reply(reply, key_prefix(), config_.distance_metric);
    }

    // Asked of the server rather than counted locally, so an index another
    // process also writes to reports the truth.
    [[nodiscard]] std::size_t size() {
        ensure_connected();
        auto reply = conn_.command(build_count_command(index_));
        if (reply.is_error()) return 0;      // an absent index counts as empty
        if (reply.type == Reply::Type::array && !reply.elements.empty()
            && reply.elements[0].type == Reply::Type::integer)
            return static_cast<std::size_t>(reply.elements[0].integer);
        if (reply.type == Reply::Type::integer)
            return static_cast<std::size_t>(reply.integer);
        return 0;
    }

    // Documents first, then the index. FT.DROPINDEX … DD would do both in one
    // command, but only on a server new enough to have it, and dropping the
    // hashes explicitly also cleans up after an index that was never created.
    void clear() {
        ensure_connected();

        const auto  pattern = glob_escape(key_prefix()) + "*";
        std::string cursor  = "0";
        do {
            auto reply = conn_.command({"SCAN", cursor, "MATCH", pattern, "COUNT", "500"});
            if (reply.is_error())
                throw Error("RedisVectorStore::clear rejected: " + reply.str.substr(0, 512));
            if (reply.type != Reply::Type::array || reply.elements.size() < 2) break;

            cursor = reply.elements[0].str;
            std::vector<std::string> del{"DEL"};
            for (const auto& k : reply.elements[1].elements) del.push_back(k.str);
            if (del.size() > 1) require_ok(conn_.command(del), "clear");
        } while (cursor != "0");

        auto dropped = conn_.command({"FT.DROPINDEX", index_});
        // "Unknown index name" means it was already gone, which is the state
        // clear() was asking for.
        if (dropped.is_error() && dropped.str.find("Unknown") == std::string::npos)
            throw Error("RedisVectorStore::clear rejected: " + dropped.str.substr(0, 512));

        index_ensured_ = false;
    }

    [[nodiscard]] const std::string& index() const { return index_; }
    [[nodiscard]] const detail::redis::Endpoint& endpoint() const { return endpoint_; }
};

static_assert(vector_store<RedisVectorStore>);
static_assert(batch_vector_store<RedisVectorStore>);

} // namespace tiny_agent
