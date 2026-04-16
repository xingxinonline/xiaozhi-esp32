#ifndef _STATUS_LIGHT_H_
#define _STATUS_LIGHT_H_

#include "device_state.h"
#include "led/light_scene.h"

enum class StatusLightOverlay {
    None,
    LowBatteryWarning,
    Charging,
};

struct StatusLightContext {
    DeviceState device_state = kDeviceStateUnknown;
    bool voice_active = false;
    bool network_connected = false;
    bool recovering_network = false;
    StatusLightOverlay transient_overlay = StatusLightOverlay::None;
};

class StatusLightController {
public:
    void SetContext(const StatusLightContext& context) {
        context_ = context;
    }

    void SetDeviceState(DeviceState state) {
        context_.device_state = state;
    }

    void SetVoiceActive(bool active) {
        context_.voice_active = active;
    }

    void SetNetworkConnected(bool connected) {
        context_.network_connected = connected;
    }

    void SetRecoveringNetwork(bool recovering) {
        context_.recovering_network = recovering;
    }

    void SetTransientOverlay(StatusLightOverlay overlay) {
        context_.transient_overlay = overlay;
    }

    const StatusLightContext& GetContext() const {
        return context_;
    }

    LightScene ComputeScene() const {
        if (context_.device_state == kDeviceStateUpgrading) {
            return LightScene::Upgrading;
        }

        if (context_.transient_overlay == StatusLightOverlay::LowBatteryWarning ||
            context_.recovering_network) {
            return LightScene::RecoveringOrError;
        }

        switch (context_.device_state) {
            case kDeviceStateUnknown:
            case kDeviceStateStarting:
                return LightScene::Booting;
            case kDeviceStateWifiConfiguring:
                return LightScene::WifiConfiguring;
            case kDeviceStateActivating:
                return LightScene::Activating;
            case kDeviceStateIdle:
                return LightScene::IdleReady;
            case kDeviceStateConnecting:
                return LightScene::Connecting;
            case kDeviceStateListening:
            case kDeviceStateAudioTesting:
                return context_.voice_active ? LightScene::ListeningActive
                                             : LightScene::ListeningPassive;
            case kDeviceStateSpeaking:
                return LightScene::Speaking;
            case kDeviceStateFatalError:
                return LightScene::RecoveringOrError;
            case kDeviceStateUpgrading:
                return LightScene::Upgrading;
            default:
                return LightScene::Booting;
        }
    }

private:
    StatusLightContext context_;
};

#endif // _STATUS_LIGHT_H_