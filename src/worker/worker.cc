#include <boost/bind.hpp>
#include <signal.h>

#include "util/common.h"
#include "worker/worker.h"
#include "kernel/kernel-registry.h"
#include "kernel/table-registry.h"

DEFINE_double(sleep_hack, 0.0, "");
DEFINE_double(sleep_time, 0.001, "");
DEFINE_string(checkpoint_dir, "checkpoints", "");

namespace dsm {
static const double kNetworkTimeout = 60.0;

// Represents an active RPC to a remote peer.
struct Worker::SendRequest {
  int target;
  int rpc_type;
  int failures;

  string payload;
  MPI::Request mpi_req;
  MPI::Status status;
  double start_time;

  SendRequest() {
    failures = 0;
  }

  ~SendRequest() {}

  bool finished() {
    return mpi_req.Test(status);
  }

  double elapsed() { return Now() - start_time; }
  double timed_out() { return elapsed() > kNetworkTimeout; }

  void Send(RPCHelper* helper) {
    start_time = Now();
    mpi_req = helper->ISendData(target, rpc_type, payload);
  }

  void Cancel() {
    mpi_req.Cancel();
    ++failures;
  }
};

struct Worker::Stub {
  int32_t id;
  int32_t epoch;
  RPCHelper *helper;

  Stub(int id, RPCHelper* rpc) : id(id), epoch(0), helper(rpc) { }

  bool TryRead(int method, Message* msg) {
    return helper->TryRead(id, method, msg);
  }

  // Send the given message type and data to this peer.
  SendRequest* Send(int method, const Message& ureq) {
    SendRequest* r = new SendRequest();
    r->target = id;
    r->rpc_type = method;
    ureq.AppendToString(&r->payload);

    r->Send(helper);
    return r;
  }
};

Worker::Worker(const ConfigData &c) {
  epoch_ = 0;

  config_.CopyFrom(c);
  config_.set_worker_id(MPI::COMM_WORLD.Get_rank() - 1);

  world_ = MPI::COMM_WORLD;
  rpc_ = new RPCHelper(&world_);

  num_peers_ = config_.num_workers();
  peers_.resize(num_peers_);
  for (int i = 0; i < num_peers_; ++i) {
    peers_[i] = new Stub(i + 1, rpc_);
  }

  running_ = true;

  // HACKHACKHACK - register ourselves with any existing tables
  Registry::TableMap &t = Registry::get_tables();
  for (Registry::TableMap::iterator i = t.begin(); i != t.end(); ++i) {
    i->second->info_.worker = this;
  }

  LOG(INFO) << "Worker " << config_.worker_id() << " registering...";
  RegisterWorkerRequest req;
  req.set_id(id());
  req.set_slots(config_.slots());
  rpc_->Send(0, MTYPE_REGISTER_WORKER, req);

  LOG(INFO) << "Worker " << config_.worker_id() << " registered.";
}

int Worker::peer_for_shard(int table, int shard) const {
  return Registry::get_tables()[table]->get_owner(shard);
}

void Worker::Run() {
  table_thread_ = new boost::thread(&Worker::TableLoop, this);
  KernelLoop();
  table_thread_->join();
}

Worker::~Worker() {
  running_ = false;

  for (int i = 0; i < peers_.size(); ++i) {
    delete peers_[i];
  }
}

void Worker::Send(int peer, int type, const Message& msg) {
//  LOG(INFO) << "Sending to peer: " << peer;
  SendRequest *p = peers_[peer]->Send(type, msg);
  {
    boost::recursive_mutex::scoped_lock sl(state_lock_);
    outgoing_requests_.insert(p);

    stats_.set_bytes_out(stats_.bytes_out() + p->payload.size());
    stats_.set_put_out(stats_.put_out() + 1);
  }

  CheckForMasterUpdates();
}

void Worker::Read(int peer, int type, Message* msg) {
  while (!peers_[peer]->TryRead(type, msg)) {
    PERIODIC(0.1, CheckForMasterUpdates());
    Sleep(FLAGS_sleep_time);
  }
}

void Worker::TableLoop() {
  int miss = 0;
  while (running_) {
    if (!HandleGetRequests()) { ++miss; }
    if (miss > 1000) {
      Sleep(FLAGS_sleep_time);
      miss = 0;
    }
  }
}

void Worker::KernelLoop() {
  MPI::Intracomm world = MPI::COMM_WORLD;

  while (running_) {
    if (kernel_queue_.empty()) {
      HandlePutRequests();
      CheckForMasterUpdates();
      Sleep(FLAGS_sleep_time);
      continue;
    }

    KernelRequest k = kernel_queue_.front();
    kernel_queue_.pop_front();

    VLOG(1) << "Received run request for " << k;

    if (peer_for_shard(k.table(), k.shard()) != config_.worker_id()) {
      LOG(FATAL) << "Received a shard I can't work on! : " << k.shard()
                 << " : " << peer_for_shard(k.table(), k.shard());
    }

    KernelInfo *helper = Registry::get_kernel(k.kernel());
    KernelId id(k.kernel(), k.table(), k.shard());
    DSMKernel* d = kernels_[id];

    if (!d) {
      d = helper->create();
      kernels_[id] = d;
      d->Init(this, k.table(), k.shard());
      d->KernelInit();
    }

    if (MPI::COMM_WORLD.Get_rank() == 1 && FLAGS_sleep_hack > 0) {
      Sleep(FLAGS_sleep_hack);
    }

    helper->Run(d, k.method());

    // Flush any table updates leftover.
    for (Registry::TableMap::iterator i = Registry::get_tables().begin();
         i != Registry::get_tables().end(); ++i) {
      i->second->SendUpdates();
    }

    while (pending_network_bytes()) {
      HandlePutRequests();
      Sleep(FLAGS_sleep_time);
    }

    kernel_done_.push_back(k);

    VLOG(1) << "Kernel finished: " << k;
    DumpProfile();
  }
}

int64_t Worker::pending_network_bytes() const {
  boost::recursive_mutex::scoped_lock sl(state_lock_);
  int64_t t = 0;

  for (unordered_set<SendRequest*>::const_iterator i = outgoing_requests_.begin(); i != outgoing_requests_.end(); ++i) {
    t += (*i)->payload.size();
  }

  return t;
}

int64_t Worker::pending_kernel_bytes() const {
  int64_t t = 0;

  Registry::TableMap &tmap = Registry::get_tables();
  for (Registry::TableMap::iterator i = tmap.begin(); i != tmap.end(); ++i) {
    t += ((GlobalTable*)i->second)->pending_write_bytes();
  }

  return t;
}

bool Worker::network_idle() const {
  return pending_network_bytes() == 0;
}

void Worker::CollectPending() {
  if (outgoing_requests_.empty())
    return;

  boost::recursive_mutex::scoped_lock sl(state_lock_);
  unordered_set<SendRequest*>::iterator i = outgoing_requests_.begin();
  while (i != outgoing_requests_.end()) {
    SendRequest *r = (*i);
    VLOG(2) << "Pending: " << MP(id(), MP(r->target, r->rpc_type));
    if (r->finished()) {
      if (r->failures > 0) {
        LOG(INFO) << "Send " << MP(id(), r->target) << " of size " << r->payload.size()
            << " succeeded after " << r->failures << " failures.";
      }
      VLOG(2) << "Finished send to " << r->target << " of size " << r->payload.size();
      delete r;
      i = outgoing_requests_.erase(i);
      continue;
    }
    if (r->timed_out()) {
      LOG_EVERY_N(WARNING, 1000)  << "Send of " << r->payload.size() << " to " << r->target << " timed out.";
      r->Cancel();
      r->Send(rpc_);
    }
    ++i;
  }
}

void Worker::UpdateEpoch(int peer, int peer_marker) {
  boost::recursive_mutex::scoped_lock sl(state_lock_);
  LOG(INFO) << "Got peer marker: " << MP(peer, MP(epoch_, peer_marker));
  if (epoch_ < peer_marker) {
    LOG(INFO) << "Checkpointing; received new epoch marker from peer:" << MP(epoch_, peer_marker);
    Checkpoint(peer_marker);
  }

  peers_[peer]->epoch = peer_marker;

  bool checkpoint_done = true;
  for (int i = 0; i < peers_.size(); ++i) {
    if (peers_[i]->epoch != epoch_) {
      checkpoint_done = false;
      LOG(INFO) << "Channel is out of date: " << i << " : " << MP(peers_[i]->epoch, epoch_);
    }
  }

  if (checkpoint_done) {
    LOG(INFO) << "All channels up to date; flushing deltas.";
    Registry::TableMap &t = Registry::get_tables();
    for (Registry::TableMap::iterator i = t.begin(); i != t.end(); ++i) {
      GlobalTable* t = i->second;
      t->finish_checkpoint();
    }

    EmptyMessage req;
    rpc_->Send(config_.master_id(), MTYPE_CHECKPOINT_DONE, req);
  }
}

void Worker::Checkpoint(int epoch) {
  boost::recursive_mutex::scoped_lock sl(state_lock_);
  if (epoch_ >= epoch) {
    LOG(INFO) << "Skipping checkpoint; " << MP(epoch_, epoch);
    return;
  }

  LOG(INFO) << "Checkpointing... " << MP(epoch_, epoch);

  epoch_ = epoch;

  Registry::TableMap &t = Registry::get_tables();
  for (Registry::TableMap::iterator i = t.begin(); i != t.end(); ++i) {
    GlobalTable* t = i->second;
    t->start_checkpoint(StringPrintf("%s/checkpoint.table_%d.epoch_%d", FLAGS_checkpoint_dir.c_str(), i->first, epoch_));
  }

  HashPut epoch_marker;
  epoch_marker.set_source(id());
  epoch_marker.set_table(-1);
  epoch_marker.set_shard(-1);
  epoch_marker.set_done(true);
  epoch_marker.set_marker(epoch_);
  for (int i = 0; i < peers_.size(); ++i) {
    peers_[i]->Send(MTYPE_PUT_REQUEST, epoch_marker);
  }
}

void Worker::Restore(int epoch) {
  boost::recursive_mutex::scoped_lock sl(state_lock_);
  LOG(INFO) << "Worker restoring state from epoch: " << epoch;
  epoch_ = epoch;

  Registry::TableMap &t = Registry::get_tables();
  for (Registry::TableMap::iterator i = t.begin(); i != t.end(); ++i) {
    GlobalTable* t = i->second;
    t->restore(StringPrintf("%s/checkpoint.table_%d.epoch_%d", FLAGS_checkpoint_dir.c_str(), i->first, epoch_));
  }

  EmptyMessage req;
  rpc_->Send(config_.master_id(), MTYPE_RESTORE_DONE, req);
}

void Worker::HandlePutRequests() {
  CollectPending();

  HashPut put;
  while (rpc_->TryRead(MPI::ANY_SOURCE, MTYPE_PUT_REQUEST, &put)) {
    if (put.marker() != -1) {
      UpdateEpoch(put.source(), put.marker());
      continue;
    }

    stats_.set_put_in(stats_.put_in() + 1);
    stats_.set_bytes_in(stats_.bytes_in() + put.ByteSize());

    GlobalTable *t = Registry::get_table(put.table());
    boost::recursive_mutex::scoped_lock sl(t->mutex());
    t->ApplyUpdates(put);


    // Record messages from our peer channel up until they checkpointed.
    if (put.epoch() < epoch_) {
      t->write_delta(put);
    }

    if (put.done() && t->tainted(put.shard())) {
      VLOG(1) << "Clearing taint on: " << MP(put.table(), put.shard());
      t->clear_tainted(put.shard());
    }
  }
}

bool Worker::HandleGetRequests() {
  PERIODIC(10, {
             LOG(INFO) << "Pending network: " << pending_network_bytes() << " rss: " << get_memory_rss();
             DumpProfile();
  });

  bool did_work = false;

  MPI::Status status;
  HashGet get_req;
  HashPut get_resp;
  while (rpc_->HasData(MPI::ANY_SOURCE, MTYPE_GET_REQUEST, status)) {
    rpc_->Read(MPI::ANY_SOURCE, MTYPE_GET_REQUEST, &get_req);
//    LOG(INFO) << "Get request: " << get_req;

    did_work = true;

    stats_.set_get_in(stats_.get_in() + 1);
    stats_.set_bytes_in(stats_.bytes_in() + get_req.ByteSize());

    get_resp.Clear();
    get_resp.set_source(config_.worker_id());
    get_resp.set_table(get_req.table());
    get_resp.set_shard(-1);
    get_resp.set_done(true);
    get_resp.set_epoch(epoch_);
    HashPutCoder h(&get_resp);

    {
      GlobalTable* t = Registry::get_table(get_req.table());
      boost::recursive_mutex::scoped_lock sl(t->mutex());

      if (!t->contains_str(get_req.key())) {
        get_resp.set_missing_key(true);
      } else {
        string v;
        t->get_local(get_req.key(), &v);
        h.add_pair(get_req.key(), v);
      }
    }

    SendRequest *r = peers_[status.Get_source() - 1]->Send(MTYPE_GET_RESPONSE, get_resp);

    boost::recursive_mutex::scoped_lock sl(state_lock_);
    outgoing_requests_.insert(r);

    VLOG(2) << "Returning result for " << MP(get_req.table(), get_req.shard());
  }

  return did_work;
}

void Worker::CheckForMasterUpdates() {
  boost::recursive_mutex::scoped_lock sl(state_lock_);
  // Check for shutdown.
  EmptyMessage msg;
  KernelRequest k;

  if (rpc_->TryRead(config_.master_id(), MTYPE_WORKER_SHUTDOWN, &msg)) {
    VLOG(1) << "Shutting down worker " << config_.worker_id();
    running_ = false;
    return;
  }

  StartCheckpoint checkpoint_msg;
  while (rpc_->TryRead(config_.master_id(), MTYPE_CHECKPOINT, &checkpoint_msg)) {
    Checkpoint(checkpoint_msg.epoch());
  }

  StartRestore restore_msg;
  while (rpc_->TryRead(config_.master_id(), MTYPE_RESTORE, &restore_msg)) {
    Restore(restore_msg.epoch());
  }

  ShardAssignmentRequest shard_req;
  set<GlobalTable*> dirty_tables;
  while (rpc_->TryRead(config_.master_id(), MTYPE_SHARD_ASSIGNMENT, &shard_req)) {
    for (int i = 0; i < shard_req.assign_size(); ++i) {
      const ShardAssignment &a = shard_req.assign(i);
      GlobalTable *t = Registry::get_table(a.table());
      int old_owner = t->get_owner(a.shard());
      t->set_owner(a.shard(), a.new_worker());
      VLOG(1) << "Setting owner: " << MP(a.shard(), a.new_worker());

      if (a.new_worker() == id() && old_owner != id()) {
        VLOG(1)  << "Setting self as owner of " << MP(a.table(), a.shard());

        // Don't consider ourselves canonical for this shard until we receive updates
        // from the old owner.
        if (old_owner != -1) {
          VLOG(1) << "Setting " << MP(a.table(), a.shard()) << " as tainted.  Old owner was: " << old_owner;
          t->set_tainted(a.shard());
        }
      } else if (old_owner == id() && a.new_worker() != id()) {
        VLOG(1) << "Lost ownership of " << MP(a.table(), a.shard()) << " to " << a.new_worker();
        // A new worker has taken ownership of this shard.  Flush our data out.
        t->set_dirty(a.shard());
        dirty_tables.insert(t);
      }
    }

    // Flush any tables we no longer own.
    for (set<GlobalTable*>::iterator i = dirty_tables.begin(); i != dirty_tables.end(); ++i) {
      (*i)->SendUpdates();
    }
  }

  // Check for new kernels to run, and report finished kernels to the master.
  while (rpc_->TryRead(config_.master_id(), MTYPE_RUN_KERNEL, &k)) {
    kernel_queue_.push_back(k);
  }

  if (network_idle()) {
    while (!kernel_done_.empty()) {
      rpc_->Send(config_.master_id(), MTYPE_KERNEL_DONE, kernel_done_.front());
      kernel_done_.pop_front();
    }
  }
}

} // end namespace
