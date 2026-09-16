#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace aibox::pipeline {

// ===================================================================
// A single INI file parsed into sections -> (key -> value)
// ===================================================================
struct IniSection {
  std::string name;
  std::vector<std::pair<std::string, std::string>> entries;  // ordered
};

struct IniFile {
  std::string              file_path;
  std::vector<IniSection>  sections;
};

// ===================================================================
// IniParser
// ===================================================================
class IniParser {
 public:
  // Parse a file.  Supports:
  //   [section]
  //   key=value
  //   ; comment
  //   include=relative/path  (relative to file being parsed)
  //
  // Max recursion depth for includes (default 10).
  explicit IniParser(int max_include_depth = 10);

  struct ParseResult {
    IniFile    root;
    std::string error;   // empty = success
    int         line    = 0;
  };

  ParseResult Parse(const std::string& file_path) const;

  static std::string DirName(const std::string& path);

 private:
  int max_include_depth_;

  void ParseFile(const std::string& file_path,
                 IniFile* out,
                 std::string* error,
                 int* error_line,
                 int depth) const;

  static std::string Trim(const std::string& s);
};

}  // namespace aibox::pipeline
