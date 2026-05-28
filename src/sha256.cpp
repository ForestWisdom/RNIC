#include "rnic/sha256.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rnic {

namespace {

constexpr std::array<uint32_t, 64> k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
    0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
    0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
    0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
uint32_t majority(uint32_t a, uint32_t b, uint32_t c) {
  return (a & b) ^ (a & c) ^ (b & c);
}

} // namespace

Sha256::Sha256() {
  state_[0] = 0x6a09e667;
  state_[1] = 0xbb67ae85;
  state_[2] = 0x3c6ef372;
  state_[3] = 0xa54ff53a;
  state_[4] = 0x510e527f;
  state_[5] = 0x9b05688c;
  state_[6] = 0x1f83d9ab;
  state_[7] = 0x5be0cd19;
}

void Sha256::transform(const uint8_t block[64]) {
  uint32_t m[64];
  for (int i = 0; i < 16; ++i) {
    m[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
           (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
    const uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
    m[i] = m[i - 16] + s0 + m[i - 7] + s1;
  }

  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t temp1 = h + s1 + choose(e, f, g) + k[i] + m[i];
    const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t temp2 = s0 + majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const void *data, size_t len) {
  if (finalized_) {
    throw std::runtime_error("cannot update finalized sha256");
  }
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    data_[datalen_++] = bytes[i];
    if (datalen_ == 64) {
      transform(data_);
      bitlen_ += 512;
      datalen_ = 0;
    }
  }
}

std::string Sha256::final_hex() {
  if (finalized_) {
    throw std::runtime_error("sha256 already finalized");
  }
  finalized_ = true;
  size_t i = datalen_;

  data_[i++] = 0x80;
  if (i > 56) {
    while (i < 64) {
      data_[i++] = 0x00;
    }
    transform(data_);
    std::memset(data_, 0, 56);
  } else {
    while (i < 56) {
      data_[i++] = 0x00;
    }
  }

  bitlen_ += datalen_ * 8;
  for (int j = 7; j >= 0; --j) {
    data_[63 - j] = static_cast<uint8_t>((bitlen_ >> (j * 8)) & 0xff);
  }
  transform(data_);

  std::ostringstream out;
  for (uint32_t word : state_) {
    out << std::hex << std::setfill('0') << std::setw(8) << word;
  }
  return out.str();
}

std::string sha256_file_hex(const std::string &path) {
  FILE *fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    throw std::runtime_error("failed to open file for sha256: " + path);
  }
  Sha256 sha;
  std::vector<uint8_t> buffer(4 * 1024 * 1024);
  while (true) {
    const size_t n = std::fread(buffer.data(), 1, buffer.size(), fp);
    if (n > 0) {
      sha.update(buffer.data(), n);
    }
    if (n < buffer.size()) {
      if (std::ferror(fp)) {
        std::fclose(fp);
        throw std::runtime_error("failed while reading file for sha256: " + path);
      }
      break;
    }
  }
  std::fclose(fp);
  return sha.final_hex();
}

} // namespace rnic
