// ===================================================================
// Batch planning unit tests (Phase 1-2 data layer + orchestrator)
// Plain assert-based, no GStreamer dependency.
// ===================================================================
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "aibox/common/Types.hpp"
#include "aibox/common/ConfigTypes.hpp"
#include "aibox/orchestrator/Plan.hpp"
#include "aibox/orchestrator/PipelineOrchestrator.hpp"
#include "aibox/config/TxtSnapshotLoader.hpp"

using namespace aibox::pipeline;

static int g_failures = 0;
#define CHECK(cond) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_failures; \
  } \
} while (0)

// ---------------------------------------------------------------
// helpers: build a minimal batch snapshot
// ---------------------------------------------------------------
static ModelConfig MakeModel(const std::string& id, int batch) {
  ModelConfig m;
  m.model_id = id;
  m.name     = id;
  m.batch    = batch;
  m.model_path = "/tmp/" + id + ".bin";

  TensorSpec boxes;
  boxes.name  = "save_infer_model_scale_0_tmp_0";
  boxes.shape = {batch, 8400, 4};
  TensorSpec scores;
  scores.name  = "save_infer_model_scale_1_tmp_0";
  scores.shape = {batch, 4, 8400};
  m.output.tensors = {boxes, scores};
  m.output.class_count = 4;
  return m;
}

static AlgorithmConfig MakeAlgorithm(const std::string& code, int stage_count,
                                     const std::string& model_id) {
  AlgorithmConfig a;
  a.code        = code;
  a.name        = code;
  a.type        = stage_count > 1 ? AlgorithmType::kSerial : AlgorithmType::kSingle;
  a.output_type = OutputType::kDetection;
  a.stage_count = stage_count;
  for (int i = 0; i < stage_count; ++i) {
    StageConfig sc;
    sc.stage_id    = "stage_" + std::to_string(i + 1);
    sc.model_id    = model_id;
    sc.preprocess  = "ppyoloe_stretch";
    sc.postprocess = "ppyoloe_helmet";
    sc.pipeline.postprocess_module = "ppyoloe";
    // valid serial-full-frame chain defaults (Phase 4 contract)
    sc.input_source_type = (i == 0) ? InputSourceType::kFrame
                                    : InputSourceType::kStage;
    if (i > 0)
      sc.input_source_stage_id = "stage_" + std::to_string(i);
    sc.input_crop = InputCropType::kFull;
    a.stages.push_back(std::move(sc));
  }
  return a;
}

static SnapshotConfig MakeSnapshot(const std::vector<std::string>& cameras,
                                   const std::vector<AlgorithmConfig>& algos,
                                   const std::vector<ModelConfig>& models,
                                   int allowed_batch) {
  SnapshotConfig cfg;
  cfg.global.demo_name = "ut_batch";
  cfg.batch.enabled = true;
  cfg.batch.partial_group_policy = "reject";
  cfg.batch.input_caps =
      "video/x-raw,format=NV12_Q08C,width=1920,height=1080,framerate=25/1";
  cfg.batch.allowed_batch_sizes = {1, 2, 4};

  for (size_t i = 0; i < cameras.size(); ++i) {
    StreamConfig sc;
    sc.camera_id = cameras[i];
    sc.rtsp_url  = "rtsp://test/" + cameras[i];
    sc.inference.algorithm = algos[0].code;
    cfg.streams.push_back(std::move(sc));
  }
  cfg.algorithms = algos;
  cfg.models     = models;
  (void)allowed_batch;
  return cfg;
}

// ---------------------------------------------------------------
// 1. B4 four cameras -> one B4 group, config-order slots
// ---------------------------------------------------------------
static void test_b4_four_cameras_single_group() {
  auto algo = MakeAlgorithm("b4_test", 1, "model_b4");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002", "cam-003", "cam-004"},
                           {algo}, {MakeModel("model_b4", 4)}, 4);

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(r.ok());
  if (!r.ok()) return;

  const auto& plan = r.value;
  CHECK(plan.batch_group_plans.size() == 1);
  CHECK(plan.time_mux_group_plans.empty());
  if (plan.batch_group_plans.size() != 1) return;

  const auto& bg = plan.batch_group_plans[0];
  CHECK(bg.batch_size == 4);
  CHECK(bg.streams.size() == 4);
  // config order preserved → slot order
  CHECK(bg.streams[0].camera_id == "cam-001");
  CHECK(bg.streams[3].camera_id == "cam-004");
  CHECK(bg.streams[0].stream_id == 0);
  CHECK(bg.streams[3].stream_id == 3);
  CHECK(bg.streams[0].batch_sink_pad_name == "sink_0");
  CHECK(bg.streams[3].demux_src_pad_name == "src_3");
  // stage fields
  CHECK(plan.stage_plans.size() == 1);
  if (!plan.stage_plans.empty()) {
    CHECK(plan.stage_plans[0].model_batch == 4);
    CHECK(plan.stage_plans[0].batch_group_id == bg.batch_group_id);
  }
  // stream plans marked as batch mode
  for (const auto& sp : plan.stream_plans)
    CHECK(sp.execution_mode == ExecutionMode::kMultistreamBatch);
}

// ---------------------------------------------------------------
// 2. B2 four cameras -> two B2 groups
// ---------------------------------------------------------------
static void test_b2_four_cameras_two_groups() {
  auto algo = MakeAlgorithm("b2_test", 1, "model_b2");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002", "cam-003", "cam-004"},
                           {algo}, {MakeModel("model_b2", 2)}, 2);

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(r.ok());
  if (!r.ok()) return;

  const auto& plan = r.value;
  CHECK(plan.batch_group_plans.size() == 2);
  if (plan.batch_group_plans.size() != 2) return;

  CHECK(plan.batch_group_plans[0].streams.size() == 2);
  CHECK(plan.batch_group_plans[1].streams.size() == 2);
  CHECK(plan.batch_group_plans[0].streams[0].camera_id == "cam-001");
  CHECK(plan.batch_group_plans[0].streams[1].camera_id == "cam-002");
  CHECK(plan.batch_group_plans[1].streams[0].camera_id == "cam-003");
  CHECK(plan.batch_group_plans[1].streams[1].camera_id == "cam-004");
  // each group's stream_ids restart at 0
  CHECK(plan.batch_group_plans[1].streams[0].stream_id == 0);
  // stages: one per group (single stage each)
  CHECK(plan.stage_plans.size() == 2);
}

// ---------------------------------------------------------------
// 3. B4 three cameras + reject -> build fails
// ---------------------------------------------------------------
static void test_b4_three_cameras_reject_fails() {
  auto algo = MakeAlgorithm("b4_3c", 1, "model_b4");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002", "cam-003"},
                           {algo}, {MakeModel("model_b4", 4)}, 4);

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok())
    CHECK(r.status.error_code == "BATCH_GROUP_SIZE_MISMATCH");
}

// ---------------------------------------------------------------
// 4. allow_sparse -> UNSUPPORTED_BATCH_POLICY
// ---------------------------------------------------------------
static void test_allow_sparse_unsupported() {
  auto algo = MakeAlgorithm("sparse", 1, "model_b4");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002", "cam-003"},
                           {algo}, {MakeModel("model_b4", 4)}, 4);
  cfg.batch.partial_group_policy = "allow_sparse";

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok())
    CHECK(r.status.error_code == "UNSUPPORTED_BATCH_POLICY");
}

// ---------------------------------------------------------------
// 5. two stages with different model batch -> fails
// ---------------------------------------------------------------
static void test_mixed_stage_batch_fails() {
  AlgorithmConfig a = MakeAlgorithm("mixed", 2, "model_b4");
  a.stages[1].model_id = "model_b2";

  auto cfg = MakeSnapshot({"cam-001", "cam-002", "cam-003", "cam-004"},
                          {a},
                          {MakeModel("model_b4", 4), MakeModel("model_b2", 2)}, 4);

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok())
    CHECK(r.status.error_code == "BATCH_STAGE_MISMATCH");
}

// ---------------------------------------------------------------
// 6. output tensor shape[0] != batch -> fails
// ---------------------------------------------------------------
static void test_tensor_shape_mismatch_fails() {
  ModelConfig m = MakeModel("model_b4_bad", 4);
  m.output.tensors[0].shape = {1, 8400, 4};   // batch=1 shape on batch=4 model

  auto algo = MakeAlgorithm("badshape", 1, "model_b4_bad");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002", "cam-003", "cam-004"},
                           {algo}, {m}, 4);

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok())
    CHECK(r.status.error_code == "BATCH_TENSOR_MISMATCH");
}

// ---------------------------------------------------------------
// 7. legacy (batch disabled) still goes time-mux path
// ---------------------------------------------------------------
static void test_legacy_still_time_mux() {
  auto algo = MakeAlgorithm("legacy", 1, "model_b1");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002"}, {algo}, {MakeModel("model_b1", 1)}, 1);
  cfg.batch.enabled = false;
  cfg.timemux.max_group_size = 4;

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(r.ok());
  if (!r.ok()) return;
  CHECK(r.value.batch_group_plans.empty());
  CHECK(!r.value.time_mux_group_plans.empty());
  CHECK(r.value.execution_mode == ExecutionMode::kTimeMux);
  for (const auto& sp : r.value.stream_plans)
    CHECK(sp.execution_mode == ExecutionMode::kTimeMux);
}

// ---------------------------------------------------------------
// 8. TxtSnapshotLoader [output.N] parsing
// ---------------------------------------------------------------
static void test_loader_output_n_parsing() {
  // Write a temp snapshot: batch model with two [output.N] sections.
  const std::string dir = "test_batch_tmp";
  const std::string snapshot = dir + "/pipeline.txt";
  {
    FILE* f = nullptr;
    std::string cmd = "mkdir -p " + dir;
    system(cmd.c_str());
    f = fopen(snapshot.c_str(), "w");
    assert(f);
    fprintf(f, "[global]\ndemo_name=t\nmax_streams=4\nenabled_streams=4\n");
    fprintf(f, "[timemux]\nmax_group_size=4\n");
    fprintf(f, "[streams]\ninclude=streams/cam-001.txt\n");
    fprintf(f, "[algorithms]\ninclude=algorithms/a.txt\n");
    fprintf(f, "[models]\ninclude=models/m.txt\n");
    fclose(f);

    cmd = "mkdir -p " + dir + "/streams " + dir + "/algorithms " + dir + "/models";
    system(cmd.c_str());
    f = fopen((dir + "/streams/cam-001.txt").c_str(), "w");
    fprintf(f, "[stream]\ncamera_id=cam-001\nrtsp_url=rtsp://x/0\n[decode]\n"
               "[inference]\nalgorithm=a1\n");
    fclose(f);
    f = fopen((dir + "/algorithms/a.txt").c_str(), "w");
    fprintf(f, "[algorithm]\ncode=a1\nstage_count=1\n[stage.1]\nstage_id=s1\nmodel_id=m1\n"
               "[stage.1.pipeline]\n");
    fclose(f);
    f = fopen((dir + "/models/m.txt").c_str(), "w");
    fprintf(f, "[model]\nmodel_id=m1\nbatch=2\nmodel_path=../data/models/x.bin\n"
               "[input]\nname=image\nwidth=640\nheight=640\nchannels=3\n"
               "[preprocess]\n[output.0]\nname=boxes\nshape=2,8400,4\ndtype=float32\n"
               "[output.1]\nname=scores\nshape=2,4,8400\ndtype=float32\n"
               "[postprocess]\n");
    fclose(f);
  }

  TxtSnapshotLoader loader;
  auto r = loader.Load(snapshot);
  CHECK(r.ok());
  if (!r.ok()) { system(("rm -rf " + dir).c_str()); return; }

  const auto& cfg = r.value;
  CHECK(cfg.models.size() == 1);
  if (cfg.models.empty()) { system(("rm -rf " + dir).c_str()); return; }

  const auto& out = cfg.models[0].output;
  CHECK(out.tensors.size() == 2);
  if (out.tensors.size() != 2) { system(("rm -rf " + dir).c_str()); return; }

  CHECK(out.tensors[0].name == "boxes");
  CHECK(out.tensors[0].shape.size() == 3 && out.tensors[0].shape[0] == 2);
  CHECK(out.tensors[1].name == "scores");
  CHECK(out.tensors[1].shape.size() == 3 && out.tensors[1].shape[0] == 2);
  CHECK(cfg.models[0].batch == 2);

  system(("rm -rf " + dir).c_str());
}

// ---------------------------------------------------------------
// 9. legacy single [output] section still parses (compat)
// ---------------------------------------------------------------
static void test_loader_legacy_output_compat() {
  const std::string dir = "test_batch_tmp2";
  const std::string snapshot = dir + "/pipeline.txt";
  {
    system(("mkdir -p " + dir + "/streams " + dir + "/algorithms " + dir + "/models").c_str());
    FILE* f = fopen(snapshot.c_str(), "w");
    fprintf(f, "[global]\ndemo_name=t\nmax_streams=1\nenabled_streams=1\n");
    fprintf(f, "[streams]\ninclude=streams/cam-001.txt\n");
    fprintf(f, "[algorithms]\ninclude=algorithms/a.txt\n");
    fprintf(f, "[models]\ninclude=models/m.txt\n");
    fclose(f);
    f = fopen((dir + "/streams/cam-001.txt").c_str(), "w");
    fprintf(f, "[stream]\ncamera_id=cam-001\nrtsp_url=rtsp://x/0\n[decode]\n"
               "[inference]\nalgorithm=a1\n");
    fclose(f);
    f = fopen((dir + "/algorithms/a.txt").c_str(), "w");
    fprintf(f, "[algorithm]\ncode=a1\nstage_count=1\n[stage.1]\nstage_id=s1\nmodel_id=m1\n"
               "[stage.1.pipeline]\n");
    fclose(f);
    f = fopen((dir + "/models/m.txt").c_str(), "w");
    fprintf(f, "[model]\nmodel_id=m1\nbatch=1\nmodel_path=../data/models/x.bin\n"
               "[input]\nname=images\nwidth=640\nheight=640\nchannels=3\n"
               "[preprocess]\n[output]\nname=output0\ndtype=float16\nclass_count=9\n"
               "[postprocess]\n");
    fclose(f);
  }

  TxtSnapshotLoader loader;
  auto r = loader.Load(snapshot);
  CHECK(r.ok());
  if (r.ok()) {
    const auto& out = r.value.models[0].output;
    CHECK(out.tensors.size() == 1);
    if (out.tensors.size() == 1) {
      CHECK(out.tensors[0].name == "output0");
      // legacy fallback shape
      CHECK(out.tensors[0].shape.size() == 3 && out.tensors[0].shape[0] == 1 &&
            out.tensors[0].shape[1] == 300 && out.tensors[0].shape[2] == 6);
    }
  }
  system(("rm -rf " + dir).c_str());
}

// ---------------------------------------------------------------
// malformed [output.N] / batch validation failure tests
// ---------------------------------------------------------------
static void write_snapshot(const std::string& dir, const std::string& model_body) {
  system(("mkdir -p " + dir + "/streams " + dir + "/algorithms " + dir + "/models").c_str());
  FILE* f = fopen((dir + "/pipeline.txt").c_str(), "w");
  fprintf(f, "[global]\ndemo_name=t\nmax_streams=4\nenabled_streams=4\n");
  fprintf(f, "[streams]\ninclude=streams/cam-001.txt\n");
  fprintf(f, "[algorithms]\ninclude=algorithms/a.txt\n");
  fprintf(f, "[models]\ninclude=models/m.txt\n");
  fclose(f);
  f = fopen((dir + "/streams/cam-001.txt").c_str(), "w");
  fprintf(f, "[stream]\ncamera_id=cam-001\nrtsp_url=rtsp://x/0\n[decode]\n"
             "[inference]\nalgorithm=a1\n");
  fclose(f);
  f = fopen((dir + "/algorithms/a.txt").c_str(), "w");
  fprintf(f, "[algorithm]\ncode=a1\nstage_count=1\n[stage.1]\nstage_id=s1\nmodel_id=m1\n"
             "[stage.1.pipeline]\n");
  fclose(f);
  f = fopen((dir + "/models/m.txt").c_str(), "w");
  fprintf(f, "%s", model_body.c_str());
  fclose(f);
}

static void test_loader_output_missing_name_fails() {
  const std::string dir = "test_batch_tmp3";
  write_snapshot(dir,
      "[model]\nmodel_id=m1\nbatch=2\nmodel_path=../data/models/x.bin\n"
      "[input]\nname=image\nwidth=640\nheight=640\nchannels=3\n[preprocess]\n"
      "[output.0]\nshape=2,8400,4\ndtype=float32\n[postprocess]\n");
  TxtSnapshotLoader loader;
  auto r = loader.Load(dir + "/pipeline.txt");
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "MODEL_OUTPUT_NO_NAME");
  system(("rm -rf " + dir).c_str());
}

static void test_loader_output_missing_shape_fails() {
  const std::string dir = "test_batch_tmp4";
  write_snapshot(dir,
      "[model]\nmodel_id=m1\nbatch=2\nmodel_path=../data/models/x.bin\n"
      "[input]\nname=image\nwidth=640\nheight=640\nchannels=3\n[preprocess]\n"
      "[output.0]\nname=boxes\ndtype=float32\n[postprocess]\n");
  TxtSnapshotLoader loader;
  auto r = loader.Load(dir + "/pipeline.txt");
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "MODEL_OUTPUT_NO_SHAPE");
  system(("rm -rf " + dir).c_str());
}

static void test_batch_zero_fails() {
  auto algo = MakeAlgorithm("b0", 1, "model_b0");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002"}, {algo}, {MakeModel("model_b0", 0)}, 4);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_INVALID_MODEL_BATCH");
}

static void test_batch_not_allowed_fails() {
  auto algo = MakeAlgorithm("b3", 1, "model_b3");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002", "cam-003"},
                           {algo}, {MakeModel("model_b3", 3)}, 4);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_SIZE_NOT_ALLOWED");
}

static void test_dup_tensor_name_fails() {
  ModelConfig m = MakeModel("model_dup", 2);
  m.output.tensors[1].name = m.output.tensors[0].name;   // duplicate
  auto algo = MakeAlgorithm("dup", 1, "model_dup");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002"}, {algo}, {m}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_TENSOR_DUP_NAME");
}

static void test_bad_tensor_dim_fails() {
  ModelConfig m = MakeModel("model_dim", 2);
  m.output.tensors[0].shape = {2, 0, 4};   // zero dim
  auto algo = MakeAlgorithm("dim", 1, "model_dim");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002"}, {algo}, {m}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_TENSOR_BAD_DIM");
}

static void test_no_output_tensors_fails() {
  ModelConfig m = MakeModel("model_noout", 2);
  m.output.tensors.clear();
  auto algo = MakeAlgorithm("noout", 1, "model_noout");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002"}, {algo}, {m}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_NO_OUTPUT_TENSORS");
}

// ---------------------------------------------------------------
// Phase 4: stage dependency chain validation
// ---------------------------------------------------------------
static void test_stage_dep_chain_fails() {
  AlgorithmConfig a = MakeAlgorithm("serial_dep", 2, "model_b2");
  a.stages[1].input_source_type    = InputSourceType::kStage;
  a.stages[1].input_source_stage_id = "nonexistent_stage";   // wrong dep

  auto cfg = MakeSnapshot({"cam-001", "cam-002"}, {a}, {MakeModel("model_b2", 2)}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_STAGE_DEP");
}

static void test_roi_crop_unsupported() {
  AlgorithmConfig a = MakeAlgorithm("serial_roi", 2, "model_b2");
  a.stages[1].input_source_type    = InputSourceType::kStage;
  a.stages[1].input_source_stage_id = a.stages[0].stage_id;
  a.stages[1].input_crop           = InputCropType::kBBox;   // ROI crop

  auto cfg = MakeSnapshot({"cam-001", "cam-002"}, {a}, {MakeModel("model_b2", 2)}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "UNSUPPORTED_BATCH_ROI_CROP");
}

static void test_stage1_must_be_frame() {
  AlgorithmConfig a = MakeAlgorithm("serial_s1", 2, "model_b2");
  a.stages[0].input_source_type = InputSourceType::kStage;   // first stage not frame

  auto cfg = MakeSnapshot({"cam-001", "cam-002"}, {a}, {MakeModel("model_b2", 2)}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_STAGE1_INPUT");
}

static void test_dup_stage_id_fails() {
  AlgorithmConfig a = MakeAlgorithm("serial_dup", 2, "model_b2");
  a.stages[1].stage_id = a.stages[0].stage_id;   // duplicate id

  auto cfg = MakeSnapshot({"cam-001", "cam-002"}, {a}, {MakeModel("model_b2", 2)}, 2);
  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(!r.ok());
  if (!r.ok()) CHECK(r.status.error_code == "BATCH_DUP_STAGE_ID");
}

// ---------------------------------------------------------------
// 0817 mutex: no [batching] / enabled=0 / model.batch=1 / real snapshots
// ---------------------------------------------------------------
static std::string FindRepoFile(const char* rel) {
  std::vector<std::string> roots;
  if (const char* e = std::getenv("AIBOX_ROOT")) roots.push_back(e);
  roots.push_back(".");
  roots.push_back("..");
  roots.push_back("../..");
  for (const auto& r : roots) {
    std::string p = r + "/" + rel;
    FILE* f = fopen(p.c_str(), "r");
    if (f) { fclose(f); return p; }
  }
  return {};
}

static void ExpectTimeMuxPlan(const ExecutionPlan& plan) {
  CHECK(plan.execution_mode == ExecutionMode::kTimeMux);
  CHECK(plan.batch_group_plans.empty());
  CHECK(!plan.time_mux_group_plans.empty());
  for (const auto& sp : plan.stream_plans)
    CHECK(sp.execution_mode == ExecutionMode::kTimeMux);
}

static void test_no_batching_section_stays_time_mux() {
  const std::string dir = "test_batch_tmp_nobatch";
  write_snapshot(dir,
      "[model]\nmodel_id=m1\nbatch=1\nmodel_path=../data/models/x.bin\n"
      "[input]\nname=images\nwidth=640\nheight=640\nchannels=3\n[preprocess]\n"
      "[output]\nname=output0\ndtype=float16\nclass_count=9\n[postprocess]\n");
  TxtSnapshotLoader loader;
  auto cfg = loader.Load(dir + "/pipeline.txt");
  CHECK(cfg.ok());
  if (cfg.ok()) {
    CHECK(cfg.value.batch.enabled == false);
    PipelineOrchestrator orch;
    auto r = orch.BuildExecutionPlan(cfg.value);
    CHECK(r.ok());
    if (r.ok()) ExpectTimeMuxPlan(r.value);
  }
  system(("rm -rf " + dir).c_str());
}

static void test_batching_enabled_zero_stays_time_mux() {
  const std::string dir = "test_batch_tmp_en0";
  system(("mkdir -p " + dir + "/streams " + dir + "/algorithms " + dir + "/models").c_str());
  FILE* f = fopen((dir + "/pipeline.txt").c_str(), "w");
  fprintf(f, "[global]\ndemo_name=t\nmax_streams=1\nenabled_streams=1\n");
  fprintf(f, "[timemux]\nmax_group_size=4\n");
  fprintf(f, "[batching]\nenabled=0\ninput_caps=video/x-raw,format=NV12\n");
  fprintf(f, "[streams]\ninclude=streams/cam-001.txt\n");
  fprintf(f, "[algorithms]\ninclude=algorithms/a.txt\n");
  fprintf(f, "[models]\ninclude=models/m.txt\n");
  fclose(f);
  f = fopen((dir + "/streams/cam-001.txt").c_str(), "w");
  fprintf(f, "[stream]\ncamera_id=cam-001\nrtsp_url=rtsp://x/0\n[decode]\n"
             "[inference]\nalgorithm=a1\n");
  fclose(f);
  f = fopen((dir + "/algorithms/a.txt").c_str(), "w");
  fprintf(f, "[algorithm]\ncode=a1\nstage_count=1\n[stage.1]\nstage_id=s1\nmodel_id=m1\n"
             "[stage.1.pipeline]\n");
  fclose(f);
  f = fopen((dir + "/models/m.txt").c_str(), "w");
  fprintf(f, "[model]\nmodel_id=m1\nbatch=2\nmodel_path=../data/models/x.bin\n"
             "[input]\nname=image\nwidth=640\nheight=640\nchannels=3\n[preprocess]\n"
             "[output.0]\nname=boxes\nshape=2,8400,4\ndtype=float32\n[postprocess]\n");
  fclose(f);

  TxtSnapshotLoader loader;
  auto cfg = loader.Load(dir + "/pipeline.txt");
  CHECK(cfg.ok());
  if (cfg.ok()) {
    CHECK(cfg.value.batch.enabled == false);
    PipelineOrchestrator orch;
    auto r = orch.BuildExecutionPlan(cfg.value);
    CHECK(r.ok());
    if (r.ok()) ExpectTimeMuxPlan(r.value);
  }
  system(("rm -rf " + dir).c_str());
}

static void test_model_batch1_does_not_enable_b2() {
  auto algo = MakeAlgorithm("b1_model", 1, "model_b1");
  auto cfg  = MakeSnapshot({"cam-001", "cam-002"}, {algo}, {MakeModel("model_b1", 1)}, 1);
  cfg.batch.enabled = false;
  cfg.timemux.max_group_size = 4;

  PipelineOrchestrator orch;
  auto r = orch.BuildExecutionPlan(cfg);
  CHECK(r.ok());
  if (!r.ok()) return;
  CHECK(cfg.models[0].batch == 1);
  ExpectTimeMuxPlan(r.value);
}

static void test_real_snapshots_stay_time_mux() {
  const char* rels[] = {
    "config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_1cam.txt",
    "config_snapshots/demo_pp_yolo_2stage_serial/pipeline_ppyolo_2stage_3cam_webrtc.txt",
    "config_snapshots/demo_2stage_yolo_serial/pipeline_otm_3cam_webrtc.txt",
  };
  TxtSnapshotLoader loader;
  PipelineOrchestrator orch;
  for (const char* rel : rels) {
    std::string path = FindRepoFile(rel);
    CHECK(!path.empty());
    if (path.empty()) {
      fprintf(stderr, "FAIL missing snapshot %s (set AIBOX_ROOT or run from repo root)\n", rel);
      continue;
    }
    auto cfg = loader.Load(path);
    CHECK(cfg.ok());
    if (!cfg.ok()) {
      fprintf(stderr, "FAIL load %s: %s %s\n", path.c_str(),
              cfg.status.error_code.c_str(), cfg.status.message.c_str());
      continue;
    }
    CHECK(cfg.value.batch.enabled == false);
    auto r = orch.BuildExecutionPlan(cfg.value);
    CHECK(r.ok());
    if (r.ok()) ExpectTimeMuxPlan(r.value);
  }
}

int main() {
  test_b4_four_cameras_single_group();
  test_b2_four_cameras_two_groups();
  test_b4_three_cameras_reject_fails();
  test_allow_sparse_unsupported();
  test_mixed_stage_batch_fails();
  test_tensor_shape_mismatch_fails();
  test_legacy_still_time_mux();
  test_loader_output_n_parsing();
  test_loader_legacy_output_compat();
  test_loader_output_missing_name_fails();
  test_loader_output_missing_shape_fails();
  test_batch_zero_fails();
  test_batch_not_allowed_fails();
  test_dup_tensor_name_fails();
  test_bad_tensor_dim_fails();
  test_no_output_tensors_fails();
  test_stage_dep_chain_fails();
  test_roi_crop_unsupported();
  test_stage1_must_be_frame();
  test_dup_stage_id_fails();
  test_no_batching_section_stays_time_mux();
  test_batching_enabled_zero_stays_time_mux();
  test_model_batch1_does_not_enable_b2();
  test_real_snapshots_stay_time_mux();

  if (g_failures == 0) {
    printf("[PASS] all batch planning tests passed\n");
    return 0;
  }
  fprintf(stderr, "[FAIL] %d assertion(s) failed\n", g_failures);
  return 1;
}
