#pragma once
#include <cstdio>
#include <string>

#ifdef __PS5__
extern "C" int sceKernelSendNotificationRequest(unsigned int, void*, size_t, int);
#endif

namespace botty {
inline void notifySystem(const std::string& message) noexcept {
#ifdef __PS5__
  struct Request { unsigned char reserved[45]; char message[3075]; } request{};
  std::snprintf(request.message,sizeof(request.message),"%s",message.c_str());
  (void)sceKernelSendNotificationRequest(0,&request,sizeof(request),0);
#else
  (void)message;
#endif
}
}
