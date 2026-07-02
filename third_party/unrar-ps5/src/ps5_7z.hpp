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

struct SevenZArchiveInfo
{
  RAR_EXIT code;
  std::string reason;
  std::string missing_volume;
  unsigned long long input_bytes;
  unsigned long long total_bytes;
  unsigned long long max_file_bytes;
  unsigned file_count;
  unsigned dir_count;
  unsigned unknown_size_count;
};

SevenZArchiveInfo Probe7zArchive(const std::string &ArchivePath,
                                 const std::string &Password,
                                 uint Threads);

SevenZExtractResult Run7zExtract(const std::string &ArchivePath,
                                 const std::string &DestPath,
                                 const std::string &Password,
                                 uint Threads);

#endif
