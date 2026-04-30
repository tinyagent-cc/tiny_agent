#pragma once
#include "transport.hpp"
#include "../core/tool.hpp"
#include "../core/log.hpp"
#include <memory>

namespace tiny_agent::mcp {

template<transport_like Transport>
class Client {
    Transport transport_;
    Log       log_;
    int  next_id_{1};
    bool initialized_ = false;

    json rpc(const std::string& method, const json& params = json::object()) {
        int id = next_id_++;
        json req;
        req["jsonrpc"] = "2.0";
        req["id"]      = id;
        req["method"]  = method;
        req["params"]  = params;

        log_.debug("mcp", "rpc -> " + method + " (id=" + std::to_string(id) + ")");
        log_.trace("mcp", "rpc request: " + req.dump());
        transport_.send(req);

        for (int attempts = 0; attempts < 100; ++attempts) {
            auto resp = transport_.receive();
            log_.trace("mcp", "rpc raw response: " + resp.dump());
            if (resp.contains("id") && resp["id"].template get<int>() == id) {
                if (resp.contains("error")) {
                    auto err = resp["error"].value("message", std::string{"unknown"});
                    log_.error("mcp", "rpc error [" + method + "]: " + err);
                    throw MCPError("MCP error: " + err);
                }
                log_.debug("mcp", "rpc <- " + method + " ok");
                return resp["result"];
            }
        }
        log_.error("mcp", "no response for rpc id " + std::to_string(id));
        throw MCPError("no response for RPC id " + std::to_string(id));
    }

    void notify(const std::string& method, const json& params = json::object()) {
        json req;
        req["jsonrpc"] = "2.0";
        req["method"]  = method;
        req["params"]  = params;
        log_.debug("mcp", "notify -> " + method);
        transport_.send(req);
    }

public:
    explicit Client(Transport transport, Log log = {})
        : transport_(std::move(transport)), log_(std::move(log)) {}

    void initialize(const std::string& client_name = "tiny_agent",
                    const std::string& protocol_version = "2025-11-25") {
        log_.info("mcp", "initializing (client=" + client_name
            + " protocol=" + protocol_version + ")");
        json info;
        info["name"]    = client_name;
        info["version"] = "0.2.0";

        json params;
        params["protocolVersion"] = protocol_version;
        params["capabilities"]    = json::object();
        params["clientInfo"]      = info;

        auto result = rpc("initialize", params);
        log_.trace("mcp", "server capabilities: " + result.dump());
        notify("notifications/initialized");
        initialized_ = true;
        log_.info("mcp", "initialized successfully");
    }

    std::vector<ToolSchema> list_tools() {
        if (!initialized_) initialize();
        log_.debug("mcp", "listing tools");
        auto result = rpc("tools/list");
        std::vector<ToolSchema> out;
        for (auto& t : result["tools"])
            out.push_back({
                t["name"].template get<std::string>(),
                t.value("description", std::string{}),
                t.value("inputSchema", json::object())
            });
        log_.info("mcp", "discovered " + std::to_string(out.size()) + " tools");
        for (auto& s : out)
            log_.debug("mcp", "  tool: " + s.name + " — " + s.description);
        return out;
    }

    json call_tool(const std::string& name, const json& arguments = json::object()) {
        if (!initialized_) initialize();
        log_.info("mcp", "calling tool: " + name);
        log_.trace("mcp", "tool args [" + name + "]: " + arguments.dump());

        json params;
        params["name"]      = name;
        params["arguments"] = arguments;
        auto result = rpc("tools/call", params);

        log_.trace("mcp", "tool result [" + name + "]: " + result.dump());

        if (result.contains("content") && result["content"].is_array()) {
            for (auto& c : result["content"])
                if (c.value("type", std::string{}) == "text")
                    return c["text"];
            return result["content"];
        }
        return result;
    }

    std::vector<DynamicTool> as_tools() {
        auto schemas = list_tools();
        std::vector<DynamicTool> out;
        for (auto& s : schemas) {
            auto name = s.name;
            out.push_back(DynamicTool{
                std::move(s),
                [this, name](const json& args) -> json {
                    return call_tool(name, args);
                }
            });
        }
        return out;
    }

    [[nodiscard]] Log& log() { return log_; }
    [[nodiscard]] const Log& log() const { return log_; }
};

template<transport_like T>
Client(T) -> Client<T>;

template<transport_like T>
Client(T, Log) -> Client<T>;

#ifndef _WIN32

inline auto connect_stdio(const std::string& command,
                          const std::vector<std::string>& args = {},
                          Log log = {}) {
    log.info("mcp", "connecting stdio: " + command);
    auto transport = StdioTransport(command, args);
    Client client(std::move(transport), std::move(log));
    client.initialize();
    return client;
}

#endif

} // namespace tiny_agent::mcp
