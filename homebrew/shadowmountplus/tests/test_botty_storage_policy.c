#include <assert.h>
#include "sm_botty_storage_policy.h"
int main(void) {
  assert(sm_botty_storage_allowed("PPSA12345", "", 0, 0, false, false, false, false));
  assert(sm_botty_storage_allowed("PPSA12345", "PPSA99071", 42, 42, true, false, false, false));
  assert(!sm_botty_storage_allowed("PPSA99071", "PPSA99071", 42, 42, true, false, false, true));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA12346", 42, 42, true, false, false, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", -1, 42, true, false, false, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", 43, 42, true, false, false, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", 0, 42, true, false, false, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", 42, 42, false, false, false, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", 42, 42, true, true, false, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", 42, 42, true, false, true, false));
  assert(!sm_botty_storage_allowed("PPSA12345", "PPSA99071", 42, 42, true, false, false, true));
  return 0;
}
