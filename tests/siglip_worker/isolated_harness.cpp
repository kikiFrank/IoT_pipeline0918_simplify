#include <nats/nats.h>
#include "aibox/siglip_worker/JsonUtil.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(natsStatus status, const char* action) {
  if (status != NATS_OK)
    throw std::runtime_error(std::string(action) + ": " + natsStatus_GetText(status));
}
void Reply(natsConnection* nc, natsSubscription*, natsMsg* msg, void* closure) {
  const auto* media = static_cast<std::string*>(closure);
  const std::string subject(natsMsg_GetSubject(msg));
  std::string body;
  if (subject == "test.siglip.rpc.alarm") {
    body = R"({"items":[{"alarm_id":"test-alarm","event_id":"source-test","timestamp_ms":1793180000000,"payload_json":{"camera_id":"cam-test","source_event_ids":["source-test"]}}],"next_cursor_ms":null,"next_cursor_id":null})";
    const auto request = aibox::siglip::ParseJson(std::string(natsMsg_GetData(msg), natsMsg_GetDataLength(msg)));
    const auto id = aibox::siglip::String(aibox::siglip::RootObject(request.get()), "event_id");
    body.replace(body.find("test-alarm"), 10, id);
  } else
    body = "{\"found\":true,\"uploaded\":false,\"cloud_url\":\"\",\"file_path\":\"" +
           *media + "\",\"thumbnail_path\":\"\"}";
  if (const char* reply = natsMsg_GetReply(msg))
    natsConnection_Publish(nc, reply, body.data(), body.size());
  natsMsg_Destroy(msg);
}
}
int main(int argc, char** argv) {
  if (argc < 3) return 2;
  const std::string mode = argv[1];
  natsConnection* nc = nullptr;
  try {
    Check(natsConnection_ConnectTo(&nc, argv[2]), "connect");
    if (mode == "setup") {
      jsCtx* js = nullptr;
      Check(natsConnection_JetStream(&js, nc, nullptr), "jetstream");
      jsStreamConfig config;
      Check(jsStreamConfig_Init(&config), "stream config");
      config.Name = "ALARM_READY_SIGLIP_ISOLATED";
      const char* subject = "test.siglip.ready";
      config.Subjects = &subject;
      config.SubjectsLen = 1;
      jsStreamInfo* info = nullptr;
      Check(js_AddStream(&info, js, &config, nullptr, nullptr), "add test stream");
      jsStreamInfo_Destroy(info);
      jsCtx_Destroy(js);
    } else if (mode == "mock") {
      if (argc != 4) return 2;
      std::string media = argv[3];
      natsSubscription* alarm = nullptr;
      natsSubscription* picture = nullptr;
      Check(natsConnection_Subscribe(&alarm, nc, "test.siglip.rpc.alarm", Reply, &media), "alarm subscribe");
      Check(natsConnection_Subscribe(&picture, nc, "test.siglip.rpc.media", Reply, &media), "media subscribe");
      Check(natsConnection_FlushTimeout(nc, 3000), "flush subscriptions");
      std::cout << "MOCK_READY" << std::endl;
      std::this_thread::sleep_for(std::chrono::seconds(120));
      natsSubscription_Destroy(alarm);
      natsSubscription_Destroy(picture);
    } else if (mode == "publish") {
      std::string body = R"({"alarm_id":"test-alarm","event_id":"test-asset","source_event_id":"source-test","timestamp_ms":1793180001000})";
      if (argc == 4) {
        const std::string id = argv[3];
        if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos)
          throw std::runtime_error("unsafe test ID");
        body.replace(body.find("test-alarm"), 10, id);
      }
      Check(natsConnection_Publish(nc, "test.siglip.ready", body.data(), body.size()), "publish");
      Check(natsConnection_FlushTimeout(nc, 3000), "flush publish");
    } else if (mode == "bad") {
      Check(natsConnection_PublishString(nc, "test.siglip.ready", "{invalid-json"), "publish bad");
      Check(natsConnection_FlushTimeout(nc, 3000), "flush bad");
    } else if (mode == "rpc") {
      if (argc != 5) return 2;
      natsMsg* reply = nullptr;
      Check(natsConnection_RequestString(&reply, nc, argv[3], argv[4], 3000), "RPC");
      std::cout.write(natsMsg_GetData(reply), natsMsg_GetDataLength(reply));
      std::cout << '\n';
      natsMsg_Destroy(reply);
    } else if (mode == "status") {
      jsCtx* js = nullptr;
      Check(natsConnection_JetStream(&js, nc, nullptr), "jetstream");
      jsConsumerInfo* info = nullptr;
      Check(js_GetConsumerInfo(&info, js, "ALARM_READY_SIGLIP_ISOLATED",
                               "siglip-worker-isolated", nullptr, nullptr), "consumer info");
      std::cout << "ACK_FLOOR=" << info->AckFloor.Stream
                << " PENDING=" << info->NumPending << '\n';
      jsConsumerInfo_Destroy(info);
      jsCtx_Destroy(js);
    } else return 2;
    natsConnection_Destroy(nc);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    if (nc) natsConnection_Destroy(nc);
    return 1;
  }
}
