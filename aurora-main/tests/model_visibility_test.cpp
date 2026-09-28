// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx_test_common.hpp"
#include "gx/model_visibility.hpp"
#include <aurora/aurora.h>

namespace {
class HiddenModelTest : public GXFifoTest {
protected:
  void SetUp() override {
    GXFifoTest::SetUp();
    flush_and_capture();
    aurora::gx::hiddenModelArrays.clear();
    aurora::gx::hiddenModelDraws = 0;
    aurora::gx::hiddenModelLastDraws = 0;
    auto& state = gxState();
    state.lastVtxFmt = GX_VTXFMT0;
    state.lastVtxSize = 1;
    state.projType = GX_PERSPECTIVE;
    state.vtxDesc[GX_VA_POS] = GX_INDEX8;
    state.arrays[GX_VA_POS].data = source.data();
    state.arrays[GX_VA_POS].size = source.size();
    state.arrays[GX_VA_POS].stride = 12;
    std::memcpy(pos(), matrix.data(), sizeof(float) * 12);
    state.stateDirty = true;
    aurora_hide_model_array(source.data(), matrix.data());
  }
  void TearDown() override { aurora_clear_hidden_model_arrays(); }
  float* pos() { return reinterpret_cast<float*>(&gxState().pnMtx[0].pos); }
  void draw() { decode_fifo({static_cast<u8>(GX_TRIANGLES), 0, 3, 0, 1, 2}); }
  std::array<uint8_t, 36> source{};
  std::array<float, 12> matrix{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
};

TEST_F(HiddenModelTest, LocalBodyIsSkippedBeforeVertexUploadAndClearRestoresIt) {
  draw();
  EXPECT_TRUE(aurora::gfx::testing::last_pushed_vertices().empty());
  aurora_clear_hidden_model_arrays();
  EXPECT_EQ(aurora_hidden_model_draw_count(), 1u);
  draw();
  EXPECT_FALSE(aurora::gfx::testing::last_pushed_vertices().empty());
}

TEST_F(HiddenModelTest, OpponentUsingSameArrayIsDrawn) {
  pos()[3] += 100;
  draw();
  EXPECT_FALSE(aurora::gfx::testing::last_pushed_vertices().empty());
  EXPECT_EQ(aurora::gx::hiddenModelDraws, 0u);
}

TEST_F(HiddenModelTest, ArmsAndIndexedJointsAreKept) {
  std::array<uint8_t, 36> arms{};
  gxState().arrays[GX_VA_POS].data = arms.data();
  EXPECT_FALSE(aurora::gx::model_array_hidden());
  gxState().arrays[GX_VA_POS].data = source.data();
  gxState().vtxDesc[GX_VA_PNMTXIDX] = GX_DIRECT;
  EXPECT_FALSE(aurora::gx::model_array_hidden());
}

TEST_F(HiddenModelTest, DirectPositionsOrthographicAndInvalidMatricesAreKept) {
  gxState().vtxDesc[GX_VA_POS] = GX_DIRECT;
  EXPECT_FALSE(aurora::gx::model_array_hidden());
  gxState().vtxDesc[GX_VA_POS] = GX_INDEX8;
  gxState().projType = GX_ORTHOGRAPHIC;
  EXPECT_FALSE(aurora::gx::model_array_hidden());
  gxState().projType = GX_PERSPECTIVE;
  pos()[0] = __builtin_nanf("");
  EXPECT_FALSE(aurora::gx::model_array_hidden());
}

TEST_F(HiddenModelTest, RawBridgeDrawAlsoSkipsLocalBody) {
  const std::array<uint8_t, 3> vertices{0, 1, 2};
  ASSERT_TRUE(aurora::gx::fifo::submit_raw_draw(GX_TRIANGLES, GX_VTXFMT0, vertices.data(), 3, vertices.size()));
  EXPECT_TRUE(aurora::gfx::testing::last_pushed_vertices().empty());
  EXPECT_EQ(aurora::gx::hiddenModelDraws, 1u);
}
} // namespace
