#include "RemoteDesktop.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"

#include <fcntl.h>
#include <wayland-client.h>
#include <protocols/virtual-keyboard-unstable-v1-protocol.h>
#include <protocols/wlr-virtual-pointer-unstable-v1-protocol.h>

#include <regex>
#include <filesystem>
#include <stdfloat>

#include "libei-1.0/libei.h"
#include "libei-1.0//liboeffis.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <libei.h>
#include <poll.h>

enum DeviceType {
    None = 0x0,
    Keyboard = 0x1,
    Pointer = 0x2,
    TouchScreen = 0x4,
    All = (Keyboard | Pointer | TouchScreen),
};

CRemoteDesktopPortal::CRemoteDesktopPortal() {
    Debug::log(LOG, "[remote-desktop] initializing remote desktop portal");
    m_pObject = sdbus::createObject(*g_pPortalManager->getConnection(), OBJECT_PATH);
    m_pObject->registerMethod(INTERFACE_NAME, "CreateSession", "oosa{sv}", "ua{sv}", [&](sdbus::MethodCall c) { onCreateSession(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "SelectDevices", "oosa{sv}", "ua{sv}", [&](sdbus::MethodCall c) { onSelectDevices(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "Start", "oossa{sv}", "ua{sv}", [&](sdbus::MethodCall c) { onStart(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyPointerMotion", "oa{sv}dd", "", [&](sdbus::MethodCall c) { onNotifyPointerMotion(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyPointerMotionAbsolute", "oa{sv}udd", "", [&](sdbus::MethodCall c) { onNotifyPointerMotionAbsolute(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyPointerButton", "oa{sv}iu", "", [&](sdbus::MethodCall c) { onNotifyPointerButton(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyPointerAxis", "oa{sv}dd", "", [&](sdbus::MethodCall c) { onNotifyPointerAxis(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyPointerAxisDiscrete", "oa{sv}ui", "", [&](sdbus::MethodCall c) { onNotifyPointerAxisDiscrete(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyKeyboardKeycode", "oa{sv}iu", "", [&](sdbus::MethodCall c) { onNotifyKeyboardKeycode(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyKeyboardKeysym", "oa{sv}iu", "", [&](sdbus::MethodCall c) { onNotifyKeyboardKeysym(c); });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyTouchDown", "oa{sv}uudd", "", [&](sdbus::MethodCall c) {  });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyTouchMotion", "oa{sv}uudd", "", [&](sdbus::MethodCall c) { });
    m_pObject->registerMethod(INTERFACE_NAME, "NotifyTouchUp", "oa{sv}uudd", "", [&](sdbus::MethodCall c) {  });

    //NotifyTouchDown
    //NotifyTouchMotion
    //NotifyTouchUp
    m_pObject->registerMethod(INTERFACE_NAME, "ConnectToEIS", "osa{sv}", "h", [&](sdbus::MethodCall c) { onConnectToEIS(c); });

    //m_pObject->registerMethod(INTERFACE_NAME, "Screenshot", "ossa{sv}", "ua{sv}", [&](sdbus::MethodCall c) { onScreenshot(c); });
    //m_pObject->registerMethod(INTERFACE_NAME, "PickColor", "ossa{sv}", "ua{sv}", [&](sdbus::MethodCall c) { onPickColor(c); });
    m_pObject->registerProperty(INTERFACE_NAME, "AvailableDeviceTypes", "u", [](sdbus::PropertyGetReply& reply) -> void { reply << (uint)(Keyboard | Pointer); });
    m_pObject->registerProperty(INTERFACE_NAME, "version", "u", [](sdbus::PropertyGetReply& reply) -> void { reply << (uint)2; });

    m_pObject->finishRegistration();

    Debug::log(LOG, "[remote-desktop] remote desktop successful");
}

void CRemoteDesktopPortal::onCreateSession(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on create session");
    // Create variables to hold the extracted data
    sdbus::ObjectPath requestHandle, sessionHandle;
    call >> requestHandle;
    call >> sessionHandle;

    std::string appID;
    call >> appID;
    // Extract the parameters from the call

    Debug::log(LOG, "[remote-desktop] handle: {}", requestHandle.c_str());
    Debug::log(LOG, "[remote-desktop] session handle: {}", sessionHandle.c_str());
    Debug::log(LOG, "[remote-desktop] app id: {}", appID.c_str());
    // make sure the session is created
    auto reply = call.createReply();
    reply << (uint32_t)0;
    reply << std::unordered_map<std::string, sdbus::Variant>{};
    reply.send();
}

void CRemoteDesktopPortal::onStart(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on start");

    auto reply = call.createReply();
    reply << (uint32_t)0;
    reply << std::unordered_map<std::string, sdbus::Variant>{};
    reply.send();
    Debug::log(LOG, "[remote-desktop] on start completed");
}

void CRemoteDesktopPortal::onConnectToEIS(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on connect to eis");
    // ei* ei = ei_new_receiver(nullptr);
    // ei_configure_name(ei, "ei-remote-desktop");
    //
    // ei_setup_backend_socket(ei, "eis-0");
    // int sockfd = ei_get_fd(ei);

    // Extract the method call arguments
    std::string session_handle;
    std::string app_id;
    std::unordered_map<std::string, sdbus::Variant> options;

    Debug::log(LOG, "[remote-desktop] on read data");

    const char* socket_path = "/run/user/1000/eis-0";

    // Create a socket
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "Error creating socket: " << strerror(errno) << std::endl;
        return;
    }

    // Set up the socket address structure
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    // Connect to the socket
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "Error connecting to socket: " << strerror(errno) << std::endl;
        close(sockfd);
        return;
    }

    // Log the connection success
    Debug::log(LOG, "[remote-desktop] Connected to the socket. File descriptor: {}", sockfd);
    // struct pollfd fds = {
    //     .fd = ei_get_fd(ei),
    //     .events = POLLIN,
    //     .revents = 0,
    // };
    //
    // while (poll(&fds, 1, 2000) > -1) {
    //     ei_dispatch(ei);
    //
    //     while (true) {
    //         ei_event *e = ei_get_event(ei);
    //         if (!e)
    //             break;
    //
    //         struct ei_seat *seat = ei_event_get_seat(e);
    //
    //         Debug::log(LOG, "Jakis event");
    //     }
    // }


    // Create a reply and send the file descriptor back to the caller
    auto reply = call.createReply();
    //reply << (uint32_t)0;
    reply << (sdbus::UnixFd) sockfd;
    reply.send();

    // The socket file descriptor should be left open, so we do not close it here.
    Debug::log(LOG, "[remote-desktop] on connect to eis completed");
}


void CRemoteDesktopPortal::onNotifyKeyboardKeycode(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on notify keyboard keycode");
}

void CRemoteDesktopPortal::onNotifyKeyboardKeysym(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on keyboard key sm");
}

void CRemoteDesktopPortal::onNotifyPointerButton(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on pointer button");
}

void CRemoteDesktopPortal::onNotifyPointerMotion(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on notify pointer motion");
    unsigned int dx, dy;
    call >> dx;
    call >> dy;

    timespec ts;
    int      clockTime = clock_gettime(CLOCK_MONOTONIC, &ts);
    zwlr_virtual_pointer_v1_motion(
        wlr->pointer,
        clockTime,
        dx,
        dy
    );
    zwlr_virtual_pointer_v1_frame(wlr->pointer);
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);

    auto reply = call.createReply();
    reply << (uint32_t)0;
    reply << std::unordered_map<std::string, sdbus::Variant>{};
    reply.send();
}

void CRemoteDesktopPortal::onNotifyPointerMotionAbsolute(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on notify pointer motion");
    timespec       ts;

    unsigned int x, y;
    call >> x;
    call >> y;

    int clockTime = clock_gettime(CLOCK_MONOTONIC, &ts);

    zwlr_virtual_pointer_v1_motion_absolute(
        wlr->pointer,
        clockTime,
        x,
        y,
        4520,
        1440
    );
    zwlr_virtual_pointer_v1_frame(wlr->pointer);
    wl_display_flush(g_pPortalManager->m_sWaylandConnection.display);

    auto reply = call.createReply();
    reply << (uint32_t)0;
    reply << std::unordered_map<std::string, sdbus::Variant>{1|2};
    reply.send();
}

void CRemoteDesktopPortal::onNotifyPointerAxisDiscrete(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on pointer axis discrete");
    auto reply = call.createReply();
    reply << (uint32_t)0;
    reply << std::unordered_map<std::string, sdbus::Variant>{1|2};
    reply.send();
}

void CRemoteDesktopPortal::onNotifyPointerAxis(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on notify pointer axis");
    auto reply = call.createReply();
    reply << (uint32_t)0;
    reply << std::unordered_map<std::string, sdbus::Variant>{1|2};
    reply.send();
}

void CRemoteDesktopPortal::onSelectDevices(sdbus::MethodCall& call) {
    Debug::log(LOG, "[remote-desktop] on select devices");

    if(! wlr->pointer) {
        wlr->pointer  = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        g_pPortalManager->m_sWaylandConnection.pointerMgr,
        nullptr
        //g_pPortalManager->m_sWaylandConnection.seat
        );
    }

    if(! wlr->keyboard) {
        wlr->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        g_pPortalManager->m_sWaylandConnection.keyboardMgr,
        nullptr
        //g_pPortalManager->m_sWaylandConnection.seat
        );
    }

    auto reply = call.createReply();

    reply << (uint32_t)0;
    // send map of devices keyboard and pointer
    reply << std::unordered_map<std::string, sdbus::Variant>{Keyboard | Pointer};
    reply.send();

    Debug::log(LOG, "[remote-desktop] on select devices finished");
}
