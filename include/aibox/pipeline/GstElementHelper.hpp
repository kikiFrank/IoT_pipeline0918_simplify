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

// MobileNetV2 color: RGB U16 [0,65535] -> ImageNet-normalized QNN UFIXED16.
// Retains the converter's writable DMA allocation and all ROI/frame metadata.
bool EncodeColorInputInPlace(GstBuffer* buffer, double quant_scale,
                             int quant_offset, std::string* out_error);

}  // namespace aibox::pipeline
