#pragma once
/* C ABI shared by a realm adapter and its separate host process.
 * Native WoW DLLs need an adapter; they are not required to export this API. */
#include <stdint.h>
#ifdef _WIN32
#define WOWEE_EXTENSION_CALL __cdecl
#define WOWEE_EXTENSION_EXPORT __declspec(dllexport)
#else
#define WOWEE_EXTENSION_CALL
#define WOWEE_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define WOWEE_EXTENSION_ABI 1u
#define WOWEE_EXTENSION_UNHANDLED 0
#define WOWEE_EXTENSION_HANDLED 1
#define WOWEE_EXTENSION_ERROR -1
struct WoweeExtensionSession {
    uint32_t size;
    uint32_t abi;
    const char* server_host;
    uint32_t server_port;
    uint32_t client_build;
    const char* library_path;
    const char* client_path;
    const char* options_json;
};
struct WoweeExtensionApi {
    uint32_t size;
    uint32_t abi;
    /* Return HANDLED only after initialization succeeds. No credentials or
     * SRP/session secrets are supplied to adapters. */
    int (WOWEE_EXTENSION_CALL *start)(const struct WoweeExtensionSession*);
    void (WOWEE_EXTENSION_CALL *stop)(void);
    /* Only return HANDLED when all requested bytes were produced by the
     * library runtime. UNHANDLED permits a static client image fallback. */
    int (WOWEE_EXTENSION_CALL *read_memory)(const char* module, uint32_t address,
                                           uint32_t length, uint8_t* output);
    const char* (WOWEE_EXTENSION_CALL *last_error)(void);
};
typedef const struct WoweeExtensionApi* (WOWEE_EXTENSION_CALL *WoweeExtensionEntry)(uint32_t abi);
/* The exported name is exactly wowee_extension_api (undecorated on x86). */
#ifdef __cplusplus
}
#endif
