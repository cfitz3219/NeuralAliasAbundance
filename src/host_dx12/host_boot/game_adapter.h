/* A54: the contract between the shared host bootstrap and a game adapter
 * linked into the same proxy DLL (COLLABORATION.md B-005 Â§2, packaging).
 *
 * The host owns startup: it runs on its own init thread after the loader
 * lock, initialises the hooking core and settings, starts the overlay (which
 * owns Present/ResizeBuffers/ExecuteCommandLists and the discovery thread),
 * then hands over to the adapter in two phases:
 *   na_adapter_prepare()  - BEFORE overlay Start: register menu sections
 *                           (na::menu::Register is not thread-safe and must
 *                           precede Start). Return 0 to opt out.
 *   na_adapter_start()    - AFTER overlay Start on the init thread: register
 *                           variant slots (na::hooking::variants::Register),
 *                           SubscribeDiscovery / SubscribeExecute, warm-up
 *                           when a device is known. Return 0 on failure; the
 *                           host records it and keeps running without the
 *                           adapter.
 *   na_adapter_stop()     - only from test hosts, after the host proved
 *                           quiescence (overlay Stop returned true). Never
 *                           called at process exit (PROCESS_DETACH is a no-op).
 *   na_adapter_name()     - short name for the status line ("Anno 1800").
 * The build links exactly one adapter library (CMake NA_GAME_ADAPTER_LIB) or
 * the host's stub, which reports "no adapter". No frozen ABI is involved:
 * this is a link-time contract inside one binary.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int na_adapter_prepare(void);
int na_adapter_start(void);
void na_adapter_stop(void);
const char* na_adapter_name(void);

#ifdef __cplusplus
}
#endif
