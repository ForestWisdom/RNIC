#pragma once

#include "rnic/protocol.hpp"

#include <ucp/api/ucp.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rnic {

class UcxStreamLane {
public:
  UcxStreamLane(LaneSpec spec, bool server);
  ~UcxStreamLane();

  UcxStreamLane(const UcxStreamLane &) = delete;
  UcxStreamLane &operator=(const UcxStreamLane &) = delete;

  void connect();
  void send_all(const void *data, size_t length);
  void recv_all(void *data, size_t length);
  const LaneSpec &spec() const { return spec_; }

private:
  void init_ucx();
  void server_connect_oob();
  void client_connect_oob();
  void wait_request(void *request);
  void close_endpoint();
  void create_endpoint_from_peer_address(const void *address, size_t length);

  LaneSpec spec_;
  bool server_ = false;
  ucp_config_t *config_ = nullptr;
  ucp_context_h context_ = nullptr;
  ucp_worker_h worker_ = nullptr;
  ucp_ep_h ep_ = nullptr;
};

} // namespace rnic
