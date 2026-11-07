// Contract test for rpc.pipeline.control / rpc.pipeline.control_state.
//
// Uses a dummy fakesrc->fakesink pipeline instead of the QNN graph, so it can
// run on any board with GStreamer and a reachable NATS server. If NATS is not
// reachable the test prints SKIP and succeeds, keeping CTest green on hosts
// that do not run a bus.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gst/gst.h>

#include "aibox/adapter/NatsAdapter.hpp"
#include "aibox/control/PipelineControlServer.hpp"
#include "aibox_contracts.pb.h"

namespace {

int g_failures = 0;

void Check(bool cond, const std::string& what) {
  if (!cond) {
    std::cerr << "FAIL: " << what << std::endl;
    ++g_failures;
  } else {
    std::cout << "ok: " << what << std::endl;
  }
}

struct Inbox {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> payload;

  void Clear() {
    std::lock_guard<std::mutex> lock(mu);
    payload.clear();
  }

  bool Wait(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                       [this] { return !payload.empty(); });
  }

  void Set(const std::string& data) {
    std::lock_guard<std::mutex> lock(mu);
    payload.assign(data.begin(), data.end());
    cv.notify_all();
  }
};

aibox::ControlReq MakeReq(aibox::ControlAction action,
                          const std::string& model_path = "") {
  aibox::ControlReq req;
  req.set_action(action);
  req.set_model_path(model_path);
  return req;
}

bool ControlRequest(aibox::adapter::NatsAdapter& nats, Inbox& inbox,
                    const std::string& reply_subject,
                    const aibox::ControlReq& req,
                    aibox::ControlResp* resp) {
  inbox.Clear();
  std::string bytes;
  if (!req.SerializeToString(&bytes)) return false;
  if (!nats.PublishRequest(
          aibox::control::PipelineControlServer::kControlSubject, reply_subject,
          std::vector<uint8_t>(bytes.begin(), bytes.end()))) {
    return false;
  }
  if (!inbox.Wait(5000)) return false;
  return resp->ParseFromArray(inbox.payload.data(),
                              static_cast<int>(inbox.payload.size()));
}

bool StateRequest(aibox::adapter::NatsAdapter& nats, Inbox& inbox,
                  const std::string& reply_subject,
                  aibox::PipelineControlState* state) {
  inbox.Clear();
  if (!nats.PublishRequest(
          aibox::control::PipelineControlServer::kStateSubject, reply_subject,
          std::vector<uint8_t>{})) {
    return false;
  }
  if (!inbox.Wait(5000)) return false;
  return state->ParseFromArray(inbox.payload.data(),
                               static_cast<int>(inbox.payload.size()));
}

GstElement* BuildDummyPipeline() {
  GstElement* pipeline = gst_pipeline_new("control-test");
  GstElement* src = gst_element_factory_make("fakesrc", nullptr);
  GstElement* sink = gst_element_factory_make("fakesink", nullptr);
  if (!pipeline || !src || !sink) {
    if (pipeline) gst_object_unref(pipeline);
    return nullptr;
  }
  gst_bin_add_many(GST_BIN(pipeline), src, sink, nullptr);
  if (!gst_element_link(src, sink)) {
    gst_object_unref(pipeline);
    return nullptr;
  }
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  return pipeline;
}

int RunChecks(aibox::adapter::NatsAdapter& client,
              const std::string& reply_subject) {
  Inbox inbox;
  client.Subscribe(reply_subject,
                   [&inbox](const std::string&, const std::string&,
                            const std::string& payload) { inbox.Set(payload); });
  Check(client.Flush(2000), "client inbox subscription flushed");

  aibox::ControlResp resp;

  Check(ControlRequest(client, inbox, reply_subject,
                       MakeReq(aibox::ControlAction::PAUSE), &resp) &&
            resp.ok(),
        "PAUSE accepted");

  aibox::PipelineControlState state;
  Check(StateRequest(client, inbox, reply_subject, &state) &&
            state.state() == aibox::PipelineControlState::PAUSED,
        "control_state reports PAUSED");

  Check(ControlRequest(client, inbox, reply_subject,
                       MakeReq(aibox::ControlAction::RELOAD_MODEL,
                               "/var/aibox/models/next.bin"),
                       &resp) &&
            !resp.ok() && resp.error().find("not supported") != std::string::npos,
        "RELOAD_MODEL rejected with an explicit error");

  const bool state_after_reload =
      StateRequest(client, inbox, reply_subject, &state);
  Check(state_after_reload && state.state() == aibox::PipelineControlState::PAUSED,
        "rejected RELOAD_MODEL leaves the pipeline PAUSED (readable=" +
            std::string(state_after_reload ? "yes" : "no") +
            ", state=" + std::to_string(static_cast<int>(state.state())) + ")");

  Check(ControlRequest(client, inbox, reply_subject,
                       MakeReq(aibox::ControlAction::RESUME), &resp) &&
            resp.ok(),
        "RESUME accepted");

  Check(StateRequest(client, inbox, reply_subject, &state) &&
            state.state() == aibox::PipelineControlState::RUNNING,
        "control_state reports RUNNING");

  client.Unsubscribe(reply_subject);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  GstElement* pipeline = BuildDummyPipeline();
  if (pipeline == nullptr) {
    std::cerr << "FAIL: could not build the dummy GStreamer pipeline"
              << std::endl;
    return 1;
  }

  const char* url_env = std::getenv("AIBOX_TEST_NATS_URL");
  const std::string nats_url =
      (url_env != nullptr && url_env[0] != '\0') ? url_env
                                                 : "nats://127.0.0.1:4222";

  int rc = 0;
  {
    aibox::adapter::NatsAdapter server_nats;
    aibox::adapter::NatsAdapter::Config server_cfg;
    server_cfg.nats_url = nats_url;
    server_cfg.client_name = "pipeline-control-test-server";
    server_cfg.max_pending = 0;

    if (!server_nats.Connect(server_cfg)) {
      std::cout << "SKIP: NATS not reachable at " << nats_url << std::endl;
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 0;
    }

    aibox::control::PipelineControlServer server(server_nats, pipeline,
                                                 "test-model");
    if (!server.Start()) {
      std::cerr << "FAIL: PipelineControlServer::Start" << std::endl;
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 1;
    }

    aibox::adapter::NatsAdapter client_nats;
    aibox::adapter::NatsAdapter::Config client_cfg;
    client_cfg.nats_url = nats_url;
    client_cfg.client_name = "pipeline-control-test-client";
    client_cfg.max_pending = 0;

    if (!client_nats.Connect(client_cfg)) {
      std::cerr << "FAIL: client NATS connect" << std::endl;
      server.Stop();
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      return 1;
    }

    const std::string reply_subject =
        "_INBOX.pipeline-control-test." + std::to_string(getpid());
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    std::thread runner([&] {
      rc = RunChecks(client_nats, reply_subject);
      g_main_loop_quit(loop);
    });
    g_main_loop_run(loop);
    runner.join();
    g_main_loop_unref(loop);

    server.Stop();
    client_nats.Unsubscribe(reply_subject);
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }

  gst_object_unref(pipeline);
  aibox::adapter::NatsAdapter::CloseLibraryAndWait();
  return rc;
}
