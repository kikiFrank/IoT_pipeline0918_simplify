#include "aibox/config/IniParser.hpp"

#include <fstream>
#include <sstream>

namespace aibox::pipeline {

IniParser::IniParser(int max_include_depth)
    : max_include_depth_(max_include_depth) {}

std::string IniParser::Trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  while (e > b && (s[e-1] == ' ' || s[e-1] == '\t' || s[e-1] == '\r')) --e;
  return s.substr(b, e - b);
}

std::string IniParser::DirName(const std::string& path) {
  auto pos = path.find_last_of("/\\");
  if (pos == std::string::npos) return ".";
  return path.substr(0, pos);
}

IniParser::ParseResult IniParser::Parse(const std::string& file_path) const {
  ParseResult result;
  ParseFile(file_path, &result.root, &result.error, &result.line, 0);
  result.root.file_path = file_path;
  return result;
}

void IniParser::ParseFile(const std::string& file_path,
                          IniFile* out,
                          std::string* error,
                          int* error_line,
                          int depth) const {
  if (depth >= max_include_depth_) {
    *error = "max include depth exceeded: " + std::to_string(depth);
    *error_line = 0;
    return;
  }

  // Resolve absolute path for include resolution
  std::string abs_path = file_path;
  // If relative, resolve relative to the root file's directory
  // (handled by caller — this method always receives resolved paths)

  std::ifstream ifs(abs_path);
  if (!ifs.is_open()) {
    *error = "cannot open file: " + abs_path;
    *error_line = 0;
    return;
  }

  std::string dir = DirName(abs_path);
  std::string line_str;
  int line_no = 0;

  IniSection* current_section = nullptr;

  while (std::getline(ifs, line_str)) {
    ++line_no;
    std::string trimmed = Trim(line_str);

    // skip empty / comments
    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
      continue;

    // [section]
    if (trimmed[0] == '[' && trimmed.back() == ']') {
      std::string sec_name = Trim(trimmed.substr(1, trimmed.size() - 2));
      out->sections.emplace_back();
      current_section = &out->sections.back();
      current_section->name = sec_name;
      continue;
    }

    // include=path
    if (trimmed.rfind("include=", 0) == 0) {
      std::string rel = Trim(trimmed.substr(8));
      std::string inc_path = dir + "/" + rel;
      ParseFile(inc_path, out, error, error_line, depth + 1);
      if (!error->empty()) return;
      continue;
    }

    // key=value
    auto eq = trimmed.find('=');
    if (eq != std::string::npos) {
      std::string key   = Trim(trimmed.substr(0, eq));
      std::string value = Trim(trimmed.substr(eq + 1));

      if (!current_section) {
        *error = "key=value outside any section at line " + std::to_string(line_no);
        *error_line = line_no;
        return;
      }

      current_section->entries.emplace_back(key, value);
      continue;
    }

    // unrecognized line
    *error = "unrecognized line " + std::to_string(line_no) + ": " + trimmed;
    *error_line = line_no;
    return;
  }
}

}  // namespace aibox::pipeline
