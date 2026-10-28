#pragma once

#include "core/Bytes.h"

#include <string>
#include <vector>

namespace cardbridge::fileio {

// All paths are UTF-8 (the install folder has brackets, mod folders have
// Japanese and Chinese names); on Windows they are converted to wide paths so
// the narrow-ANSI trap never applies. Read-only by default: nothing here
// writes except WriteFile / WriteTextFile, which only the exporter calls.

Bytes ReadFile(const std::string& path);
// Reads `size` bytes at `offset` (throws if the file is shorter).
Bytes ReadRange(const std::string& path, uint64_t offset, size_t size);
uint64_t FileSize(const std::string& path);
bool Exists(const std::string& path);
int64_t LastWriteTicks(const std::string& path);

void WriteFile(const std::string& path, const uint8_t* data, size_t size);
void WriteTextFile(const std::string& path, const std::string& text);
void CreateDirectories(const std::string& path);

// Recursively lists regular files whose lower-cased extension is in `exts`
// (each with its leading dot). Unreadable folders are skipped and reported in
// `skipped`.
std::vector<std::string> ListFiles(const std::string& root, const std::vector<std::string>& exts,
                                   std::vector<std::string>* skipped = nullptr);

std::string Join(const std::string& a, const std::string& b);
std::string FileName(const std::string& path);
std::string Stem(const std::string& path);

} // namespace cardbridge::fileio
