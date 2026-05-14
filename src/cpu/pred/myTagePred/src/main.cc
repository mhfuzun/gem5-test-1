#include "tage.h"

#include "json.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using json = nlohmann::json;

#ifdef DEBUG
#define LOG(x)                                                                \
  do {                                                                        \
    std::cerr << "[DEBUG] " << x << std::endl;                                \
  } while (0)
#else
#define LOG(x)                                                                \
  do {                                                                        \
  } while (0)
#endif

namespace app {
struct Config
{
  string input_file = "pc_file.log";
  string cfg_file = "tage.json";
  int core_id = -1;          // -1 => all cores
  std::size_t max_count = 0; // 0 => no limit
};

bool parse_args(int argc, char **argv, Config &cfg) {
  // Optional args:
  //   1) input_file (default: pc_file.log)
  //   2) cfg_file   (default: tage.json)
  //   3) core_id    (default: -1, all cores)
  //   4) max_count  (default: 0, no limit)
  if (argc >= 2)
    cfg.input_file = argv[1];
  if (argc >= 3)
    cfg.cfg_file = argv[2];
  if (argc >= 4)
    cfg.core_id = std::atoi(argv[3]);
  if (argc >= 5)
    cfg.max_count =
        static_cast<std::size_t>(std::strtoull(argv[4], nullptr, 10));

  if (argc > 5) {
    cerr << "Usage: " << argv[0]
         << " [input_file] [cfg_file] [core_id] [max_count]\n";
    return false;
  }
  return true;
}
} // namespace app

[[maybe_unused]] static tage_cfg_t make_cfg(int addrWidth, int comp_count,
                                            int depth, int history_width[],
                                            int tag_width[]) {
  tage_cfg_t cfg{};
  (void)addrWidth;

  cfg.bimodal_depth = 1024;
  cfg.ghistory_length = 128;
  cfg.comp_count = comp_count;
  const int idx_width = idxWidthForDepth(static_cast<std::uint64_t>(depth));
  cfg.pc_hash_start_for_idx = 2; // like bimodal
  cfg.pc_hash_width_for_idx = idx_width;
  cfg.pc_hash_start_for_tag = 10;
  cfg.pc_hash_width_for_tag = 12;
  cfg.table_cfg.resize(comp_count);
  for (int i = 0; i < comp_count; ++i) {
    cfg.table_cfg[i].depth = depth;
    cfg.table_cfg[i].usefull_width = 2;
    cfg.table_cfg[i].ctr_width = 3;
    cfg.table_cfg[i].tag_width = tag_width[i];
    cfg.table_cfg[i].history_width = history_width[i];
  }
  return cfg;
}

static tage_cfg_t read_cfg(string jsonfile) {
  std::ifstream fin(jsonfile);
  if (!fin) {
    throw std::runtime_error("cannot open config file: " + jsonfile);
  }

  json root;
  fin >> root;

  auto require_int = [&](const json &node, const char *key) -> int {
    if (!node.contains(key)) {
      throw std::runtime_error("missing required integer field: " +
                               string(key));
    }
    return node.at(key).get<int>();
  };

  tage_cfg_t cfg{};
  cfg.bimodal_depth = require_int(root, "bimodal_depth");
  cfg.ghistory_length = require_int(root, "ghistory_length");
  cfg.pchistory_length = require_int(root, "pchistory_length");
  cfg.pc_hash_start_for_idx = require_int(root, "pc_hash_start_for_idx");
  cfg.pc_hash_width_for_idx = require_int(root, "pc_hash_width_for_idx");
  cfg.pc_hash_start_for_tag = require_int(root, "pc_hash_start_for_tag");
  cfg.pc_hash_width_for_tag = require_int(root, "pc_hash_width_for_tag");

  cfg.allocate_randomplacement = require_int(root, "allocate_randomplacement");
  cfg.allocate_longerThanProvider =
      require_int(root, "allocate_longerThanProvider");
  cfg.periodicreset = require_int(root, "periodicreset");
  cfg.periodicreset_branchperiod =
      require_int(root, "periodicreset_branchperiod");

  if (root.contains("random_type")) {
    const string random_type = root.at("random_type").get<string>();
    if (random_type == "SIMRAND") {
      cfg.random_type = SIMRAND;
    } else if (random_type == "LFSR") {
      cfg.random_type = LFSR;
    } else {
      throw std::runtime_error("random_type must be SIMRAND or LFSR");
    }
  }
  cfg.lfsr_width = require_int(root, "lfsr_width");
  cfg.lfsr_seed = static_cast<std::uint64_t>(
      std::stoi(root.at("lfsr_seed").get<string>(), 0, 16));
  cfg.lfsr_misprediction_update =
      require_int(root, "lfsr_misprediction_update");

  if (!root.contains("table_cfg") || !root.at("table_cfg").is_array()) {
    throw std::runtime_error("missing required array field: table_cfg");
  }

  const auto &tables = root.at("table_cfg");
  cfg.table_cfg.reserve(tables.size());
  for (std::size_t i = 0; i < tables.size(); ++i) {
    const auto &table = tables.at(i);
    table_cfg_t table_cfg{};
    table_cfg.depth = require_int(table, "depth");
    table_cfg.usefull_width = require_int(table, "usefull_width");
    table_cfg.ctr_width = require_int(table, "ctr_width");
    table_cfg.tag_width = require_int(table, "tag_width");
    table_cfg.history_width = require_int(table, "history_width");
    table_cfg.pchistory_start = require_int(table, "pchistory_start");
    table_cfg.pchistory_width = require_int(table, "pchistory_width");
    cfg.table_cfg.push_back(table_cfg);
  }

  cfg.comp_count = static_cast<int>(cfg.table_cfg.size());
  if (root.contains("comp_count")) {
    const int comp_count = root.at("comp_count").get<int>();
    if (comp_count != cfg.comp_count) {
      throw std::runtime_error(
          "comp_count does not match table_cfg size in config file: " +
          jsonfile);
    }
  }

  return cfg;
}

struct trace_entry_t
{
  int coreid;
  std::uint64_t addr;
  bool res;
  std::uint64_t targetAddr;
};

/**
    $ head -n4 pc_file.log
    core   0: 0x0000000080001050 0 0x000000008000105c
    core   0: 0x000000008000267c 0 0x00000000800027f8
    core   0: 0x00000000800027c0 0 0x00000000800026a4
    core   0: 0x00000000800027f0 0 0x00000000800027c8
*/
static std::vector<trace_entry_t>
getTraceList(const string &file_path, int core_id, std::size_t max_count) {
  std::ifstream fin(file_path);
  if (!fin) {
    throw std::runtime_error("cannot open trace file: " + file_path);
  }

  // Large buffer improves throughput for multi-GB files.
  static std::vector<char> io_buffer(4 * 1024 * 1024);
  fin.rdbuf()->pubsetbuf(io_buffer.data(),
                         static_cast<std::streamsize>(io_buffer.size()));

  std::vector<trace_entry_t> traces;
  if (max_count > 0)
    traces.reserve(max_count);
  else
    traces.reserve(1u << 20);

  string line;
  std::size_t line_no = 0;
  while (std::getline(fin, line)) {
    ++line_no;

    int parsed_core = -1;
    int outcome = 0;
    unsigned long long addr = 0;
    unsigned long long target = 0;

    // Example:
    // core   0: 0x0000000080001050 0 0x000000008000105c
    const int parsed = std::sscanf(line.c_str(), "core %d: 0x%llx %d 0x%llx",
                                   &parsed_core, &addr, &outcome, &target);

    if (parsed != 4)
      continue;
    if (core_id >= 0 && parsed_core != core_id)
      continue;

    traces.push_back(trace_entry_t{
        parsed_core,
        static_cast<std::uint64_t>(addr),
        outcome != 0,
        static_cast<std::uint64_t>(target),
    });

    if (max_count > 0 && traces.size() >= max_count)
      break;

    if ((traces.size() % 1'000'000u) == 0u)
      LOG("loaded " << traces.size() << " entries");
  }

  LOG("trace parse done, lines=" << line_no << ", entries=" << traces.size());
  return traces;
}

static int trace_loop(const app::Config &config) {
  /*
  int addrWidth = 32;
  int componentCount = 4;
  int depth = 1024;
  int history_width[] = {5, 15, 44, 130};
  int tag_width[] = {8, 8, 9, 9};

  auto cfg = make_cfg(
      addrWidth,
      componentCount,
      depth,
      history_width,
      tag_width
  );
  */
  auto cfg = read_cfg(config.cfg_file);
  cfg.printSummary();

  const auto traces =
      getTraceList(config.input_file, config.core_id, config.max_count);
  if (traces.empty()) {
    cerr << "No trace entries found in '" << config.input_file << "'.\n";
    return EXIT_FAILURE;
  }

  tage predictor(cfg);

  std::uint64_t total = 0;
  std::uint64_t correct = 0;
  std::uint64_t taken = 0;
  std::uint64_t not_taken = 0;

  // start_timer()
  auto start = std::chrono::high_resolution_clock::now();

  for (const auto &entry : traces) {
    const bool pred = predictor.getPrediction(entry.addr);
    if (pred == entry.res)
      ++correct;

    predictor.update(entry.addr, entry.res);

    ++total;
    if (entry.res)
      ++taken;
    else
      ++not_taken;
  }

  // stop_timer()
  auto end = std::chrono::high_resolution_clock::now();

  predictor.DisplayStatistics();

  const double acc =
      (total == 0)
          ? 0.0
          : (100.0 * static_cast<double>(correct) /
             static_cast<double>(total));
  cout << "\n";
  cout << "Results: \n";
  cout << "Trace file      : " << config.input_file << '\n';
  cout << "Config file     : " << config.cfg_file << '\n';
  cout << "Core filter     : " << config.core_id << '\n';
  cout << "Loaded entries  : " << traces.size() << '\n';
  cout << "Processed       : " << total << '\n';
  cout << "Correct         : " << correct << '\n';
  cout << "Wrong           : " << (total - correct) << '\n';
  cout << "Taken/Not-taken : " << taken << "/" << not_taken << '\n';
  cout << "Accuracy        : " << acc << "%\n";
  cout << "misp/KI         : " << (double)(total - correct) / (total / 1000.0)
       << " misp/KI\n";

  // display_time_duration()
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  std::chrono::hh_mm_ss time_parts{duration};
  std::cout << "\nElapsed time: ";
  if (time_parts.minutes().count() > 0) {
    std::cout << time_parts.minutes().count() << " min ";
  }
  std::cout << time_parts.seconds().count() << " sec\n";

  return EXIT_SUCCESS;
}

int main(int argc, char **argv) {

  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  app::Config config;

  if (!app::parse_args(argc, argv, config))
    return EXIT_FAILURE;

  try {
    return trace_loop(config);
  } catch (const std::exception &e) {
    cerr << "Error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
