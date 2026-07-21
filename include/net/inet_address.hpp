#ifndef CO_MIRA_NET_INET_ADDRESS_HPP
#define CO_MIRA_NET_INET_ADDRESS_HPP

#include "net/error.hpp"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>

namespace mira::co::net {

class inet_address {
public:
  inet_address() noexcept { this->assign_ipv4(0, false); }

  explicit inet_address(uint16_t port, bool loopback = false, sa_family_t family = AF_INET) {
    if (family == AF_INET) {
      this->assign_ipv4(port, loopback);
      return;
    }

    if (family == AF_INET6) {
      sockaddr_in6 address{};
      address.sin6_family = AF_INET6;
      address.sin6_port = htons(port);
      address.sin6_addr = loopback ? in6addr_loopback : in6addr_any;
      this->assign(&address, sizeof(address));
      return;
    }

    co::log("unsupported address family: {}", family);
    throw std::invalid_argument("unsupported address family");
  }

  inet_address(std::string_view ip, uint16_t port, sa_family_t family = AF_INET) {
    const std::string text(ip);

    if (family == AF_INET) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      if (::inet_pton(AF_INET, text.c_str(), &address.sin_addr) != 1) {
        co::log("invalid IPv4 address: {}", text);
        throw std::invalid_argument("invalid IPv4 address");
      }
      this->assign(&address, sizeof(address));
      return;
    }

    if (family == AF_INET6) {
      sockaddr_in6 address{};
      address.sin6_family = AF_INET6;
      address.sin6_port = htons(port);
      if (::inet_pton(AF_INET6, text.c_str(), &address.sin6_addr) != 1) {
        co::log("invalid IPv6 address: {}", text);
        throw std::invalid_argument("invalid IPv6 address");
      }
      this->assign(&address, sizeof(address));
      return;
    }

    co::log("unsupported address family: {}", family);
    throw std::invalid_argument("unsupported address family");
  }

  [[nodiscard]] static inet_address from_native(const sockaddr *address, socklen_t length) {
    const socklen_t required_length = address != nullptr && address->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
    if (address == nullptr || (address->sa_family != AF_INET && address->sa_family != AF_INET6) || length < required_length ||
        length > sizeof(sockaddr_storage)) {
      co::log("invalid native internet address");
      throw std::invalid_argument("invalid native internet address");
    }

    inet_address result;
    std::memset(&result.storage_, 0, sizeof(result.storage_));
    std::memcpy(&result.storage_, address, length);
    result.length_ = length;
    return result;
  }

  [[nodiscard]] const sockaddr *data() const noexcept { return reinterpret_cast<const sockaddr *>(&this->storage_); }
  [[nodiscard]] sockaddr *data() noexcept { return reinterpret_cast<sockaddr *>(&this->storage_); }
  [[nodiscard]] socklen_t length() const noexcept { return this->length_; }
  [[nodiscard]] sa_family_t family() const noexcept { return this->data()->sa_family; }

  [[nodiscard]] uint16_t port() const noexcept {
    if (this->family() == AF_INET)
      return ntohs(reinterpret_cast<const sockaddr_in *>(&this->storage_)->sin_port);
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&this->storage_)->sin6_port);
  }

  [[nodiscard]] std::string ip() const {
    char output[INET6_ADDRSTRLEN]{};
    const void *source = nullptr;

    if (this->family() == AF_INET)
      source = &reinterpret_cast<const sockaddr_in *>(&this->storage_)->sin_addr;
    else
      source = &reinterpret_cast<const sockaddr_in6 *>(&this->storage_)->sin6_addr;

    if (::inet_ntop(this->family(), source, output, sizeof(output)) == nullptr)
      detail::throw_errno("inet_ntop");

    return output;
  }

private:
  void assign_ipv4(uint16_t port, bool loopback) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    this->assign(&address, sizeof(address));
  }

  template <typename Address> void assign(const Address *address, socklen_t length) noexcept {
    std::memset(&this->storage_, 0, sizeof(this->storage_));
    std::memcpy(&this->storage_, address, length);
    this->length_ = length;
  }

  sockaddr_storage storage_{};
  socklen_t length_ = sizeof(sockaddr_in);
};

} // namespace mira::co::net

#endif
