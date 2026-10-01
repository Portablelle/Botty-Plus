#ifndef PS5_7Z_HPP
#define PS5_7Z_HPP

#include "rar.hpp"

#include <string>

struct SevenZExtractResult
{
  RAR_EXIT code;
  std::string reason;
  std::string missing_volume;
};

SevenZExtractResult Run7zExtract(const std::string &ArchivePath,
                                 const std::string &DestPath,
                                 const std::string &Password,
                                 uint Threads);

#endif
