#pragma once

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

namespace aibox::pipeline {

// ===================================================================
// Phase 1 terminal callback (debug/verify only): reads frame meta + iterates
// every ROI meta and prints to stderr. No struct population. Kept for probing.
// ===================================================================
GstFlowReturn OnTerminalSamplePrint(GstAppSink* sink, gpointer user_data);

// ===================================================================
// Phase 2 terminal callback: parse the fused terminal buffer into an
// aibox::aggregator::AggregatedResult (camera_id/frame_id/pts + all stages'
// detections) and Submit() it to the ResultAggregator passed as user_data.
// This is the extraction step that replaces the old C++ cross-stage join
// (fusion already happened in-pipeline via qtimetamux).
//
//   user_data MUST be a valid aibox::aggregator::ResultAggregator*.
// ===================================================================
GstFlowReturn OnTerminalSampleToAggregator(GstAppSink* sink, gpointer user_data);

}  // namespace aibox::pipeline
