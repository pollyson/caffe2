#include "caffe2/core/net.h"

#include <set>
#include <stack>
#include <unordered_map>
#include <unordered_set>

#include "caffe2/core/operator.h"
#include "caffe2/core/timer.h"
#include "caffe2/proto/caffe2.pb.h"

CAFFE2_DEFINE_bool(
    caffe2_disable_chaining,
    false,
    "Disable chaining logic (some latent multi-device issues).");

namespace caffe2 {

namespace {

bool sameDevice(const OperatorDef& lhs, const OperatorDef& rhs) {
  return lhs.device_option().device_type() ==
      rhs.device_option().device_type() &&
      lhs.device_option().cuda_gpu_id() == rhs.device_option().cuda_gpu_id();
}

using OpIndex = int;
DAGNetBase::ExecutionChains singleChains(
    const std::vector<internal::OperatorNode>& nodes) {
  DAGNetBase::ExecutionChains chains;
  for (auto i = 0; i < nodes.size(); ++i) {
    chains[i] = {i};
  }
  return chains;
}

DAGNetBase::ExecutionChains computeChains(
    const std::vector<internal::OperatorNode>& nodes) {
  vector<int> initial_frontier;
  for (int idx = 0; idx < nodes.size(); ++idx) {
    if (nodes[idx].parents_.size() == 0) {
      initial_frontier.push_back(idx);
    }
  }

  // We need to construct the node_seen_count to know how many inner edges each
  // node has.
  std::unordered_map<OpIndex, int> node_seen_count;

  for (int root_index : initial_frontier) {
    const auto& root = nodes[root_index];
    std::stack<std::pair<OpIndex, std::vector<int>::const_iterator>>
        depth_stack;
    depth_stack.push(make_pair(root_index, root.children_.begin()));
    node_seen_count[root_index]++;
    CAFFE_ENFORCE(
        node_seen_count[root_index] == 1,
        "root node ",
        root_index,
        " visit count must be == 1");

    while (depth_stack.size() > 0) {
      auto cur = depth_stack.top();
      depth_stack.pop();
      if (cur.second != nodes[cur.first].children_.end()) {
        OpIndex node_index = *cur.second;
        node_seen_count[node_index]++;
        cur.second++;
        depth_stack.push(cur);
        if (node_seen_count[node_index] == 1) {
          // Visit each child only once.
          depth_stack.push(
              make_pair(node_index, nodes[node_index].children_.begin()));
        }
      }
    }
  }
  // Now, we compute the set of execution chains An execution chain is
  // a linear set of nodes that can be executed on a single stream
  // (e.g. a chain of single input, single output operators)
  DAGNetBase::ExecutionChains chains;
  std::unordered_set<OpIndex> seen_nodes;
  std::vector<OpIndex> chain;
  std::pair<OpIndex, std::vector<int>::const_iterator> cur;
  std::stack<std::pair<OpIndex, std::vector<int>::const_iterator>> depth_stack;
  auto check_current_for_chaining = [&]() -> bool {
    return (
        node_seen_count[cur.first] == 1 &&
        (chain.size() == 0 || sameDevice(
                                  nodes[cur.first].operator_->def(),
                                  nodes[chain.back()].operator_->def())));
  };
  auto commit_chain = [&]() {
    if (chain.size() > 0) {
      CAFFE_ENFORCE(
          chains.insert({chain.front(), chain}).second,
          "Chain ",
          chain.front(),
          " was already added.");
      VLOG(2) << "Added chain: " << chain.front() << "with elements";
      for (auto ch : chain) {
        VLOG(2) << ch << ", ";
      }
      chain.clear();
    }
  };
  auto depth_traverse = [&]() {
    while (cur.second != nodes[cur.first].children_.end() &&
           seen_nodes.find(*cur.second) != seen_nodes.end()) {
      cur.second++;
    }

    if (cur.second != nodes[cur.first].children_.end()) {
      auto next = make_pair(*cur.second, nodes[*cur.second].children_.begin());
      depth_stack.push(cur);
      depth_stack.push(next);
    }
  };
  for (int root_index : initial_frontier) {
    depth_stack.push(
        make_pair(root_index, nodes[root_index].children_.begin()));
    while (depth_stack.size() > 0) {
      cur = depth_stack.top();
      depth_stack.pop();
      if (seen_nodes.find(cur.first) == seen_nodes.end()) {
        seen_nodes.insert(cur.first);
        // Has one child, can be candidate for chain or can be added to the
        // previous chain.
        if (nodes[cur.first].children_.size() == 1) {
          if (check_current_for_chaining()) {
            // Add oneself to the current chain.
            VLOG(1) << "Adding to existing chain" << cur.first;
            chain.push_back(cur.first);
            int index = *nodes[cur.first].children_.begin();
            depth_stack.push(make_pair(index, nodes[index].children_.begin()));
          } else {
            // Can't belong to the previous chain, commit previous chain and
            // start a new one.
            commit_chain();
            chain.push_back(cur.first);
            int index = *nodes[cur.first].children_.begin();
            depth_stack.push(make_pair(index, nodes[index].children_.begin()));
          }
        } else if (
            nodes[cur.first].children_.size() == 0 &&
            check_current_for_chaining()) {
          // Add current node to the current chain and commit.
          chain.push_back(cur.first);
          commit_chain();
        } else {
          // Node has more than one child.
          commit_chain();
          // Add current node as an independent chain since it won't be a part
          // of a bigger chain.
          chain.push_back(cur.first);
          commit_chain();
          depth_traverse();
        }
      } else {
        // This node has been seen before, we will only traverse its children.
        // Commit any pending chains and continue traversing.
        commit_chain();
        depth_traverse();
      }
    } // End while

    // Check if this if is even needed.
    commit_chain();
  }
  CAFFE_ENFORCE(
      seen_nodes.size() == nodes.size(),
      "Haven't seen all the nodes, expected number of nodes ",
      nodes.size(),
      ", but seen only ",
      seen_nodes.size(),
      ".");
  return chains;
}
}

CAFFE_DEFINE_REGISTRY(NetRegistry, NetBase, const NetDef&, Workspace*);

NetBase::NetBase(const NetDef& def, Workspace* /* unused */)
    : external_input_(def.external_input().begin(), def.external_input().end()),
      external_output_(
          def.external_output().begin(),
          def.external_output().end()),
      name_(def.name()) {
  // Go through the operators and make sure that blobs are correctly made.
  std::set<string> known_blobs(
      external_input_.begin(), external_input_.end());
  std::set<string> remaining_output(
      external_output_.begin(), external_output_.end());
  for (const auto& blob : known_blobs) {
    remaining_output.erase(blob);
  }
  for (const OperatorDef& op : def.op()) {
    for (const string& in : op.input()) {
      if (!known_blobs.count(in)) {
        if (external_input_.size()) {
          CAFFE_THROW(
              "op ",
              op.type(),
              ": Source for input ",
              in,
              " is unknown for net ",
              def.name(),
              ", operator ",
              ProtoDebugString(op));
        } else {
          // If we are not declaring input and output, we will simply VLOG it
          // for debugging purposes.
          VLOG(1) << "op " << op.type() << ": input " << in << " is unknown.";
        }
      }
    }
    for (const string& out : op.output()) {
      known_blobs.insert(out);
      remaining_output.erase(out);
    }
  }
  // Finally, check if all declared outputs are being created.
  CAFFE_ENFORCE(
      remaining_output.size() == 0,
      "Some of the blobs are declared as output but never produced by the "
      "net ",
      def.name(),
      ", the first one is ",
      *remaining_output.begin());
}

unique_ptr<NetBase> CreateNet(const NetDef& net_def, Workspace* ws) {
  // In default, we will return a simple network that just runs all operators
  // sequentially.
  if (!net_def.has_type()) {
    return make_unique<SimpleNet>(net_def, ws);
  }
  return NetRegistry()->Create(net_def.type(), net_def, ws);
}

SimpleNet::SimpleNet(const NetDef& net_def, Workspace* ws)
    : NetBase(net_def, ws) {
  VLOG(1) << "Constructing SimpleNet " << net_def.name();
  bool net_def_has_device_option = net_def.has_device_option();
  // Initialize the operators
  for (const OperatorDef& operator_def : net_def.op()) {
    VLOG(1) << "Creating operator " << operator_def.name()
            << ":" << operator_def.type();
    if (!operator_def.has_device_option() && net_def_has_device_option) {
      // In the case that the operator def does not specify a device option but
      // the net def has a default option, we copy the device option over to the
      // operator def.
      OperatorDef temp_def(operator_def);
      temp_def.mutable_device_option()->CopyFrom(net_def.device_option());
      operators_.emplace_back(CreateOperator(temp_def, ws));
    } else {
      operators_.emplace_back(CreateOperator(operator_def, ws));
    }
  }
}

bool SimpleNet::Run() {
  VLOG(1) << "Running net " << name_;
  for (auto& op : operators_) {
    VLOG(1) << "Running operator " << op->def().name()
            << "(" << op->def().type() << ").";
    if (!op->Run()) {
      LOG(ERROR) << "Operator failed: "
                      << ProtoDebugString(op->def());
      return false;
    }
  }
  return true;
}

bool SimpleNet::RunAsync() {
  VLOG(1) << "Running net " << name_;
  for (auto& op : operators_) {
    VLOG(1) << "Running operator " << op->def().name()
            << "(" << op->def().type() << ").";
    if (!op->RunAsync()) {
      LOG(ERROR) << "Operator failed: "
                 << ProtoDebugString(op->def());
      return false;
    }
  }
  return true;
}

vector<float> SimpleNet::TEST_Benchmark(
    const int warmup_runs,
    const int main_runs,
    const bool run_individual) {
  LOG(INFO) << "Starting benchmark.";
  LOG(INFO) << "Running warmup runs.";
  CAFFE_ENFORCE(
      warmup_runs >= 0,
      "Number of warm up runs should be non negative, provided ",
      warmup_runs,
      ".");
  for (int i = 0; i < warmup_runs; ++i) {
    CAFFE_ENFORCE(Run(), "Warmup run ", i, " has failed.");
  }

  LOG(INFO) << "Main runs.";
  CAFFE_ENFORCE(
      main_runs >= 0,
      "Number of main runs should be non negative, provided ",
      main_runs,
      ".");
  Timer timer;
  for (int i = 0; i < main_runs; ++i) {
    CAFFE_ENFORCE(Run(), "Main run ", i, " has failed.");
  }
  auto millis = timer.MilliSeconds();
  LOG(INFO) << "Main run finished. Milliseconds per iter: "
                 << millis / main_runs
                 << ". Iters per second: " << 1000.0 * main_runs / millis;

  vector<float> time_per_op(operators_.size(), 0);
  CaffeMap<string, float> time_per_op_type;
  if (run_individual) {
    for (int i = 0; i < main_runs; ++i) {
      int idx = 0;
      for (auto& op : operators_) {
        const string& op_type = op->def().type();
        timer.Start();
        CAFFE_ENFORCE(
            op->Run(),
            "operator ",
            op->def().name(),
            "(",
            op_type,
            ") has failed.");
        float spent = timer.MilliSeconds();
        time_per_op[idx] += spent;
        time_per_op_type[op_type] += spent;
        ++idx;
      }
    }

    int idx = 0;
    for (auto& op : operators_) {
      const string& op_type = op->def().type();
      const string& print_name =
          (op->def().name().size()
               ? op->def().name()
               : (op->def().output_size() ? op->def().output(0) : "NO_OUTPUT"));
      LOG(INFO) << "Operator #" << idx << " (" << print_name << ", " << op_type
                << ") " << time_per_op[idx] / main_runs << " ms/iter";
      ++idx;
    }
    LOG(INFO) << "Time per operator type:";
    for (const auto& item : time_per_op_type) {
      LOG(INFO) << std::setw(15) << std::setfill(' ')
                     << item.second / main_runs
                     << " " << item.first;
    }
  }
  // We will reuse time_per_op to return the result of BenchmarkNet.
  for (int i = 0; i < time_per_op.size(); ++i) {
    time_per_op[i] /= main_runs;
  }
  time_per_op.insert(time_per_op.begin(), millis / main_runs);
  return time_per_op;
}

DAGNetBase::DAGNetBase(const NetDef& net_def, Workspace* ws)
    : NetBase(net_def, ws), operator_nodes_(net_def.op_size()) {
  // Blob creator allows us to track which operator created which blob.
  VLOG(1) << "Constructing DAGNet " << net_def.name();
  std::map<string, int> blob_creator;
  std::map<string, std::set<int> > blob_readers;
  bool net_def_has_device_option = net_def.has_device_option();
  // Initialize the operators
  for (int idx = 0; idx < net_def.op_size(); ++idx) {
    const OperatorDef& op_def = net_def.op(idx);
    VLOG(1) << "Creating operator #" << idx << ": "
            << op_def.name() << ":" << op_def.type();
    if (!op_def.has_device_option() && net_def_has_device_option) {
      OperatorDef temp_def(op_def);
      temp_def.mutable_device_option()->CopyFrom(net_def.device_option());
      operator_nodes_[idx].operator_ = CreateOperator(temp_def, ws);
    } else {
      operator_nodes_[idx].operator_ = CreateOperator(op_def, ws);
    }
    // Check the inputs, and set up parents if necessary. This addressese the
    // read after write case.
    auto checkInputs = [&](
        const google::protobuf::RepeatedPtrField<std::string>& inputs) {
      for (const string& input : inputs) {
        if (blob_creator.count(input) == 0) {
          VLOG(1) << "Input " << input << " not produced by this net. "
                  << "Assuming it is pre-existing.";
        } else {
          int parent = blob_creator[input];
          VLOG(1) << "op dependency (RaW " << input << "): " << parent << "->"
                  << idx;
          operator_nodes_[idx].parents_.push_back(parent);
          operator_nodes_[parent].children_.push_back(idx);
        }
        // Add the current idx to the readers of this input.
        blob_readers[input].insert(idx);
      }
    };
    checkInputs(op_def.input());
    checkInputs(op_def.control_input());

    // Check the outputs.
    for (const string& output : op_def.output()) {
      if (blob_creator.count(output) != 0) {
        // This addresses the write after write case - we will assume that all
        // writes are inherently sequential.
        int waw_parent = blob_creator[output];
        VLOG(1) << "op dependency (WaW " << output << "): "
                      << waw_parent << "->" << idx;
        operator_nodes_[idx].parents_.push_back(waw_parent);
        operator_nodes_[waw_parent].children_.push_back(idx);
      }
      // This addresses the write after read case - we will assume that writes
      // should only occur after all previous reads are finished.
      for (const int war_parent : blob_readers[output]) {
        VLOG(1) << "op dependency (WaR " << output << "): "
                      << war_parent << "->" << idx;
        operator_nodes_[idx].parents_.push_back(war_parent);
        operator_nodes_[war_parent].children_.push_back(idx);
      }
      // Renew the creator of the output name.
      blob_creator[output] = idx;
      // The write would create an implicit barrier that all earlier readers of
      // this output is now parents of the current op, and future writes would
      // not need to depend on these earlier readers. Thus, we can clear up the
      // blob readers.
      blob_readers[output].clear();
    }
  }

  // Now, make sure that the parent list and the children list do not contain
  // duplicated items.
  for (int i = 0; i < operator_nodes_.size(); ++i) {
    auto& node = operator_nodes_[i];
    // Sort, remove duplicates, and delete self dependency.
    auto& p = node.parents_;
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    p.erase(std::remove(p.begin(), p.end(), i), p.end());
    // Do the same for the children vector.
    auto& c = node.children_;
    std::sort(c.begin(), c.end());
    c.erase(std::unique(c.begin(), c.end()), c.end());
    c.erase(std::remove(c.begin(), c.end(), i), c.end());
  }

  execution_chains_ =
      (FLAGS_caffe2_disable_chaining ? singleChains(operator_nodes_)
                                     : computeChains(operator_nodes_));

  LOG(INFO) << "Number of parallel execution chains "
            << execution_chains_.size()
            << " Number of operators = " << net_def.op_size();
  // TODO: do we want to make sure that there are no loops in the
  // dependency graph?

  // Figure out the initial frontier - this is the one we will feed into the job
  // queue to start a run.
  for (int idx = 0; idx < operator_nodes_.size(); ++idx) {
    if (operator_nodes_[idx].parents_.size() == 0) {
      initial_frontier_.push_back(idx);
    }
  }
  // Finally, start the workers.
  int num_workers = net_def.has_num_workers() ? net_def.num_workers() : 1;
  CAFFE_ENFORCE(num_workers > 0, "Must have a positive number of workers.");
  if (num_workers == 1) {
    LOG(WARNING) << "Number of workers is 1: this means that all operators "
                 << "will be executed sequentially. Did you forget to set "
                 << "num_workers in the NetDef?";
  }
  for (int i = 0; i < num_workers; ++i) {
    VLOG(1) << "Start worker #" << i;
    workers_.push_back(std::thread(&DAGNetBase::WorkerFunction, this));
  }
}

DAGNetBase::~DAGNetBase() {
  // Safely join all the workers before exiting.
  job_queue_.NoMoreJobs();
  VLOG(1) << "Joining workers.";
  for (auto& worker : workers_) {
    worker.join();
  }
}

bool DAGNetBase::Run() {
  // Lock the run_in_progress_ lock so that we do not accidentally call Run()
  // in parallel.
  std::unique_lock<std::mutex> run_lock(run_in_progress_);
  VLOG(1) << "Running parallel net.";
  // First, set up job queue.
  remaining_ops_ = operator_nodes_.size();
  success_ = true;
  // TODO(jiayq): Start all worker threads.
  // Initialize the runtime parent count.
  for (auto& node : operator_nodes_) {
    node.runtime_parent_count_ = node.parents_.size();
  }
  // Kickstart the job queue.
  for (auto& value : initial_frontier_) {
    job_queue_.Push(value);
  }
  std::unique_lock<std::mutex> mutex_lock(remaining_ops_mutex_);
  while (remaining_ops_ > 0) {
    VLOG(2) << "Remaining ops to run: " << remaining_ops_;
    cv_.wait(mutex_lock);
  }
  VLOG(2) << "All ops finished running.";
  for (const auto& op : operator_nodes_) {
    CAFFE_ENFORCE(
        op.runtime_parent_count_ == 0,
        "Operator ",
        op.operator_->def().name(),
        "(",
        op.operator_->def().type(),
        ") has some runtime parents left.");
  }
  // If the above while loop finished, we know that the current run finished.
  return success_;
}

void DAGNetBase::WorkerFunction() {
  // WorkerFunctions() is an infinite loop until there are no more jobs to run.
  while (true) {
    int idx = 0;
    // If there is no more jobs - meaning that the DAGNetBase is destructing -
    // we will exit safely.
    if (!job_queue_.Pop(&idx)) {
      return;
    }
    VLOG(1) << "Running operator #" << idx << " "
            << operator_nodes_[idx].operator_->def().name()
            << "(" << operator_nodes_[idx].operator_->def().type() << ").";
    CAFFE_ENFORCE(
        execution_chains_.find(idx) != execution_chains_.end(),
        "Can't find chain ",
        idx,
        ".");
    const auto& chain = execution_chains_[idx];
    bool this_success = RunAt(execution_chains_[idx]);
    if (!this_success) {
      LOG(ERROR) << "Operator chain failed: "
                 << ProtoDebugString(operator_nodes_[idx].operator_->def());
    }

    // Do book-keeping
    for (const auto idx: chain) {
      for (const auto child: operator_nodes_[idx].children_) {
        const int count = --operator_nodes_[child].runtime_parent_count_;
        CAFFE_ENFORCE(
            count >= 0,
            "Found runtime parent count smaller than zero for ",
            "operator node ",
            operator_nodes_[child].operator_->def().name(),
            "(",
            operator_nodes_[child].operator_->def().type(),
            ").");

        if (count != 0) {
          continue;
        }

        if (std::find(chain.begin(), chain.end(), child) != chain.end()) {
          // already executed
          continue;
        }
        VLOG(2) << "Pushing operator #" << child << " to queue.";
        job_queue_.Push(child);
      }
    }

    // Notify that the processed op is incremented by one.
    {
      std::unique_lock<std::mutex> mutex_lock(remaining_ops_mutex_);
      remaining_ops_ -= chain.size();
      success_ &= this_success;
      CAFFE_ENFORCE(
          remaining_ops_ >= 0,
          "All the operations should be finished by now, still have ",
          remaining_ops_,
          " remaining.");
    }
    cv_.notify_one();
    VLOG(2) << "Finished executing operator #" << idx;
  }
}

vector<float> DAGNetBase::TEST_Benchmark(
    const int warmup_runs,
    const int main_runs,
    const bool run_individual) {
  LOG(INFO) << "Starting benchmark.";
  LOG(INFO) << "Running warmup runs.";
  CAFFE_ENFORCE(
      warmup_runs >= 0,
      "Number of warm up runs should be non negative, provided ",
      warmup_runs,
      ".");
  for (int i = 0; i < warmup_runs; ++i) {
    CAFFE_ENFORCE(Run(), "Warmup run ", i, " has failed.");
  }

  LOG(INFO) << "Main runs.";
  CAFFE_ENFORCE(
      main_runs >= 0,
      "Number of main runs should be non negative, provided ",
      main_runs,
      ".");
  Timer timer;
  for (int i = 0; i < main_runs; ++i) {
    CAFFE_ENFORCE(Run(), "Main run ", i, " has failed.");
  }
  auto millis = timer.MilliSeconds();
  LOG(INFO) << "Main run finished. Milliseconds per iter: "
                 << millis / main_runs
                 << ". Iters per second: " << 1000.0 * main_runs / millis;

  if (run_individual) {
    LOG(INFO) << "DAGNet does not do per-op benchmark. To do so, "
                      "switch to a simple net type.";
  }
  return vector<float>{millis / main_runs};
}

class DAGNet : public DAGNetBase {
 public:
  using DAGNetBase::DAGNetBase;

 protected:
  bool RunAt(const std::vector<int>& chain) override {
    bool success = true;
    for (const auto idx : chain) {
      success &= operator_nodes_[idx].operator_->Run();
    }
    return success;
  }
};

namespace {

REGISTER_NET(simple, SimpleNet);
REGISTER_NET(dag, DAGNet);

}

}  // namespace caffe2
