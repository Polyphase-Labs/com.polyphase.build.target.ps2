/**
 * @file Network_PS2.cpp
 * @brief PS2 networking stubs (Phase 0-2). smap / sceNet stack is out of v1
 *        scope; engine's HttpBackend_Stub handles HTTP.
 *
 * If a future phase wants smap (the PS2's BBN expansion bay Ethernet),
 * the layout is: SifLoadModule smap.irx + smap_init on the IOP, then
 * EE-side connect via socket() calls that round-trip through the IOP
 * RPC. Out of scope for now.
 */

#if defined(POLYPHASE_PLATFORM_ADDON)

#include "Network/Network.h"
#include "Log.h"

void NET_Initialize()
{
    LogDebug("Network_PS2: phase 0-2 stub (smap deferred indefinitely)");
}

void NET_Shutdown() {}
void NET_Update()   {}

bool NET_IsActive() { return false; }

SocketHandle NET_SocketCreate()        { return -1; }
SocketHandle NET_SocketCreateStream()  { return -1; }
void NET_SocketBind(SocketHandle /*s*/, uint32_t /*ip*/, uint16_t /*port*/) {}
int32_t NET_SocketRecv(SocketHandle /*s*/, char* /*buf*/, uint32_t /*size*/) { return -1; }
int32_t NET_SocketRecvFrom(SocketHandle /*s*/, char* /*buf*/, uint32_t /*size*/, uint32_t& /*addr*/, uint16_t& /*port*/) { return -1; }
int32_t NET_SocketSendTo(SocketHandle /*s*/, const char* /*buf*/, uint32_t /*size*/, uint32_t /*addr*/, uint16_t /*port*/) { return -1; }
void NET_SocketClose(SocketHandle /*s*/) {}
void NET_SocketSetBlocking(SocketHandle /*s*/, bool /*blocking*/) {}
void NET_SocketSetBroadcast(SocketHandle /*s*/, bool /*broadcast*/) {}
void NET_SocketGetIpAndPort(SocketHandle /*s*/, uint32_t& outIp, uint16_t& outPort) { outIp = 0; outPort = 0; }

bool    NET_SocketConnect(SocketHandle /*s*/, uint32_t /*ip*/, uint16_t /*port*/, int32_t /*timeoutMs*/) { return false; }
int32_t NET_SocketSend(SocketHandle /*s*/, const char* /*buf*/, uint32_t /*size*/) { return -1; }

uint32_t NET_ResolveHost(const char* /*hostname*/) { return 0; }
uint32_t NET_IpStringToUint32(const char* /*ipString*/) { return 0; }
void     NET_IpUint32ToString(uint32_t /*ip*/, char* outIpString) { if (outIpString) outIpString[0] = '\0'; }

uint32_t NET_GetIpAddress()  { return 0; }
uint32_t NET_GetSubnetMask() { return 0; }

#endif // POLYPHASE_PLATFORM_ADDON
