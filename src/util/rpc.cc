#include "util/rpc.h"
#include "util/common.h"

DECLARE_bool(localtest);
DEFINE_bool(rpc_log, false, "");

namespace dsm {

class MPIHelper : public RPCHelper, private boost::noncopyable {
public:
  MPIHelper() :
    mpi_world_(&MPI::COMM_WORLD), my_rank_(MPI::COMM_WORLD.Get_rank()) {
  }

  // Try to read a message from the given peer and rpc channel = 0; return false if no
  // message is immediately available.
  bool TryRead(int target, int method, Message *msg);
  bool HasData(int target, int method);
  bool HasData(int target, int method, MPI::Status &status);

  int Read(int src, int method, Message *msg);
  int ReadAny(int *src, int method, Message *msg);
  void Send(int target, int method, const Message &msg);
  void SyncSend(int target, int method, const Message &msg);

  void SendData(int peer_id, int rpc_id, const string& data);
  MPI::Request ISendData(int peer_id, int rpc_id, const string& data);

  // For whatever reason, MPI doesn't offer tagged broadcasts, we simulate that
  // here.
  void Broadcast(int method, const Message &msg);
  void SyncBroadcast(int method, const Message &msg);
private:
  boost::recursive_mutex mpi_lock_;
  MPI::Comm *mpi_world_;
  int my_rank_;
};


RPCHelper* get_rpc_helper() {
  static RPCHelper* helper = NULL;
  if (!helper) { helper = new MPIHelper(); }
  return helper;
}

#define rpc_log(logmsg, src, target, method) VLOG_IF(2, FLAGS_rpc_log) << StringPrintf("source %d target: %d rpc: %d %s", src, target, method, string((logmsg)).c_str());
#define rpc_lock boost::recursive_mutex::scoped_lock sl(mpi_lock_);

bool MPIHelper::HasData(int target, int method) {
  rpc_lock;

  MPI::Status st;
  PERIODIC(1, rpc_log("IProbe", my_rank_, target, method));
  return mpi_world_->Iprobe(target, method, st);
}

bool MPIHelper::HasData(int target, int method, MPI::Status &status) {
  rpc_lock;

  PERIODIC(1, rpc_log("IProbe", my_rank_, target, method));
  return mpi_world_->Iprobe(target, method, status);
}

bool MPIHelper::TryRead(int target, int method, Message *msg) {
  rpc_lock;
  bool success = false;
  string scratch;
  MPI::Status status;

  PERIODIC(1, rpc_log("IProbe", my_rank_, target, method));
  MPI::Status probe_result;
  if (mpi_world_->Iprobe(target, method, probe_result)) {
    success = true;

    rpc_log("IProbeSuccess", my_rank_, target, method);

    scratch.resize(probe_result.Get_count(MPI::BYTE));

    mpi_world_->Recv(&scratch[0], scratch.size(), MPI::BYTE, target, method, status);
    rpc_log("ReadDone", my_rank_, target, method);

    CHECK(msg->ParseFromString(scratch)) << "Failed to parse message of size: " << scratch.size()
      << "source: " << target << " dest: " << my_rank_ << " method: " << method;
  }

//  rpc_log("IProbeDone", my_rank_, target, method);
  return success;
}

int MPIHelper::Read(int target, int method, Message *msg) {
  rpc_lock;

  int r_size = 0;
  string scratch;
  MPI::Status status;

  rpc_log("BProbeStart", my_rank_, target, method);
  MPI::Status probe_result;
  mpi_world_->Probe(target, method, probe_result);
  rpc_log("BProbeDone", my_rank_, target, method);

  r_size = probe_result.Get_count(MPI::BYTE);
  scratch.resize(r_size);

  mpi_world_->Recv(&scratch[0], r_size, MPI::BYTE, target, method, status);

  VLOG(2) << "Read message: " << MP(target, method);

  msg->ParseFromString(scratch);
  return r_size;
}

int MPIHelper::ReadAny(int *src, int method, Message *msg) {
  rpc_lock;
  int r_size = 0;
  string scratch;

  while (!HasData(MPI_ANY_SOURCE, method)) {
    sched_yield();
  }

  MPI::Status probe_result;
  mpi_world_->Probe(MPI_ANY_SOURCE, method, probe_result);

  r_size = probe_result.Get_count(MPI::BYTE);
  int r_src = probe_result.Get_source();

  scratch.resize(r_size);
  mpi_world_->Recv(&scratch[0], r_size, MPI::BYTE, r_src, method, probe_result);
  msg->ParseFromString(scratch);

  if (src) { *src = r_src; }

  return r_size;
}

void MPIHelper::Send(int target, int method, const Message &msg) {
  rpc_lock;
  rpc_log("SendStart", my_rank_, target, method);
  string scratch;
  msg.AppendToString(&scratch);

  mpi_world_->Send(&scratch[0], scratch.size(), MPI::BYTE, target, method);
  rpc_log("SendDone", my_rank_, target, method);
}

void MPIHelper::SyncSend(int target, int method, const Message &msg) {
  rpc_lock;
  rpc_log("SyncSendStart", my_rank_, target, method);
  string scratch;

  scratch.clear();
  msg.AppendToString(&scratch);
  mpi_world_->Ssend(&scratch[0], scratch.size(), MPI::BYTE, target, method);
  rpc_log("SyncSendDone", my_rank_, target, method);
}

void MPIHelper::SendData(int target, int method, const string& msg) {
  rpc_lock;
  rpc_log("SendData", my_rank_, target, method);
  mpi_world_->Send(&msg[0], msg.size(), MPI::BYTE, target, method);
}


MPI::Request MPIHelper::ISendData(int target, int method, const string& msg) {
  rpc_lock;
  rpc_log("ISendData", my_rank_, target, method);
  return mpi_world_->Issend(&msg[0], msg.size(), MPI::BYTE, target, method);
}


// For whatever reason, MPI doesn't offer tagged broadcasts, we simulate that
// here.
void MPIHelper::Broadcast(int method, const Message &msg) {
  rpc_lock;
  for (int i = 1; i < mpi_world_->Get_size(); ++i) {
    Send(i, method, msg);
  }
}

void MPIHelper::SyncBroadcast(int method, const Message &msg) {
  rpc_lock;
  for (int i = 1; i < mpi_world_->Get_size(); ++i) {
    SyncSend(i, method, msg);
  }
}

}
