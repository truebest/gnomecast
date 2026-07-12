#ifndef GNOMECAST_RDP_LOG_H
#define GNOMECAST_RDP_LOG_H

#include <stdbool.h>

#include "rdp_ffi.h"

bool native_rdp_log_is_enabled(RdpLogLevel level);
void native_rdp_log_emit(RdpLogLevel level, const char *session, const char *target,
                         const char *message);

#endif
