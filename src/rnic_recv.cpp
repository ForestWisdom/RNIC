#include "rnic/protocol.hpp"
#include "rnic/sha256.hpp"
#include "rnic/ucx_stream.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace rnic;

namespace {

struct Options {
  std::string output;
  std::vector<LaneSpec> lanes;
  std::string results_json;
  bool sink = false;
  uint32_t depth = 16;
  std::string verify = "chunk";
  std::string engine = "tag";
  bool register_buffers = false;
  std::string cpu_list;
};

struct SharedState {
  std::mutex mutex;
  TransferMeta meta;
  bool meta_ready = false;
  int fd = -1;
  std::string output;
  std::atomic<uint64_t> received_chunks{0};
  std::atomic<uint64_t> received_bytes{0};
};

void usage(const char *argv0) {
  std::cerr
      << "Usage: " << argv0 << " --output FILE --lane " << lane_spec_help(false)
      << " [--lane ...] [--results-json FILE] [--sink]"
      << " [--depth N] [--verify none|chunk|file|both]"
      << " [--engine tag|rma] [--register-buffers] [--cpu-list LIST]\n\n"
      << "Examples:\n"
      << "  " << argv0
      << " --output model.bin --lane nic,10.102.0.239,5001,tcp,ens11\n"
      << "  " << argv0
      << " --output model.bin --lane nic,10.102.0.239,5001,tcp,ens11 "
      << "--lane rdma,192.168.2.248,5002,rc_x,mlx5_0:1\n";
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
    if (arg == "--output") {
      opt.output = require_value(arg);
    } else if (arg == "--lane") {
      opt.lanes.push_back(parse_lane_spec(require_value(arg), false));
    } else if (arg == "--results-json") {
      opt.results_json = require_value(arg);
    } else if (arg == "--sink") {
      opt.sink = true;
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
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  if (opt.output.empty() && !opt.sink) {
    throw std::runtime_error("--output is required");
  }
  if (opt.lanes.empty()) {
    throw std::runtime_error("at least one --lane is required");
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
  if (opt.engine == "rma" && (!opt.sink || opt.verify != "none")) {
    throw std::runtime_error("--engine rma currently requires --sink --verify none");
  }
  return opt;
}

TransferMeta meta_from_header(const FrameHeader &header) {
  TransferMeta meta;
  meta.file_size = header.file_size;
  meta.total_chunks = header.total_chunks;
  meta.chunk_size = header.chunk_size;
  meta.sha256_hex = header.sha256_hex;
  meta.mode = header.mode;
  return meta;
}

void apply_meta(SharedState &state, const FrameHeader &header, bool sink) {
  validate_frame_header(header);
  if (header.type != static_cast<uint16_t>(FrameType::Meta)) {
    throw std::runtime_error("first frame on each lane must be metadata");
  }
  std::lock_guard<std::mutex> lock(state.mutex);
  const TransferMeta incoming = meta_from_header(header);
  if (!state.meta_ready) {
    state.meta = incoming;
    if (!sink) {
      state.fd = open(state.output.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (state.fd < 0) {
        throw std::runtime_error("failed to open output file: " + state.output);
      }
      if (ftruncate(state.fd, static_cast<off_t>(state.meta.file_size)) != 0) {
        throw std::runtime_error("failed to preallocate output file");
      }
    }
    state.meta_ready = true;
    return;
  }

  if (incoming.file_size != state.meta.file_size ||
      incoming.total_chunks != state.meta.total_chunks ||
      incoming.chunk_size != state.meta.chunk_size ||
      incoming.sha256_hex != state.meta.sha256_hex ||
      incoming.mode != state.meta.mode) {
    throw std::runtime_error("lane metadata does not match previous lanes");
  }
}

void write_json(const std::string &path, const TransferMeta &meta,
                const std::vector<LaneStats> &stats, double total_seconds,
                const std::string &actual_sha, bool ok, bool sink, uint32_t depth,
                const std::string &verify, const std::string &engine,
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
               "  \"role\": \"receiver\",\n"
               "  \"mode\": \"%s\",\n"
               "  \"file_size\": %llu,\n"
               "  \"chunk_size\": %u,\n"
               "  \"total_chunks\": %llu,\n"
               "  \"expected_sha256\": \"%s\",\n"
               "  \"actual_sha256\": \"%s\",\n"
               "  \"sha256_ok\": %s,\n"
               "  \"sink\": %s,\n"
               "  \"depth\": %u,\n"
               "  \"verify\": \"%s\",\n"
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
               meta.sha256_hex.c_str(), actual_sha.c_str(), ok ? "true" : "false",
               sink ? "true" : "false", depth, json_escape(verify).c_str(),
               json_escape(engine).c_str(), register_buffers ? "true" : "false",
               json_escape(cpu_list).c_str(),
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

struct RecvSlot {
  std::vector<uint8_t> buffer;
  ucp_mem_h memh = nullptr;
  void *request = nullptr;
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

bool process_slot(RecvSlot &slot, const UcxRecvResult &result, SharedState &state,
                  LaneStats &stats, bool sink, bool chunk_verify) {
  if (result.length < sizeof(FrameHeader)) {
    throw std::runtime_error("received tag frame smaller than header");
  }
  const auto *header = reinterpret_cast<const FrameHeader *>(slot.buffer.data());
  validate_frame_header(*header);
  if (static_cast<FrameType>(header->type) != FrameType::Data) {
    throw std::runtime_error("received non-data tag frame");
  }
  if (result.tag != static_cast<ucp_tag_t>(header->chunk_id + 1)) {
    throw std::runtime_error("tag does not match chunk id");
  }
  if (result.length != sizeof(FrameHeader) + header->length) {
    throw std::runtime_error("tag frame length does not match header length");
  }
  if (header->length > state.meta.chunk_size) {
    throw std::runtime_error("received chunk larger than configured chunk size");
  }
  if (header->offset + header->length > state.meta.file_size) {
    throw std::runtime_error("received chunk outside file bounds");
  }

  const uint8_t *payload = slot.buffer.data() + sizeof(FrameHeader);
  if (chunk_verify && fnv1a64(payload, header->length) != header->checksum) {
    throw std::runtime_error("chunk checksum mismatch at chunk " +
                             std::to_string(header->chunk_id));
  }
  if (!sink) {
    ssize_t n = pwrite(state.fd, payload, header->length,
                       static_cast<off_t>(header->offset));
    if (n != static_cast<ssize_t>(header->length)) {
      throw std::runtime_error("pwrite failed for chunk " +
                               std::to_string(header->chunk_id));
    }
  }
  state.received_chunks += 1;
  state.received_bytes += header->length;
  stats.chunks += 1;
  stats.bytes += header->length;
  return true;
}

bool post_recv(UcxStreamLane &lane, RecvSlot &slot, size_t frame_size,
               UcxRecvResult &immediate) {
  slot.request = lane.post_tag_recv(slot.buffer.data(), frame_size, 0, 0, &immediate,
                                    slot.memh);
  return slot.request == nullptr;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options opt = parse_args(argc, argv);
    SharedState state;
    state.output = opt.output;
    const bool chunk_verify = opt.verify == "chunk" || opt.verify == "both";
    const bool file_verify = opt.verify == "file" || opt.verify == "both";
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
          UcxStreamLane lane(opt.lanes[i], true);
          lane.connect();

          FrameHeader meta_frame{};
          lane.recv_all(&meta_frame, sizeof(meta_frame));
          apply_meta(state, meta_frame, opt.sink);
          const uint64_t lane_expected_chunks = meta_frame.checksum;

          std::vector<RecvSlot> slots(opt.depth);
          const size_t frame_size = sizeof(FrameHeader) + state.meta.chunk_size;
          for (auto &slot : slots) {
            slot.buffer.resize(frame_size);
            if (opt.register_buffers) {
              slot.memh = lane.map_memory(slot.buffer.data(), slot.buffer.size());
            }
          }
          const auto lane_start = std::chrono::steady_clock::now();
          if (opt.engine == "rma") {
            std::vector<uint8_t> rma_buffer(opt.depth * state.meta.chunk_size);
            ucp_mem_h rma_memh = lane.map_memory(rma_buffer.data(), rma_buffer.size());
            size_t packed_rkey_len = 0;
            void *packed_rkey = lane.pack_rkey(rma_memh, packed_rkey_len);
            RmaRegionInfo info{};
            info.address = reinterpret_cast<uint64_t>(rma_buffer.data());
            info.length = rma_buffer.size();
            info.rkey_length = packed_rkey_len;
            lane.send_all(&info, sizeof(info));
            lane.send_all(packed_rkey, packed_rkey_len);

            FrameHeader done{};
            lane.recv_all(&done, sizeof(done));
            validate_frame_header(done);
            if (static_cast<FrameType>(done.type) != FrameType::Done) {
              throw std::runtime_error("expected done frame after rma data");
            }
            lane.release_packed_rkey(packed_rkey);
            lane.unmap_memory(rma_memh);
            stats[i].chunks = lane_expected_chunks;
            stats[i].bytes = lane_expected_chunks * state.meta.chunk_size;
            if (stats[i].bytes > state.meta.file_size) {
              stats[i].bytes = state.meta.file_size;
            }
            state.received_chunks += lane_expected_chunks;
            state.received_bytes += stats[i].bytes;
            const auto lane_end = std::chrono::steady_clock::now();
            stats[i].seconds =
                std::chrono::duration<double>(lane_end - lane_start).count();
            for (auto &slot : slots) {
              lane.unmap_memory(slot.memh);
              slot.memh = nullptr;
            }
            return;
          }
          uint64_t posted = 0;
          uint64_t completed = 0;
          for (auto &slot : slots) {
            if (posted < lane_expected_chunks) {
              UcxRecvResult immediate{};
              if (post_recv(lane, slot, frame_size, immediate)) {
                process_slot(slot, immediate, state, stats[i], opt.sink, chunk_verify);
                ++completed;
              }
              ++posted;
            }
          }

          while (completed < lane_expected_chunks) {
            bool made_progress = false;
            for (auto &slot : slots) {
              if (slot.request == nullptr) {
                continue;
              }
              UcxRecvResult result{};
              if (lane.test_recv(slot.request, result)) {
                slot.request = nullptr;
                process_slot(slot, result, state, stats[i], opt.sink, chunk_verify);
                ++completed;
                made_progress = true;
                if (posted < lane_expected_chunks) {
                  UcxRecvResult immediate{};
                  if (post_recv(lane, slot, frame_size, immediate)) {
                    process_slot(slot, immediate, state, stats[i], opt.sink,
                                 chunk_verify);
                    ++completed;
                  }
                  ++posted;
                }
              }
            }
            if (!made_progress) {
              lane.progress();
            }
          }

          FrameHeader done{};
          lane.recv_all(&done, sizeof(done));
          validate_frame_header(done);
          if (static_cast<FrameType>(done.type) != FrameType::Done) {
            throw std::runtime_error("expected done frame after tag data");
          }
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
    if (state.fd >= 0) {
      fsync(state.fd);
      close(state.fd);
      state.fd = -1;
    }
    if (failed) {
      for (const auto &error : errors) {
        if (!error.empty()) {
          std::cerr << "lane error: " << error << "\n";
        }
      }
      return 2;
    }
    if (!state.meta_ready) {
      throw std::runtime_error("no metadata received");
    }
    if (state.received_chunks != state.meta.total_chunks) {
      throw std::runtime_error("received chunk count does not match metadata");
    }

    const auto total_end = std::chrono::steady_clock::now();
    const double total_seconds =
        std::chrono::duration<double>(total_end - total_start).count();
    const std::string actual_sha = (!opt.sink && file_verify) ? sha256_file_hex(opt.output) : "";
    const bool ok = opt.sink || !file_verify || actual_sha == state.meta.sha256_hex;
    write_json(opt.results_json, state.meta, stats, total_seconds, actual_sha, ok,
               opt.sink, opt.depth, opt.verify, opt.engine, opt.register_buffers,
               opt.cpu_list);
    if (!ok) {
      throw std::runtime_error("final SHA256 mismatch");
    }
    double active_seconds = 0.0;
    for (const auto &s : stats) {
      active_seconds = std::max(active_seconds, s.seconds);
    }

    std::cout << "received file_size=" << state.meta.file_size
              << " chunks=" << state.meta.total_chunks
              << " seconds=" << total_seconds
              << " throughput_mib_s=" << (state.meta.file_size / 1048576.0) / total_seconds
              << " active_seconds=" << active_seconds
              << " active_throughput_mib_s="
              << (active_seconds > 0.0
                      ? (state.meta.file_size / 1048576.0) / active_seconds
                      : 0.0)
              << " sha256_ok=" << (opt.sink ? "skipped_sink" : "true") << "\n";
    for (const auto &s : stats) {
      std::cout << "lane=" << s.name << " chunks=" << s.chunks
                << " bytes=" << s.bytes << " seconds=" << s.seconds << "\n";
    }
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "rnic_recv: " << ex.what() << "\n";
    usage(argv[0]);
    return 1;
  }
}
