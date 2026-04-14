#include "start_remote_mcp_tool.h"

#include <esp_log.h>

#include "application.h"

static const char* TAG = "StartRemoteMcpTool";

namespace {

ListeningMode ParseListeningMode(Application& app, const std::string& mode_str) {
    if (mode_str.empty() || mode_str == "manual_stop") {
        return kListeningModeManualStop;
    }
    if (mode_str == "default") {
        return app.GetDefaultListeningMode();
    }
    if (mode_str == "auto_stop") {
        return kListeningModeAutoStop;
    }

    throw std::runtime_error("Invalid listening_mode: expected manual_stop, auto_stop, or default");
}

const char* ListeningModeToString(ListeningMode mode) {
    switch (mode) {
        case kListeningModeManualStop:
            return "manual_stop";
        case kListeningModeAutoStop:
            return "auto_stop";
        case kListeningModeRealtime:
            return "realtime";
        default:
            return "unknown";
    }
}

void AddOptionalStringToJson(cJSON* root, const char* key, const std::string& value) {
    if (!value.empty()) {
        cJSON_AddStringToObject(root, key, value.c_str());
    }
}

void AddOptionalIntToJson(cJSON* root, const char* key, const std::optional<int>& value) {
    if (value.has_value()) {
        cJSON_AddNumberToObject(root, key, value.value());
    }
}

std::string BuildTriggerJson(const StartRemoteTriggerContext& trigger_context) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "trigger_id", trigger_context.trigger_id.c_str());
    AddOptionalStringToJson(root, "source", trigger_context.source);
    AddOptionalStringToJson(root, "phase", trigger_context.phase);
    AddOptionalStringToJson(root, "book_id", trigger_context.book_id);
    AddOptionalStringToJson(root, "book_title", trigger_context.book_title);
    AddOptionalStringToJson(root, "book_author", trigger_context.book_author);
    AddOptionalIntToJson(root, "start_page", trigger_context.start_page);
    AddOptionalIntToJson(root, "end_page", trigger_context.end_page);
    AddOptionalStringToJson(root, "plan_name", trigger_context.plan_name);

    char* json = cJSON_PrintUnformatted(root);
    std::string result = json != nullptr ? json : "{}";
    if (json != nullptr) {
        cJSON_free(json);
    }
    cJSON_Delete(root);
    return result;
}

cJSON* CreateAcceptedResult(const std::string& trigger_id) {
    auto* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "accepted");
    cJSON_AddStringToObject(result, "trigger_id", trigger_id.c_str());
    return result;
}

cJSON* CreateInjectedResult(const std::string& trigger_id) {
    auto* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "status", "trigger_injected");
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
        "Request the device to start a conversation remotely. "
        "If the device is idle, it starts a new session; if it is already speaking or listening, it injects the trigger into the ongoing session. "
        "This call returns immediately after the start request or trigger injection is queued. "
        "`trigger_id` is required. You can optionally inline reading context with source, phase, book_id, book_title, book_author, start_page, end_page, plan_name, and listening_mode.",
        PropertyList({
            Property("trigger_id", kPropertyTypeString),
            Property("source", kPropertyTypeString, std::string("scheduled_task")),
            Property("phase", kPropertyTypeString, std::string("")),
            Property("book_id", kPropertyTypeString, std::string("")),
            Property("book_title", kPropertyTypeString, std::string("")),
            Property("book_author", kPropertyTypeString, std::string("")),
            Property("start_page", kPropertyTypeInteger, -1),
            Property("end_page", kPropertyTypeInteger, -1),
            Property("plan_name", kPropertyTypeString, std::string("")),
            Property("listening_mode", kPropertyTypeString, std::string("manual_stop"))
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
    auto book_id = properties["book_id"].value<std::string>();
    auto book_title = properties["book_title"].value<std::string>();
    auto book_author = properties["book_author"].value<std::string>();
    auto start_page_value = properties["start_page"].value<int>();
    auto end_page_value = properties["end_page"].value<int>();
    auto plan_name = properties["plan_name"].value<std::string>();
    auto& app = Application::GetInstance();
    auto listening_mode = ParseListeningMode(app, properties["listening_mode"].value<std::string>());

    if (start_page_value < -1) {
        throw std::runtime_error("Invalid start_page: must be omitted or >= 0");
    }
    if (end_page_value < -1) {
        throw std::runtime_error("Invalid end_page: must be omitted or >= 0");
    }

    last_trigger_context_ = {
        .trigger_id = std::move(trigger_id),
        .source = std::move(source),
        .phase = std::move(phase),
        .book_id = std::move(book_id),
        .book_title = std::move(book_title),
        .book_author = std::move(book_author),
        .start_page = start_page_value >= 0 ? std::optional<int>(start_page_value) : std::nullopt,
        .end_page = end_page_value >= 0 ? std::optional<int>(end_page_value) : std::nullopt,
        .plan_name = std::move(plan_name),
    };

    auto state = app.GetDeviceState();
    if (state == kDeviceStateIdle) {
        ESP_LOGI(TAG, "Accepted remote conversation start: trigger_id=%s source=%s phase=%s book_id=%s book_title=%s start_page=%d end_page=%d plan_name=%s listening_mode=%s",
            last_trigger_context_.trigger_id.c_str(),
            last_trigger_context_.source.c_str(),
            last_trigger_context_.phase.empty() ? "" : last_trigger_context_.phase.c_str(),
            last_trigger_context_.book_id.empty() ? "" : last_trigger_context_.book_id.c_str(),
            last_trigger_context_.book_title.empty() ? "" : last_trigger_context_.book_title.c_str(),
            last_trigger_context_.start_page.value_or(-1),
            last_trigger_context_.end_page.value_or(-1),
            last_trigger_context_.plan_name.empty() ? "" : last_trigger_context_.plan_name.c_str(),
            ListeningModeToString(listening_mode));

        app.SetPendingTriggerContext(last_trigger_context_);
        app.SetPendingListeningMode(listening_mode);
        app.StartListening();
        return CreateAcceptedResult(last_trigger_context_.trigger_id);
    }

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        auto trigger_json = BuildTriggerJson(last_trigger_context_);
        if (app.AbortSpeaking(kAbortReasonRemoteTrigger, trigger_json)) {
            ESP_LOGI(TAG, "Injected remote trigger into ongoing conversation: trigger_id=%s state=%s",
                last_trigger_context_.trigger_id.c_str(), DeviceStateMachine::GetStateName(state));
            return CreateInjectedResult(last_trigger_context_.trigger_id);
        }

        ESP_LOGW(TAG, "Failed to inject remote trigger: trigger_id=%s state=%s",
            last_trigger_context_.trigger_id.c_str(), DeviceStateMachine::GetStateName(state));
        return CreateRejectedResult(last_trigger_context_.trigger_id, "audio_channel_unavailable", state);
    }

    ESP_LOGW(TAG, "Rejected remote conversation start: trigger_id=%s state=%s",
        last_trigger_context_.trigger_id.c_str(), DeviceStateMachine::GetStateName(state));
    return CreateRejectedResult(last_trigger_context_.trigger_id, "device_busy", state);
}