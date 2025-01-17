#pragma once

#include <ATen/Dispatch.h>
#include <ATen/core/Tensor.h>
#include <c10/util/irange.h>

#ifndef AT_PER_OPERATOR_HEADERS
#include <ATen/NativeFunctions.h>
#else
#include <ATen/ops/result_type_native.h>
#endif

namespace at::native {
namespace {
// Check if tensor list has either a boolean tensor or a integer tensor
TORCH_API bool has_integral_tensor(TensorList tensors, const bool includeBool) {
  return std::any_of(
      tensors.begin(), tensors.end(), [&includeBool](const auto& t) {
        return at::isIntegralType(t.scalar_type(), includeBool);
      });
}
// check if tensor list has bool tensors
TORCH_API bool has_bool_tensor(TensorList tensors) {
  return std::any_of(tensors.begin(), tensors.end(), [](const auto& t) -> bool {
    return t.scalar_type() == ScalarType::Bool;
  });
}

// Check foreach API restrictions
// - Tensor lists must be non-empty.
// - All TensorLists and ScalarLists must have the same number of elements.
// - Corresponding tensors must have the same size.
TORCH_API void check_foreach_api_restrictions(TensorList tensors) {
  TORCH_CHECK(!tensors.empty(), "Tensor list must have at least one tensor.");
}

TORCH_API void check_foreach_api_restrictions(
    TensorList tensors,
    ArrayRef<Scalar> scalars) {
  check_foreach_api_restrictions(tensors);
  TORCH_CHECK(
      tensors.size() == scalars.size(),
      "Tensor list must have same number of elements as scalar list.");
}

TORCH_API void check_foreach_api_restrictions(
    TensorList tensors1,
    TensorList tensors2) {
  TORCH_CHECK(!tensors1.empty(), "Tensor list must have at least one tensor.");
  TORCH_CHECK(!tensors2.empty(), "Tensor list must have at least one tensor.");
  TORCH_CHECK(
      tensors1.size() == tensors2.size(),
      "Tensor lists must have the same number of tensors, got ",
      tensors1.size(),
      " and ",
      tensors2.size());
}

TORCH_API void check_foreach_api_restrictions(
    TensorList tensors1,
    TensorList tensors2,
    TensorList tensors3) {
  TORCH_CHECK(!tensors1.empty(), "Tensor list must have at least one tensor.");
  TORCH_CHECK(!tensors2.empty(), "Tensor list must have at least one tensor.");
  TORCH_CHECK(!tensors3.empty(), "Tensor list must have at least one tensor.");
  TORCH_CHECK(
      tensors1.size() == tensors2.size(),
      "Tensor lists must have the same number of tensors, got ",
      tensors1.size(),
      " and ",
      tensors2.size());
  TORCH_CHECK(
      tensors1.size() == tensors3.size(),
      "Tensor lists must have the same number of tensors, got ",
      tensors1.size(),
      " and ",
      tensors3.size());
}

TORCH_API void check_foreach_api_restrictions(
    TensorList tensors1,
    TensorList tensors2,
    TensorList tensors3,
    ArrayRef<Scalar> scalars) {
  check_foreach_api_restrictions(tensors1, tensors2, tensors3);
  TORCH_CHECK(
      tensors1.size() == scalars.size(),
      "Tensor list must have same number of elements as scalar list, got ",
      tensors1.size(),
      " and ",
      scalars.size());
}

// To go via 'fast' path, several conditions must be satisfied
// - All tensors in all lists must have the same dtype.
// - All tensors must be on the same device
// - All tensors must have strided layout
// - All tensors must be non-overlapping and dense
// - Resulting tensor must have the same dtype as the input one

// Please, make sure to call check_foreach_api_restrictions before calling this
// method. There is a set of preconditions that have to be satisfied.
TORCH_API bool check_fast_path_restrictions(
    ArrayRef<TensorList> tensorLists,
    ArrayRef<Scalar> scalarList = {},
    bool does_op_promote_integer_inputs_to_float = false) {
  const auto expected_dtype = tensorLists[0][0].dtype();
  const auto expected_device = tensorLists[0][0].device();

  auto is_tensor_okay = [&](const Tensor& tensor) {
    return tensor.dtype() == expected_dtype &&
        tensor.device() == expected_device && tensor.layout() == at::kStrided &&
        tensor.is_non_overlapping_and_dense();
  };

  for (const auto& tensorList : tensorLists) {
    for (const auto& tensor : tensorList) {
      if (!is_tensor_okay(tensor)) {
        return false;
      }
    }
  }

  // Check if corresponding tensors in tensor lists have the same sizes and
  // strides.
  for (const auto& tensor_list : tensorLists) {
    for (const auto j : c10::irange(tensorLists[0].size())) {
      if (tensorLists[0][j].sizes() != tensor_list[j].sizes()) {
        return false;
      }
      if (tensorLists[0][j].strides() != tensor_list[j].strides()) {
        return false;
      }
    }
  }

  // This function has already checked that `tensorList[j][i]` for all j, i has
  // the same dtype using `is_tensor_okay` function above. This means we only
  // need to check if {tensorList[0][0], tensorList[0][1], tensorList[0][2],
  // ...} do type promotion with scalarLIst.
  for (const auto i : c10::irange(tensorLists[0].size())) {
    // For division, integer inputs will result in float.
    if (does_op_promote_integer_inputs_to_float) {
      if (at::isIntegralType(
              tensorLists[0][i].scalar_type(), /*includeBool*/ true)) {
        return false;
      }
    }
    if (!scalarList.empty()) {
      const auto& scalar =
          scalarList.size() == 1 ? scalarList[0] : scalarList[i];
      const auto& tensor = tensorLists[0][i];
      // note(mkozuki): This check might be responsible for
      // `_foreach_add(bool_tensors, bool_tensors)` being pushed to slow path.
      if (tensor.scalar_type() != at::native::result_type(scalar, tensor)) {
        return false;
      }
    }
  }

  return true;
}

TORCH_API std::vector<c10::Scalar> convert_tensor_to_scalar_list(
    const Tensor& scalarList_,
    int64_t expect_length) {
  std::vector<c10::Scalar> scalarList;
  TORCH_CHECK(
      scalarList_.device() == c10::kCPU,
      "Expected scalars to be on CPU, got ",
      scalarList_.device(),
      " instead.");
  TORCH_CHECK(
      scalarList_.is_contiguous(), "Expected scalars to be contiguous.");
  TORCH_CHECK(
      scalarList_.dim() == 1,
      "Expected packed scalar Tensor to be of dimension 1. Got ",
      scalarList_.dim(),
      " instead.");
  AT_DISPATCH_ALL_TYPES_AND_COMPLEX_AND4(
      kComplexHalf,
      kHalf,
      kBool,
      kBFloat16,
      scalarList_.scalar_type(),
      "convert_tensor_to_scalar_list",
      [&]() {
        const scalar_t* scalar_data = scalarList_.data_ptr<scalar_t>();
        TORCH_CHECK(
            (expect_length == scalarList_.size(0)),
            "Expected length of scalars to match input of length ",
            expect_length,
            " but got ",
            scalarList_.size(0),
            " instead.");
        for (int64_t i = 0; i < scalarList_.size(0); i++) {
          scalarList.push_back(c10::Scalar(scalar_data[i]));
        }
      });
  return scalarList;
}

TORCH_API bool can_use_fast_route(
    ArrayRef<TensorList> tensorLists,
    ArrayRef<Scalar> scalarList = {},
    bool does_op_promote_integer_inputs_to_float = false) {
  return check_fast_path_restrictions(
      tensorLists, scalarList, does_op_promote_integer_inputs_to_float);
}

TORCH_API bool can_use_fast_route(
    TensorList tensors1,
    TensorList tensors2,
    bool does_op_promote_integer_inputs_to_float = false) {
  return can_use_fast_route(
      {tensors1, tensors2}, {}, does_op_promote_integer_inputs_to_float);
}

} // namespace
} // namespace at::native
