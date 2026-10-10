// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#define BOTTY_REST_MAGIC UINT64_C(0x425454594b4d3031)
#define BOTTY_REST_FW UINT64_C(0x13000000)
#define BOTTY_REST_TTL 180U
#define BOTTY_REST_CONTROL "/data/botty/rest-mode-control.bin"
#define BOTTY_REST_STATUS "/data/botty/rest-mode-companion.json"
#define BOTTY_REST_THREAD "botty_rest_resident"
typedef struct botty_rest_control {
  uint64_t magic,version,firmware,pid,boot_seconds,boot_microseconds,expires,sequence;
} botty_rest_control;
static inline int botty_rest_control_active(const botty_rest_control* c,uint64_t now,uint64_t boot_seconds,uint64_t boot_microseconds) {
  return c->magic==BOTTY_REST_MAGIC&&c->version==1&&c->firmware==BOTTY_REST_FW&&
    c->pid>1&&c->pid<=INT32_MAX&&c->boot_seconds==boot_seconds&&
    c->boot_microseconds==boot_microseconds&&c->expires>now&&
    c->expires-now<=BOTTY_REST_TTL&&c->sequence>0;
}
