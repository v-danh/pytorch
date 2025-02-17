#include <c10/util/irange.h>
#include <torch/csrc/lazy/core/shape.h>
#include <torch/csrc/lazy/core/tensor.h>

C10_DEFINE_bool(
    ltc_enable_symbolic_shapes,
    false,
    "Enables calculation of if dims are symbolic");

namespace torch {
namespace lazy {

Shape::Shape(at::ScalarType scalar_type, c10::ArrayRef<int64_t> sizes)
    : scalar_type_(scalar_type), sizes_(sizes.begin(), sizes.end()) {}

std::string Shape::to_string() const {
  return c10::str(toString(scalar_type_), "[", c10::Join(",", sizes_), "]");
}

bool Shape::operator==(const Shape& other) const {
  return scalar_type_ == other.scalar_type_ && sizes_ == other.sizes_;
}

std::ostream& operator<<(std::ostream& out, const Shape& shape) {
  return out << shape.to_string();
}

size_t Shape::numel() const {
  size_t elts = 1;
  for (auto size : sizes_) {
    elts *= size;
  }
  return elts;
}

hash_t Shape::hash(bool bakeInSizes) const {
  if (bakeInSizes) {
    return HashCombine(
        Hash(scalar_type_),
        DataHash(sizes_.data(), sizes_.size() * sizeof(int64_t)));
  } else {
    return HashCombine(Hash(scalar_type_), Hash(sizes_.size()));
  }
}

Shape Shape::with_symbolic_dims(
    c10::optional<std::vector<bool>> symbolic_dims) const {
  Shape copy = *this;
  copy.is_symbolic_ = symbolic_dims;
  return copy;
}

bool symbolicShapeEnabled() {
  static bool enabled = std::getenv("LTC_ENABLE_SYMBOLIC_SHAPES") != nullptr;
  return enabled || FLAGS_ltc_enable_symbolic_shapes;
}

c10::SymbolicShape get_symbolic_shape(at::Tensor& tensor) {
  auto ltc_tensor = TryGetLtcTensor(tensor);
  if (!ltc_tensor) {
    // Set Concrete sizes for Concrete tensors
    return c10::SymbolicShape(tensor.sizes());
  }
  const Shape& input_shape = ltc_tensor->GetIrValue()->shape();
  auto& is_symbolic = input_shape.is_symbolic();
  if (!is_symbolic.has_value()) {
    return c10::SymbolicShape();
  }
  auto sizes = input_shape.sizes();
  TORCH_INTERNAL_ASSERT(
      sizes.size() == is_symbolic->size(),
      "Dims of two values are not consistent");
  std::vector<c10::optional<int64_t>> symbolic_dims;
  for (int64_t i = 0; i < sizes.size(); i++) {
    if (is_symbolic->at(i)) {
      symbolic_dims.emplace_back(c10::nullopt);
    } else {
      symbolic_dims.emplace_back(sizes.at(i));
    }
  }
  return c10::SymbolicShape(symbolic_dims);
}

void applySymbolicShapesOnLT(
    const char* schema_str,
    std::vector<c10::IValue> args,
    std::vector<Shape>& result_shapes) {
  std::vector<jit::SSAInput> converted_args;
  // TODO: Determine if there are any unknown values in LazyTensor
  const c10::FunctionSchema& schema =
      jit::getOperatorForLiteral(schema_str)->schema();

  for (auto& arg : args) {
    // Handle list of tensors
    if (arg.isTensorList()) {
      at::List<at::Tensor> tensor_list = arg.toTensorList();
      for (at::Tensor tensor : tensor_list) {
        converted_args.emplace_back(get_symbolic_shape(tensor));
      }
    } else if (arg.isTensor()) {
      auto ss = get_symbolic_shape(arg.toTensor());
      converted_args.emplace_back(ss);
    } else {
      // If we need to support symbolic ints, here is the place
      // to add it.
      converted_args.emplace_back(arg);
    }
  }
  auto res_symbolic = jit::calculateSymbolicShapesOnOp(&schema, converted_args);
  if (!res_symbolic) {
    for (int64_t i = 0; i < res_symbolic->size(); i++) {
      result_shapes[i] = result_shapes[i].with_symbolic_dims(c10::nullopt);
    }
  } else {
    TORCH_INTERNAL_ASSERT(
        res_symbolic->size() == result_shapes.size(),
        "Result shape size is not consistent");
    for (int64_t i = 0; i < res_symbolic->size(); i++) {
      auto sym_dims = res_symbolic->at(i).symbolicDims();
      if (sym_dims.has_value()) {
        result_shapes[i] = result_shapes[i].with_symbolic_dims(*sym_dims);
      }
    }
  }
}

} // namespace lazy
} // namespace torch
