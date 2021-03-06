#pragma once
#include <stdint.h>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXCore::HLE {
  uint64_t EPoll_Create(FEXCore::Core::InternalThreadState *Thread, int size);
  uint64_t EPoll_Wait(FEXCore::Core::InternalThreadState *Thread, int epfd, void *events, int maxevents, int timeout);
  uint64_t EPoll_Ctl(FEXCore::Core::InternalThreadState *Thread, int epfd, int op, int fd, void *event);
  uint64_t EPoll_Pwait(FEXCore::Core::InternalThreadState *Thread, int epfd, void *events, int maxevent, int timeout, const void* sigmask);
  uint64_t EPoll_Create1(FEXCore::Core::InternalThreadState *Thread, int flags);
}
