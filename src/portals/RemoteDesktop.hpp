#pragma once

#include <sdbus-c++/sdbus-c++.h>
#include <protocols/virtual-keyboard-unstable-v1-protocol.h>
#include <protocols/wlr-virtual-pointer-unstable-v1-protocol.h>

struct WlrContext {
    struct zwlr_virtual_pointer_v1 *pointer;
    int wheel_mult;
    struct zwp_virtual_keyboard_v1 *keyboard;
};

class CRemoteDesktopPortal {
public:
    CRemoteDesktopPortal();
    WlrContext* wlr;
    //
    void onCreateSession(sdbus::MethodCall& call);
    void onSelectDevices(sdbus::MethodCall& call);
    void onStart(sdbus::MethodCall& call);
    void onNotifyPointerMotion(sdbus::MethodCall& call);
    void onNotifyPointerMotionAbsolute(sdbus::MethodCall& call);
    void onNotifyPointerButton(sdbus::MethodCall& call);
    void onNotifyPointerAxis(sdbus::MethodCall& call);
    void onNotifyPointerAxisDiscrete(sdbus::MethodCall& call);
    void onNotifyKeyboardKeycode(sdbus::MethodCall& call);
    void onNotifyKeyboardKeysym(sdbus::MethodCall& call);
    void onConnectToEIS(sdbus::MethodCall& call);

private:
    std::unique_ptr<sdbus::IObject> m_pObject;

    const std::string               INTERFACE_NAME = "org.freedesktop.impl.portal.RemoteDesktop";
    const std::string               OBJECT_PATH    = "/org/freedesktop/portal/desktop";
};
