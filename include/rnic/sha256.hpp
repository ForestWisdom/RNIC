#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rnic {

class Sha256 {
public:
  Sha256();
  void update(const void *data, size_t len);
  std::string final_hex();

private:
  void transform(const uint8_t block[64]);

  uint8_t data_[64]{};
  uint32_t state_[8]{};
  uint64_t bitlen_ = 0;
  size_t datalen_ = 0;
  bool finalized_ = false;
};

std::string sha256_file_hex(const std::string &path);

} // namespace rnic
