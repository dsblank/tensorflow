/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/gpu/cublas_gemm_pad_for_tensor_cores.h"

#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"
#include "tensorflow/compiler/xla/service/hlo_matchers.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/tests/test_utils.h"
#include "tensorflow/compiler/xla/util.h"

namespace op = xla::testing::opcode_matchers;

namespace xla {
namespace gpu {
namespace {

class CublasGemmPadForTensorCoresTest : public HloTestBase {};

TEST_F(CublasGemmPadForTensorCoresTest, OneDotRootComputation) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %param1 = f16[2048,1024] parameter(0)
    %param2 = f16[1024,33708] parameter(1)
    ROOT %dot.2309 = f16[2048,33708]{1,0} dot(f16[2048,1024]{1,0} %param1,
                f16[1024,33708]{0,1} %param2),
                lhs_contracting_dims={1}, rhs_contracting_dims={0}
                })")
                    .ValueOrDie();

  EXPECT_TRUE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
  SCOPED_TRACE(module->ToString());

  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(
      root,
      AllOf(
          op::Shape("f16[2048, 33708]"),
          op::Slice(AllOf(
              op::Shape("f16[2048, 33712]"),
              op::Dot(AllOf(op::Shape("f16[2048, 1024]"),
                            op::Pad(AllOf(op::Shape("f16[2048, 1024]"),
                                          op::Parameter()),
                                    AllOf(op::Shape("f32[]"), op::Constant()))),
                      AllOf(op::Shape("f16[1024, 33712]"),
                            op::Pad(AllOf(op::Shape("f16[1024, 33708]"),
                                          op::Parameter()),
                                    AllOf(op::Shape("f32[]"), op::Constant()))),
                      /*lhs_contracting_dim=*/1,
                      /*rhs_contracting_dim=*/0)))));
}

TEST_F(CublasGemmPadForTensorCoresTest, TwoDotsComputation) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %param1 = f16[2048,1024] parameter(0)
    %param2 = f16[1024,33708] parameter(1)
    %param3 = f16[33708, 1] parameter(2)
    %dot1 = f16[2048,33708]{1,0} dot(f16[2048,1024]{1,0} %param1,
                f16[1024,33708]{0,1} %param2),
                lhs_contracting_dims={1}, rhs_contracting_dims={0}
    ROOT %dot2 = f16[2048, 1]{1,0} dot(f16[2048,33708]{1,0} %dot1,
                f16[33708, 1]{0,1} %param3),
                lhs_contracting_dims={1}, rhs_contracting_dims={0}
  })")
                    .ValueOrDie();

  EXPECT_TRUE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
  SCOPED_TRACE(module->ToString());

  auto* root = module->entry_computation()->root_instruction();
  EXPECT_THAT(
      root,
      AllOf(
          op::Shape("f16[2048, 1]"),
          op::Slice(AllOf(
              op::Shape("f16[2048, 8]"),
              op::Dot(
                  AllOf(
                      op::Shape("f16[2048, 33712]"),
                      AllOf(
                          op::Shape("f16[2048, 33712]"),
                          AllOf(
                              op::Shape("f16[2048, 33712]"),
                              op::Pad(
                                  AllOf(op::Shape("f16[2048, 33708]"),
                                        op::Slice(AllOf(
                                            op::Shape("f16[2048, 33712]"),
                                            op::Dot(
                                                AllOf(op::Shape(
                                                          "f16[2048, 1024]"),
                                                      op::Pad()),
                                                AllOf(op::Shape(
                                                          "f16[1024, 33712]"),
                                                      op::Pad()),
                                                1, 0)))),
                                  AllOf(op::Shape("f32[]"), op::Constant()))))),
                  AllOf(op::Shape("f16[33712, 8]"),
                        AllOf(op::Shape("f16[33712, 8]"),
                              op::Pad(
                                  AllOf(op::Shape("f16[33708, 1]"),
                                        op::Parameter()),
                                  AllOf(op::Shape("f32[]"), op::Constant())))),
                  /*lhs_contracting_dim=*/1, /*rhs_contracting_dim=*/0)))));

  auto* dot2 = root->operand(0)->operand(0)->operand(0)->operand(0);
  EXPECT_THAT(
      dot2,
      AllOf(op::Dot(
          AllOf(op::Shape("f16[2048, 1024]"),
                op::Pad(AllOf(op::Shape("f16[2048, 1024]"), op::Parameter()),
                        AllOf(op::Shape("f32[]"), op::Constant()))),
          AllOf(op::Shape("f16[1024, 33712]"),
                op::Pad(AllOf(op::Shape("f16[1024, 33708]"), op::Parameter()),
                        AllOf(op::Shape("f32[]"), op::Constant()))),
          /*lhs_contracting_dim=*/1, /*rhs_contracting_dim=*/0)));
}

TEST_F(CublasGemmPadForTensorCoresTest, NoDotComputation) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %x = f32[] parameter(0)
    %y = f32[] parameter(1)
    ROOT %maximum = f32[] maximum(f32[] %x, f32[] %y)
  })")
                    .ValueOrDie();

  EXPECT_FALSE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
}

TEST_F(CublasGemmPadForTensorCoresTest, F32DotComputation) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %param1 = f32[2048,1024] parameter(0)
    %param2 = f32[1024,33708] parameter(1)
    ROOT %dot.2309 = f32[2048,33708]{1,0} dot(f32[2048,1024]{1,0} %param1,
                f32[1024,33708]{0,1} %param2),
                lhs_contracting_dims={1}, rhs_contracting_dims={0}})")
                    .ValueOrDie();

  EXPECT_FALSE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
}

TEST_F(CublasGemmPadForTensorCoresTest, F64DotComputation) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %param1 = f64[2048,1024] parameter(0)
    %param2 = f64[1024,33708] parameter(1)
    ROOT %dot.2309 = f64[2048,33708]{1,0} dot(f64[2048,1024]{1,0} %param1,
                f64[1024,33708]{0,1} %param2),
                lhs_contracting_dims={1}, rhs_contracting_dims={0}})")
                    .ValueOrDie();

  EXPECT_FALSE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
}

TEST_F(CublasGemmPadForTensorCoresTest, MultiplesOf8DotComputation) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %param1 = f16[2048,1024] parameter(0)
    %param2 = f16[1024,33712] parameter(1)
    ROOT %dot.2309 = f16[2048,33712]{1,0} dot(f16[2048,1024]{1,0} %param1,
                f16[1024,33712]{0,1} %param2),
                lhs_contracting_dims={1}, rhs_contracting_dims={0}})")
                    .ValueOrDie();

  EXPECT_FALSE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
}

TEST_F(CublasGemmPadForTensorCoresTest, CheckSavingMetadata) {
  auto module = ParseAndReturnVerifiedModule(R"(
  HloModule TestModule

  ENTRY TestComputation {
    %param1 = f16[2048,1024] parameter(0)
    %param2 = f16[1024,33708] parameter(1)
    ROOT %dot.2309 = f16[2048,33708]{1,0} dot(f16[2048,1024]{1,0} %param1,
                f16[1024,33708]{0,1} %param2),
                lhs_contracting_dims={1}, rhs_contracting_dims={0},
                metadata={op_type="MatMul" op_name="transformer_v2/Transformer/decode/embedding_shared_weights_1/presoftmax_linear/MatMul"}
                })")
                    .ValueOrDie();

  SCOPED_TRACE(module->ToString());

  EXPECT_TRUE(CublasGemmPadForTensorCores().Run(module.get()).ValueOrDie());
  auto metadata = module->entry_computation()->root_instruction()->metadata();
  EXPECT_EQ("MatMul", metadata.op_type());
  EXPECT_EQ(
      "transformer_v2/Transformer/decode/embedding_shared_weights_1/"
      "presoftmax_linear/MatMul",
      metadata.op_name());
}

}  // anonymous namespace
}  // namespace gpu
}  // namespace xla
