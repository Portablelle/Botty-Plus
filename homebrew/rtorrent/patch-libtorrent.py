#!/usr/bin/env python3
"""Apply the PS5 compatibility fixes to pinned libtorrent."""
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

# fd_open_file logs after a failed open; logging must not mask its errno.
p=Path(sys.argv[1])/'src/torrent/net/fd.cc'
s=p.read_text()
old='''  if (fd == -1) {
    LT_LOG_FLAG_ERROR("fd_open_file failed to open file");
    return -1;
  }
'''
new='''  if (fd == -1) {
    const int open_error = errno;
    LT_LOG_FLAG_ERROR("fd_open_file failed to open file");
    errno = open_error;
    return -1;
  }
'''
if old in s:
    assert s.count(old)==1
    s=s.replace(old,new)
else:
    assert new in s, 'Unexpected pinned fd_open_file implementation'
p.write_text(s)

# sysconf's advertised budget need not match the payload's descriptor limit.
# Reclaim only this manager's file descriptors, leaving peer sockets untouched.
p=Path(sys.argv[1])/'src/torrent/data/file_manager.cc'
s=p.read_text()
old='''  if (!fd.open(file->frozen_path().str(), prot, flags)) {
    m_files_failed_counter++;
    return false;
  }
'''
previous='''  bool opened = fd.open(file->frozen_path().str(), prot, flags);

  if (!opened && errno == EMFILE) {
    // Detached files may still be awaiting their asynchronous close.
    m_fd_close_queue->wait_for(0);
    opened = fd.open(file->frozen_path().str(), prot, flags);

    if (!opened && errno == EMFILE && !empty()) {
      // The actual process limit is below the advertised cache budget.
      // Evict one least-recently-used file and wait for its real close.
      evict_least_active(1);
      m_fd_close_queue->wait_for(0);
      opened = fd.open(file->frozen_path().str(), prot, flags);
    }
  }

  if (!opened) {
    m_files_failed_counter++;
    return false;
  }
'''
new = '  const int previous_errno = errno;\n' + previous
new += '''
  // A successful retry must not turn an empty-file mapping into an I/O error.
  // ChunkList clears errno before hashing and treats a missing extent as a skip.
  errno = previous_errno;
'''
if old in s:
    assert s.count(old)==1
    s=s.replace(old,new)
elif new in s:
    pass
elif previous in s:
    assert s.count(previous)==1
    s=s.replace(previous,new)
else:
    raise AssertionError('Unexpected pinned FileManager::open implementation')
p.write_text(s)
