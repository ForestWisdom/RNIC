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
  void server_listen();
  void client_connect();
  void wait_request(void *request);
  void close_endpoint();

  LaneSpec spec_;
  bool server_ = false;
  ucp_config_t *config_ = nullptr;
  ucp_context_h context_ = nullptr;
  ucp_worker_h worker_ = nullptr;
  ucp_listener_h listener_ = nullptr;
  ucp_ep_h ep_ = nullptr;
  ucp_conn_request_h conn_request_ = nullptr;
};

} // namespace rnic
