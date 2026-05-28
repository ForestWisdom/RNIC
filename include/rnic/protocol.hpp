#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace rnic {

constexpr uint32_t kFrameMagic = 0x524e4943; // "RNIC"
constexpr uint16_t kProtocolVersion = 1;

enum class FrameType : uint16_t {
  Meta = 1,
  Data = 2,
  Done = 3,
};

struct FrameHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint64_t file_size;
  uint64_t total_chunks;
  uint64_t chunk_id;
  uint64_t offset;
  uint32_t length;
  uint32_t chunk_size;
  uint64_t checksum;
  char sha256_hex[65];
  char mode[32];
  char lane[32];
};

struct LaneSpec {
  std::string name;
  std::string host;
  uint16_t port = 0;
  std::string tls;
  std::string net_devices;
  uint32_t weight = 1;
};

struct TransferMeta {
  uint64_t file_size = 0;
  uint64_t total_chunks = 0;
  uint32_t chunk_size = 0;
  std::string sha256_hex;
  std::string mode;
};

struct LaneStats {
  std::string name;
  uint64_t chunks = 0;
  uint64_t bytes = 0;
  double seconds = 0.0;
};

LaneSpec parse_lane_spec(const std::string &spec, bool require_host);
std::string lane_spec_help(bool sender);
FrameHeader make_frame(FrameType type, const TransferMeta &meta,
                       const std::string &lane_name);
void validate_frame_header(const FrameHeader &header);
uint64_t fnv1a64(const void *data, size_t length);
std::string json_escape(const std::string &value);
std::string now_timestamp();

} // namespace rnic
