#include "aibox/pipeline/GstElementHelper.hpp"
#include <cstring>
#include <sstream>

namespace aibox::pipeline {

bool FactoryExists(const std::string& factory_name) {
  GstElementFactory* f = gst_element_factory_find(factory_name.c_str());
  if (!f) return false;
  gst_object_unref(f);
  return true;
}

GstElement* MakeElement(const std::string& factory, const std::string& name,
                        std::string* out_error) {
  GstElement* e = gst_element_factory_make(factory.c_str(), name.c_str());
  if (!e && out_error)
    *out_error = "failed to create element: " + factory + " name=" + name;
  return e;
}

bool SetCapsFilter(GstElement* capsfilter, const std::string& caps_str,
                   std::string* out_error) {
  GstCaps* caps = gst_caps_from_string(caps_str.c_str());
  if (!caps) {
    if (out_error) *out_error = "invalid caps: " + caps_str;
    return false;
  }
  g_object_set(capsfilter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  return true;
}

// Ported from rebuild: pipeline/GstElementHelpers.cpp
bool SetEnumPropertyByNick(GstElement* elem, const char* prop,
                           const char* nick, std::string* out_error) {
  GParamSpec* pspec = g_object_class_find_property(
      G_OBJECT_GET_CLASS(elem), prop);
  if (!pspec) {
    if (out_error) {
      *out_error = std::string("property not found: ") + prop;
    }
    return false;
  }

  if (!G_IS_PARAM_SPEC_ENUM(pspec)) {
    if (out_error) {
      *out_error = std::string("property is not an enum: ") + prop;
    }
    return false;
  }

  GType enum_type = G_PARAM_SPEC_VALUE_TYPE(pspec);
  GEnumClass* enum_class = G_ENUM_CLASS(g_type_class_ref(enum_type));
  if (!enum_class) {
    if (out_error) *out_error = std::string("cannot get enum class for: ") + prop;
    return false;
  }

  GEnumValue* ev = g_enum_get_value_by_nick(enum_class, nick);
  if (!ev) {
    if (out_error) {
      *out_error = std::string("enum nick not found: ") + nick +
                   " for property: " + prop;
    }
    g_type_class_unref(enum_class);
    return false;
  }

  g_object_set(elem, prop, ev->value, nullptr);
  g_type_class_unref(enum_class);
  return true;
}

bool SetFlagsPropertyByNick(GstElement* elem, const char* prop,
                            const char* nick, std::string* out_error) {
  GParamSpec* pspec = g_object_class_find_property(
      G_OBJECT_GET_CLASS(elem), prop);
  if (!pspec) {
    if (out_error) {
      *out_error = std::string("property not found: ") + prop;
    }
    return false;
  }

  if (!G_IS_PARAM_SPEC_FLAGS(pspec)) {
    if (out_error) {
      *out_error = std::string("property is not flags: ") + prop;
    }
    return false;
  }

  GType flags_type = G_PARAM_SPEC_VALUE_TYPE(pspec);
  GFlagsClass* flags_class = G_FLAGS_CLASS(g_type_class_ref(flags_type));
  if (!flags_class) {
    if (out_error) *out_error = std::string("cannot get flags class for: ") + prop;
    return false;
  }

  GFlagsValue* fv = g_flags_get_value_by_nick(flags_class, nick);
  if (!fv) {
    if (out_error) {
      *out_error = std::string("flags nick not found: ") + nick +
                   " for property: " + prop;
    }
    g_type_class_unref(flags_class);
    return false;
  }

  g_object_set(elem, prop, fv->value, nullptr);
  g_type_class_unref(flags_class);
  return true;
}

}  // namespace aibox::pipeline
