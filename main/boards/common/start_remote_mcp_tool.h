#ifndef START_REMOTE_MCP_TOOL_H
#define START_REMOTE_MCP_TOOL_H

#include <string>

#include "mcp_server.h"

struct StartRemoteTriggerContext {
    std::string trigger_id;
    std::string source;
    std::string phase;
};

class StartRemoteMcpTool {
public:
    StartRemoteMcpTool() = default;

    void Initialize();
    const StartRemoteTriggerContext& GetLastTriggerContext() const { return last_trigger_context_; }

private:
    ReturnValue HandleStartRemote(const PropertyList& properties);

    StartRemoteTriggerContext last_trigger_context_;
};

#endif // START_REMOTE_MCP_TOOL_H