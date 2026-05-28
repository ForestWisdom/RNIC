#include "rnic/protocol.hpp"
#include "rnic/sha256.hpp"
#include "rnic/ucx_stream.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace rnic;

namespace {

struct Options {
  std::string output;
  std::vector<LaneSpec> lanes;
  std::string results_json;
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
      << " [--lane ...] [--results-json FILE]\n\n"
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
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  if (opt.output.empty()) {
    throw std::runtime_error("--output is required");
  }
  if (opt.lanes.empty()) {
    throw std::runtime_error("at least one --lane is required");
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

void apply_meta(SharedState &state, const FrameHeader &header) {
  validate_frame_header(header);
  if (header.type != static_cast<uint16_t>(FrameType::Meta)) {
    throw std::runtime_error("first frame on each lane must be metadata");
  }
  std::lock_guard<std::mutex> lock(state.mutex);
  const TransferMeta incoming = meta_from_header(header);
  if (!state.meta_ready) {
    state.meta = incoming;
    state.fd = open(state.output.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (state.fd < 0) {
      throw std::runtime_error("failed to open output file: " + state.output);
    }
    if (ftruncate(state.fd, static_cast<off_t>(state.meta.file_size)) != 0) {
      throw std::runtime_error("failed to preallocate output file");
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
                const std::string &actual_sha, bool ok) {
  if (path.empty()) {
    return;
  }
  FILE *fp = std::fopen(path.c_str(), "w");
  if (!fp) {
    throw std::runtime_error("failed to open results json: " + path);
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
               "  \"total_seconds\": %.6f,\n"
               "  \"throughput_mib_s\": %.3f,\n"
               "  \"lanes\": [\n",
               now_timestamp().c_str(), json_escape(meta.mode).c_str(),
               static_cast<unsigned long long>(meta.file_size), meta.chunk_size,
               static_cast<unsigned long long>(meta.total_chunks),
               meta.sha256_hex.c_str(), actual_sha.c_str(), ok ? "true" : "false",
               total_seconds, (meta.file_size / 1048576.0) / total_seconds);
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

} // namespace

int main(int argc, char **argv) {
  try {
    const Options opt = parse_args(argc, argv);
    SharedState state;
    state.output = opt.output;
    std::vector<LaneStats> stats(opt.lanes.size());
    std::atomic<bool> failed{false};
    std::vector<std::string> errors(opt.lanes.size());
    std::vector<std::thread> threads;

    const auto total_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < opt.lanes.size(); ++i) {
      threads.emplace_back([&, i]() {
        try {
          stats[i].name = opt.lanes[i].name;
          UcxStreamLane lane(opt.lanes[i], true);
          lane.connect();

          FrameHeader meta_frame{};
          lane.recv_all(&meta_frame, sizeof(meta_frame));
          apply_meta(state, meta_frame);

          std::vector<uint8_t> buffer(state.meta.chunk_size);
          const auto lane_start = std::chrono::steady_clock::now();
          while (true) {
            FrameHeader header{};
            lane.recv_all(&header, sizeof(header));
            validate_frame_header(header);
            const auto type = static_cast<FrameType>(header.type);
            if (type == FrameType::Done) {
              break;
            }
            if (type != FrameType::Data) {
              throw std::runtime_error("unexpected frame type on data stream");
            }
            if (header.length > state.meta.chunk_size) {
              throw std::runtime_error("received chunk larger than configured chunk size");
            }
            if (header.offset + header.length > state.meta.file_size) {
              throw std::runtime_error("received chunk outside file bounds");
            }
            lane.recv_all(buffer.data(), header.length);
            const uint64_t checksum = fnv1a64(buffer.data(), header.length);
            if (checksum != header.checksum) {
              throw std::runtime_error("chunk checksum mismatch at chunk " +
                                       std::to_string(header.chunk_id));
            }
            ssize_t n = pwrite(state.fd, buffer.data(), header.length,
                               static_cast<off_t>(header.offset));
            if (n != static_cast<ssize_t>(header.length)) {
              throw std::runtime_error("pwrite failed for chunk " +
                                       std::to_string(header.chunk_id));
            }
            state.received_chunks += 1;
            state.received_bytes += header.length;
            stats[i].chunks += 1;
            stats[i].bytes += header.length;
          }
          const auto lane_end = std::chrono::steady_clock::now();
          stats[i].seconds =
              std::chrono::duration<double>(lane_end - lane_start).count();
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
    const std::string actual_sha = sha256_file_hex(opt.output);
    const bool ok = actual_sha == state.meta.sha256_hex;
    write_json(opt.results_json, state.meta, stats, total_seconds, actual_sha, ok);
    if (!ok) {
      throw std::runtime_error("final SHA256 mismatch");
    }

    std::cout << "received file_size=" << state.meta.file_size
              << " chunks=" << state.meta.total_chunks
              << " seconds=" << total_seconds
              << " throughput_mib_s=" << (state.meta.file_size / 1048576.0) / total_seconds
              << " sha256_ok=true\n";
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
