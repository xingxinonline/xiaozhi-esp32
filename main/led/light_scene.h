#ifndef _LIGHT_SCENE_H_
#define _LIGHT_SCENE_H_

enum class LightScene {
    Booting,
    WifiConfiguring,
    Activating,
    IdleReady,
    Connecting,
    ListeningPassive,
    ListeningActive,
    Speaking,
    Upgrading,
    RecoveringOrError,
};

#endif // _LIGHT_SCENE_H_