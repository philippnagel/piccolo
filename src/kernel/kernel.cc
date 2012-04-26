#include <stdio.h>

#include "kernel/kernel.h"
#include "kernel/table-registry.h"

namespace piccolo {

class Worker;
void KernelBase::initialize_internal(Worker* w, int table_id, int shard) {
  w_ = w;
  table_id_ = table_id;
  shard_ = shard;
}

void KernelBase::set_args(const MarshalledMap& args) {
  args_ = args;
}

GlobalTable* KernelBase::get_table(int id) {
  GlobalTable* t = (GlobalTable*)TableRegistry::Get()->table(id);
  CHECK_NE(t, (void*)NULL);
  return t;
}

KernelRegistry* KernelRegistry::Get() {
  static KernelRegistry* r = NULL;
  if (!r) { r = new KernelRegistry; }
  return r;
}

}
