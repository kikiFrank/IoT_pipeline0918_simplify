// 逻辑单测：aiboxtimemux 严格轮转 + 跳空（#include 方式，测完可删除，源码零改动）。
//
// 原理：直接 #include 插件源码，使 static 的 find_next_ready_slot /
// find_pad_by_slot 与 GstAiboxTimeMuxPad 在本翻译单元可见。手工构造 GstAiboxTimeMux
// 裸结构体（绕过 gst_aibox_time_mux_init 的 pad 创建），只填充调度所需字段：
//   sinkpads (GPtrArray*), max_sinks, next_slot, next_slot_id。
// find_next_ready_slot 仅读 sinkpads / max_sinks，不触碰 GStreamer 运行时对象，
// 故无需起 pipeline / 无需 QNN。
//
// 编译（板端，项目根）：
//   g++ -std=c++17 -I plugins/aiboxtimemux -I plugins/aiboxframemeta_common \
//       $(pkg-config --cflags --libs gstreamer-1.0) \
//       test/timemux/test_find_next_ready_slot.cpp -o /tmp/test_timemux_rr
//
// 运行：/tmp/test_timemux_rr
#include "gstaiboxtimemux.cpp"

#include <cstdio>
#include <cstdlib>

static int g_failures = 0;

#define ASSERT_TRUE(expr, msg)                                        \
  do {                                                                \
    if (!(expr)) {                                                    \
      std::fprintf (stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
      g_failures++;                                                   \
    } else {                                                          \
      std::printf ("ok: %s\n", msg);                                  \
    }                                                                 \
  } while (0)

// 构造一个手工 GstAiboxTimeMux，只填充调度相关字段。
static GstAiboxTimeMux *
make_mux (guint max_sinks)
{
  GstAiboxTimeMux *self = (GstAiboxTimeMux *) g_try_new0 (GstAiboxTimeMux, 1);
  if (!self)
    return nullptr;
  self->sinkpads    = g_ptr_array_new ();
  self->max_sinks   = max_sinks;
  self->next_slot   = 0;
  self->next_slot_id = 0;
  return self;
}

// 添加一个 sink pad ctx（直接推入 sinkpads 数组），slot_id 唯一。
static GstAiboxTimeMuxPad *
add_pad (GstAiboxTimeMux * self, guint slot_id, gboolean has_buffer,
         gboolean eos, gboolean flushing)
{
  GstAiboxTimeMuxPad *ctx = (GstAiboxTimeMuxPad *) g_try_new0 (GstAiboxTimeMuxPad, 1);
  ctx->slot_id   = slot_id;
  ctx->has_buffer = has_buffer;
  ctx->eos       = eos;
  ctx->flushing  = flushing;
  g_ptr_array_add (self->sinkpads, ctx);
  return ctx;
}

static void
free_mux (GstAiboxTimeMux * self)
{
  g_ptr_array_free (self->sinkpads, TRUE);   // 释放每个 ctx（g_new0 分配）
  g_free (self);
}

// 用例 1：三路全有帧，严格 RR 依次 +1。
static void
test_all_ready_rr ()
{
  GstAiboxTimeMux *m = make_mux (3);
  add_pad (m, 0, TRUE, FALSE, FALSE);
  add_pad (m, 1, TRUE, FALSE, FALSE);
  add_pad (m, 2, TRUE, FALSE, FALSE);

  // from_slot=0 → 下一个 ready 应为 1
  ASSERT_TRUE (find_next_ready_slot (m, 0) == 1, "rr: 0 -> 1");
  ASSERT_TRUE (find_next_ready_slot (m, 1) == 2, "rr: 1 -> 2");
  ASSERT_TRUE (find_next_ready_slot (m, 2) == 0, "rr: 2 -> 0 (wrap)");
  free_mux (m);
}

// 用例 2：中间一路空，跳空。
static void
test_skip_empty ()
{
  GstAiboxTimeMux *m = make_mux (3);
  add_pad (m, 0, TRUE,  FALSE, FALSE);
  add_pad (m, 1, FALSE, FALSE, FALSE);   // 空路
  add_pad (m, 2, TRUE,  FALSE, FALSE);

  // from_slot=0：i=1 命中 slot1 但 has_buffer=FALSE 跳过；i=2 命中 slot2 有帧 → 返回 2
  ASSERT_TRUE (find_next_ready_slot (m, 0) == 2, "skip: 0 -> 2 (skip empty slot1)");
  // from_slot=2：i=1 命中 slot0 有帧 → 返回 0
  ASSERT_TRUE (find_next_ready_slot (m, 2) == 0, "skip: 2 -> 0");
  free_mux (m);
}

// 用例 3：全空 → NO_SLOT。
static void
test_all_empty ()
{
  GstAiboxTimeMux *m = make_mux (3);
  add_pad (m, 0, FALSE, FALSE, FALSE);
  add_pad (m, 1, FALSE, FALSE, FALSE);
  add_pad (m, 2, FALSE, FALSE, FALSE);

  ASSERT_TRUE (find_next_ready_slot (m, 0) == NO_SLOT, "all-empty -> NO_SLOT");
  free_mux (m);
}

// 用例 4：一路 EOS 被跳过；全 EOS 返回 NO_SLOT。
static void
test_eos ()
{
  GstAiboxTimeMux *m = make_mux (3);
  add_pad (m, 0, TRUE, FALSE, FALSE);
  add_pad (m, 1, TRUE, TRUE,  FALSE);   // EOS
  add_pad (m, 2, TRUE, FALSE, FALSE);

  // from_slot=0：slot1 EOS 跳过 → slot2
  ASSERT_TRUE (find_next_ready_slot (m, 0) == 2, "eos: skip eos slot1 -> 2");

  // 全 EOS → NO_SLOT
  GstAiboxTimeMuxPad *p0 = (GstAiboxTimeMuxPad *) g_ptr_array_index (m->sinkpads, 0);
  GstAiboxTimeMuxPad *p2 = (GstAiboxTimeMuxPad *) g_ptr_array_index (m->sinkpads, 2);
  p0->eos = TRUE; p2->eos = TRUE;
  ASSERT_TRUE (find_next_ready_slot (m, 0) == NO_SLOT, "eos: all-eos -> NO_SLOT");
  free_mux (m);
}

// 用例 5：flushing 路被跳过（等价于驱逐态）。
static void
test_flushing_skipped ()
{
  GstAiboxTimeMux *m = make_mux (3);
  add_pad (m, 0, TRUE, FALSE, FALSE);
  add_pad (m, 1, TRUE, FALSE, TRUE);    // flushing
  add_pad (m, 2, TRUE, FALSE, FALSE);

  ASSERT_TRUE (find_next_ready_slot (m, 0) == 2, "flushing: skip slot1 -> 2");
  free_mux (m);
}

// 用例 6：空数组 → NO_SLOT。
static void
test_empty_array ()
{
  GstAiboxTimeMux *m = make_mux (3);   // 未 add_pad
  ASSERT_TRUE (find_next_ready_slot (m, 0) == NO_SLOT, "empty-array -> NO_SLOT");
  free_mux (m);
}

int
main ()
{
  gst_init (nullptr, nullptr);

  test_all_ready_rr ();
  test_skip_empty ();
  test_all_empty ();
  test_eos ();
  test_flushing_skipped ();
  test_empty_array ();

  if (g_failures == 0) {
    std::printf ("\nALL PASS\n");
    return 0;
  }
  std::printf ("\n%d FAILURE(S)\n", g_failures);
  return 1;
}
