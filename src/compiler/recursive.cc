/*!
 * Copyright 2017 by Contributors
 * \file recursive.cc
 * \brief Recursive compiler
 * \author Philip Cho
 */

#include <treelite/common.h>
#include <treelite/compiler.h>
#include <treelite/tree.h>
#include <treelite/semantic.h>
#include <dmlc/data.h>
#include <dmlc/json.h>
#include <queue>
#include <algorithm>
#include <iterator>
#include "param.h"

namespace {

using Annotation = std::vector<std::vector<size_t>>;
class SplitCondition : public treelite::semantic::Condition {
 public:
  using NumericAdapter
    = std::function<std::string(treelite::Tree::Operator, unsigned,
                                treelite::tl_float)>;
  explicit SplitCondition(const treelite::Tree::Node& node,
                          const NumericAdapter& numeric_adapter)
   : split_index(node.split_index()), default_left(node.default_left()),
     op(node.comparison_op()), threshold(node.threshold()),
     numeric_adapter(numeric_adapter) {}
  explicit SplitCondition(const treelite::Tree::Node& node,
                          NumericAdapter&& numeric_adapter)
   : split_index(node.split_index()), default_left(node.default_left()),
     op(node.comparison_op()), threshold(node.threshold()),
     numeric_adapter(std::move(numeric_adapter)) {}
  CLONEABLE_BOILERPLATE(SplitCondition)
  inline std::string Compile() const override {
    const std::string bitmap
      = std::string("data[") + std::to_string(split_index) + "].missing != -1";
    return ((default_left) ?  (std::string("!(") + bitmap + ") || ")
                            : (std::string(" (") + bitmap + ") && "))
            + numeric_adapter(op, split_index, threshold);
  }

 private:
  unsigned split_index;
  bool default_left;
  treelite::Tree::Operator op;
  treelite::tl_float threshold;
  NumericAdapter numeric_adapter;
};

}  // namespace anonymous

namespace treelite {
namespace compiler {

DMLC_REGISTRY_FILE_TAG(recursive);

std::vector<std::vector<tl_float>> ExtractCutPoints(const Model& model);

struct Metadata {
  int num_features;
  std::vector<std::vector<tl_float>> cut_pts;

  inline void Init(const Model& model, bool extract_cut_pts = false) {
    num_features = model.num_features;
    if (extract_cut_pts) {
      cut_pts = std::move(ExtractCutPoints(model));
    }
  }
};

template <typename QuantizePolicy>
class RecursiveCompiler : public Compiler, private QuantizePolicy {
 public:
  explicit RecursiveCompiler(const CompilerParam& param)
    : param(param) {
    LOG(INFO) << "RecursiveCompiler yah";
  }
  
  using CodeBlock = semantic::CodeBlock;
  using PlainBlock = semantic::PlainBlock;
  using FunctionBlock = semantic::FunctionBlock;
  using SequenceBlock = semantic::SequenceBlock;
  using IfElseBlock = semantic::IfElseBlock;

  std::unique_ptr<CodeBlock>
  Export(const Model& model) override {
    Metadata info;
    info.Init(model, QuantizePolicy::QuantizeFlag());
    QuantizePolicy::Init(std::move(info));

    Annotation annotation;
    bool annotate = false;
    if (param.annotate_in != "NULL") {
      std::unique_ptr<dmlc::Stream> fi(dmlc::Stream::Create(
                                       param.annotate_in.c_str(), "r"));
      dmlc::istream is(fi.get());
      auto reader = common::make_unique<dmlc::JSONReader>(&is);
      reader->Read(&annotation);
      annotate = true;
    }

    SequenceBlock sequence;
    sequence.Reserve(model.trees.size() + 3);
    sequence.PushBack(PlainBlock("float sum = 0.0f;"));
    sequence.PushBack(PlainBlock(QuantizePolicy::Preprocessing()));
    for (size_t tree_id = 0; tree_id < model.trees.size(); ++tree_id) {
      const Tree& tree = model.trees[tree_id];
      if (!annotation.empty()) {
        sequence.PushBack(common::MoveUniquePtr(WalkTree(tree,
                                                annotation[tree_id])));
      } else {
        sequence.PushBack(common::MoveUniquePtr(WalkTree(tree, {})));
      }
    }
    sequence.PushBack(PlainBlock("return sum;"));

    FunctionBlock function("float predict_margin(union Entry* data)",
      std::move(sequence));

    auto preamble = QuantizePolicy::Preamble();
    preamble.emplace_back();
    if (annotate) {
      preamble.emplace_back("#define LIKELY(x)     __builtin_expect(!!(x), 1)");
      preamble.emplace_back("#define UNLIKELY(x)   __builtin_expect(!!(x), 0)");
    }

    auto file = common::make_unique<SequenceBlock>();
    file->Reserve(2);
    file->PushBack(PlainBlock(std::move(preamble)));
    file->PushBack(std::move(function));

    return std::move(file);
  }

 private:
  CompilerParam param;
  
  std::unique_ptr<CodeBlock> WalkTree(const Tree& tree,
                                      const std::vector<size_t>& counts) const {
    return WalkTree_(tree, counts, 0);
  }

  std::unique_ptr<CodeBlock> WalkTree_(const Tree& tree,
                                       const std::vector<size_t>& counts,
                                       int nid) const {
    using semantic::LikelyDirection;
    const Tree::Node& node = tree[nid];
    if (node.is_leaf()) {
      const tl_float leaf_value = node.leaf_value();
      return std::unique_ptr<CodeBlock>(new PlainBlock(
        std::string("sum += ") + common::FloatToString(leaf_value) + ";"));
    } else {
      LikelyDirection likely_direction = LikelyDirection::kNone;
      if (!counts.empty()) {
        const size_t left_count = counts[node.cleft()];
        const size_t right_count = counts[node.cright()];
        likely_direction = (left_count > right_count) ? LikelyDirection::kLeft
                                                      : LikelyDirection::kRight;
      }
      return std::unique_ptr<CodeBlock>(new IfElseBlock(
        SplitCondition(node, QuantizePolicy::NumericAdapter()),
        common::MoveUniquePtr(WalkTree_(tree, counts, node.cleft())),
        common::MoveUniquePtr(WalkTree_(tree, counts, node.cright())),
        likely_direction)
      );
    }
  }
};

class MetadataStore {
 protected:
  void Init(const Metadata& info) {
    this->info = info;
  }
  void Init(Metadata&& info) {
    this->info = std::move(info);
  }
  const Metadata& GetInfo() const {
    return info;
  }
  MetadataStore() = default;
  MetadataStore(const MetadataStore& other) = default;
  MetadataStore(MetadataStore&& other) = default;
 private:
  Metadata info;
};

class NoQuantize : private MetadataStore {
 protected:
  template <typename... Args>
  void Init(Args&&... args) {
    MetadataStore::Init(std::forward<Args>(args)...);
  }
  SplitCondition::NumericAdapter NumericAdapter() const {
    return [] (Tree::Operator op, unsigned split_index, tl_float threshold) {
      std::ostringstream oss;
      oss << "data[" << split_index << "].fvalue "
          << semantic::OpName(op) << " " << threshold;
      return oss.str();
    };
  }
  std::vector<std::string> Preamble() const {
    return {"union Entry {",
            "  int missing;",
            "  float fvalue;",
            "};"};
  }
  std::vector<std::string> Preprocessing() const {
    return {};
  }
  bool QuantizeFlag() const {
    return false;
  }
};

class Quantize : private MetadataStore {
 protected:
  template <typename... Args>
  void Init(Args&&... args) {
    MetadataStore::Init(std::forward<Args>(args)...);
    quant_preamble = {
      std::string("for (int i = 0; i < ")
      + std::to_string(GetInfo().num_features) + "; ++i) {",
      "  if (data[i].missing != -1) {",
      "    data[i].qvalue = quantize(data[i].fvalue, i);",
      "  }",
      "}"};
  }
  SplitCondition::NumericAdapter NumericAdapter() const {
    const std::vector<std::vector<tl_float>>& cut_pts = GetInfo().cut_pts; 
    return [&cut_pts] (Tree::Operator op, unsigned split_index,
                       tl_float threshold) {
      std::ostringstream oss;
      const auto& v = cut_pts[split_index];
      auto loc = common::binary_search(v.begin(), v.end(), threshold);
      CHECK(loc != v.end());
      oss << "data[" << split_index << "].qvalue " << semantic::OpName(op)
          << " " << static_cast<size_t>(loc - v.begin()) * 2;
      return oss.str();
    };
  }
  std::vector<std::string> Preamble() const {
    std::vector<std::string> ret{"union Entry {",
                                 "  int missing;",
                                 "  float fvalue;",
                                 "  int qvalue;",
                                 "};"};
    ret.emplace_back("static const float threshold[] = {");
    {
      std::ostringstream oss, oss2;
      size_t length = 2;
      oss << "  ";
      for (const auto& e : GetInfo().cut_pts) {
        for (const auto& value : e) {
          oss2.clear(); oss2.str(std::string()); oss2 << value;
          common::WrapText(&oss, &length, oss2.str(), 80);
        }
      }
      ret.push_back(oss.str());
      ret.emplace_back("};");
    }
    ret.emplace_back("static const int th_begin[] = {");
    {
      std::ostringstream oss, oss2;
      size_t length = 2;
      size_t accum = 0;
      oss << "  ";
      for (const auto& e : GetInfo().cut_pts) {
        oss2.clear(); oss2.str(std::string()); oss2 << accum;
        common::WrapText(&oss, &length, oss2.str(), 80);
        accum += e.size();
      }
      ret.push_back(oss.str());
      ret.emplace_back("};");
    }
    ret.emplace_back("static const int th_len[] = {");
    {
      std::ostringstream oss, oss2;
      size_t length = 2;
      oss << "  ";
      for (const auto& e : GetInfo().cut_pts) {
        oss2.clear(); oss2.str(std::string()); oss2 << e.size();
        common::WrapText(&oss, &length, oss2.str(), 80);
      }
      ret.push_back(oss.str());
      ret.emplace_back("};");
    }

    auto func = semantic::FunctionBlock(
        "static inline int quantize(float val, unsigned fid)",
        semantic::PlainBlock(
           {"const float* array = &threshold[th_begin[fid]];",
            "int len = th_len[fid];",
            "int low = 0;",
            "int high = len;",
            "int mid;",
            "float mval;",
            "if (val < array[0]) {",
            "  return -10;",
            "}",
            "while (low + 1 < high) {",
            "  mid = (low + high) / 2;",
            "  mval = array[mid];",
            "  if (val == mval) {",
            "    return mid * 2;",
            "  } else if (val < mval) {",
            "    high = mid;",
            "  } else {",
            "    low = mid;",
            "  }",
            "}",
            "if (array[low] == val) {",
            "  return low * 2;",
            "} else if (high == len) {",
            "  return len * 2;",
            "} else {",
            "  return low * 2 + 1;",
            "}"})).Compile();
    ret.insert(ret.end(), func.begin(), func.end());
    return ret;
  }
  std::vector<std::string> Preprocessing() const {
    return quant_preamble;
  }
  bool QuantizeFlag() const {
    return true;
  }
 private:
  std::vector<std::string> quant_preamble;
};

inline std::vector<std::vector<tl_float>>
ExtractCutPoints(const Model& model) {
  std::vector<std::vector<tl_float>> cut_pts;

  std::vector<std::set<tl_float>> thresh_;
  cut_pts.resize(model.num_features);
  thresh_.resize(model.num_features);
  for (size_t i = 0; i < model.trees.size(); ++i) {
    const Tree& tree = model.trees[i];
    std::queue<int> Q;
    Q.push(0);
    while (!Q.empty()) {
      const int nid = Q.front();
      const Tree::Node& node = tree[nid];
      Q.pop();
      if (!node.is_leaf()) {
        const tl_float threshold = node.threshold();
        const unsigned split_index = node.split_index();
        thresh_[split_index].insert(threshold);
        Q.push(node.cleft());
        Q.push(node.cright());
      }
    }
  }
  for (int i = 0; i < model.num_features; ++i) {
    std::copy(thresh_[i].begin(), thresh_[i].end(),
              std::back_inserter(cut_pts[i]));
  }
  return cut_pts;
}

TREELITE_REGISTER_COMPILER(RecursiveCompiler, "recursive")
.describe("A compiler with a recursive approach")
.set_body([](const CompilerParam& param) -> Compiler* {
    if (param.quantize > 0) {
      return new RecursiveCompiler<Quantize>(param);
    } else {
      return new RecursiveCompiler<NoQuantize>(param);
    }
  });
}  // namespace compiler
}  // namespace treelite