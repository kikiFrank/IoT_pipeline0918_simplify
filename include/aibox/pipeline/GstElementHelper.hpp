#pragma once

#include <string>
#include <gst/gst.h>
#include "aibox/common/Types.hpp"

namespace aibox::pipeline {

// --- factory check ---
bool FactoryExists(const std::string& factory_name);

// --- element creation ---
GstElement* MakeElement(const std::string& factory, const std::string& name,
                        std::string* out_error);

// --- caps filter ---
bool SetCapsFilter(GstElement* capsfilter, const std::string& caps_str,
                   std::string* out_error);

// --- enum property by nick (GEnum / GFlags) ---
bool SetEnumPropertyByNick(GstElement* elem, const char* prop,
                           const char* nick, std::string* out_error);
bool SetFlagsPropertyByNick(GstElement* elem, const char* prop,
                            const char* nick, std::string* out_error);

}  // namespace aibox::pipeline
