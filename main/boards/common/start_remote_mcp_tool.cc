#include "start_remote_mcp_tool.h"

#include <esp_log.h>

#include "application.h"

static const char* TAG = "StartRemoteMcpTool";

namespace {

cJSON* CreateAcceptedResult(const std::string& trigger_id) {
    auto* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "accepted");
    cJSON_AddStringToObject(result, "trigger_id", trigger_id.c_str());
    return result;
}

cJSON* CreateRejectedResult(const std::string& trigger_id, const char* reason, DeviceState state) {
    auto* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "rejected");
    cJSON_AddStringToObject(result, "trigger_id", trigger_id.c_str());
    cJSON_AddStringToObject(result, "reason", reason);
    cJSON_AddStringToObject(result, "state", DeviceStateMachine::GetStateName(state));
    return result;
}

} // namespace

void StartRemoteMcpTool::Initialize() {
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.conversation.start_remote",
        "Request the device to start a conversation remotely when it is idle. "
        "This call returns immediately after the start request is queued. "
        "`trigger_id` is required. `source` defaults to `scheduled_task`, and `phase` is optional trigger context.",
        PropertyList({
            Property("trigger_id", kPropertyTypeString),
            Property("source", kPropertyTypeString, std::string("scheduled_task")),
            Property("phase", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleStartRemote(properties);
        });

    ESP_LOGI(TAG, "StartRemoteMcpTool initialized");
}

ReturnValue StartRemoteMcpTool::HandleStartRemote(const PropertyList& properties) {
    auto trigger_id = properties["trigger_id"].value<std::string>();
    auto source = properties["source"].value<std::string>();
    auto phase = properties["phase"].value<std::string>();

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state != kDeviceStateIdle) {
        ESP_LOGW(TAG, "Rejected remote conversation start: trigger_id=%s state=%s",
            trigger_id.c_str(), DeviceStateMachine::GetStateName(state));
        return CreateRejectedResult(trigger_id, "device_busy", state);
    }

    last_trigger_context_ = {
        .trigger_id = std::move(trigger_id),
        .source = std::move(source),
        .phase = std::move(phase),
    };

    ESP_LOGI(TAG, "Accepted remote conversation start: trigger_id=%s source=%s phase=%s",
        last_trigger_context_.trigger_id.c_str(),
        last_trigger_context_.source.c_str(),
        last_trigger_context_.phase.empty() ? "" : last_trigger_context_.phase.c_str());

    app.StartListening();
    return CreateAcceptedResult(last_trigger_context_.trigger_id);
}