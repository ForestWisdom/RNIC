#include "rnic/protocol.hpp"
#include "rnic/sha256.hpp"
#include "rnic/ucx_stream.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace rnic;

namespace {

struct Options {
  std::string input;
  std::string source = "file";
  uint64_t size = 0;
  std::string mode = "hybrid";
  uint32_t chunk_size = 16 * 1024 * 1024;
  uint32_t depth = 16;
  std::string verify = "chunk";
  std::string engine = "tag";
  bool register_buffers = false;
  std::string cpu_list;
  std::vector<LaneSpec> lanes;
  std::string results_json;
};

void usage(const char *argv0) {
  std::cerr
      << "Usage: " << argv0
      << " [--input FILE | --source zero --size BYTES] --mode MODE --lane "
      << lane_spec_help(true)
      << " [--lane ...] [--chunk-size BYTES] [--depth N]"
      << " [--verify none|chunk|file|both] [--engine tag|rma]"
      << " [--register-buffers] [--cpu-list LIST] [--results-json FILE]\n\n"
      << "Examples:\n"
      << "  " << argv0
      << " --input model.bin --mode nic-only "
      << "--lane nic,10.102.0.239,5001,tcp,eno1np0,1\n"
      << "  " << argv0
      << " --input model.bin --mode hybrid "
      << "--lane nic,10.102.0.239,5001,tcp,eno1np0,30 "
      << "--lane rdma,192.168.2.248,5002,rc_x,mlx5_0:1,70\n";
}

Options parse_args(int argc, char **argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string &name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + name);
      }
      return argv[++i];
    };
    if (arg == "--input") {
      opt.input = require_value(arg);
    } else if (arg == "--source") {
      opt.source = require_value(arg);
    } else if (arg == "--size") {
      opt.size = std::stoull(require_value(arg));
    } else if (arg == "--mode") {
      opt.mode = require_value(arg);
    } else if (arg == "--chunk-size") {
      opt.chunk_size = static_cast<uint32_t>(std::stoul(require_value(arg)));
    } else if (arg == "--depth") {
      opt.depth = static_cast<uint32_t>(std::stoul(require_value(arg)));
    } else if (arg == "--verify") {
      opt.verify = require_value(arg);
    } else if (arg == "--engine") {
      opt.engine = require_value(arg);
    } else if (arg == "--register-buffers") {
      opt.register_buffers = true;
    } else if (arg == "--cpu-list") {
      opt.cpu_list = require_value(arg);
    } else if (arg == "--lane") {
      opt.lanes.push_back(parse_lane_spec(require_value(arg), true));
    } else if (arg == "--results-json") {
      opt.results_json = require_value(arg);
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  if (opt.source != "file" && opt.source != "zero") {
    throw std::runtime_error("--source must be file or zero");
  }
  if (opt.source == "file" && opt.input.empty()) {
    throw std::runtime_error("--input is required");
  }
  if (opt.source == "zero" && opt.size == 0) {
    throw std::runtime_error("--size is required for --source zero");
  }
  if (opt.lanes.empty()) {
    throw std::runtime_error("at least one --lane is required");
  }
  if (opt.chunk_size == 0) {
    throw std::runtime_error("--chunk-size must be greater than zero");
  }
  if (opt.depth == 0) {
    throw std::runtime_error("--depth must be greater than zero");
  }
  if (opt.verify != "none" && opt.verify != "chunk" && opt.verify != "file" &&
      opt.verify != "both") {
    throw std::runtime_error("--verify must be none, chunk, file, or both");
  }
  if (opt.engine != "tag" && opt.engine != "rma") {
    throw std::runtime_error("--engine must be tag or rma");
  }
  if (opt.engine == "rma" && (opt.source != "zero" || opt.verify != "none")) {
    throw std::runtime_error("--engine rma currently requires --source zero --verify none");
  }
  return opt;
}

uint64_t file_size(const std::string &path) {
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) {
    throw std::runtime_error("failed to stat input file: " + path);
  }
  if (!S_ISREG(st.st_mode)) {
    throw std::runtime_error("input is not a regular file: " + path);
  }
  return static_cast<uint64_t>(st.st_size);
}

std::vector<std::vector<uint64_t>> assign_chunks(const std::vector<LaneSpec> &lanes,
                                                 uint64_t total_chunks) {
  std::vector<std::vector<uint64_t>> out(lanes.size());
  std::vector<int64_t> current(lanes.size(), 0);
  uint64_t total_weight = 0;
  for (const auto &lane : lanes) {
    total_weight += lane.weight;
  }

  for (uint64_t chunk = 0; chunk < total_chunks; ++chunk) {
    size_t best = 0;
    for (size_t i = 0; i < lanes.size(); ++i) {
      current[i] += lanes[i].weight;
      if (current[i] > current[best]) {
        best = i;
      }
    }
    out[best].push_back(chunk);
    current[best] -= static_cast<int64_t>(total_weight);
  }
  return out;
}

void write_json(const std::string &path, const TransferMeta &meta,
                const std::vector<LaneStats> &stats, double total_seconds,
                uint32_t depth, const std::string &verify,
                const std::string &source, const std::string &engine,
                bool register_buffers, const std::string &cpu_list) {
  if (path.empty()) {
    return;
  }
  FILE *fp = std::fopen(path.c_str(), "w");
  if (!fp) {
    throw std::runtime_error("failed to open results json: " + path);
  }
  double active_seconds = 0.0;
  for (const auto &s : stats) {
    active_seconds = std::max(active_seconds, s.seconds);
  }
  std::fprintf(fp,
               "{\n"
               "  \"timestamp\": \"%s\",\n"
               "  \"role\": \"sender\",\n"
               "  \"mode\": \"%s\",\n"
               "  \"file_size\": %llu,\n"
               "  \"chunk_size\": %u,\n"
               "  \"total_chunks\": %llu,\n"
               "  \"sha256\": \"%s\",\n"
               "  \"depth\": %u,\n"
               "  \"verify\": \"%s\",\n"
               "  \"source\": \"%s\",\n"
               "  \"engine\": \"%s\",\n"
               "  \"register_buffers\": %s,\n"
               "  \"cpu_list\": \"%s\",\n"
               "  \"total_seconds\": %.6f,\n"
               "  \"throughput_mib_s\": %.3f,\n"
               "  \"active_seconds\": %.6f,\n"
               "  \"active_throughput_mib_s\": %.3f,\n"
               "  \"lanes\": [\n",
               now_timestamp().c_str(), json_escape(meta.mode).c_str(),
               static_cast<unsigned long long>(meta.file_size), meta.chunk_size,
               static_cast<unsigned long long>(meta.total_chunks),
               meta.sha256_hex.c_str(), depth, json_escape(verify).c_str(),
               json_escape(source).c_str(), json_escape(engine).c_str(),
               register_buffers ? "true" : "false", json_escape(cpu_list).c_str(),
               total_seconds,
               (meta.file_size / 1048576.0) / total_seconds, active_seconds,
               active_seconds > 0.0 ? (meta.file_size / 1048576.0) / active_seconds
                                    : 0.0);
  for (size_t i = 0; i < stats.size(); ++i) {
    const auto &s = stats[i];
    std::fprintf(fp,
                 "    {\"name\": \"%s\", \"chunks\": %llu, \"bytes\": %llu, "
                 "\"seconds\": %.6f, \"throughput_mib_s\": %.3f}%s\n",
                 json_escape(s.name).c_str(), static_cast<unsigned long long>(s.chunks),
                 static_cast<unsigned long long>(s.bytes), s.seconds,
                 s.seconds > 0.0 ? (s.bytes / 1048576.0) / s.seconds : 0.0,
                 i + 1 == stats.size() ? "" : ",");
  }
  std::fprintf(fp, "  ]\n}\n");
  std::fclose(fp);
}

struct SendSlot {
  std::vector<uint8_t> buffer;
  ucp_mem_h memh = nullptr;
  void *request = nullptr;
  uint64_t bytes = 0;
};

std::vector<int> parse_cpu_list(const std::string &value) {
  std::vector<int> cpus;
  if (value.empty()) {
    return cpus;
  }
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    const auto dash = item.find('-');
    if (dash == std::string::npos) {
      cpus.push_back(std::stoi(item));
    } else {
      const int begin = std::stoi(item.substr(0, dash));
      const int end = std::stoi(item.substr(dash + 1));
      for (int cpu = begin; cpu <= end; ++cpu) {
        cpus.push_back(cpu);
      }
    }
  }
  return cpus;
}

void bind_thread_to_cpu(const std::vector<int> &cpus, size_t lane_index) {
  if (cpus.empty()) {
    return;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpus[lane_index % cpus.size()], &set);
  const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  if (rc != 0) {
    throw std::runtime_error("pthread_setaffinity_np failed");
  }
}

void wait_for_one(UcxStreamLane &lane, std::vector<SendSlot> &slots) {
  while (true) {
    for (auto &slot : slots) {
      if (slot.request != nullptr && lane.test_send(slot.request)) {
        slot.request = nullptr;
        return;
      }
    }
    lane.progress();
  }
}

void wait_for_all(UcxStreamLane &lane, std::vector<SendSlot> &slots) {
  bool any = true;
  while (any) {
    any = false;
    for (auto &slot : slots) {
      if (slot.request != nullptr) {
        any = true;
        if (lane.test_send(slot.request)) {
          slot.request = nullptr;
        }
      }
    }
    if (any) {
      lane.progress();
    }
  }
}

SendSlot &free_slot(UcxStreamLane &lane, std::vector<SendSlot> &slots) {
  while (true) {
    for (auto &slot : slots) {
      if (slot.request == nullptr) {
        return slot;
      }
    }
    wait_for_one(lane, slots);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options opt = parse_args(argc, argv);
    const uint64_t size = opt.source == "file" ? file_size(opt.input) : opt.size;
    TransferMeta meta;
    meta.file_size = size;
    meta.chunk_size = opt.chunk_size;
    meta.total_chunks = (size + opt.chunk_size - 1) / opt.chunk_size;
    meta.mode = opt.mode;

    const bool chunk_verify = opt.verify == "chunk" || opt.verify == "both";
    const bool file_verify = opt.verify == "file" || opt.verify == "both";
    if (file_verify && opt.source == "file") {
      std::cerr << "Computing SHA256 for " << opt.input << "...\n";
      meta.sha256_hex = sha256_file_hex(opt.input);
      std::cerr << "SHA256: " << meta.sha256_hex << "\n";
    } else if (file_verify && opt.source != "file") {
      throw std::runtime_error("--verify file/both requires --source file");
    }

    const int fd = opt.source == "file" ? open(opt.input.c_str(), O_RDONLY) : -1;
    if (opt.source == "file" && fd < 0) {
      throw std::runtime_error("failed to open input file: " + opt.input);
    }

    const auto assignments = assign_chunks(opt.lanes, meta.total_chunks);
    const auto cpus = parse_cpu_list(opt.cpu_list);
    std::vector<LaneStats> stats(opt.lanes.size());
    std::atomic<bool> failed{false};
    std::vector<std::string> errors(opt.lanes.size());
    std::vector<std::thread> threads;

    const auto total_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < opt.lanes.size(); ++i) {
      threads.emplace_back([&, i]() {
        try {
          bind_thread_to_cpu(cpus, i);
          stats[i].name = opt.lanes[i].name;
          UcxStreamLane lane(opt.lanes[i], false);
          lane.connect();

          FrameHeader meta_frame = make_frame(FrameType::Meta, meta, opt.lanes[i].name);
          meta_frame.checksum = assignments[i].size();
          lane.send_all(&meta_frame, sizeof(meta_frame));

          std::vector<SendSlot> slots(opt.depth);
          for (auto &slot : slots) {
            slot.buffer.resize(sizeof(FrameHeader) + meta.chunk_size);
            if (opt.register_buffers) {
              slot.memh = lane.map_memory(slot.buffer.data(), slot.buffer.size());
            }
          }
          const auto lane_start = std::chrono::steady_clock::now();
          ucp_rkey_h rkey = nullptr;
          RmaRegionInfo rma_info{};
          std::vector<uint8_t> rkey_buffer;
          if (opt.engine == "rma") {
            lane.recv_all(&rma_info, sizeof(rma_info));
            rkey_buffer.resize(rma_info.rkey_length);
            lane.recv_all(rkey_buffer.data(), rkey_buffer.size());
            rkey = lane.unpack_rkey(rkey_buffer.data());
          }
          for (uint64_t chunk_id : assignments[i]) {
            SendSlot &slot = free_slot(lane, slots);
            const uint64_t offset = chunk_id * meta.chunk_size;
            const uint32_t length =
                static_cast<uint32_t>(std::min<uint64_t>(meta.chunk_size,
                                                         meta.file_size - offset));
            auto *header = reinterpret_cast<FrameHeader *>(slot.buffer.data());
            uint8_t *payload = slot.buffer.data() + sizeof(FrameHeader);
            if (opt.source == "file") {
              ssize_t n = pread(fd, payload, length, static_cast<off_t>(offset));
              if (n != static_cast<ssize_t>(length)) {
                throw std::runtime_error("pread failed for chunk " +
                                         std::to_string(chunk_id));
              }
            }

            if (opt.engine == "tag") {
              *header = make_frame(FrameType::Data, meta, opt.lanes[i].name);
              header->chunk_id = chunk_id;
              header->offset = offset;
              header->length = length;
              header->checksum = chunk_verify ? fnv1a64(payload, length) : 0;
              const size_t frame_len = sizeof(FrameHeader) + length;
              slot.request = lane.post_tag_send(slot.buffer.data(), frame_len,
                                                static_cast<ucp_tag_t>(chunk_id + 1),
                                                slot.memh);
            } else {
              const uint64_t remote_addr =
                  rma_info.address + (chunk_id % opt.depth) * meta.chunk_size;
              slot.request = lane.post_put(payload, length, remote_addr, rkey,
                                           slot.memh);
            }
            slot.bytes = length;
            stats[i].chunks += 1;
            stats[i].bytes += length;
          }
          wait_for_all(lane, slots);
          if (rkey != nullptr) {
            lane.destroy_rkey(rkey);
          }

          FrameHeader done = make_frame(FrameType::Done, meta, opt.lanes[i].name);
          lane.send_all(&done, sizeof(done));
          const auto lane_end = std::chrono::steady_clock::now();
          stats[i].seconds =
              std::chrono::duration<double>(lane_end - lane_start).count();
          for (auto &slot : slots) {
            lane.unmap_memory(slot.memh);
            slot.memh = nullptr;
          }
        } catch (const std::exception &ex) {
          errors[i] = ex.what();
          failed = true;
        }
      });
    }

    for (auto &thread : threads) {
      thread.join();
    }
    if (fd >= 0) {
      close(fd);
    }
    if (failed) {
      for (const auto &error : errors) {
        if (!error.empty()) {
          std::cerr << "lane error: " << error << "\n";
        }
      }
      return 2;
    }

    const auto total_end = std::chrono::steady_clock::now();
    const double total_seconds =
        std::chrono::duration<double>(total_end - total_start).count();
    write_json(opt.results_json, meta, stats, total_seconds, opt.depth, opt.verify,
               opt.source, opt.engine, opt.register_buffers, opt.cpu_list);
    double active_seconds = 0.0;
    for (const auto &s : stats) {
      active_seconds = std::max(active_seconds, s.seconds);
    }

    std::cout << "sent file_size=" << meta.file_size
              << " chunks=" << meta.total_chunks
              << " seconds=" << total_seconds
              << " throughput_mib_s=" << (meta.file_size / 1048576.0) / total_seconds
              << " active_seconds=" << active_seconds
              << " active_throughput_mib_s="
              << (active_seconds > 0.0 ? (meta.file_size / 1048576.0) / active_seconds
                                       : 0.0)
              << "\n";
    for (const auto &s : stats) {
      std::cout << "lane=" << s.name << " chunks=" << s.chunks
                << " bytes=" << s.bytes << " seconds=" << s.seconds << "\n";
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "rnic_send: " << ex.what() << "\n";
    usage(argv[0]);
    return 1;
  }
}
