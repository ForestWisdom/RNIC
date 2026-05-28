#pragma once

#include "rnic/protocol.hpp"

#include <ucp/api/ucp.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace rnic {

struct UcxRecvResult {
  ucp_tag_t tag = 0;
  size_t length = 0;
};

class UcxStreamLane {
public:
  UcxStreamLane(LaneSpec spec, bool server);
  ~UcxStreamLane();

  UcxStreamLane(const UcxStreamLane &) = delete;
  UcxStreamLane &operator=(const UcxStreamLane &) = delete;

  void connect();
  void send_all(const void *data, size_t length);
  void recv_all(void *data, size_t length);
  void *post_tag_send(const void *data, size_t length, ucp_tag_t tag);
  void *post_tag_recv(void *data, size_t length, ucp_tag_t tag,
                      ucp_tag_t tag_mask, UcxRecvResult *immediate);
  bool test_send(void *request);
  bool test_recv(void *request, UcxRecvResult &result);
  void progress();
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
