#pragma once
#include "client.hpp"
#include <httplib.h>
#include <deque>
#include <sstream>

namespace tiny_agent::mcp {

// ── Configuration for HTTP transport ────────────────────────────────────────

struct HttpConfig {
    std::string base_url;
    std::string endpoint = "/mcp";
    int timeout_seconds  = 120;
    std::map<std::string, std::string> headers;
};

// ── Streamable HTTP transport (MCP spec 2025-11-25) ─────────────────────────
//
// Sends JSON-RPC messages as HTTP POST to a single MCP endpoint.
// Handles both application/json and text/event-stream (SSE) responses.
// Manages session lifecycle via MCP-Session-Id header.

class HttpTransport {
    httplib::Client client_;
    std::string     endpoint_;
    std::string     session_id_;
    std::deque<json> buffer_;
    std::map<std::string, std::string> extra_headers_;

public:
    explicit HttpTransport(HttpConfig config)
        : client_(config.base_url)
        , endpoint_(std::move(config.endpoint))
        , extra_headers_(std::move(config.headers))
    {
        client_.set_read_timeout(config.timeout_seconds);
#ifdef __APPLE__
        client_.set_ca_cert_path("/etc/ssl/cert.pem");
#endif
    }

    HttpTransport(const std::string& base_url, std::string endpoint = "/mcp")
        : HttpTransport(HttpConfig{.base_url = base_url, .endpoint = std::move(endpoint)})
    {}

    ~HttpTransport() {
        if (!session_id_.empty()) {
            try {
                httplib::Headers hdrs;
                hdrs.emplace("MCP-Session-Id", session_id_);
                client_.Delete(endpoint_, hdrs);
            } catch (...) {}
        }
    }

    HttpTransport(const HttpTransport&) = delete;
    HttpTransport& operator=(const HttpTransport&) = delete;
    HttpTransport(HttpTransport&&) = default;
    HttpTransport& operator=(HttpTransport&&) = default;

    void send(const json& message) {
        auto hdrs = make_headers();
        bool is_request = message.contains("id");

        auto res = client_.Post(
            endpoint_, hdrs, message.dump(), "application/json");

        if (!res)
            throw MCPError(
                "HTTP request failed: " + httplib::to_string(res.error()));

        if (res->has_header("MCP-Session-Id"))
            session_id_ = res->get_header_value("MCP-Session-Id");

        if (is_request) {
            if (res->status != 200)
                throw MCPError("MCP HTTP error (status="
                    + std::to_string(res->status) + "): " + res->body);

            auto ct = res->get_header_value("Content-Type");
            if (ct.find("text/event-stream") != std::string::npos)
                parse_sse(res->body);
            else
                buffer_.push_back(json::parse(res->body));
        } else {
            if (res->status != 202 && res->status != 200)
                throw MCPError("MCP HTTP notification rejected (status="
                    + std::to_string(res->status) + ")");
        }
    }

    json receive() {
        if (buffer_.empty())
            throw MCPError("no response available");
        auto msg = std::move(buffer_.front());
        buffer_.pop_front();
        return msg;
    }

private:
    httplib::Headers make_headers() const {
        httplib::Headers hdrs;
        hdrs.emplace("Accept", "application/json, text/event-stream");
        for (auto& [k, v] : extra_headers_)
            hdrs.emplace(k, v);
        if (!session_id_.empty()) {
            hdrs.emplace("MCP-Session-Id", session_id_);
            hdrs.emplace("MCP-Protocol-Version", "2025-11-25");
        }
        return hdrs;
    }

    // ── SSE parser ──────────────────────────────────────────────────────
    //
    // Extracts JSON-RPC messages from data: fields of SSE events.
    // Handles multi-line data fields and the priming event (empty data)
    // described in the spec.

    void parse_sse(const std::string& body) {
        std::istringstream stream(body);
        std::string line;
        std::string data;

        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty()) {
                flush_sse_event(data);
            } else if (line.starts_with("data:")) {
                auto value = line.substr(5);
                if (!value.empty() && value.front() == ' ')
                    value.erase(0, 1);
                if (!value.empty()) {
                    if (!data.empty()) data += "\n";
                    data += value;
                }
            }
        }

        flush_sse_event(data);
    }

    void flush_sse_event(std::string& data) {
        if (data.empty()) return;
        try { buffer_.push_back(json::parse(data)); }
        catch (...) {}
        data.clear();
    }
};

static_assert(transport_like<HttpTransport>);

// ── Factory — mirrors connect_stdio ─────────────────────────────────────────

inline auto connect_http(HttpConfig config, Log log = {}) {
    log.info("mcp", "connecting http: " + config.base_url + config.endpoint);
    auto transport = HttpTransport(std::move(config));
    Client client(std::move(transport), std::move(log));
    client.initialize();
    return client;
}

inline auto connect_http(const std::string& base_url,
                          const std::string& endpoint = "/mcp",
                          Log log = {}) {
    return connect_http(
        HttpConfig{.base_url = base_url, .endpoint = endpoint},
        std::move(log));
}

} // namespace tiny_agent::mcp
