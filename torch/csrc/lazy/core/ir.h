#pragma once

#include <ATen/core/symbol.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <c10/core/ScalarType.h>
#include <c10/util/ArrayRef.h>
#include <torch/csrc/lazy/core/hash.h>
#include <torch/csrc/lazy/core/shape.h>
#include <torch/csrc/lazy/core/ir_metadata.h>
#include <c10/util/Flags.h>

C10_DECLARE_bool(ltc_enable_dynamic_shapes);

namespace torch {
namespace lazy {

static const hash_t kHashSeed(static_cast<uint32_t>(0x5a2d296e9));

class Node;
struct Output;
struct Value;

using NodePtr = std::shared_ptr<Node>;

// The Kind of operation a Node can be associated to.
struct TORCH_API OpKind {
  OpKind() = default;
  explicit OpKind(c10::Symbol op) : op(op) {}

  bool operator==(const OpKind& rhs) const {
    return op == rhs.op;
  }
  bool operator!=(const OpKind& rhs) const {
    return !operator==(rhs);
  }
  bool operator<(const OpKind& rhs) const {
    return c10::unique_t(op) < c10::unique_t(rhs.op);
  }

  hash_t hash() const;

  std::string ToString() const {
    return op.toQualString();
  }

  // Retrieves an existing operation object, or creates a new one. Operations
  // that are specific to lazy tensors, should live within the 'lazy_tensors::'
  // namespace.
  static OpKind Get(const std::string& name);

  c10::Symbol op;
};

inline std::ostream& operator<<(std::ostream& stream, const OpKind& op) {
  stream << op.ToString();
  return stream;
}

using OpList = c10::ArrayRef<Value>;

hash_t OperandHashes(const OpList& operands, const hash_t& seed, bool bakeInSizes);
// A node in the graph. Nodes for operations which require extra data to be
// stored for lowering should inherit from this class and add an operation
// specific member there. For example, a constant might create a new
// NodeConstant class (inheriting from Node) with an extra lazy_tensors::Literal
// field, or a tensor value might create a new NodeTensor with a computation
// client data handle in it.
class TORCH_API Node {
 public:
  static bool enableDynamicShape();

  // Creates a new node with the given op name. The op is a unique identifier
  // for the operation. The num_outputs tells how many outputs a given operation
  // generates.
  //
  // None leaf node's node_hash does not contains shape information always.
  // So we pass in the hash value rather than a function.
  Node(OpKind op, size_t num_outputs, hash_t node_hash, std::function<hash_t(bool)> dag_hash_fn);

  // Contructor used to create leaf nodes.
  Node(OpKind op, size_t num_outputs, std::function<hash_t(bool)> node_hash_fn);

  // Construct node with operands and shapes
  Node(OpKind op, OpList operands, std::vector<Shape>&& shapes,
       size_t num_outputs = 1, hash_t hash_seed = kHashSeed);

  // Construct node with operands and no shape
  Node(OpKind op, OpList operands, size_t num_outputs = 1,
       hash_t hash_seed = kHashSeed);

  // Construct node with shape and no operands
  Node(OpKind op, Shape shape, size_t num_outputs = 1,
       hash_t hash_seed = kHashSeed);

  virtual ~Node();

  static hash_t GetOpHash(OpKind op, const Shape& shape, hash_t hash_seed, bool bakeInSizes);

  const OpKind& op() const {
    return op_;
  }

  size_t num_outputs() const {
    return num_outputs_;
  }

  // Retrieves the full shape of the IR Node.
  virtual c10::ArrayRef<Shape> shapes() const;

  // Retrieves the shape of the output at a given index.
  virtual const Shape& shape(size_t output_index = 0) const;

  virtual const std::vector<Output>& operands() const;

  virtual const Output& operand(size_t i) const;

  hash_t node_hash() const {
    return node_hash_;
  }

  hash_t hash() const {
    return enableDynamicShape() ? dag_hash_without_sizes_ : dag_hash_with_sizes_;
  }

  hash_t hash_without_sizes() const {
    return dag_hash_without_sizes_;
  }

  hash_t hash_with_sizes() const {
    return dag_hash_with_sizes_;
  }

  const MetaData& metadata() const {
    return metadata_;
  }

  UserMetaData* user_metadata() const {
    return user_metadata_.get();
  }

  std::shared_ptr<UserMetaData> SetUserMetadata(
      std::shared_ptr<UserMetaData> user_meta) {
    std::swap(user_metadata_, user_meta);
    return user_meta;
  }

  virtual std::string ToString() const;

 private:
  // The ID of the operation captured by this node.
  OpKind op_;
  size_t num_outputs_ = 1;

  // The hash value of this node.
  hash_t node_hash_;
  // dag_hash represents the hash value of the graph rooted at this node. There are 2 variants, one
  // with sizes info and one without. We need 2 such hashes to support dynamic
  // shape. Here are the logic to pick the hash in the 2 major scenarios that a hash is needed:
  // - shape cache: in this case, we always use the dag hash with size info. This way, looking up the
  //   shape for one node does not get the shape for another node with the same rank but different sizes
  // - lookup the compiled graph by a hash: in this case, we will use the dag hash
  //   WITHOUT size info if dynamic shape is enabled and use the dag hash WITH size info otherwise.
  // The different requirement for the hash in these 2 scenarios forces us to maintain 2
  // different hashes.
  hash_t dag_hash_without_sizes_;
  hash_t dag_hash_with_sizes_;
  // The IR specific metadata attached to the IR node.
  MetaData metadata_;
  // The IR framework user can attach a user defined metadata object deriving
  // from UserMetaData.
  std::shared_ptr<UserMetaData> user_metadata_;

protected:
  // Adds node's index output number as operand.
  void AddOperand(NodePtr node, size_t index = 0);

  std::vector<Shape> shapes_;
  // A node holds a real reference to its operands.
  std::vector<NodePtr> operands_;
  // Outputs do not hold references on the nodes, and neither do the uses, since
  // otherwise we get into circular reference counting.
  std::vector<Output> operands_as_outputs_;
};



inline std::ostream& operator<<(std::ostream& stream, const Node& node) {
  stream << node.ToString();
  return stream;
}

// TODO(alanwaketan): Support r-value reference argument type.
template <typename T, typename... Args>
NodePtr MakeNode(Args&&... args) {
  return std::make_shared<T>(std::forward<Args>(args)...);
}

template <typename T>
const T* NodeCast(const Node* node, OpKind op) {
  if (op != node->op()) {
    return nullptr;
  }
#ifdef NDEBUG
  return static_cast<const T*>(node);
#else
  return &dynamic_cast<const T&>(*node);
#endif
}


// Represents a specific output produced by a node. Since the output of a node
// can be composed by multiple outputs, the node+index coordinates fully qualify
// each single output.
struct TORCH_API Output {
  struct Hasher {
    size_t operator()(const Output& output) const;
  };

  Output() = default;
  explicit Output(const Node* node, size_t index = 0)
      : node(node), index(index) {}

  hash_t hash() const;

  bool operator==(const Output& rhs) const {
    return node == rhs.node && index == rhs.index;
  }
  bool operator!=(const Output& rhs) const {
    return !operator==(rhs);
  }

  const Shape& shape() const {
    return node->shape(index);
  }

  std::string ToString() const;

  // The node providing the output.
  const Node* node{nullptr};
  // The index in the node's output this output refers to.
  size_t index{0};
};

inline std::ostream& operator<<(std::ostream& stream, const Output& output) {
  stream << output.ToString();
  return stream;
}

template <typename T>
using OutputMap = std::unordered_map<Output, T, Output::Hasher>;

// Represents an input/operand for a Node object.
struct TORCH_API Value {
  Value() = default;
  /* implicit */ Value(NodePtr&& node, size_t index = 0) : node(std::move(node)), index(index) {}
  /* implicit */ Value(const NodePtr& node, size_t index = 0) : node(node), index(index) {}

  hash_t hash() const;
  hash_t hash_with_sizes() const;
  hash_t hash_without_sizes() const;

  operator bool() const {
    return node != nullptr;
  }

  operator Output() const {
    return Output(node.get(), index);
  }

  const Shape& shape() const {
    return node->shape(index);
  }

  Node* operator->() const {
    return node.get();
  }

  NodePtr node;
  size_t index = 0;
};

} // namespace lazy
} // namespace torch

namespace c10 {
  // Explicit template instantiation to make ArrayRef<Value> work
  template class at::ArrayRef<torch::lazy::Value>;
}
