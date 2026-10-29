#include "aibox/siglip_worker/SiglipNatsClient.hpp"
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace aibox::siglip {
namespace {
void Check(natsStatus status, const std::string& action) {
  if (status != NATS_OK)
    throw std::runtime_error(action + ": " + natsStatus_GetText(status));
}
}  // namespace
SiglipNatsClient::SiglipNatsClient(const WorkerConfig& config) : config_(config) {
  Check(natsConnection_ConnectTo(&connection_, config_.nats_url.c_str()), "NATS connect");
  try { Check(natsConnection_JetStream(&js_, connection_, nullptr), "JetStream context"); }
  catch (...) { natsConnection_Destroy(connection_); connection_ = nullptr; throw; }
}
SiglipNatsClient::~SiglipNatsClient() {
  // Destroy releases this client's binding; it does not delete the server durable.
  if (subscription_) natsSubscription_Destroy(subscription_);
  if (js_) jsCtx_Destroy(js_);
  if (connection_) natsConnection_Destroy(connection_);
}
void SiglipNatsClient::PrepareConsumer() {
  jsStreamInfo* stream = nullptr;
  Check(js_GetStreamInfo(&stream, js_, config_.stream.c_str(), nullptr, nullptr), "get stream");
  bool subject_present = false;
  if (stream && stream->Config)
    for (int i = 0; i < stream->Config->SubjectsLen; ++i)
      if (stream->Config->Subjects[i] && config_.subject == stream->Config->Subjects[i])
        subject_present = true;
  const auto max_age = stream && stream->Config ? stream->Config->MaxAge : 0;
  const bool compatible = stream && stream->Config &&
                          stream->Config->Retention != js_WorkQueuePolicy;
  jsStreamInfo_Destroy(stream);
  if (!compatible) throw std::runtime_error("stream retention does not support independent consumers");
  if (!subject_present) throw std::runtime_error("stream does not contain alarm-ready subject");
  std::cout << "[SIGLIP_STREAM] max_age_ns=" << max_age << '\n';
  jsConsumerInfo* existing = nullptr;
  natsStatus status = js_GetConsumerInfo(&existing, js_, config_.stream.c_str(),
                                         config_.durable.c_str(), nullptr, nullptr);
  if (status == NATS_NOT_FOUND) {
    jsConsumerConfig settings;
    Check(jsConsumerConfig_Init(&settings), "consumer config");
    settings.Durable = config_.durable.c_str();
    settings.FilterSubject = config_.subject.c_str();
    settings.DeliverPolicy = js_DeliverNew;
    settings.AckPolicy = js_AckExplicit;
    settings.AckWait = static_cast<std::int64_t>(config_.ack_wait_ms) * 1000000;
    settings.MaxDeliver = -1;
    settings.MaxAckPending = 1;
    settings.MaxRequestBatch = 1;
    Check(js_AddConsumer(&existing, js_, config_.stream.c_str(), &settings, nullptr, nullptr),
          "create dedicated durable");
    std::cout << "[SIGLIP_CONSUMER] created DeliverNew; earlier history excluded\n";
  } else {
    Check(status, "get durable");
  }
  if (!existing || !existing->Config) {
    if (existing) jsConsumerInfo_Destroy(existing);
    throw std::runtime_error("consumer information missing config");
  }
  const auto* actual = existing->Config;
  const bool valid = actual->FilterSubject && config_.subject == actual->FilterSubject &&
                     actual->AckPolicy == js_AckExplicit &&
                     actual->AckWait == static_cast<std::int64_t>(config_.ack_wait_ms) * 1000000 &&
                     actual->MaxDeliver == -1 && actual->MaxAckPending == 1 &&
                     actual->DeliverSubject == nullptr && actual->BackOffLen == 0;
  std::cout << "[SIGLIP_CONSUMER] pending=" << existing->NumPending
            << " ack_floor=" << existing->AckFloor.Stream << '\n';
  jsConsumerInfo_Destroy(existing);
  if (!valid) throw std::runtime_error("existing durable contract differs; no automatic reset");
}
void SiglipNatsClient::BindConsumer() {
  if (subscription_) throw std::runtime_error("consumer already bound");
  jsSubOptions options;
  Check(jsSubOptions_Init(&options), "subscription options");
  options.Stream = config_.stream.c_str();
  options.Consumer = config_.durable.c_str();
  Check(js_PullSubscribe(&subscription_, js_, config_.subject.c_str(), config_.durable.c_str(),
                         nullptr, &options, nullptr), "bind existing durable");
}
bool SiglipNatsClient::Fetch(Delivery* delivery) {
  natsMsgList list{};
  const auto status = natsSubscription_Fetch(&list, subscription_, 1,
                                               config_.fetch_timeout_ms, nullptr);
  if (status == NATS_TIMEOUT) return false;
  Check(status, "fetch alarm-ready");
  if (list.Count != 1 || !list.Msgs || !list.Msgs[0]) {
    natsMsgList_Destroy(&list);
    return false;
  }
  std::unique_ptr<natsMsg, decltype(&natsMsg_Destroy)> message(list.Msgs[0], &natsMsg_Destroy);
  list.Msgs[0] = nullptr;
  natsMsgList_Destroy(&list);
  const char* payload = natsMsg_GetData(message.get());
  const int size = natsMsg_GetDataLength(message.get());
  if (size < 0 || size > 1048576) throw std::runtime_error("alarm-ready payload outside limit");
  delivery->payload.assign(payload, payload + size);
  jsMsgMetaData* meta = nullptr;
  Check(natsMsg_GetMetaData(&meta, message.get()), "read delivery metadata");
  delivery->stream_sequence = meta->Sequence.Stream;
  delivery->consumer_sequence = meta->Sequence.Consumer;
  delivery->attempts = meta->NumDelivered;
  jsMsgMetaData_Destroy(meta);
  delivery->message = message.release();
  return true;
}
std::string SiglipNatsClient::Request(const std::string& subject, const std::string& payload) {
  natsMsg* reply = nullptr;
  Check(natsConnection_RequestString(&reply, connection_, subject.c_str(), payload.c_str(),
                                      config_.request_timeout_ms), "RPC " + subject);
  std::unique_ptr<natsMsg, decltype(&natsMsg_Destroy)> guard(reply, &natsMsg_Destroy);
  const int length = natsMsg_GetDataLength(reply);
  if (length < 0 || length > 4 * 1024 * 1024) throw std::runtime_error("RPC response outside limit");
  return std::string(natsMsg_GetData(reply), length);
}
void SiglipNatsClient::Progress(natsMsg* message) {
  Check(natsMsg_InProgress(message, nullptr), "in-progress heartbeat");
}
void SiglipNatsClient::Ack(natsMsg* message) {
  Check(natsMsg_AckSync(message, nullptr, nullptr), "AckSync");
}
void SiglipNatsClient::Nak(natsMsg* message, int delay_ms) {
  Check(natsMsg_NakWithDelay(message, delay_ms, nullptr), "NakWithDelay");
}
void SiglipNatsClient::Term(natsMsg* message) {
  Check(natsMsg_Term(message, nullptr), "Term");
}
}  // namespace aibox::siglip
