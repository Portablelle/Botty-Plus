#pragma once
#include "installation-runtime.hpp"
#include <zlib.h>
#ifdef __PS5__
#include "../build/service-updater-embedded.hpp"
#endif

namespace botty {
inline void launchEmbeddedServiceUpdater() {
#ifdef __PS5__
  if(serviceUpdaterSize>NativeTransaction::maxFileBytes||serviceUpdaterSize<4||sizeof(serviceUpdaterGzip)>NativeTransaction::maxFileBytes)throw std::runtime_error("Embedded service updater bound is invalid.");
  std::string payload(serviceUpdaterSize,'\0');z_stream stream{};
  stream.next_in=const_cast<Bytef*>(serviceUpdaterGzip);stream.avail_in=sizeof(serviceUpdaterGzip);stream.next_out=reinterpret_cast<Bytef*>(&payload[0]);stream.avail_out=payload.size();
  if(inflateInit2(&stream,16+MAX_WBITS)!=Z_OK)throw std::runtime_error("Cannot initialize updater decompression.");
  const auto result=inflate(&stream,Z_FINISH);const auto bytes=stream.total_out;const auto remaining=stream.avail_in;inflateEnd(&stream);
  if(result!=Z_STREAM_END||bytes!=serviceUpdaterSize||remaining||nativeHash(payload)!=serviceUpdaterHash)throw std::runtime_error("Embedded service updater verification failed.");
  installationLoad(payload);
#else
  throw std::runtime_error("Embedded updater launch requires PS5 runtime validation.");
#endif
}
}
