#pragma once
#include <cstddef>
#include "ts3_functions.h"

#ifdef _WIN32
#define GHOST_EXPORT __declspec(dllexport)
#else
#define GHOST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
GHOST_EXPORT const char* ts3plugin_name();
GHOST_EXPORT const char* ts3plugin_version();
GHOST_EXPORT int ts3plugin_apiVersion();
GHOST_EXPORT const char* ts3plugin_author();
GHOST_EXPORT const char* ts3plugin_description();
GHOST_EXPORT void ts3plugin_setFunctionPointers(const TS3Functions funcs);
GHOST_EXPORT void ts3plugin_registerPluginID(const char* id);
GHOST_EXPORT int ts3plugin_init();
GHOST_EXPORT void ts3plugin_shutdown();
GHOST_EXPORT int ts3plugin_offersConfigure();
GHOST_EXPORT void ts3plugin_configure(void*, void*);
GHOST_EXPORT int ts3plugin_requestAutoload();
GHOST_EXPORT void ts3plugin_initMenus(PluginMenuItem*** items, char** icon);
GHOST_EXPORT void ts3plugin_freeMemory(void* data);
GHOST_EXPORT void ts3plugin_onMenuItemEvent(uint64 handler, PluginMenuType type, int menuId, uint64 item);
GHOST_EXPORT void ts3plugin_onConnectStatusChangeEvent(uint64 handler, int status, unsigned int error);
GHOST_EXPORT void ts3plugin_currentServerConnectionChanged(uint64 handler);
GHOST_EXPORT int ts3plugin_onServerErrorEvent(uint64 handler, const char* message, unsigned int error,
                                            const char* returnCode, const char* extraMessage);
}
