#include <map>
#include <iostream>
#include <chrono>
#include <random>
#include <algorithm>

#include <gutter_tree.h>
#include <standalone_gutters.h>
#include "../include/graph.h"
#include "../include/graph_worker.h"

// static variable for enforcing that only one graph is open at a time
bool Graph::open_graph = false;

Graph::Graph(node_id_t num_nodes, int num_inserters): num_nodes(num_nodes) {
  if (open_graph) throw MultipleGraphsException();

#ifdef VERIFY_SAMPLES_F
  std::cout << "Verifying samples..." << std::endl;
#endif
  Supernode::configure(num_nodes);
  representatives = new std::set<node_id_t>();
  supernodes = new Supernode*[num_nodes];
  parent = new node_id_t[num_nodes];
  size = new node_id_t[num_nodes];
  seed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
  std::mt19937_64 r(seed);
  seed = r();

  std::fill(size, size + num_nodes, 1);
  for (node_id_t i = 0; i < num_nodes; ++i) {
    representatives->insert(i);
    supernodes[i] = Supernode::makeSupernode(num_nodes,seed);
    parent[i] = i;
  }
  num_updates = 0; // REMOVE this later
  
  std::tuple<bool, bool, std::string> conf = configure_system(); // read the configuration file to configure the system
  copy_in_mem = std::get<1>(conf);
  std::string disk_loc = std::get<2>(conf);
  backup_file = disk_loc + "supernode_backup.data";
  // Create the buffering system and start the graphWorkers
  if (std::get<0>(conf))
    gts = new GutterTree(disk_loc, num_nodes, GraphWorker::get_num_groups(), true);
  else
    gts = new StandAloneGutters(num_nodes, GraphWorker::get_num_groups(), num_inserters);

  GraphWorker::start_workers(this, gts, Supernode::get_size());
  open_graph = true;
}

Graph::Graph(const std::string& input_file, int num_inserters) : num_updates(0) {
  if (open_graph) throw MultipleGraphsException();
  
  vec_t sketch_fail_factor;
  auto binary_in = std::fstream(input_file, std::ios::in | std::ios::binary);
  binary_in.read((char*)&seed, sizeof(seed));
  binary_in.read((char*)&num_nodes, sizeof(num_nodes));
  binary_in.read((char*)&sketch_fail_factor, sizeof(sketch_fail_factor));
  Supernode::configure(num_nodes, sketch_fail_factor);

#ifdef VERIFY_SAMPLES_F
  std::cout << "Verifying samples..." << std::endl;
#endif
  representatives = new std::set<node_id_t>();
  supernodes = new Supernode*[num_nodes];
  parent = new node_id_t[num_nodes];
  size = new node_id_t[num_nodes];
  std::fill(size, size+num_nodes, 1);
  for (node_id_t i = 0; i < num_nodes; ++i) {
    representatives->insert(i);
    supernodes[i] = Supernode::makeSupernode(num_nodes, seed, binary_in);
    parent[i] = i;
  }
  binary_in.close();

  std::tuple<bool, bool, std::string> conf = configure_system(); // read the configuration file to configure the system
  copy_in_mem = std::get<1>(conf);
  std::string disk_loc = std::get<2>(conf);
  backup_file = disk_loc + "supernode_backup.data";
  // Create the buffering system and start the graphWorkers
  if (std::get<0>(conf))
    gts = new GutterTree(disk_loc, num_nodes, GraphWorker::get_num_groups(), true);
  else
    gts = new StandAloneGutters(num_nodes, GraphWorker::get_num_groups(), num_inserters);

  GraphWorker::start_workers(this, gts, Supernode::get_size());
  open_graph = true;
}

Graph::~Graph() {
  for (unsigned i=0;i<num_nodes;++i)
    free(supernodes[i]); // free because memory is malloc'd in make_supernode
  delete[] supernodes;
  delete[] parent;
  delete[] size;
  delete representatives;
  GraphWorker::stop_workers(); // join the worker threads
  delete gts;
  open_graph = false;
}

void Graph::generate_delta_node(node_id_t node_n, uint64_t node_seed, node_id_t
               src, const std::vector<node_id_t> &edges, Supernode *delta_loc) {
  std::vector<vec_t> updates;
  updates.reserve(edges.size());
  for (const auto& edge : edges) {
    if (src < edge) {
      updates.push_back(static_cast<vec_t>(
                            nondirectional_non_self_edge_pairing_fn(src, edge)));
    } else {
      updates.push_back(static_cast<vec_t>(
                            nondirectional_non_self_edge_pairing_fn(edge, src)));
    }
  }
  Supernode::delta_supernode(node_n, node_seed, updates, delta_loc);
}
void Graph::batch_update(node_id_t src, const std::vector<node_id_t> &edges, Supernode *delta_loc) {
  if (update_locked) throw UpdateLockedException();

  num_updates += edges.size();
  generate_delta_node(supernodes[src]->n, supernodes[src]->seed, src, edges, delta_loc);
  supernodes[src]->apply_delta_update(delta_loc);
}

inline void Graph::sample_supernodes(std::pair<Edge, SampleSketchRet> *query,
               std::vector<node_id_t> &reps) {
  bool except = false;
  std::exception_ptr err;
  #pragma omp parallel for default(none) shared(query, reps, except, err)
  for (node_id_t i = 0; i < reps.size(); ++i) { // NOLINT(modernize-loop-convert)
    // wrap in a try/catch because exiting through exception is undefined behavior in OMP
    try {
      query[reps[i]] = supernodes[reps[i]]->sample();

    } catch (...) {
      except = true;
      err = std::current_exception();
    }
  }
  // Did one of our threads produce an exception?
  if (except) std::rethrow_exception(err);
}

inline std::vector<std::vector<node_id_t>> Graph::supernodes_to_merge(std::pair<Edge, SampleSketchRet>
               *query, std::vector<node_id_t> &reps) {
  std::vector<std::vector<node_id_t>> to_merge(num_nodes);
  std::vector<node_id_t> new_reps;
  for (auto i : reps) {
    // unpack query result
    Edge edge = query[i].first;
    SampleSketchRet ret_code = query[i].second;

    // try this query again next round as it failed this round
    if (ret_code == FAIL) {
      modified = true;
      new_reps.push_back(i);
      continue;
    }
    if (ret_code == ZERO) {
#ifdef VERIFY_SAMPLES_F
      verifier->verify_cc(i);
#endif
      continue;
    }

    // query dsu
    node_id_t a = get_parent(edge.first);
    node_id_t b = get_parent(edge.second);
    if (a == b) continue;

#ifdef VERIFY_SAMPLES_F
    verifier->verify_edge(edge);
#endif

    // make a the parent of b
    if (size[a] < size[b]) std::swap(a,b);
    parent[b] = a;
    size[a] += size[b];

    // add b and any of the nodes to merge with it to a's vector
    to_merge[a].push_back(b);
    to_merge[a].insert(to_merge[a].end(), to_merge[b].begin(), to_merge[b].end());
    to_merge[b].clear();
    modified = true;
  }

  // remove nodes added to new_reps due to sketch failures that
  // did end up being able to merge after all
  std::vector<node_id_t> temp_vec;
  for (node_id_t a : new_reps)
    if (to_merge[a].empty()) temp_vec.push_back(a);
  std::swap(new_reps, temp_vec);

  // add to new_reps all the nodes we will merge into
  for (node_id_t a = 0; a < num_nodes; a++)
    if (!to_merge[a].empty()) new_reps.push_back(a);

  reps = new_reps;
  return to_merge;
}

inline void Graph::merge_supernodes(Supernode** copy_supernodes, std::vector<node_id_t> &new_reps,
               std::vector<std::vector<node_id_t>> &to_merge, bool make_copy) {
  bool except = false;
  std::exception_ptr err;
  // loop over the to_merge vector and perform supernode merging
  #pragma omp parallel for default(shared)
  for (node_id_t i = 0; i < new_reps.size(); i++) { // NOLINT(modernize-loop-convert)
    // OMP requires a traditional for-loop to work
    node_id_t a = new_reps[i];
    try {
      if (make_copy && copy_in_mem) { // make a copy of a
        copy_supernodes[a] = Supernode::makeSupernode(*supernodes[a]);
      }

      // perform merging of nodes b into node a
      for (node_id_t b : to_merge[a]) {
        supernodes[a]->merge(*supernodes[b]);
      }
    } catch (...) {
      except = true;
      err = std::current_exception();
    }
  }

  // Did one of our threads produce an exception?
  if (except) std::rethrow_exception(err);
}

std::vector<std::set<node_id_t>> Graph::boruvka_emulation(bool make_copy) {
  printf("Total number of updates to sketches before CC %lu\n", num_updates.load()); // REMOVE this later
  update_locked = true; // disallow updating the graph after we run the alg

  cc_alg_start = std::chrono::steady_clock::now();
  bool first_round = true;
  Supernode** copy_supernodes;
  if (make_copy && copy_in_mem) 
    copy_supernodes = new Supernode*[num_nodes];
  std::pair<Edge, SampleSketchRet> query[num_nodes];
  std::vector<node_id_t> reps(num_nodes);
  std::vector<node_id_t> backed_up;
  std::fill(size, size + num_nodes, 1);
  for (node_id_t i = 0; i < num_nodes; ++i) {
    reps[i] = i;
    if (make_copy && copy_in_mem) 
      copy_supernodes[i] = nullptr;
  }

  // function to restore supernodes after CC if make_copy is specified
  auto cleanup_copy = [&make_copy, this, &backed_up, &copy_supernodes]() {
    if (make_copy) {
      if(copy_in_mem) {
        // restore original supernodes and free memory
        for (node_id_t i : backed_up) {
          if (supernodes[i] != nullptr) free(supernodes[i]);
          supernodes[i] = copy_supernodes[i];
        }
        delete[] copy_supernodes;
      } else {
        restore_from_disk(backed_up);
      }
    }
  };

  try {
    do {
      modified = false;
      sample_supernodes(query, reps);
      std::vector<std::vector<node_id_t>> to_merge = supernodes_to_merge(query, reps);
      // make a copy if necessary
      if (make_copy && first_round) {
        backed_up = reps;
        if (!copy_in_mem) backup_to_disk(backed_up);
      }

      merge_supernodes(copy_supernodes, reps, to_merge, first_round && make_copy);

#ifdef VERIFY_SAMPLES_F
      if (!first_round && fail_round_2) throw OutOfQueriesException();
#endif
      first_round = false;
    } while (modified);
  } catch (...) {
    cleanup_copy();
    std::rethrow_exception(std::current_exception());
  }

  // calculate connected components using DSU structure
  std::map<node_id_t, std::set<node_id_t>> temp;
  for (node_id_t i = 0; i < num_nodes; ++i)
    temp[get_parent(i)].insert(i);
  std::vector<std::set<node_id_t>> retval;
  retval.reserve(temp.size());
  for (const auto& it : temp) retval.push_back(it.second);

  cleanup_copy();

  cc_alg_end = std::chrono::steady_clock::now();
  return retval;
}

void Graph::backup_to_disk(const std::vector<node_id_t>& ids_to_backup) {
  // Make a copy on disk
  std::fstream binary_out(backup_file, std::ios::out | std::ios::binary);
  if (!binary_out.is_open()) {
    std::cerr << "Failed to open file for writing backup!" << backup_file << std::endl;
    exit(EXIT_FAILURE);
  }
  for (node_id_t idx : ids_to_backup) {
    supernodes[idx]->write_binary(binary_out);
  }
  binary_out.close();
}

// given a list of ids restore those supernodes from disk
// IMPORTANT: ids_to_restore must be the same as ids_to_backup
void Graph::restore_from_disk(const std::vector<node_id_t>& ids_to_restore) {
  // restore from disk
  std::fstream binary_in(backup_file, std::ios::in | std::ios::binary);
  if (!binary_in.is_open()) {
    std::cerr << "Failed to open file for reading backup!" << backup_file << std::endl;
    exit(EXIT_FAILURE);
  }
  for (node_id_t idx : ids_to_restore) {
    free(this->supernodes[idx]);
    this->supernodes[idx] = Supernode::makeSupernode(num_nodes, seed, binary_in);
  }
}

std::vector<std::set<node_id_t>> Graph::connected_components(bool cont) {
  flush_start = std::chrono::steady_clock::now();
  gts->force_flush(); // flush everything in guttering system to make final updates
  GraphWorker::pause_workers(); // wait for the workers to finish applying the updates
  flush_end = std::chrono::steady_clock::now();
  // after this point all updates have been processed from the buffer tree

  if (!cont)
    return boruvka_emulation(false); // merge in place
  
  // if backing up in memory then perform copying in boruvka
  bool except = false;
  std::exception_ptr err;
  std::vector<std::set<node_id_t>> ret;
  try {
    ret = boruvka_emulation(true);
  } catch (...) {
    except = true;
    err = std::current_exception();
  }

  // get ready for ingesting more from the stream
  // reset dsu and resume graph workers
  for (node_id_t i = 0; i < num_nodes; i++) {
    supernodes[i]->reset_query_state();
    parent[i] = i;
    size[i] = 1;
  }
  update_locked = false;
  GraphWorker::unpause_workers();

  // check if boruvka errored
  if (except) std::rethrow_exception(err);

  return ret;
}

node_id_t Graph::get_parent(node_id_t node) {
  if (parent[node] == node) return node;
  return parent[node] = get_parent(parent[node]);
}

void Graph::write_binary(const std::string& filename) {
  gts->force_flush(); // flush everything in buffering system to make final updates
  GraphWorker::pause_workers(); // wait for the workers to finish applying the updates
  // after this point all updates have been processed from the buffering system

  auto binary_out = std::fstream(filename, std::ios::out | std::ios::binary);
  auto fail_factor = Sketch::get_failure_factor();
  binary_out.write((char*)&seed, sizeof(seed));
  binary_out.write((char*)&num_nodes, sizeof(num_nodes));
  binary_out.write((char*)&fail_factor, sizeof(fail_factor));
  for (node_id_t i = 0; i < num_nodes; ++i) {
    supernodes[i]->write_binary(binary_out);
  }
  binary_out.close();
}
