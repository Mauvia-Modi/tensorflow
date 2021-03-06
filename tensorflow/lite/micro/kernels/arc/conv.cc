/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/lite/kernels/internal/reference/conv.h"

#include "mli_api.h"  // NOLINT
#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/reference/integer_ops/conv.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/padding.h"
#include "tensorflow/lite/micro/kernels/arc/mli_tf_utils.h"

namespace tflite {
namespace ops {
namespace micro {
namespace conv {

constexpr int kInputTensor = 0;
constexpr int kFilterTensor = 1;
constexpr int kBiasTensor = 2;
constexpr int kOutputTensor = 0;
constexpr int kMaxChannels = 256;

// This file has 2 implementation of Conv.

const int kTensorNotAllocated = -1;

struct OpData {
  TfLitePaddingValues padding;
  // The scaling factor from input to output (aka the 'real multiplier') can
  // be represented as a fixed point multiplier plus a left shift.
  int32_t output_multiplier;
  int output_shift;

  // Per channel output multiplier and shift.
  // TODO(b/141139247): Allocate these dynamically when possible.
  int32_t per_channel_output_multiplier[kMaxChannels];
  int32_t per_channel_output_shift[kMaxChannels];

  // The range of the fused activation layer. For example for kNone and
  // uint8_t these would be 0 and 255.
  int32_t output_activation_min;
  int32_t output_activation_max;
};

inline PaddingType RuntimePaddingType(TfLitePadding padding) {
  switch (padding) {
    case TfLitePadding::kTfLitePaddingSame:
      return PaddingType::kSame;
    case TfLitePadding::kTfLitePaddingValid:
      return PaddingType::kValid;
    case TfLitePadding::kTfLitePaddingUnknown:
    default:
      return PaddingType::kNone;
  }
}

TfLiteStatus CalculateOpData(TfLiteContext* context, TfLiteNode* node,
                             TfLiteConvParams* params, int width, int height,
                             int filter_width, int filter_height, int out_width,
                             int out_height, const TfLiteType data_type,
                             OpData* data) {
  bool has_bias = node->inputs->size == 3;
  // Check number of inputs/outputs
  TF_LITE_ENSURE(context, has_bias || node->inputs->size == 2);
  TF_LITE_ENSURE_EQ(context, node->outputs->size, 1);

  // Matching GetWindowedOutputSize in TensorFlow.
  auto padding = params->padding;
  data->padding = ComputePaddingHeightWidth(
      params->stride_height, params->stride_width,
      params->dilation_height_factor, params->dilation_width_factor, height,
      width, filter_height, filter_width, padding, &out_height, &out_width);

  // Note that quantized inference requires that all tensors have their
  // parameters set. This is usually done during quantized training.
  if (data_type != kTfLiteFloat32) {
    const TfLiteTensor* input = GetInput(context, node, kInputTensor);
    const TfLiteTensor* filter = GetInput(context, node, kFilterTensor);
    const TfLiteTensor* bias =
        GetOptionalInputTensor(context, node, kBiasTensor);
    TfLiteTensor* output = GetOutput(context, node, kOutputTensor);

    TF_LITE_ENSURE_STATUS(tflite::PopulateConvolutionQuantizationParams(
        context, input, filter, bias, output, params->activation,
        &data->output_multiplier, &data->output_shift,
        &data->output_activation_min, &data->output_activation_max,
        data->per_channel_output_multiplier,
        reinterpret_cast<int*>(data->per_channel_output_shift)));
  }
  return kTfLiteOk;
}

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  return nullptr;
}

void Free(TfLiteContext* context, void* buffer) {}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  return kTfLiteOk;
}

void EvalQuantized(TfLiteContext* context, TfLiteNode* node,
                   TfLiteConvParams* params, OpData* data,
                   const TfLiteTensor* input, const TfLiteTensor* filter,
                   const TfLiteTensor* bias, TfLiteTensor* im2col,
                   TfLiteTensor* hwcn_weights, TfLiteTensor* output) {
  const int32_t input_offset = -input->params.zero_point;
  const int32_t filter_offset = -filter->params.zero_point;
  const int32_t output_offset = output->params.zero_point;

  ConvParams op_params;
  op_params.padding_type = RuntimePaddingType(params->padding);
  op_params.padding_values.width = data->padding.width;
  op_params.padding_values.height = data->padding.height;
  op_params.stride_width = params->stride_width;
  op_params.stride_height = params->stride_height;
  op_params.dilation_width_factor = params->dilation_width_factor;
  op_params.dilation_height_factor = params->dilation_height_factor;
  op_params.input_offset = input_offset;
  op_params.weights_offset = filter_offset;
  op_params.output_offset = output_offset;
  op_params.output_multiplier = data->output_multiplier;
  op_params.output_shift = -data->output_shift;
  op_params.quantized_activation_min = data->output_activation_min;
  op_params.quantized_activation_max = data->output_activation_max;
  reference_ops::Conv(op_params, GetTensorShape(input),
                      GetTensorData<uint8_t>(input), GetTensorShape(filter),
                      GetTensorData<uint8_t>(filter), GetTensorShape(bias),
                      GetTensorData<int32_t>(bias), GetTensorShape(output),
                      GetTensorData<uint8_t>(output), GetTensorShape(im2col),
                      GetTensorData<uint8_t>(im2col), nullptr);
}

void EvalQuantizedPerChannel(TfLiteContext* context, TfLiteNode* node,
                             TfLiteConvParams* params, OpData* data,
                             const TfLiteTensor* input,
                             const TfLiteTensor* filter,
                             const TfLiteTensor* bias, TfLiteTensor* output,
                             TfLiteTensor* im2col) {
  // Run Conv MLI kernel
  // MLI optimized version only supports int8 dataype and dilation factor of 1
  if ((input->type == kTfLiteInt8) && (params->dilation_width_factor == 1) &&
      (params->dilation_height_factor == 1)) {
    mli_tensor mli_in = {0};
    mli_tensor mli_weights = {0};
    mli_tensor mli_bias = {0};
    mli_tensor mli_out = {0};
    mli_conv2d_cfg cfg = {};

    // reuse space allocated for OpData parameters
    mli_weights.el_params.asym.scale.pi16 =
        (int16_t*)data->per_channel_output_multiplier;
    mli_bias.el_params.asym.scale.pi16 =
        (int16_t*)data->per_channel_output_shift;

    int16_t filter_zero_point = 0;
    int16_t bias_zero_point = 0;
    mli_weights.el_params.asym.zero_point.pi16 = &filter_zero_point;
    mli_bias.el_params.asym.zero_point.pi16 = &bias_zero_point;

    ConvertToMliTensor<int8_t>(input, &mli_in);
    ConvertToMliTensorPerChannel<int8_t>(filter, &mli_weights);
    ConvertToMliTensorPerChannel<int32_t>(bias, &mli_bias);
    ConvertToMliTensor<int8_t>(output, &mli_out);

    if (params->activation == kTfLiteActRelu) {
      cfg.relu.type = MLI_RELU_GEN;
    } else if (params->activation == kTfLiteActRelu6) {
      cfg.relu.type = MLI_RELU_6;
    } else if (params->activation == kTfLiteActRelu1) {
      cfg.relu.type = MLI_RELU_1;
    } else {
      cfg.relu.type = MLI_RELU_NONE;
    }

    cfg.stride_width = params->stride_width;
    cfg.stride_height = params->stride_height;
    if (params->padding == kTfLitePaddingValid) {
      cfg.padding_left = 0;
      cfg.padding_right = 0;
      cfg.padding_top = 0;
      cfg.padding_bottom = 0;
    } else {
      cfg.padding_left = data->padding.width;
      cfg.padding_right = data->padding.width + data->padding.width_offset;
      cfg.padding_top = data->padding.height;
      cfg.padding_bottom = data->padding.height + data->padding.height_offset;
    }

    mli_point_to_subtsr_cfg substr_cfg_in = {
        {0, 0}, 2, static_cast<uint8_t>(mli_in.shape[1])};
    mli_point_to_subtsr_cfg substr_cfg_out = {
        {0, 0}, 2, static_cast<uint8_t>(mli_out.shape[1])};
    mli_tensor sub_mli_in = {0};
    mli_tensor sub_mli_out = {0};

    const int batches =
        MatchingDim(GetTensorShape(input), 0, GetTensorShape(output), 0);

    for (int i = 0; i < batches; i++) {
      substr_cfg_in.start_coord[0] = i;
      substr_cfg_out.start_coord[0] = i;
      mli_hlp_point_to_subtensor(&mli_in, &substr_cfg_in, &sub_mli_in);
      mli_hlp_point_to_subtensor(&mli_out, &substr_cfg_out, &sub_mli_out);

      mli_krn_conv2d_hwc_sa8_sa8_sa32(&sub_mli_in, &mli_weights, &mli_bias,
                                      &cfg, &sub_mli_out);
    }
  } else {
    ConvParams op_params;
    op_params.input_offset = -input->params.zero_point;
    op_params.output_offset = output->params.zero_point;
    op_params.stride_height = params->stride_height;
    op_params.stride_width = params->stride_width;
    op_params.dilation_height_factor = params->dilation_height_factor;
    op_params.dilation_width_factor = params->dilation_width_factor;
    op_params.padding_values.height = data->padding.height;
    op_params.padding_values.width = data->padding.width;

    reference_integer_ops::ConvPerChannel(
        op_params, data->per_channel_output_multiplier,
        data->per_channel_output_shift, GetTensorShape(input),
        GetTensorData<int8>(input), GetTensorShape(filter),
        GetTensorData<int8>(filter), GetTensorShape(bias),
        GetTensorData<int32>(bias), GetTensorShape(output),
        GetTensorData<int8>(output));
  }
}

void EvalFloat(TfLiteContext* context, TfLiteNode* node,
               TfLiteConvParams* params, OpData* data,
               const TfLiteTensor* input, const TfLiteTensor* filter,
               const TfLiteTensor* bias, TfLiteTensor* im2col,
               TfLiteTensor* hwcn_weights, TfLiteTensor* output) {
  float output_activation_min, output_activation_max;
  CalculateActivationRange(params->activation, &output_activation_min,
                           &output_activation_max);

  ConvParams op_params;
  op_params.padding_type = RuntimePaddingType(params->padding);
  op_params.padding_values.width = data->padding.width;
  op_params.padding_values.height = data->padding.height;
  op_params.stride_width = params->stride_width;
  op_params.stride_height = params->stride_height;
  op_params.dilation_width_factor = params->dilation_width_factor;
  op_params.dilation_height_factor = params->dilation_height_factor;
  op_params.float_activation_min = output_activation_min;
  op_params.float_activation_max = output_activation_max;

  reference_ops::Conv(op_params, GetTensorShape(input),
                      GetTensorData<float>(input), GetTensorShape(filter),
                      GetTensorData<float>(filter), GetTensorShape(bias),
                      GetTensorData<float>(bias), GetTensorShape(output),
                      GetTensorData<float>(output), GetTensorShape(im2col),
                      GetTensorData<float>(im2col));
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  auto* params = reinterpret_cast<TfLiteConvParams*>(node->builtin_data);

  TfLiteTensor* output = GetOutput(context, node, kOutputTensor);
  const TfLiteTensor* input = GetInput(context, node, kInputTensor);
  const TfLiteTensor* filter = GetInput(context, node, kFilterTensor);
  const TfLiteTensor* bias = GetOptionalInputTensor(context, node, kBiasTensor);

  int input_width = input->dims->data[2];
  int input_height = input->dims->data[1];
  int filter_width = filter->dims->data[2];
  int filter_height = filter->dims->data[1];
  int output_width = output->dims->data[2];
  int output_height = output->dims->data[1];

  OpData data;

  // All per-channel quantized tensors need valid zero point and scale arrays.
  if (input->type == kTfLiteInt8) {
    TF_LITE_ENSURE_EQ(context, filter->quantization.type,
                      kTfLiteAffineQuantization);

    const auto* affine_quantization =
        reinterpret_cast<TfLiteAffineQuantization*>(
            filter->quantization.params);
    TF_LITE_ENSURE(context, affine_quantization);
    TF_LITE_ENSURE(context, affine_quantization->scale);
    TF_LITE_ENSURE(context, affine_quantization->zero_point);
    // Conv is quantized along dimension 0:
    // https://www.tensorflow.org/lite/performance/quantization_spec
    TF_LITE_ENSURE_EQ(context, filter->dims->data[0],
                      affine_quantization->scale->size);
    TF_LITE_ENSURE_EQ(context, filter->dims->data[0],
                      affine_quantization->zero_point->size);
  }

  TF_LITE_ENSURE_STATUS(CalculateOpData(
      context, node, params, input_width, input_height, filter_width,
      filter_height, output_width, output_height, input->type, &data));

  switch (input->type) {  // Already know in/out types are same.
    case kTfLiteFloat32:
      EvalFloat(context, node, params, &data, input, filter, bias, nullptr,
                nullptr, output);
      break;
    case kTfLiteInt8:
      EvalQuantizedPerChannel(context, node, params, &data, input, filter, bias,
                              output, nullptr);
      break;
    case kTfLiteUInt8:
      EvalQuantized(context, node, params, &data, input, filter, bias, nullptr,
                    nullptr, output);
      break;
    default:
      TF_LITE_KERNEL_LOG(context, "Type %s (%d) not supported.",
                         TfLiteTypeGetName(input->type), input->type);
      return kTfLiteError;
  }
  return kTfLiteOk;
}

}  // namespace conv

TfLiteRegistration* Register_CONV_2D() {
  static TfLiteRegistration r = {conv::Init, conv::Free, conv::Prepare,
                                 conv::Eval};
  return &r;
}

}  // namespace micro
}  // namespace ops
}  // namespace tflite
