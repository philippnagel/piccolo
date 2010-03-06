#include "client.h"
#include <cblas.h>

using namespace dsm;

DEFINE_int32(edge_size, 1000, "");

static const int kBlockSize = 100;
static int bRows = -1;
static int bCols = -1;

struct Block { double d[kBlockSize*kBlockSize]; };

static TypedGlobalTable<int, Block>* matrix_a = NULL;
static TypedGlobalTable<int, Block>* matrix_b = NULL;
static TypedGlobalTable<int, Block>* matrix_c = NULL;

static Block block_sum(const Block &a, const Block& b) {
  Block c;
  for (int i = 0; i < kBlockSize * kBlockSize; ++i) {
    c.d[i] = a.d[i] + b.d[i];
  }
  return c;
}

struct MatrixMultiplicationKernel : public DSMKernel {
  void Initialize() {
    LOG(INFO) << "Initializing...";
    Block b, z;
    memset(b.d, 2, kBlockSize * kBlockSize * sizeof(double));
    memset(z.d, 0, kBlockSize * kBlockSize * sizeof(double));

    int cs = current_shard();
    int ns = matrix_a->num_shards();

    for (int by = 0; by < bRows; by ++) {
      for (int bx = 0; bx < bCols; bx ++) {
        if (by % ns != cs) { continue; }
        LOG(INFO) << "Putting... " << MP(by, bx);
        matrix_a->put(by * bCols + bx, b);
        matrix_b->put(by * bCols + bx, b);
        matrix_c->put(by * bCols + bx, z);
      }
    }
  }

  void Multiply() {
    int cs = current_shard();
    int ns = matrix_a->num_shards();

    for (int k = 0; k < bRows; k++) {
      for (int i = 0; i < bRows; i++) {
        for (int j = 0; j < bCols; j++) {
          if (i % ns != cs) { continue; }
//          LOG(INFO) << "Multiplying..." << i << "," << j << "," << k << "," << cs;
          Block a = matrix_a->get(i * bCols + k);
          Block b = matrix_b->get(k * bCols + j);
          Block c;
          cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                      kBlockSize, kBlockSize, kBlockSize, 1,
                      a.d, kBlockSize, b.d, kBlockSize, 1, c.d, kBlockSize);

          matrix_c->put(i * bCols + j, c);
        }
      }
    }
  }
};

REGISTER_KERNEL(MatrixMultiplicationKernel);
REGISTER_METHOD(MatrixMultiplicationKernel, Initialize);
REGISTER_METHOD(MatrixMultiplicationKernel, Multiply);

int MatrixMultiplication(ConfigData& conf) {
  bCols = FLAGS_edge_size / kBlockSize;
  bRows = FLAGS_edge_size / kBlockSize;

  LOG(INFO) << "Create matrices with " << conf.num_workers() << " shards.";
  matrix_a = Registry::create_table<int, Block>(0, conf.num_workers(), &ModSharding, &block_sum);
  matrix_b = Registry::create_table<int, Block>(1, conf.num_workers(), &ModSharding, &block_sum);
  matrix_c = Registry::create_table<int, Block>(2, conf.num_workers(), &ModSharding, &block_sum);

  if (MPI::COMM_WORLD.Get_rank() == 0) {
    Master m(conf);

    m.run_one(Master::RunDescriptor::C("MatrixMultiplicationKernel", "Initialize", 0));
    m.run_all(Master::RunDescriptor::C("MatrixMultiplicationKernel", "Multiply", 0));
  } else {
    conf.set_worker_id(MPI::COMM_WORLD.Get_rank() - 1);
    Worker w(conf);
    w.Run();

    LOG(INFO) << "Worker " << conf.worker_id() << " :: " << w.get_stats();
  }

  return 0;
}
REGISTER_RUNNER(MatrixMultiplication);