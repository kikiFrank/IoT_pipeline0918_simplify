/// @file mask_policy_tests.cpp
/// @brief Scenario 15 (fail-open revision): FaceMaskPolicy::Decide full matrix.
///        UNAVAILABLE / STALE / missing state meta / broken detection meta /
///        illegal bboxes => CLEAR (original video passes through).
///        Only a fully valid VALID_FACES result draws boxes.

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "aibox/face/face_mask_policy.hpp"

using aibox::face::MaskDecision;
using aibox::face::MaskPolicyInput;

TEST(MaskPolicyTest, MissingOrMismatchedStateMetaIsClear) {
  MaskPolicyInput in;
  in.state_meta_present = false;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);

  in.state_meta_present = true;
  in.abi_version_ok = false;
  in.meta_size_ok = true;
  in.result_status = 1;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);

  in.abi_version_ok = true;
  in.meta_size_ok = false;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);
}

TEST(MaskPolicyTest, UnavailableAndValidEmptyAreClear) {
  MaskPolicyInput in;
  in.state_meta_present = in.abi_version_ok = in.meta_size_ok = true;
  in.result_status = 0;  // UNAVAILABLE / STALE
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);
  in.result_status = 2;  // VALID_EMPTY
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);
}

TEST(MaskPolicyTest, ValidFacesWithBrokenDetectionMetaIsClear) {
  MaskPolicyInput in;
  in.state_meta_present = in.abi_version_ok = in.meta_size_ok = true;
  in.result_status = 1;  // VALID_FACES

  in.det_meta_present = false;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);

  in.det_meta_present = true;
  in.n_faces = 0;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);

  in.n_faces = 5;
  in.n_faces_over_max = 1;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);

  in.n_faces_over_max = 0;
  in.any_valid_bbox = false;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::CLEAR);

  in.any_valid_bbox = true;
  EXPECT_EQ(aibox::face::DecideMask(in), MaskDecision::BOXES);
}

TEST(MaskPolicyTest, FullMatrixTableDriven) {
  // (state_present, abi_ok, size_ok, status, det_present, n_faces, over_max,
  //  any_valid, expected)
  struct Row {
    bool sp, abi, sz;
    std::uint32_t status;
    bool det;
    std::uint32_t n;
    std::uint32_t over;
    bool valid_box;
    MaskDecision expected;
  };
  const Row rows[] = {
      {false, true, true, 1, true, 1, 0, true, MaskDecision::CLEAR},
      {true, false, true, 1, true, 1, 0, true, MaskDecision::CLEAR},
      {true, true, false, 1, true, 1, 0, true, MaskDecision::CLEAR},
      {true, true, true, 0, true, 1, 0, true, MaskDecision::CLEAR},
      {true, true, true, 1, false, 1, 0, true, MaskDecision::CLEAR},
      {true, true, true, 1, true, 0, 0, true, MaskDecision::CLEAR},
      {true, true, true, 1, true, 40, 1, true, MaskDecision::CLEAR},
      {true, true, true, 1, true, 3, 0, false, MaskDecision::CLEAR},
      {true, true, true, 1, true, 3, 0, true, MaskDecision::BOXES},
      {true, true, true, 2, false, 0, 0, false, MaskDecision::CLEAR},
  };
  for (const auto& r : rows) {
    MaskPolicyInput in;
    in.state_meta_present = r.sp;
    in.abi_version_ok = r.abi;
    in.meta_size_ok = r.sz;
    in.result_status = r.status;
    in.det_meta_present = r.det;
    in.n_faces = r.n;
    in.n_faces_over_max = r.over;
    in.any_valid_bbox = r.valid_box;
    EXPECT_EQ(aibox::face::DecideMask(in), r.expected)
        << "sp=" << r.sp << " abi=" << r.abi << " sz=" << r.sz
        << " st=" << r.status << " det=" << r.det << " n=" << r.n;
  }
}

TEST(MaskPolicyTest, StatusEnumValuesMatchGstFaceResultStateStatus) {
  // Must stay in sync with GstFaceResultStateStatus in
  // plugins/qtifaceinfer/gst_plugin/gstfaceresultstatemeta.h
  EXPECT_EQ(static_cast<std::uint32_t>(aibox::face::kMaskStatusUnavailable), 0u);
  EXPECT_EQ(static_cast<std::uint32_t>(aibox::face::kMaskStatusValidFaces), 1u);
  EXPECT_EQ(static_cast<std::uint32_t>(aibox::face::kMaskStatusValidEmpty), 2u);
}

// ---------------------------------------------------------------------------
// ValidateBoxes: the ONE validated list for policy + mask generation
// ---------------------------------------------------------------------------
TEST(MaskPolicyTest, ValidateBoxesKeepsOnlyDrawableClampedBoxes) {
  using aibox::face::RawBox;
  const RawBox raw[] = {
      {10, 20, 30, 40},                       // valid
      {std::numeric_limits<float>::quiet_NaN(), 0, 10, 10},  // NaN rejected
      {-50, -50, 50, 50},                     // clamped to 0,0..50,50
      {600, 300, 700, 400},                   // x2 clamped to max_x=639
      {0, 0, 0, 10},                          // w=0 rejected
      {0, 0, 10, 0},                          // h=0 rejected
      {100, 100, 50, 150},                    // inverted rejected
  };
  const auto boxes = aibox::face::ValidateBoxes(raw, 7, 640, 360);
  ASSERT_EQ(boxes.size(), 3u);
  EXPECT_EQ(boxes[0].x, 10);
  EXPECT_EQ(boxes[0].y, 20);
  EXPECT_EQ(boxes[0].w, 20);
  EXPECT_EQ(boxes[0].h, 20);
  EXPECT_EQ(boxes[1].x, 0);   // clamped
  EXPECT_EQ(boxes[1].y, 0);
  EXPECT_EQ(boxes[1].w, 50);
  EXPECT_EQ(boxes[2].x, 600);
  EXPECT_EQ(boxes[2].w, 39);  // 639-600 after clamp
}

TEST(MaskPolicyTest, ValidateBoxesEmptyOnNullZeroDimsOrAllInvalid) {
  using aibox::face::RawBox;
  EXPECT_TRUE(aibox::face::ValidateBoxes(nullptr, 3, 640, 360).empty());
  const RawBox raw[] = {{1, 1, 2, 2}};
  EXPECT_TRUE(aibox::face::ValidateBoxes(raw, 1, 0, 360).empty());
  EXPECT_TRUE(aibox::face::ValidateBoxes(raw, 1, 640, 0).empty());
  const RawBox bad[] = {{std::numeric_limits<float>::infinity(), 0, 1, 1}};
  EXPECT_TRUE(aibox::face::ValidateBoxes(bad, 1, 640, 360).empty());
}
