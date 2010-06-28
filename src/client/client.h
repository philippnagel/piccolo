#ifndef CLIENT_H_
#define CLIENT_H_

#include "util/common.h"
#include "util/file.h"

#include "worker/worker.h"
#include "master/master.h"
#include "kernel/kernel.h"
#include "kernel/table-registry.h"

#include "kernel/table.h"
#include "kernel/global-table.h"
#include "kernel/local-table.h"

#ifndef SWIG
DECLARE_int32(shards);
DECLARE_int32(iterations);
#endif

#endif /* CLIENT_H_ */