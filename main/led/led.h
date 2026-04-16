#ifndef _LED_H_
#define _LED_H_

#include "light_scene.h"

class Led {
public:
    virtual ~Led() = default;
    // Set the led state based on the device state
    virtual void OnStateChanged() = 0;
    virtual void ApplyScene(LightScene scene) {
        (void)scene;
        OnStateChanged();
    }
};


class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
