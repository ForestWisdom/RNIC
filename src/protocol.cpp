#include "rnic/protocol.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace rnic {

namespace {

std::vector<std::string> split_csv(const std::string &value) {
  std::vector<std::string> out;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ',')) {
    out.push_back(item);
  }
  return out;
}

void copy_cstr(char *dst, size_t dst_len, const std::string &src) {
  std::memset(dst, 0, dst_len);
  std::strncpy(dst, src.c_str(), dst_len - 1);
}

} // namespace

LaneSpec parse_lane_spec(const std::string &spec, bool require_host) {
  auto parts = split_csv(spec);
  const size_t expected = require_host ? 6 : 5;
  if (parts.size() != expected) {
    throw std::runtime_error("invalid lane spec: " + spec);
  }

  LaneSpec lane;
  lane.name = parts[0];
  if (require_host) {
    lane.host = parts[1];
    lane.port = static_cast<uint16_t>(std::stoul(parts[2]));
    lane.tls = parts[3];
    lane.net_devices = parts[4];
    lane.weight = static_cast<uint32_t>(std::stoul(parts[5]));
  } else {
    lane.host = parts[1];
    lane.port = static_cast<uint16_t>(std::stoul(parts[2]));
    lane.tls = parts[3];
    lane.net_devices = parts[4];
    lane.weight = 1;
  }

  if (lane.name.empty() || lane.port == 0 || lane.tls.empty()) {
    throw std::runtime_error("lane name, port, and tls must be non-empty");
  }
  std::replace(lane.tls.begin(), lane.tls.end(), '+', ',');
  if (lane.weight == 0) {
    throw std::runtime_error("lane weight must be greater than zero");
  }
  return lane;
}

std::string lane_spec_help(bool sender) {
  if (sender) {
    return "name,remote_host,port,ucx_tls,ucx_net_devices,weight";
  }
  return "name,bind_host,port,ucx_tls,ucx_net_devices";
}

FrameHeader make_frame(FrameType type, const TransferMeta &meta,
                       const std::string &lane_name) {
  FrameHeader header{};
  header.magic = kFrameMagic;
  header.version = kProtocolVersion;
  header.type = static_cast<uint16_t>(type);
  header.file_size = meta.file_size;
  header.total_chunks = meta.total_chunks;
  header.chunk_size = meta.chunk_size;
  copy_cstr(header.sha256_hex, sizeof(header.sha256_hex), meta.sha256_hex);
  copy_cstr(header.mode, sizeof(header.mode), meta.mode);
  copy_cstr(header.lane, sizeof(header.lane), lane_name);
  return header;
}

void validate_frame_header(const FrameHeader &header) {
  if (header.magic != kFrameMagic) {
    throw std::runtime_error("received frame with bad magic");
  }
  if (header.version != kProtocolVersion) {
    throw std::runtime_error("received frame with unsupported protocol version");
  }
}

uint64_t fnv1a64(const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (char c : value) {
    switch (c) {
    case '\\':
      out << "\\\\";
      break;
    case '"':
      out << "\\\"";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      out << c;
      break;
    }
  }
  return out.str();
}

std::string now_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

} // namespace rnic
