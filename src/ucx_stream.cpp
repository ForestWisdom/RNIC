#include "rnic/ucx_stream.hpp"

#include <arpa/inet.h>
#include <ucs/type/status.h>

#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace rnic {

namespace {

void check_status(ucs_status_t status, const std::string &what) {
  if (status != UCS_OK) {
    throw std::runtime_error(what + ": " + ucs_status_string(status));
  }
}

sockaddr_in make_sockaddr(const std::string &host, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  const std::string bind_host = host.empty() ? "0.0.0.0" : host;
  if (inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
    throw std::runtime_error("invalid IPv4 address: " + bind_host);
  }
  return addr;
}

} // namespace

UcxStreamLane::UcxStreamLane(LaneSpec spec, bool server)
    : spec_(std::move(spec)), server_(server) {}

UcxStreamLane::~UcxStreamLane() {
  if (ep_ != nullptr) {
    ucp_ep_close_nbx(ep_, nullptr);
    ep_ = nullptr;
  }
  if (listener_ != nullptr) {
    ucp_listener_destroy(listener_);
  }
  if (worker_ != nullptr) {
    ucp_worker_destroy(worker_);
  }
  if (context_ != nullptr) {
    ucp_cleanup(context_);
  }
  if (config_ != nullptr) {
    ucp_config_release(config_);
  }
}

void UcxStreamLane::init_ucx() {
  check_status(ucp_config_read(nullptr, nullptr, &config_), "ucp_config_read");
  if (!spec_.tls.empty()) {
    check_status(ucp_config_modify(config_, "TLS", spec_.tls.c_str()),
                 "ucp_config_modify TLS");
  }
  if (!spec_.net_devices.empty() && spec_.net_devices != "-") {
    check_status(ucp_config_modify(config_, "NET_DEVICES", spec_.net_devices.c_str()),
                 "ucp_config_modify NET_DEVICES");
  }

  ucp_params_t params{};
  params.field_mask = UCP_PARAM_FIELD_FEATURES;
  params.features = UCP_FEATURE_STREAM;
  check_status(ucp_init(&params, config_, &context_), "ucp_init");

  ucp_worker_params_t worker_params{};
  worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
  worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;
  check_status(ucp_worker_create(context_, &worker_params, &worker_),
               "ucp_worker_create");
}

void UcxStreamLane::connect() {
  init_ucx();
  if (server_) {
    server_listen();
  } else {
    client_connect();
  }
}

void UcxStreamLane::server_listen() {
  struct HandlerState {
    ucp_conn_request_h request = nullptr;
  } state;

  auto cb = [](ucp_conn_request_h conn_request, void *arg) {
    auto *handler_state = static_cast<HandlerState *>(arg);
    if (handler_state->request == nullptr) {
      handler_state->request = conn_request;
    }
  };

  const sockaddr_in addr = make_sockaddr(spec_.host, spec_.port);
  ucp_listener_params_t params{};
  params.field_mask = UCP_LISTENER_PARAM_FIELD_SOCK_ADDR |
                      UCP_LISTENER_PARAM_FIELD_CONN_HANDLER;
  params.sockaddr.addr = reinterpret_cast<const sockaddr *>(&addr);
  params.sockaddr.addrlen = sizeof(addr);
  params.conn_handler.cb = cb;
  params.conn_handler.arg = &state;

  check_status(ucp_listener_create(worker_, &params, &listener_),
               "ucp_listener_create");
  while (state.request == nullptr) {
    ucp_worker_progress(worker_);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  conn_request_ = state.request;

  ucp_ep_params_t ep_params{};
  ep_params.field_mask = UCP_EP_PARAM_FIELD_CONN_REQUEST;
  ep_params.conn_request = conn_request_;
  check_status(ucp_ep_create(worker_, &ep_params, &ep_), "ucp_ep_create server");
}

void UcxStreamLane::client_connect() {
  const sockaddr_in addr = make_sockaddr(spec_.host, spec_.port);
  ucp_ep_params_t ep_params{};
  ep_params.field_mask = UCP_EP_PARAM_FIELD_SOCK_ADDR;
  ep_params.sockaddr.addr = reinterpret_cast<const sockaddr *>(&addr);
  ep_params.sockaddr.addrlen = sizeof(addr);
  check_status(ucp_ep_create(worker_, &ep_params, &ep_), "ucp_ep_create client");
}

void UcxStreamLane::wait_request(void *request) {
  while (true) {
    const ucs_status_t status = ucp_request_check_status(request);
    if (status == UCS_INPROGRESS) {
      ucp_worker_progress(worker_);
      continue;
    }
    ucp_request_free(request);
    check_status(status, "ucx operation");
    return;
  }
}

void UcxStreamLane::send_all(const void *data, size_t length) {
  const char *cursor = static_cast<const char *>(data);
  size_t remaining = length;
  while (remaining > 0) {
    ucp_request_param_t params{};
    const size_t n = remaining;
    ucs_status_ptr_t request = ucp_stream_send_nbx(ep_, cursor, n, &params);
    if (UCS_PTR_IS_ERR(request)) {
      throw std::runtime_error("ucp_stream_send_nbx: " +
                               std::string(ucs_status_string(UCS_PTR_STATUS(request))));
    }
    if (request != nullptr) {
      wait_request(request);
    }
    cursor += n;
    remaining -= n;
  }
}

void UcxStreamLane::recv_all(void *data, size_t length) {
  char *cursor = static_cast<char *>(data);
  size_t remaining = length;
  while (remaining > 0) {
    size_t received = 0;
    ucp_request_param_t params{};
    params.op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS;
    params.flags = UCP_STREAM_RECV_FLAG_WAITALL;
    ucs_status_ptr_t request =
        ucp_stream_recv_nbx(ep_, cursor, remaining, &received, &params);
    if (UCS_PTR_IS_ERR(request)) {
      throw std::runtime_error("ucp_stream_recv_nbx: " +
                               std::string(ucs_status_string(UCS_PTR_STATUS(request))));
    }
    if (request == nullptr) {
      if (received == 0) {
        throw std::runtime_error("ucx stream closed while receiving");
      }
    } else {
      while (true) {
        const ucs_status_t status = ucp_stream_recv_request_test(request, &received);
        if (status == UCS_INPROGRESS) {
          ucp_worker_progress(worker_);
          continue;
        }
        ucp_request_free(request);
        check_status(status, "ucx stream receive");
        break;
      }
    }
    cursor += received;
    remaining -= received;
  }
}

} // namespace rnic
