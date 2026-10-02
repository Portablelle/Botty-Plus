#!/usr/bin/env python3
"""Apply the PS5 numeric-address compatibility fix to pinned libtorrent."""
from pathlib import Path
import sys
p=Path(sys.argv[1])/'src/torrent/net/socket_address.cc'
s=p.read_text()
start=s.index('try_lookup_numeric(const std::string& hostname, int family) {')
end=s.index('\nsa_inet_union\n',start)
s=s[:start]+'''try_lookup_numeric(const std::string& hostname, int family) {
  // PS5 getaddrinfo does not implement the upstream AI_NUMERICHOST contract.
  // Parse literals without DNS; ordinary hostnames remain resolver work.
  if (family == AF_INET || family == AF_UNSPEC) {
    auto address = sin_make();
    if (::inet_pton(AF_INET, hostname.c_str(), &address->sin_addr) == 1)
      return {sin_shared_ptr(std::move(address)), nullptr};
  }
  if (family == AF_INET6 || family == AF_UNSPEC) {
    auto address = sin6_make();
    if (::inet_pton(AF_INET6, hostname.c_str(), &address->sin6_addr) == 1)
      return {nullptr, sin6_shared_ptr(std::move(address))};
  }
  return {nullptr, nullptr};
}
''' + s[end:]
p.write_text(s)
