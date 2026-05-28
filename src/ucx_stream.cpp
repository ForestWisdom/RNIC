#include "rnic/ucx_stream.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
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

void socket_send_all(int fd, const void *data, size_t length) {
  const char *cursor = static_cast<const char *>(data);
  while (length > 0) {
    const ssize_t n = ::send(fd, cursor, length, 0);
    if (n <= 0) {
      throw std::runtime_error("socket send failed during UCX address exchange");
    }
    cursor += n;
    length -= static_cast<size_t>(n);
  }
}

void socket_recv_all(int fd, void *data, size_t length) {
  char *cursor = static_cast<char *>(data);
  while (length > 0) {
    const ssize_t n = ::recv(fd, cursor, length, MSG_WAITALL);
    if (n <= 0) {
      throw std::runtime_error("socket recv failed during UCX address exchange");
    }
    cursor += n;
    length -= static_cast<size_t>(n);
  }
}

uint64_t host_to_be64(uint64_t value) {
  const uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
  const uint32_t low = htonl(static_cast<uint32_t>(value & 0xffffffffu));
  return (static_cast<uint64_t>(low) << 32) | high;
}

uint64_t be64_to_host(uint64_t value) {
  const uint32_t high = ntohl(static_cast<uint32_t>(value >> 32));
  const uint32_t low = ntohl(static_cast<uint32_t>(value & 0xffffffffu));
  return (static_cast<uint64_t>(low) << 32) | high;
}

} // namespace

UcxStreamLane::UcxStreamLane(LaneSpec spec, bool server)
    : spec_(std::move(spec)), server_(server) {}

UcxStreamLane::~UcxStreamLane() {
  if (ep_ != nullptr) {
    close_endpoint();
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

void UcxStreamLane::close_endpoint() {
  ucp_request_param_t params{};
  ucs_status_ptr_t request = ucp_ep_close_nbx(ep_, &params);
  if (!UCS_PTR_IS_ERR(request) && request != nullptr) {
    while (ucp_request_check_status(request) == UCS_INPROGRESS) {
      ucp_worker_progress(worker_);
    }
    ucp_request_free(request);
  }
  ep_ = nullptr;
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
    server_connect_oob();
  } else {
    client_connect_oob();
  }
}

void UcxStreamLane::create_endpoint_from_peer_address(const void *address,
                                                      size_t length) {
  ucp_ep_params_t ep_params{};
  ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
  ep_params.address = static_cast<const ucp_address_t *>(address);
  (void)length;
  check_status(ucp_ep_create(worker_, &ep_params, &ep_), "ucp_ep_create address");
}

void UcxStreamLane::server_connect_oob() {
  ucp_address_t *local_address = nullptr;
  size_t local_length = 0;
  check_status(ucp_worker_get_address(worker_, &local_address, &local_length),
               "ucp_worker_get_address");

  const sockaddr_in addr = make_sockaddr(spec_.host, spec_.port);
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    ucp_worker_release_address(worker_, local_address);
    throw std::runtime_error("failed to create OOB listen socket");
  }
  const int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::bind(listen_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
      ::listen(listen_fd, 1) != 0) {
    ::close(listen_fd);
    ucp_worker_release_address(worker_, local_address);
    throw std::runtime_error("failed to bind/listen OOB socket on " + spec_.host +
                             ":" + std::to_string(spec_.port));
  }

  const int fd = ::accept(listen_fd, nullptr, nullptr);
  ::close(listen_fd);
  if (fd < 0) {
    ucp_worker_release_address(worker_, local_address);
    throw std::runtime_error("failed to accept OOB connection");
  }

  uint64_t peer_length_be = 0;
  socket_recv_all(fd, &peer_length_be, sizeof(peer_length_be));
  const uint64_t peer_length = be64_to_host(peer_length_be);
  std::string peer_address(peer_length, '\0');
  socket_recv_all(fd, peer_address.data(), peer_address.size());

  const uint64_t local_length_be = host_to_be64(local_length);
  socket_send_all(fd, &local_length_be, sizeof(local_length_be));
  socket_send_all(fd, local_address, local_length);
  ::close(fd);

  ucp_worker_release_address(worker_, local_address);
  create_endpoint_from_peer_address(peer_address.data(), peer_address.size());
}

void UcxStreamLane::client_connect_oob() {
  ucp_address_t *local_address = nullptr;
  size_t local_length = 0;
  check_status(ucp_worker_get_address(worker_, &local_address, &local_length),
               "ucp_worker_get_address");

  const sockaddr_in addr = make_sockaddr(spec_.host, spec_.port);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    ucp_worker_release_address(worker_, local_address);
    throw std::runtime_error("failed to create OOB client socket");
  }
  while (::connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  const uint64_t local_length_be = host_to_be64(local_length);
  socket_send_all(fd, &local_length_be, sizeof(local_length_be));
  socket_send_all(fd, local_address, local_length);

  uint64_t peer_length_be = 0;
  socket_recv_all(fd, &peer_length_be, sizeof(peer_length_be));
  const uint64_t peer_length = be64_to_host(peer_length_be);
  std::string peer_address(peer_length, '\0');
  socket_recv_all(fd, peer_address.data(), peer_address.size());
  ::close(fd);

  ucp_worker_release_address(worker_, local_address);
  create_endpoint_from_peer_address(peer_address.data(), peer_address.size());
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
