#include "../botty/src/rest-companion-protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void){
  botty_rest_control c={BOTTY_REST_MAGIC,1,BOTTY_REST_FW,42,1000,1,1180,1};
  assert(botty_rest_control_active(&c,1000,1000,1));
  assert(!botty_rest_control_active(&c,1180,1000,1));
  assert(!botty_rest_control_active(&c,999,1000,1));
  assert(!botty_rest_control_active(&c,1000,1001,1));
  assert(!botty_rest_control_active(&c,1000,1000,2));
  c.firmware=0x14000000;assert(!botty_rest_control_active(&c,1000,1000,1));c.firmware=BOTTY_REST_FW;
  c.pid=UINT64_MAX;assert(!botty_rest_control_active(&c,1000,1000,1));c.pid=42;
  c.version=2;assert(!botty_rest_control_active(&c,1000,1000,1));c.version=1;
  c.expires=0;assert(!botty_rest_control_active(&c,1000,1000,1));
  puts("Companion firmware, boot identity and control expiry passed");
}
