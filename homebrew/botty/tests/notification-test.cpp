#include "notification.hpp"
#include <cassert>
#include <cstring>

static std::string expected;
static unsigned calls=0;
extern "C" int sceKernelSendNotificationRequest(unsigned int device,void* data,size_t size,int flags) {
  assert(device==0&&flags==0&&size==3120);
  const auto* bytes=static_cast<const unsigned char*>(data);
  for(unsigned i=0;i<45;++i)assert(bytes[i]==0);
  const auto* message=reinterpret_cast<const char*>(bytes+45);
  assert(message[3074]=='\0');assert(std::string(message)==expected);
  ++calls;return -1; // A system notification failure is best-effort.
}
int main() {
  expected="Botty+: Verification complete. You can reopen Botty+.";
  botty::notifySystem(expected);assert(calls==1);
  expected=std::string(3074,'a');botty::notifySystem(std::string(4000,'a'));assert(calls==2);
}
